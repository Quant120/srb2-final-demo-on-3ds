// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 by id Software, Inc.
// Portions Copyright (C) 1998-2000 by DooM Legacy Team.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//-----------------------------------------------------------------------------
/// \file
/// \brief Nintendo 3DS system interface

#include <3ds.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "../doomdef.h"
#include "../doomstat.h"
#include "../d_event.h"
#include "../d_main.h"
#include "../d_net.h"
#include "../g_game.h"
#include "../g_input.h"
#include "../i_joy.h"
#include "../i_sound.h"
#include "../i_system.h"
#include "../i_video.h"
#include "../keys.h"
#include "../m_misc.h"
#include "i_3ds_memory.h"

#define SOC_BUFFER_SIZE (1u << 20)
#define MAX_3DS_EXIT_FUNCS 32

u32 __ctru_heap_size = SRB2_3DS_NORMAL_HEAP_SIZE;
u32 __ctru_linear_heap_size = SRB2_3DS_LINEAR_HEAP_SIZE;

byte graphics_started = false;
byte keyboard_started = false;
int i_love_bill = 0;

static void *socbuffer;
static boolean socready;
static boolean socattempted;
static boolean systemstarted;
static boolean shuttingdown;
static u64 timerepoch;
static u64 suspendtime;
static boolean suspended;
static boolean resumepending;
static aptHookCookie apthookcookie;
static boolean apthooked;
static void (*exitfuncs[MAX_3DS_EXIT_FUNCS])(void);
static size_t numexitfuncs;
int logstream = -1;

static void apthook(APT_HookType hook, void *param)
{
	(void)param;

	switch (hook)
	{
		case APTHOOK_ONSUSPEND:
			if (!suspended)
			{
				suspended = true;
				suspendtime = osGetTime();
				I_3DSAudioSuspend();
			}
			break;

		case APTHOOK_ONSLEEP:
			if (!suspended)
			{
				suspended = true;
				suspendtime = osGetTime();
				I_3DSAudioSleep();
			}
			break;

		case APTHOOK_ONRESTORE:
		case APTHOOK_ONWAKEUP:
			if (suspended)
			{
				timerepoch += osGetTime() - suspendtime;
				suspended = false;
			}
			resumepending = true;
			break;

		case APTHOOK_ONEXIT:
		default:
			break;
	}
}

boolean I_3DSNetworkReady(void)
{
	return socready;
}

boolean I_3DSEnsureNetworkReady(void)
{
	Result rc;

	if (socready)
		return true;
	if (socattempted)
		return false;

	socattempted = true;

	socbuffer = memalign(0x1000, SOC_BUFFER_SIZE);
	if (!socbuffer)
	{
		CONS_Printf("No TCP/IP driver detected\n");
		return false;
	}

	rc = socInit(socbuffer, SOC_BUFFER_SIZE);
	if (R_FAILED(rc))
	{
		CONS_Printf("No TCP/IP driver detected\n");
		free(socbuffer);
		socbuffer = NULL;
		return false;
	}

	socready = true;
	return true;
}

static void shutdownnetwork(void)
{
	if (socready)
		socExit();
	socready = false;
	socattempted = false;
	free(socbuffer);
	socbuffer = NULL;
}

void I_OutputMsg(const char *fmt, ...)
{
	char txt[1024];
	va_list ap;
	int len;

	va_start(ap, fmt);
	vsnprintf(txt, sizeof (txt), fmt, ap);
	va_end(ap);

	fputs(txt, stderr);
	fflush(stderr);

	if (logstream != INVALID_HANDLE_VALUE)
	{
		len = (int)strlen(txt);
		write(logstream, txt, (unsigned int)len);
	}
}

void I_AddExitFunc(void (*func)())
{
	if (func && numexitfuncs < MAX_3DS_EXIT_FUNCS)
		exitfuncs[numexitfuncs++] = func;
}

void I_RemoveExitFunc(void (*func)())
{
	size_t i;
	for (i = 0; i < numexitfuncs; ++i)
	{
		if (exitfuncs[i] == func)
		{
			memmove(&exitfuncs[i], &exitfuncs[i + 1],
				(numexitfuncs - i - 1) * sizeof (exitfuncs[0]));
			--numexitfuncs;
			return;
		}
	}
}

int I_StartupSystem(void)
{
	if (systemstarted)
		return 0;

	logstream = open("srb2log.txt", O_WRONLY|O_CREAT|O_TRUNC, 0666);
	gfxInitDefault();
	gfxSet3D(false);
	gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
	gfxSetDoubleBuffering(GFX_TOP, true);
	gfxSetDoubleBuffering(GFX_BOTTOM, false);
	consoleInit(GFX_BOTTOM, NULL);

	timerepoch = osGetTime();
	suspendtime = 0;
	suspended = false;
	resumepending = false;
	aptHook(&apthookcookie, apthook, NULL);
	apthooked = true;
	systemstarted = true;
	return 0;
}

void I_ShutdownSystem(void)
{
	size_t i;

	if (!systemstarted || shuttingdown)
		return;
	shuttingdown = true;

	for (i = numexitfuncs; i > 0; --i)
		if (exitfuncs[i - 1])
			exitfuncs[i - 1]();
	numexitfuncs = 0;

	shutdownnetwork();
	if (logstream >= 0)
	{
		close(logstream);
		logstream = -1;
	}
	if (apthooked)
	{
		aptUnhook(&apthookcookie);
		apthooked = false;
	}
	gfxExit();
	systemstarted = false;
}

void I_3DSPollLifecycle(void)
{
	if (!systemstarted)
		return;

	if (!aptMainLoop())
		I_Quit();

	if (resumepending)
	{
		resumepending = false;
		I_3DSVideoOnResume();
		I_3DSAudioResume();
	}
}

void I_Quit(void)
{
	static boolean quitting;
	if (quitting)
		exit(1);
	quitting = true;

	M_SaveConfig(NULL);
	G_SaveGameData();
	if (demorecording)
		G_CheckDemoStatus();
	D_QuitNetGame();
	I_ShutdownMusic();
	I_ShutdownSound();
	I_ShutdownCD();
	I_ShutdownGraphics();
	I_ShutdownSystem();
	exit(0);
}

void I_Error(const char *error, ...)
{
	char message[2048];
	va_list ap;

	va_start(ap, error);
	vsnprintf(message, sizeof (message), error, ap);
	va_end(ap);
	message[sizeof (message) - 1] = '\0';

	I_OutputMsg("Error: %s\n", message);
	consoleClear();
	printf("Error: %s\n\n", message);
	printf("Press START or B to exit.\n");
	fflush(stdout);

	while (aptMainLoop())
	{
		gspWaitForVBlank();
		hidScanInput();
		if (hidKeysDown() & (KEY_START | KEY_B))
			break;
	}

	I_ShutdownGraphics();
	I_ShutdownSystem();
	exit(1);
}

tic_t I_GetTime(void)
{
	u64 elapsed = osGetTime() - timerepoch;
	return (tic_t)((elapsed * TICRATE) / 1000u);
}

void I_StartupTimer(void)
{
	timerepoch = osGetTime();
}

void I_Sleep(void)
{
	I_3DSPollLifecycle();
	svcSleepThread(1000000LL);
}

void I_WaitVBL(int count)
{
	while (count-- > 0)
	{
		I_3DSPollLifecycle();
		gspWaitForVBlank();
	}
}

void I_OsPolling(void)
{
	I_GetEvent();
}
void I_GetJoystickEvents(void) {}
void I_GetJoystick2Events(void) {}
void I_GetMouseEvents(void) {}
void I_StartupMouse(void) {}
void I_StartupMouse2(void) {}
void I_StartupKeyboard(void) { keyboard_started = true; }

void I_Tactile(FFType Type, const JoyFF_t *Effect) { (void)Type; (void)Effect; }
void I_Tactile2(FFType Type, const JoyFF_t *Effect) { (void)Type; (void)Effect; }
void I_JoyScale(void) { Joystick.bGamepadStyle = false; }
void I_JoyScale2(void) { Joystick2.bGamepadStyle = false; }
void I_InitJoystick(void) { Joystick.bGamepadStyle = false; }
void I_InitJoystick2(void) { Joystick2.bGamepadStyle = false; }
int I_NumJoys(void) { return 1; }
const char *I_GetJoyName(int joyindex)
{
	return joyindex == 1 ? "Nintendo 3DS Circle Pad" : NULL;
}

int I_ConsoleKey(void) { return 0; }

int I_GetKey(void)
{
	event_t *ev;
	int key = 0;

	I_GetEvent();
	while (eventtail != eventhead)
	{
		ev = &events[eventtail];
		eventtail = (eventtail + 1) & (MAXEVENTS - 1);
		if (ev->type == ev_keydown)
		{
			key = ev->data1;
			if (key == KEY_JOY1)
				key = KEY_ENTER;
		}
	}
	return key;
}

ticcmd_t *I_BaseTiccmd(void)
{
	static ticcmd_t cmd;
	memset(&cmd, 0, sizeof(cmd));
	return &cmd;
}

ticcmd_t *I_BaseTiccmd2(void)
{
	static ticcmd_t cmd;
	memset(&cmd, 0, sizeof(cmd));
	return &cmd;
}

byte *I_AllocLow(int length)
{
	return (byte *)calloc(1, (size_t)length);
}

ULONG I_GetFreeMem(ULONG *total)
{
	struct mallinfo info = mallinfo();
	size_t used = info.uordblks;
	size_t heapsize = (size_t)__ctru_heap_size;

	if (total)
		*total = (ULONG)heapsize;

	return (ULONG)(used < heapsize ? heapsize - used : 0);
}

void I_GetDiskFreeSpace(INT64 *freespace)
{
	struct statvfs st;
	if (!freespace)
		return;
	if (statvfs(".", &st) == 0)
		*freespace = (INT64)st.f_bavail * (INT64)st.f_frsize;
	else
		*freespace = 0;
}

char *I_GetUserName(void)
{
	static char username[] = "Nintendo 3DS";
	return username;
}

int I_mkdir(const char *dirname, int unixright)
{
	if (mkdir(dirname, (mode_t)unixright) == 0 || errno == EEXIST)
		return 0;
	return -1;
}

const CPUInfoFlags *I_CPUInfo(void)
{
	static CPUInfoFlags info;
	memset(&info, 0, sizeof(info));
	return &info;
}

const char *I_LocateWad(void)
{
	return ".";
}

UINT64 I_FileSize(const char *filename)
{
	struct stat st;
	if(!filename || stat(filename,&st)<0) return (UINT64)-1;
	return (UINT64)st.st_size;
}

char *I_GetEnv(const char *name)
{
	return getenv(name);
}

int I_PutEnv(char *variable)
{
	return putenv(variable);
}
