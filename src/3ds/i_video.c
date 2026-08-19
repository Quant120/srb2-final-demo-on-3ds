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
/// \brief Nintendo 3DS graphics and input

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../doomdef.h"
#include "../doomstat.h"
#include "../command.h"
#include "../d_event.h"
#include "../d_main.h"
#include "../g_input.h"
#include "../i_joy.h"
#include "../i_system.h"
#include "../i_sound.h"
#include "../i_video.h"
#include "../keys.h"
#include "../screen.h"
#include "../v_video.h"

void I_UpdateMusic(void);

#define TOP_W 400
#define TOP_H 240
#define ANALOG_DEADZONE 20
#define CPAD_FULL_SCALE 145
#define NATIVE_X ((TOP_W - BASEVIDWIDTH) / 2)
#define NATIVE_Y ((TOP_H - BASEVIDHEIGHT) / 2)

rendermode_t rendermode = render_soft;
boolean highcolor = false;
boolean allow_fullscreen = false;

consvar_t cv_vidwait = {"vid_wait", "On", CV_SAVE, CV_OnOff, NULL, 0, NULL, NULL, 0, 0, NULL};
static int clearframes = 2;
static void Stretch_OnChange(void)
{
	vid.recalc = true;
	clearframes = 2;
}
static consvar_t cv_stretch = {"stretch", "Off", CV_SAVE|CV_CALL, CV_OnOff, Stretch_OnChange, 0, NULL, NULL, 0, 0, NULL};

static u16 palette_3ds[256];
static u32 previouskeys;
static int xmap[TOP_W];
static int ymap[TOP_H];

static void postkey(evtype_t type, int key)
{
	event_t ev;
	memset(&ev, 0, sizeof (ev));
	ev.type = type;
	ev.data1 = key;
	D_PostEvent(&ev);
}

static void handlebutton(u32 down, u32 up, u32 mask, int key)
{
	if (down & mask)
		postkey(ev_keydown, key);
	if (up & mask)
		postkey(ev_keyup, key);
}

static int cpadaxis(int raw)
{
	int sign = raw < 0 ? -1 : 1;
	int magnitude = raw < 0 ? -raw : raw;
	if (magnitude <= ANALOG_DEADZONE)
		return 0;
	if (magnitude > CPAD_FULL_SCALE)
		magnitude = CPAD_FULL_SCALE;
	magnitude -= ANALOG_DEADZONE;
	return sign * (magnitude * JOYAXISRANGE) / (CPAD_FULL_SCALE - ANALOG_DEADZONE);
}

void I_GetEvent(void)
{
	u32 held, down, up;
	circlePosition circle;
	event_t joy;

	I_3DSPollLifecycle();

	hidScanInput();
	held = hidKeysHeld();
	down = held & ~previouskeys;
	up = previouskeys & ~held;
	previouskeys = held;

	handlebutton(down, up, KEY_DUP, KEY_UPARROW);
	handlebutton(down, up, KEY_DDOWN, KEY_DOWNARROW);
	handlebutton(down, up, KEY_DLEFT, KEY_LEFTARROW);
	handlebutton(down, up, KEY_DRIGHT, KEY_RIGHTARROW);

	handlebutton(down, up, KEY_A, '/');
	handlebutton(down, up, KEY_A, KEY_ENTER);

	if (menuactive)
		handlebutton(down, up, KEY_B, KEY_ESCAPE);
	else
		handlebutton(down, up, KEY_B, '.');

	handlebutton(down, up, KEY_X, KEY_CTRL);     /* fire */
	handlebutton(down, up, KEY_Y, '\'');         /* light dash */
	handlebutton(down, up, KEY_L, '[');          /* strafe right */
	handlebutton(down, up, KEY_R, ']');          /* strafe left */
	handlebutton(down, up, KEY_START, KEY_ESCAPE);
	handlebutton(down, up, KEY_SELECT, KEY_CONSOLE);
	handlebutton(down, up, KEY_ZL, KEY_PGDN);
	handlebutton(down, up, KEY_ZR, KEY_PGUP);

	handlebutton(down, up, KEY_A, KEY_JOY1 + 0);
	handlebutton(down, up, KEY_B, KEY_JOY1 + 1);
	handlebutton(down, up, KEY_X, KEY_JOY1 + 2);
	handlebutton(down, up, KEY_Y, KEY_JOY1 + 3);
	handlebutton(down, up, KEY_L, KEY_JOY1 + 4);
	handlebutton(down, up, KEY_R, KEY_JOY1 + 5);

	hidCircleRead(&circle);
	memset(&joy, 0, sizeof (joy));
	joy.type = ev_joystick;
	joy.data1 = 0;
	joy.data2 = cpadaxis(circle.dx);
	joy.data3 = -cpadaxis(circle.dy);
	D_PostEvent(&joy);
}

void I_UpdateNoBlit(void) {}

static void presentframe(boolean waitvbl)
{
	u16 *fb;
	u16 fbw, fbh;
	const byte *src;
	int x, y;

	I_3DSPollLifecycle();
	I_UpdateMusic();

	if (!graphics_started || !screens[0])
		return;

	fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbw, &fbh);
	if (!fb)
		return;
	(void)fbw;
	(void)fbh;
	src = screens[0];

	if (clearframes > 0)
	{
		memset(fb, 0, TOP_W * TOP_H * sizeof (*fb));
		--clearframes;
	}
	if (cv_stretch.value)
	{
		for (x = 0; x < TOP_W; ++x)
		{
			u16 *dst = fb + x * TOP_H + (TOP_H - 1);
			int sx = xmap[x];
			for (y = 0; y < TOP_H; ++y)
				*dst-- = palette_3ds[src[ymap[y] + sx]];
		}
	}
	else
	{
		for (x = 0; x < BASEVIDWIDTH; ++x)
		{
			u16 *dst = fb + (x + NATIVE_X) * TOP_H
				+ (TOP_H - 1 - NATIVE_Y);
			const byte *column = src + x;
			for (y = 0; y < BASEVIDHEIGHT; ++y)
			{
				*dst-- = palette_3ds[*column];
				column += vid.rowbytes;
			}
		}
	}

	// top screen framebuffer
	GSPGPU_FlushDataCache(fb, TOP_W * TOP_H * sizeof (*fb));
	gfxScreenSwapBuffers(GFX_TOP, false);
	if (waitvbl)
		gspWaitForVBlank();
}

void I_FinishUpdate(void)
{
	presentframe(cv_vidwait.value != 0);
}

void I_ReadScreen(byte *scr)
{
	if (scr && screens[0])
		memcpy(scr, screens[0], (size_t)vid.width * vid.height * vid.bpp);
}

void I_SetPalette(RGBA_t *palette)
{
	int i;
	if (!palette)
		return;
	for (i = 0; i < 256; ++i)
		palette_3ds[i] = RGB8_to_565(palette[i].s.red,
			palette[i].s.green, palette[i].s.blue);
}

int VID_NumModes(void) { return 1; }
const char *VID_GetModeName(int modenum) { return modenum == 0 ? "320x200" : NULL; }
int VID_GetModeForSize(int w, int h) { (void)w; (void)h; return 0; }
void VID_PrepareModeList(void) {}

int VID_SetMode(int modenum)
{
	int x, y;
	(void)modenum;

	vid.modenum = 0;
	vid.width = BASEVIDWIDTH;
	vid.height = BASEVIDHEIGHT;
	vid.bpp = 1;
	vid.rowbytes = BASEVIDWIDTH;
	vid.recalc = true;
	vid.direct = NULL;
	vid.u.numpages = 1;

	free(vid.buffer);
	vid.buffer = (byte *)calloc((size_t)vid.rowbytes * vid.height, NUMSCREENS);
	if (!vid.buffer)
		I_Error("Not enough memory for video buffers\n");

	for (x = 0; x < TOP_W; ++x)
		xmap[x] = (x * vid.width) / TOP_W;
	for (y = 0; y < TOP_H; ++y)
		ymap[y] = ((y * vid.height) / TOP_H) * vid.rowbytes;
	clearframes = 2;
	return 1;
}

void I_3DSVideoOnResume(void)
{
	previouskeys = 0;
	clearframes = 2;
}

void I_StartupGraphics(void)
{
	if (graphics_started)
		return;
	CV_RegisterVar(&cv_vidwait);
	CV_RegisterVar(&cv_stretch);
	keyboard_started = true;
	VID_SetMode(0);
	graphics_started = true;
}

void I_ShutdownGraphics(void)
{
	if (!graphics_started)
		return;
	free(vid.buffer);
	vid.buffer = NULL;
	graphics_started = false;
	keyboard_started = false;
}
