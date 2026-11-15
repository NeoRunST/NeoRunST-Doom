#include <stdio.h>

#include "m_argv.h"

#include "doomgeneric.h"

pixel_t* DG_ScreenBuffer = NULL;

void M_FindResponseFile(void);
void D_DoomMain (void);


void doomgeneric_Create(int argc, char **argv)
{
	// save arguments
    myargc = argc;
    myargv = argv;

	M_FindResponseFile();

	// NEP port: 首次启动时物理页可能暂时吃紧, malloc 失败重试兜底
	// (实测: 重启后第一次运行 100% 失败, 第二次 100% 成功)
	DG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);
	{
		int retry;
		for (retry = 0; !DG_ScreenBuffer && retry < 20; retry++)
		{
			DG_SleepMs(100);
			DG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);
		}
	}

	DG_Init();

	D_DoomMain ();
}

