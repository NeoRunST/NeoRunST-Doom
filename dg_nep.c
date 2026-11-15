// ── dg_nep.c — doomgeneric 移植层 (NeoRunST NEP 平台) ──
// 6 个 DG_* 接口映射:
//   DG_Init        → nep_win_create 646x428 (客户区 640x400)
//   DG_DrawFrame   → nep_win_draw_blit 灌 DG_ScreenBuffer (0x00RRGGBB)
//   DG_GetTicksMs  → nep_ticks (PIT 1000Hz 开机毫秒)
//   DG_SleepMs     → nep_sleep (syscall 45, 挂起不耗 CPU)
//   DG_GetKey      → nep_win_get_key 轮询 raw 键队列 + scancode→doom key
//   DG_SetWindowTitle → nep_win_set_title
// WAD 经 -iwad 传入 VFS 路径 (doom1.wad 放 VM 磁盘 /doom/)。
// 声音无出口 (doomgeneric 官方设定), FEATURE_SOUND 未定义。

#include "doomkeys.h"
#include "doomgeneric.h"

#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "math.h"
#include "nep.h"

// ══════════════════ strtod (SDK stdlib 声明, 这里实现) ══════════════════
// 10/16 进制 + inf/nan (m_config 的 %f 默认值解析用)
double __powidf2(double a, int b);
double strtod(const char* s, char** end) {
    const char* p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }

    if ((p[0]|32) == 'i' && (p[1]|32) == 'n' && (p[2]|32) == 'f') {
        p += 3;
        if ((p[0]|32) == 'i' && (p[1]|32) == 'n' && (p[2]|32) == 'y') p += 3;
        if (end) *end = (char*)p;
        return neg ? -__builtin_inf() : __builtin_inf();
    }
    if ((p[0]|32) == 'n' && (p[1]|32) == 'a' && (p[2]|32) == 'n') {
        if (end) *end = (char*)(p + 3);
        return __builtin_nan("");
    }

    int hex = 0;
    if (p[0] == '0' && (p[1]|32) == 'x' &&
        (((p[2]|32) >= 'a' && (p[2]|32) <= 'f') ||
         (p[2] >= '0' && p[2] <= '9') || p[2] == '.')) {
        hex = 1; p += 2;
    }

    unsigned long long mant = 0;
    int ndig = 0, seen_dot = 0, any = 0;
    int exp10 = 0, exp2 = 0;

    while (1) {
        unsigned char c = (unsigned char)*p;
        int v;
        if (hex) {
            if (c >= '0' && c <= '9') v = c - '0';
            else if ((c|32) >= 'a' && (c|32) <= 'f') v = (c|32) - 'a' + 10;
            else break;
            any = 1;
            if (ndig < 19) { mant = (mant << 4) | (unsigned)v; ndig++; }
            if (seen_dot) exp2 -= 4;
            p++;
        } else {
            if (c < '0' || c > '9') {
                if (c != '.' || seen_dot) break;
                seen_dot = 1; p++;
                continue;
            }
            any = 1;
            v = c - '0';
            if (ndig < 19) { mant = mant * 10 + (unsigned)v; ndig++; }
            else if (!seen_dot) exp10++;
            if (seen_dot) exp10--;
            p++;
        }
    }
    if (!any) {
        if (end) *end = (char*)s;
        return 0.0;
    }

    if (hex) {
        if ((p[0]|32) == 'p') {
            int sg = 0, e = 0;
            p++;
            if (p[0] == '+' || p[0] == '-') { sg = (p[0] == '-'); p++; }
            while (*p >= '0' && *p <= '9') {
                if (e < 10000) e = e * 10 + (*p - '0');
                p++;
            }
            exp2 += sg ? -e : e;
        }
        if (end) *end = (char*)p;
        double v = ldexp((double)mant, exp2);
        return neg ? -v : v;
    }

    if ((p[0]|32) == 'e') {
        int sg = 0;
        long long e = 0;
        p++;
        if (p[0] == '+' || p[0] == '-') { sg = (p[0] == '-'); p++; }
        while (*p >= '0' && *p <= '9') {
            if (e < 100000) e = e * 10 + (*p - '0');
            p++;
        }
        exp10 += (int)(sg ? -e : e);
    }

    double v;
    if (mant == 0) v = 0.0;
    else if (exp10 == 0) v = (double)mant;
    else v = (double)mant * __powidf2(10.0, exp10);
    if (end) *end = (char*)p;
    return neg ? -v : v;
}

// 编译器帮助函数 (libgcc 缺失, freestanding 自补): 2 的整数幂
double __powidf2(double a, int b) {
    const int recip = (b < 0);
    double r = 1;
    while (1) {
        if (b & 1) r *= a;
        b /= 2;
        if (b == 0) break;
        a *= a;
    }
    return recip ? 1 / r : r;
}

// ══════════════════ DG_* 移植接口 ══════════════════

static int s_win = -1;

// set-1 make code → doom key (0 = 不映射)
static unsigned char convert_to_doom_key(const nep_key_event* ev) {
    if (ev->ext) {                       // 0xE0 扩展键 (方向键等)
        switch (ev->scancode) {
        case 0x48: return KEY_UPARROW;
        case 0x50: return KEY_DOWNARROW;
        case 0x4B: return KEY_LEFTARROW;
        case 0x4D: return KEY_RIGHTARROW;
        case 0x1D: return KEY_RCTRL;     // 右 Ctrl
        case 0x38: return KEY_RALT;
        default:   return 0;
        }
    }
    switch (ev->scancode) {
    case 0x1C: return KEY_ENTER;
    case 0x01: return KEY_ESCAPE;
    case 0x0E: return KEY_BACKSPACE;
    case 0x0F: return KEY_TAB;
    case 0x1D: return KEY_FIRE;          // 左 Ctrl = 开火
    case 0x2A:
    case 0x36: return KEY_RSHIFT;        // Shift = 跑
    case 0x38: return KEY_LALT;
    case 0x39: return KEY_USE;           // 空格 = 开门
    case 0x3A: return KEY_CAPSLOCK;
    case 0x4A: return KEYP_MINUS;
    case 0x4E: return KEYP_PLUS;
    default:
        return ev->ascii ? (unsigned char)ev->ascii : 0;
    }
}

void DG_Init(void) {
    // 客户区 = 总尺寸 - 6 宽 / 28 高 (WIN_BORDER 3 ×2 + 标题栏 22)
    s_win = nep_win_create("DOOM", 646, 428);
    if (s_win < 0) {
        printf("DOOM: nep_win_create failed\n");
        exit(1);
    }
    nep_win_set_title(s_win, "DOOM");
}

void DG_DrawFrame(void) {
    if (s_win < 0 || !DG_ScreenBuffer) return;
    // 640x400 一次 syscall 全量灌入 (0x00RRGGBB, 高字节 alpha=0 无影响)
    nep_win_draw_blit(s_win, 0, 0, DOOMGENERIC_RESX, DOOMGENERIC_RESY,
                      DG_ScreenBuffer);
}

void DG_SleepMs(uint32_t ms) {
    nep_sleep((long)ms);
}

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)nep_ticks();
}

int DG_GetKey(int* pressed, unsigned char* doomKey) {
    nep_key_event ev;
    if (nep_win_get_key(s_win, &ev) != 1) return 0;
    unsigned char key = convert_to_doom_key(&ev);
    if (!key) return 0;
    *pressed = ev.down ? 1 : 0;
    *doomKey = key;
    return 1;
}

void DG_SetWindowTitle(const char* title) {
    if (s_win >= 0) nep_win_set_title(s_win, title);
}

// ══════════════════ NEP 入口 ══════════════════

// [NEP port] 硬件异常处理: 注册后崩溃不再弹窗杀进程, 改为在控制台
// 打出异常现场后干净退出 (syscall 48; 类 Windows SEH)
static void doom_exc_handler(long exc, unsigned long addr) {
    printf("DOOM: fatal exception exc=%ld addr=0x%lx\n", exc, addr);
    printf("DOOM: exiting (see readme.md known issues)\n");
    nep_exit();
}

int nep_main(void) {
    nep_except_set(doom_exc_handler);   // 未接住深挖路径前先兜底报告

    // doom1.wad 放 VM 磁盘 /doom/ 目录 (文件管理器可见); 依次探测
    static char* wad_paths[] = {
        "AA://doom/doom1.wad",
        "AA://doom1.wad",
        0,
    };
    // 找到第一个存在的 WAD 路径塞进 argv (d_iwad 的 -iwad 只认给定的)
    char* argv[8];
    int argc = 0;
    argv[argc++] = (char*)"doomgeneric";
    {
        FILE* f = 0;
        int wad_arg = -1;
        for (int i = 0; wad_paths[i]; i++) {
            f = fopen(wad_paths[i], "rb");
            if (f) { fclose(f); wad_arg = i; break; }
        }
        if (wad_arg >= 0) {
            argv[argc++] = (char*)"-iwad";
            argv[argc++] = wad_paths[wad_arg];
        } else {
            printf("DOOM: doom1.wad not found (AA://doom/doom1.wad)\n");
        }
    }
    argv[argc] = 0;

    doomgeneric_Create(argc, argv);

    while (1) {
        doomgeneric_Tick();
    }
    return 0;
}
