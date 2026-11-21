#!/bin/sh
# doombuild.sh — 编译 doomgeneric 全部源 → build/*.o, 再 buildnep 链接 doom.nep
set -e
cd "$(dirname "$0")"
export PATH="/d/elf-tools/bin:$PATH"
TOOLCHAIN=/d/elf-tools/bin
CFLAGS="-ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -fno-builtin -O2 -w -DNEP_SOUND=1 -I ../dev/tools -I ../dev"

SRCS="dummy.c i_nepsound.c am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c d_iwad.c d_loop.c d_main.c d_mode.c d_net.c f_finale.c f_wipe.c g_game.c hu_lib.c hu_stuff.c info.c i_cdmus.c i_endoom.c i_joystick.c i_scale.c i_sound.c i_system.c i_timer.c memio.c m_argv.c m_bbox.c m_cheat.c m_config.c m_controls.c m_fixed.c m_menu.c m_misc.c m_random.c p_ceilng.c p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c p_map.c p_maputl.c p_mobj.c p_plats.c p_pspr.c p_saveg.c p_setup.c p_sight.c p_spec.c p_switch.c p_telept.c p_tick.c p_user.c r_bsp.c r_data.c r_draw.c r_main.c r_plane.c r_segs.c r_sky.c r_things.c sha1.c sounds.c statdump.c st_lib.c st_stuff.c s_sound.c tables.c v_video.c wi_stuff.c w_checksum.c w_file.c w_main.c w_wad.c z_zone.c w_file_stdc.c i_input.c i_video.c doomgeneric.c dg_nep.c"

mkdir -p build
for s in $SRCS; do
  # 依赖头文件 (heap.h/stdio.h 等改动也要重编): 用最新头时间戳比对
  NEWEST=$(ls -t ../dev/tools/heap.h ../dev/tools/stdlib.h ../dev/stdio.h ../dev/string.h ../dev/tools/nep.h 2>/dev/null | head -1)
  if [ ! -f "build/${s%.c}.o" ] || [ "$s" -nt "build/${s%.c}.o" ] || [ "$NEWEST" -nt "build/${s%.c}.o" ]; then
    echo "[CC] $s"
    $TOOLCHAIN/x86_64-elf-gcc -c "$s" -o "build/${s%.c}.o" $CFLAGS
  fi
done

# 链接 (buildnep -l: 全部 .o 直进链接, crt0 自动编译)
OBJS=""
for s in $SRCS; do OBJS="$OBJS build/${s%.c}.o"; done
MATHDIR=../dev/tools/math
# musl libm 全量 (43 个 .c, 含数据表与 __math_* 帮助函数)
for f in "$MATHDIR"/*.c; do
  m="$(basename "$f")"
  if [ ! -f "build/m_$m" ] || [ "$f" -nt "build/m_$m" ]; then
    echo "[CC] math/$m"
    $TOOLCHAIN/x86_64-elf-gcc -c "$f" -o "build/m_${m%.c}.o" $CFLAGS -I "$MATHDIR"
  fi
  OBJS="$OBJS build/m_${m%.c}.o"
done

../dev/buildnep.exe -l $OBJS -o doom.nep
