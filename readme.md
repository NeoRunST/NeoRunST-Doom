# neorunst-doom — DOOM on NeoRunST

[DOOMGENERIC](https://github.com/ozkl/doomgeneric) 移植到 NeoRunST 的 NEP
用户程序格式。上游代码 (doomgeneric + Chocolate DOOM 内核) 遵循其原始
许可: **GPL-2.0** — 本仓库整体按 GPL-2.0 发布, 仅 `dg_nep.c` /
`doombuild.sh` / `m_misc.c` 的少量标注修改属于 NeoRunST 移植层。

GPL 只污染本仓库 (DOOM 上游衍生), 不影响 NeoRunST 内核与 SDK
(各自独立版权)。**不要把 DOOM 源码拷进内核或 dev 工具包目录。**

## 运行

需要 **v0.18.0 之后** 版本的内核

1. 新内核 `NeoRunST-CustomOS\kernel\bin\runstknl.nep` 替换 VM 磁盘里
   `ZZ://runst/system/runstknl.nep`
2. `doom.nep` 拷进磁盘
3. **doom1.wad** (id Software 共享版, ~4MB) 放磁盘 `/doom/` 目录
   (放在根目录也可以, 移植层会依次探测 `AA://doom/doom1.wad` 与
   `AA://doom1.wad`)
4. 文件管理器点击 `doom.nep` — 弹出 640x400 的 "DOOM" 窗口

## 操作

| 按键 | DOOM 动作 |
|------|-----------|
| 方向键 | 移动 / 转向 |
| Ctrl (左右都行) | 开火 |
| Shift | 跑 |
| 空格 | 开门 / 使用 |
| Y / N | 菜单确认 / 取消 |
| Esc | 菜单 |

窗口需要**聚焦** (点击一下) 才接收键盘。

## 移植层 (dg_nep.c)

DOOMGENERIC 的 6 个平台接口 → NEP:

| 接口 | NEP 实现 |
|------|----------|
| `DG_Init` | `nep_win_create("DOOM", 646, 428)` — 客户区恰好 640x400 (总尺寸 = 客户区 + 2x3px 边框 + 22px 标题栏) |
| `DG_DrawFrame` | `nep_win_draw_blit` 一次 syscall 全量灌入 DG_ScreenBuffer (640x400x4 = 1MB, malloc 在用户堆) |
| `DG_GetTicksMs` | `nep_ticks()` (syscall 44, PIT 1000Hz 开机毫秒) |
| `DG_SleepMs` | `nep_sleep()` (syscall 45, 任务挂起不耗 CPU) |
| `DG_GetKey` | 轮询 `nep_win_get_key` (syscall 46 raw 键队列) + set-1 scancode → DOOM 键位映射 |
| `DG_SetWindowTitle` | `nep_win_set_title` (syscall 47) |

- 音频无出口 (DOOMGENERIC 官方设定, `FEATURE_SOUND` 未定义)
- WAD 经 `-iwad` 传入 VFS 路径, stdio `fopen` 读取 (内核 FILE_READ
  允许堆地址缓冲)
- 入口 `nep_main()`: 探测 WAD → 组 argv → `doomgeneric_Create` →
  `doomgeneric_Tick()` 死循环 (PIT 抢占, 不冻结 GUI)
- 自补 freestanding 缺口: `strtod` (配置 %f 解析) 与编译器帮助函数
  `__powidf2`
- SDK shim 需求见开发者工具包 readme (unistd/fcntl/inttypes/sys/*
  占位头 + stdio 的 fprintf/sscanf 扩展)

## 构建

需要 x86_64-elf 工具链在 `D:\elf-tools\bin` (或改脚本里的路径),
以及 `..\dev\` 里的 SDK (buildnep.exe + 头文件 + tools\math\):

```sh
sh doombuild.sh        # Git Bash / MSYS 下运行
```

脚本做两件事: 逐文件编译 `build/*.o` (增量, 只重编改动过的), 再用
`dev\buildnep.exe -l` 链接全部 .o (含 musl libm 全量) 输出 `doom.nep`
(~1MB)。修改源码后重跑即可, 只编译变更文件。

## 已知限制

- 无音乐 (只有音效)
- 软件渲染: 每帧 1MB blit + WM 全窗口重绘, 帧率受限于 VESA 写入速度
- 存档/配置写在程序"当前目录"相对路径 (无盘符前缀), fopen 会失败 —
  存档功能不可用, 不影响游戏本体
- 系统重启后**第一次**运行 doom.nep 可能以 Page Fault(写 0x0) 退出,
  第二次运行恢复正常 (doomgeneric 已有 malloc 重试兜底但没根治)。
  疑似首次启动时内核物理页状态差异, 欢迎排查修复
- 极低概率 General Protection Fault; 已通过 syscall 48 注册异常处理器,
  崩溃时不再弹窗杀进程, 而是在控制台打出 `DOOM: fatal exception
  exc=.. addr=..` 后干净退出。现象概率性, 疑似内核抢占/上下文路径的
  罕见缺陷, 欢迎排查 (带 ErrCode/RFLAGS/CS/SS 的弹窗版本内核可辅助定位)
