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

static void useargsfile(int *argc, char ***argv)
{
	char **newargv;
	int i;

	if (access("3ds_args.txt", R_OK) != 0)
		return;

	for (i = 1; i < *argc; i++)
		if (!strcasecmp((*argv)[i], "@3ds_args.txt"))
			return;

	newargv = calloc((size_t)*argc + 2, sizeof (*newargv));
	if (!newargv)
		return;

	newargv[0] = (*argv)[0];
	newargv[1] = strdup("@3ds_args.txt");
	if (!newargv[1])
	{
		free(newargv);
		return;
	}

	if (*argc > 1)
		memcpy(&newargv[2], &(*argv)[1], (size_t)(*argc-1) * sizeof (*newargv));

	(*argc)++;
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

int main(int argc, char **argv)
{
	setworkdir(argc, argv);
	setenv("HOME", ".", 1);
	useconfig(&argc, &argv);
	useargsfile(&argc, &argv);

	myargc = argc;
	myargv = argv;

	I_StartupSystem();
	CONS_Printf("Setting up SRB2...\n");
	D_SRB2Main();
	CONS_Printf("Entering main game loop...\n");
	D_SRB2Loop();

	return 0;
}
