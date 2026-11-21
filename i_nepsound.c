// ── i_nepsound.c — DOOM 音效模块 (NeoRunST NEP 音频 syscall 出口) ──
// DOOM 环 3 程序没有声卡直访, 走 nep_aud_* (syscall 49~57):
//   - 打开一路 11025Hz 立体声混音源 (内核线性重采样 → 48kHz HDA)
//   - CacheSFX 读 DMX 格式 lump (0x03 头 + u8 PCM), 转 s16 原速率缓存
//   - I_UpdateSound 每 tic (35Hz) 软件混音 16 通道 → 315 帧 → 源环
//   - 音量 vol 0-15, 立体声分离 sep 0-254 (vanilla 语义)
// 音乐 (MUS/GENMIDI) 不在此模块范围 (需要 OPL 合成器, 未实现)。
// 注意: NEP 平坦镜像不含 .bss, 所有静态缓冲显式落 .data。

#include "config.h"
#include "doomtype.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "nep.h"

#define NEPS_NUM_CHANNELS 16
#define NEPS_SRC_RATE     11025          // 混音源采样率
#define NEPS_FRAMES_TICK  315            // 11025 / 35Hz = 每 tic 帧数

// NEP 平坦镜像只加载 .data (不含 .bss): 静态缓冲统一落 .data
#define NEPS_IN_DATA __attribute__((section(".data")))

// 单个缓存音效: s16 单声道, 原采样率
typedef struct neps_sound_s {
    int          rate;
    unsigned int nsamples;
    signed short* samples;             // malloc (heap.h)
    sfxinfo_t*   sfxinfo;              // 归属 (释放时清 driver_data)
    struct neps_sound_s* prev, *next;
    int          use_count;
} neps_sound_t;

// 一个播放通道
typedef struct {
    neps_sound_t* snd;
    unsigned int  pos;                 // 16.16 定点步进位置
    unsigned int  step;                // rate<<16 / 11025<<16
    int           vol_l, vol_r;        // 0..255
} neps_chan_t;

static neps_sound_t* neps_cache_head NEPS_IN_DATA;
static int           neps_cache_size NEPS_IN_DATA;   // 字节
#define NEPS_CACHE_MAX (512 * 1024)              // 缓存上限 (NEP 用户堆 ~1MB)

static neps_chan_t  neps_chans[NEPS_NUM_CHANNELS] NEPS_IN_DATA;
static signed short neps_mixbuf[NEPS_FRAMES_TICK * 2] NEPS_IN_DATA;
static int          neps_aud NEPS_IN_DATA = -1;
static int          neps_initialized NEPS_IN_DATA = 0;
static int          neps_use_sfx_prefix NEPS_IN_DATA = 0;

static snddevice_t neps_devices[] = {
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
};

// ── 缓存管理 ──

static void NepsUnlink(neps_sound_t* snd) {
    if (snd->prev) snd->prev->next = snd->next;
    else           neps_cache_head = snd->next;
    if (snd->next) snd->next->prev = snd->prev;
}

static void NepsFreeSound(neps_sound_t* snd) {
    NepsUnlink(snd);
    neps_cache_size -= snd->nsamples * 2;
    if (snd->sfxinfo) snd->sfxinfo->driver_data = 0;
    free(snd->samples);
    free(snd);
}

// 释放最久未用的音效, 腾出 len 字节。0 = 没得释放了
static int NepsFindAndFree(void) {
    neps_sound_t* snd = 0;
    for (neps_sound_t* s = neps_cache_head; s; s = s->next)
        if (s->use_count == 0) snd = s;      // 尾部 (最旧)
    if (!snd) return 0;
    NepsFreeSound(snd);
    return 1;
}

static void NepsReserve(size_t len) {
    while (neps_cache_size + (int)len > NEPS_CACHE_MAX)
        if (!NepsFindAndFree()) break;
}

// 加载 (或命中缓存) 一个音效并加锁。0 = 失败
static neps_sound_t* NepsLockSound(sfxinfo_t* sfxinfo) {
    if (sfxinfo->driver_data) {
        neps_sound_t* snd = (neps_sound_t*)sfxinfo->driver_data;
        snd->use_count++;
        return snd;
    }

    // 读 lump: DMX 格式 [0x03 0x00][u16 rate][u32 len][8B pad][u8 PCM...]
    int lumpnum = sfxinfo->lumpnum;
    unsigned int lumplen = (unsigned int)W_LumpLength(lumpnum);
    byte* lump = (byte*)W_CacheLumpNum(lumpnum, PU_STATIC);

    if (lumplen < 8 || lump[0] != 0x03 || lump[1] != 0x00) {
        W_ReleaseLumpNum(lumpnum);
        return 0;
    }
    int samplerate = (lump[3] << 8) | lump[2];
    unsigned int length =
        ((unsigned int)lump[7] << 24) | ((unsigned int)lump[6] << 16) |
        ((unsigned int)lump[5] << 8)  | (unsigned int)lump[4];
    if (samplerate <= 0 || length > lumplen - 8 || length <= 48) {
        W_ReleaseLumpNum(lumpnum);
        return 0;
    }

    // DMX 跳过头尾各 16 字节 (vanilla 行为)
    byte* data = lump + 16 + 8;
    length -= 32;

    // u8 → s16 (对称化), 原速率缓存
    NepsReserve(length * 2 + sizeof(neps_sound_t));
    neps_sound_t* snd = (neps_sound_t*)malloc(sizeof(neps_sound_t));
    signed short* buf = (signed short*)malloc(length * 2);
    if (!snd || !buf) {
        if (snd) free(snd);
        if (buf) free(buf);
        W_ReleaseLumpNum(lumpnum);
        return 0;
    }
    for (unsigned int i = 0; i < length; ++i)
        buf[i] = (signed short)((data[i] << 8) - 32768);   // 128 偏置对称展开

    snd->rate = samplerate;
    snd->nsamples = length;
    snd->samples = buf;
    snd->sfxinfo = sfxinfo;
    snd->use_count = 1;
    snd->prev = 0;
    snd->next = neps_cache_head;
    if (neps_cache_head) neps_cache_head->prev = snd;
    neps_cache_head = snd;
    neps_cache_size += length * 2;

    sfxinfo->driver_data = snd;
    W_ReleaseLumpNum(lumpnum);
    return snd;
}

static void NepsUnlockSound(neps_sound_t* snd) {
    if (snd->use_count > 0) snd->use_count--;
}

// ── 通道 ──

static void NepsReleaseChannel(int c) {
    if (neps_chans[c].snd) {
        NepsUnlockSound(neps_chans[c].snd);
        neps_chans[c].snd = 0;
    }
}

static void NepsApplyParams(int c, int vol, int sep) {
    neps_chan_t* ch = &neps_chans[c];
    int v = vol * 255 / 15;                    // vol 0..15 → 0..255
    if (v > 255) v = 255;
    if (sep < 0) sep = 0;
    if (sep > 254) sep = 254;
    ch->vol_l = ((254 - sep) * v) / 254;
    ch->vol_r = (sep * v) / 254;
}

// ── sound_module_t 接口 ──

static boolean NepsInit(boolean use_sfx_prefix) {
    neps_use_sfx_prefix = use_sfx_prefix;
    for (int i = 0; i < NEPS_NUM_CHANNELS; ++i) {
        neps_chans[i].snd = 0;
        neps_chans[i].pos = 0;
        neps_chans[i].step = 1 << 16;
        neps_chans[i].vol_l = 255;
        neps_chans[i].vol_r = 255;
    }
    // 立体声源 (sep 声像在软件混音里做), 内核重采样 → 48kHz
    neps_aud = nep_aud_open(NEPS_SRC_RATE, 2, 0);
    if (neps_aud < 0) {
        printf("NEPS: audio source open failed\n");
        return 0;
    }
    neps_initialized = 1;
    return 1;
}

static void NepsShutdown(void) {
    if (!neps_initialized) return;
    for (int i = 0; i < NEPS_NUM_CHANNELS; ++i) NepsReleaseChannel(i);
    while (neps_cache_head) NepsFreeSound(neps_cache_head);
    if (neps_aud >= 0) { nep_aud_close(neps_aud); neps_aud = -1; }
    neps_initialized = 0;
}

static int NepsGetSfxLumpNum(sfxinfo_t* sfx) {
    char namebuf[9];
    if (sfx->link != NULL) sfx = sfx->link;
    if (neps_use_sfx_prefix)
        M_snprintf(namebuf, sizeof(namebuf), "ds%s", sfx->name);
    else
        M_StringCopy(namebuf, sfx->name, sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

// 每 tic 调用 (35Hz): 软件混音 315 帧立体声 → 源环
static void NepsUpdate(void) {
    if (!neps_initialized) return;

    int n = NEPS_FRAMES_TICK * 2;              // 立体声交错
    for (int i = 0; i < n; i += 2) {
        int acc_l = 0, acc_r = 0;
        for (int c = 0; c < NEPS_NUM_CHANNELS; ++c) {
            neps_chan_t* ch = &neps_chans[c];
            if (!ch->snd) continue;
            unsigned int p = ch->pos >> 16;
            if (p >= ch->snd->nsamples) {      // 播完, 释放
                NepsReleaseChannel(c);
                continue;
            }
            int s = ch->snd->samples[p];
            acc_l += (s * ch->vol_l) >> 8;
            acc_r += (s * ch->vol_r) >> 8;
            ch->pos += ch->step;
        }
        // 限幅
        if (acc_l > 32767)  acc_l = 32767;
        if (acc_l < -32768) acc_l = -32768;
        if (acc_r > 32767)  acc_r = 32767;
        if (acc_r < -32768) acc_r = -32768;
        neps_mixbuf[i]     = (signed short)acc_l;
        neps_mixbuf[i + 1] = (signed short)acc_r;
    }

    // 灌源环 (16KB 环 ≈ 372ms 缓冲, 每 tic 只进 1260B, 不会满)
    nep_aud_write(neps_aud, neps_mixbuf, (long)(n * 2));
}

static void NepsUpdateSoundParams(int handle, int vol, int sep) {
    if (!neps_initialized || handle < 0 || handle >= NEPS_NUM_CHANNELS) return;
    NepsApplyParams(handle, vol, sep);
}

static int NepsStartSound(sfxinfo_t* sfxinfo, int channel, int vol, int sep) {
    if (!neps_initialized || channel < 0 || channel >= NEPS_NUM_CHANNELS)
        return -1;
    NepsReleaseChannel(channel);
    neps_sound_t* snd = NepsLockSound(sfxinfo);
    if (!snd) return -1;
    neps_chan_t* ch = &neps_chans[channel];
    ch->snd = snd;
    ch->pos = 0;
    // 定点步进: 原速率 → 11025
    ch->step = (unsigned int)(((unsigned long long)snd->rate << 16)
                              / NEPS_SRC_RATE);
    if (ch->step == 0) ch->step = 1;
    NepsApplyParams(channel, vol, sep);
    return channel;
}

static void NepsStopSound(int handle) {
    if (!neps_initialized || handle < 0 || handle >= NEPS_NUM_CHANNELS) return;
    NepsReleaseChannel(handle);
}

static boolean NepsSoundIsPlaying(int handle) {
    if (!neps_initialized || handle < 0 || handle >= NEPS_NUM_CHANNELS)
        return 0;
    neps_chan_t* ch = &neps_chans[handle];
    if (!ch->snd) return 0;
    return (ch->pos >> 16) < ch->snd->nsamples;
}

static void NepsPrecacheSounds(sfxinfo_t* sounds, int num_sounds) {
    // 惰性加载即可 (DOOM 音效都很小, 首次播放时缓存, 不预加载)
    (void)sounds;
    (void)num_sounds;
}

sound_module_t nep_sound_module = {
    neps_devices,
    arrlen(neps_devices),
    NepsInit,
    NepsShutdown,
    NepsGetSfxLumpNum,
    NepsUpdate,
    NepsUpdateSoundParams,
    NepsStartSound,
    NepsStopSound,
    NepsSoundIsPlaying,
    NepsPrecacheSounds,
};
