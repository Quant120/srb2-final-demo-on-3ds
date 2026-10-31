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
/// \brief Main program for Nintendo 3DS

#include <3ds.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#include "../doomdef.h"
#include "../console.h"
#include "../d_main.h"
#include "../m_argv.h"
#include "../i_system.h"
#include "i_3ds_memory.h"

u32 __stacksize__ = SRB2_3DS_STACK_SIZE;

static void useconfig(int *argc, char ***argv)
{
	char **newargv;
	int i;

	for (i = 1; i < *argc; i++)
		if (!strcasecmp((*argv)[i], "-config"))
			return;

	newargv = calloc((size_t)*argc + 3, sizeof (*newargv));
	if (!newargv)
		return;

	memcpy(newargv, *argv, (size_t)*argc * sizeof (*newargv));
	newargv[*argc] = strdup("-config");
	newargv[*argc+1] = strdup("config3ds.cfg");
	if (!newargv[*argc] || !newargv[*argc+1])
	{
		free(newargv[*argc]);
		free(newargv[*argc+1]);
		free(newargv);
		return;
	}

	*argc += 2;
	*argv = newargv;
}

static boolean hasdata(void)
{
	return access("srb2.srb", R_OK) == 0 || access("srb2.wad", R_OK) == 0;
}

static void setworkdir(int argc, char **argv)
{
	static const char *dirs[] =
	{
		"sdmc:/3ds/srb2_fd109",
		"/3ds/srb2_fd109",
		"3ds/srb2_fd109"
	};
	char cwd[PATH_MAX];
	char path[PATH_MAX];
	char *slash;
	int i;
	boolean gotcwd;

	gotcwd = getcwd(cwd, sizeof (cwd)) != NULL;

	if (argc && argv[0] && argv[0][0])
	{
		snprintf(path, sizeof (path), "%s", argv[0]);
		slash = strrchr(path, '/');
		if (slash)
		{
			*slash = '\0';
			if (path[0] && chdir(path) == 0)
			{
				if (hasdata())
					return;
				if (gotcwd)
					chdir(cwd);
			}
		}
	}

	if (hasdata())
		return;

	for (i = 0; i < (int)(sizeof (dirs)/sizeof (dirs[0])); i++)
	{
		if (chdir(dirs[i]) == 0)
		{
			if (hasdata())
				return;
			if (gotcwd)
				chdir(cwd);
		}
	}

	if (gotcwd)
		chdir(cwd);
}

// 29/10/2026: boot picker. do NOT move this into the SRB2 menu.
// Game boot has not happened yet. the GPU gets ONE chance to embarrass itself.
int hwr3ds_wanted = 0;

static int pick_renderer_before_the_game_steals_everything(void)
{
    int choice = 0; // software by default, because sometimes hardware is a potato
    u32 pressed;
    int dirty = 1;

    while (aptMainLoop())
    {
        if (dirty)
        {
            consoleClear();
            printf("SRB2 FINAL DEMO - 3DS\n");
            printf("=====================\n\n");
            printf("Pick renderer BEFORE boot:\n\n");
            printf("%s Software 320x200\n", choice == 0 ? ">" : " ");
            printf("%s Citro3D HWR 320x200 (WIP)\n", choice == 1 ? ">" : " ");
            printf("\nUP / DOWN : choose\n");
            printf("A : boot selected renderer\n");
            printf("B : boot software (panic button)\n\n");
            printf("HWR is experimental!\n");
            printf("your 3DS may invent new problems\n");
            fflush(stdout);
            gfxFlushBuffers();
            dirty = 0;
        }
        gspWaitForVBlank();
        hidScanInput();
        pressed = hidKeysDown();
        if (pressed & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT))
        {
            choice = !choice;
            dirty = 1;
        }
        if (pressed & KEY_B)
            return 0;
        if (pressed & KEY_A)
            return choice;
    }
    return -1; // home button killed us before the game booted. fair.
}

int main(int argc, char **argv)
{
	setworkdir(argc, argv);
	setenv("HOME", ".", 1);
	useconfig(&argc, &argv);

	myargc = argc;
	myargv = argv;

	I_StartupSystem();
	hwr3ds_wanted = pick_renderer_before_the_game_steals_everything();
	if (hwr3ds_wanted < 0)
	{
		I_ShutdownSystem();
		return 0;
	}
	consoleClear();
	CONS_Printf("Setting up SRB2...\n");
	D_SRB2Main();
	CONS_Printf("Entering main game loop...\n");
	D_SRB2Loop();

	return 0;
}
