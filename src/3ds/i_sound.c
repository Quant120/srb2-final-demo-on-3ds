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
/// \brief Nintendo 3DS sound interface

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <limits.h>
#include <tremor/ivorbisfile.h>
#include <stdlib.h>
#include <string.h>

#include "../doomdef.h"
#include "../command.h"
#include "../i_sound.h"
#include "../i_system.h"
#include "../sounds.h"
#include "../s_sound.h"
#include "../w_wad.h"
#include "../z_zone.h"

#define SFX_CHANNELS 23
#define MUSIC_CHANNEL 23
#define NORMAL_PITCH 128

#define MUSIC_RATE 22050
#define MUSIC_BUFFERS 4
#define MUSIC_FRAMES 2048
#define MUSIC_VOICES 32
#define MUS_TICK_RATE 140

#define DIG_OGG_STREAM_BUFFERS 2
#define DIG_OGG_BUFFER_SAMPLES 4096u
#define DIG_OGG_TEMP_BYTES 4096u
#define DIG_MAX_PCM_SAMPLES (22050u * 60u * 6u)

#define WAVE_TRIANGLE 0
#define WAVE_SQUARE   1
#define WAVE_SAW      2
#define WAVE_PULSE    3
#define WAVE_NOISE    4

byte sound_started = false;
byte music_started = false;
byte midimusic_started = false;
byte digmusic_started = false;
byte cdaudio_started = false;
consvar_t cd_volume = {"cd_volume", "31", CV_SAVE, soundvolume_cons_t, NULL, 0, NULL, NULL, 0, 0, NULL};
consvar_t cdUpdate = {"cd_update", "1", CV_SAVE, NULL, NULL, 0, NULL, NULL, 0, 0, NULL};

typedef struct
{
	short *pcm;
	u32 samples;
	u16 rate;
} sfxsample_t;

typedef struct
{
	ndspWaveBuf wave;
	sfxsample_t *sample;
	int vol;
	int sep;
	int pitch;
	boolean active;
} sfxchan_t;

typedef struct
{
	u8 volume;
	u8 expression;
	u8 pan;
	u8 program;
	u8 pitch;
	u8 lastvelocity;
} muschan_t;

typedef struct
{
	boolean active;
	boolean releasing;
	u8 channel;
	u8 note;
	u8 velocity;
	u8 waveform;
	u32 phase;
	u32 step;
	u32 envelope;
	u32 releasestep;
	int leftgain;
	int rightgain;
} musvoice_t;

typedef enum
{
	MIDI_EVENT_NOTE_OFF,
	MIDI_EVENT_NOTE_ON,
	MIDI_EVENT_CONTROL,
	MIDI_EVENT_PROGRAM,
	MIDI_EVENT_PITCH,
	MIDI_EVENT_TEMPO
} midieventtype_t;

typedef struct
{
	u32 tick;
	u32 order;
	u32 value;
	u8 type;
	u8 channel;
	u8 a;
	u8 b;
} midievent_t;

static sfxchan_t channels[SFX_CHANNELS];
static int nextchannel;
static int sfxvolume = 31;

static short *musicpcm;
static ndspWaveBuf musicwaves[MUSIC_BUFFERS];
static muschan_t muschannels[16];
static musvoice_t musvoices[MUSIC_VOICES];
static u32 notesteps[128];
static u32 bendtable[256];
static u32 noisestate = 0x12345678u;

static u8 *musicdata;
static size_t musicdatalen;
static const u8 *scorestart;
static const u8 *scoreend;
static const u8 *scorepos;
static int samplestoevent;
static u32 tickremainder;
static int musicvol = 20;
static boolean musicready;
static boolean musicloaded;
static boolean musicplaying;
static boolean musicpaused;
static boolean musiclooping;
static boolean musicended;
static boolean audiosuspended;

static boolean ismidi;
static midievent_t *midievents;
static size_t nummidievents;
static size_t maxmidievents;
static size_t midieventindex;
static u32 midicurrenttick;
static u32 mididivision;
static u32 miditempo;
static u64 midisamplerem;


typedef enum
{
	MUSIC_BACKEND_NONE,
	MUSIC_BACKEND_MIDI,
	MUSIC_BACKEND_DIGITAL
} musicbackend_t;

typedef struct
{
	const u8 *data;
	size_t size;
	size_t pos;
} digmemfile_t;

static musicbackend_t musicbackend;
static ndspWaveBuf digwave;
static ndspWaveBuf oggwaves[DIG_OGG_STREAM_BUFFERS];
static short *digpcm;
static short *oggpcm[DIG_OGG_STREAM_BUFFERS];
static size_t digbytes;
static u32 digrate = MUSIC_RATE;
static u32 digsamples;
static int digvolume = 20;
static boolean digloaded;
static boolean digplaying;
static boolean digpaused;
static boolean diglooping;
static boolean digisogg;
static boolean oggopen;
static boolean oggeof;
static u8 *oggdata;
static size_t oggdatalen;
static boolean oggdatalinear;
static boolean oggdataowned;
static digmemfile_t oggsource;
static OggVorbis_File oggfile;

static int clamp(int value, int low, int high)
{
	if (value < low) return low;
	if (value > high) return high;
	return value;
}

static int clampsample(int value)
{
	if (value < -32768) return -32768;
	if (value > 32767) return 32767;
	return value;
}

static u16 readle16(const u8 *p)
{
	return (u16)(p[0] | ((u16)p[1] << 8));
}

static u16 readbe16(const u8 *p)
{
	return (u16)(((u16)p[0] << 8) | p[1]);
}

static u32 readbe32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16)
		| ((u32)p[2] << 8) | (u32)p[3];
}

static boolean channelplaying(int channel)
{
	sfxchan_t *c;
	if (channel < 0 || channel >= SFX_CHANNELS)
		return false;
	c = &channels[channel];
	if (!c->active)
		return false;
	if (c->wave.status == NDSP_WBUF_DONE || c->wave.status == NDSP_WBUF_FREE)
	{
		c->active = false;
		c->sample = NULL;
		return false;
	}
	return true;
}

static void setchannel(int channel, int vol, int sep, int pitch)
{
	sfxchan_t *c = &channels[channel];
	float mix[12];
	float gain;
	float rate;
	int left, right;

	vol = clamp(vol, 0, 255);
	sep = clamp(sep, 0, 255);
	pitch = clamp(pitch, 1, 255);

	gain = ((float)(vol + 1) * (float)sfxvolume) / (256.0f * 31.0f);
	left = clamp((255 - sep) * 2, 0, 255);
	right = clamp(sep * 2, 0, 255);
	memset(mix, 0, sizeof(mix));
	mix[0] = gain * (float)left / 255.0f;
	mix[1] = gain * (float)right / 255.0f;
	ndspChnSetMix(channel, mix);

	if (c->sample)
	{
		rate = (float)c->sample->rate * (float)pitch / (float)NORMAL_PITCH;
		if (rate < 1000.0f) rate = 1000.0f;
		if (rate > 48000.0f) rate = 48000.0f;
		ndspChnSetRate(channel, rate);
	}

	c->vol = vol;
	c->sep = sep;
	c->pitch = pitch;
}

void *I_GetSfx(sfxinfo_t *sfx)
{
	void *lump;
	const u8 *cursor;
	u16 version, rate;
	u32 samples, i;
	int lump_length;
	sfxsample_t *result;

	if (!sfx)
		return NULL;
	if (sfx->lumpnum < 0)
		sfx->lumpnum = S_GetSfxLumpNum(sfx);
	if (sfx->lumpnum < 0)
		return NULL;

	lump_length = W_LumpLength(sfx->lumpnum);
	if (lump_length < 8)
		return NULL;

	lump = W_CacheLumpNum(sfx->lumpnum, PU_SOUND);
	if (!lump)
		return NULL;
	cursor = (const u8 *)lump;
	version = (u16)(cursor[0] | ((u16)cursor[1] << 8));
	rate = (u16)(cursor[2] | ((u16)cursor[3] << 8));
	samples = (u32)cursor[4] | ((u32)cursor[5] << 8)
		| ((u32)cursor[6] << 16) | ((u32)cursor[7] << 24);
	cursor += 8;

	if (version != 3 || rate == 0 || samples == 0
		|| samples > (u32)(lump_length - 8))
	{
		Z_Free(lump);
		return NULL;
	}

	result = (sfxsample_t *)malloc(sizeof (*result));
	if (!result)
	{
		Z_Free(lump);
		return NULL;
	}
	result->pcm = (short *)linearAlloc((size_t)samples * sizeof (*result->pcm));
	if (!result->pcm)
	{
		free(result);
		Z_Free(lump);
		return NULL;
	}
	result->samples = samples;
	result->rate = rate;
	for (i = 0; i < samples; ++i)
		result->pcm[i] = (short)(((int)cursor[i] - 128) * 256);
	DSP_FlushDataCache(result->pcm, (size_t)samples * sizeof (*result->pcm));
	Z_Free(lump);
	return result;
}

void I_FreeSfx(sfxinfo_t *sfx)
{
	int i;
	sfxsample_t *sample;
	if (!sfx || !sfx->data)
		return;
	sample = (sfxsample_t *)sfx->data;
	for (i = 0; i < SFX_CHANNELS; ++i)
	{
		if (channels[i].sample == sample)
		{
			ndspChnWaveBufClear(i);
			channels[i].active = false;
			channels[i].sample = NULL;
		}
	}
	linearFree(sample->pcm);
	free(sample);
	sfx->data = NULL;
	sfx->lumpnum = -1;
}

void I_StartupSound(void)
{
	Result rc;
	int i;
	if (sound_started)
		return;
	rc = ndspInit();
	if (R_FAILED(rc))
	{
		return;
	}
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspSetMasterVol(1.0f);
	memset(channels, 0, sizeof (channels));
	for (i = 0; i < 24; ++i)
		ndspChnReset(i);
	nextchannel = 0;
	sfxvolume = 31;
	audiosuspended = false;
	sound_started = true;
}

void I_ShutdownSound(void)
{
	int i;
	if (!sound_started)
		return;
	I_ShutdownMusic();
	for (i = 0; i < SFX_CHANNELS; ++i)
	{
		ndspChnReset(i);
		channels[i].active = false;
		channels[i].sample = NULL;
	}
	ndspExit();
	audiosuspended = false;
	sound_started = false;
}

void I_3DSAudioSuspend(void)
{
	int i;

	if (!sound_started || audiosuspended)
		return;

	for (i = 0; i < SFX_CHANNELS; ++i)
		if (channels[i].active)
			ndspChnSetPaused(i, true);

	if ((musicbackend == MUSIC_BACKEND_MIDI
			&& musicplaying && !musicpaused)
		|| (musicbackend == MUSIC_BACKEND_DIGITAL
			&& digplaying && !digpaused))
		ndspChnSetPaused(MUSIC_CHANNEL, true);

	audiosuspended = true;
}

void I_3DSAudioSleep(void)
{
	if (sound_started)
		audiosuspended = true;
}

void I_3DSAudioResume(void)
{
	int i;

	if (!sound_started || !audiosuspended)
		return;

	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspSetMasterVol(1.0f);

	for (i = 0; i < SFX_CHANNELS; ++i)
		if (channels[i].active)
			ndspChnSetPaused(i, false);

	if ((musicbackend == MUSIC_BACKEND_MIDI
			&& musicplaying && !musicpaused)
		|| (musicbackend == MUSIC_BACKEND_DIGITAL
			&& digplaying && !digpaused))
		ndspChnSetPaused(MUSIC_CHANNEL, false);

	audiosuspended = false;
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
	int channel, i;
	sfxsample_t *sample;
	(void)priority;

	if (!sound_started || audiosuspended || id <= 0 || id >= NUMSFX)
		return 0;
	sample = (sfxsample_t *)S_sfx[id].data;
	if (!sample)
		return 0;

	channel = -1;
	for (i = 0; i < SFX_CHANNELS; ++i)
	{
		int candidate = (nextchannel + i) % SFX_CHANNELS;
		if (!channelplaying(candidate))
		{
			channel = candidate;
			break;
		}
	}
	if (channel < 0)
		channel = nextchannel;
	nextchannel = (channel + 1) % SFX_CHANNELS;

	ndspChnReset(channel);
	memset(&channels[channel], 0, sizeof(channels[channel]));
	channels[channel].sample = sample;
	channels[channel].active = true;
	ndspChnSetInterp(channel, NDSP_INTERP_LINEAR);
	ndspChnSetFormat(channel, NDSP_FORMAT_MONO_PCM16);
	setchannel(channel, vol, sep, pitch);
	channels[channel].wave.data_pcm16 = sample->pcm;
	channels[channel].wave.nsamples = sample->samples;
	channels[channel].wave.looping = false;
	DSP_FlushDataCache(sample->pcm, (size_t)sample->samples * sizeof(*sample->pcm));
	ndspChnWaveBufAdd(channel, &channels[channel].wave);
	return channel + 1;
}

void I_StopSound(int handle)
{
	int channel = handle - 1;
	if (channel < 0 || channel >= SFX_CHANNELS)
		return;
	ndspChnWaveBufClear(channel);
	channels[channel].active = false;
	channels[channel].sample = NULL;
}

int I_SoundIsPlaying(int handle)
{
	return channelplaying(handle - 1);
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
	int channel = handle - 1;
	if (channel < 0 || channel >= SFX_CHANNELS || !channelplaying(channel))
		return;
	setchannel(channel, vol, sep, pitch);
}

void I_SetSfxVolume(int volume)
{
	int i;
	sfxvolume = clamp(volume, 0, 31);
	for (i = 0; i < SFX_CHANNELS; ++i)
		if (channelplaying(i))
			setchannel(i, channels[i].vol,
				channels[i].sep, channels[i].pitch);
}

static void musicvolume(void)
{
	float mix[12];
	float gain = (float)clamp(musicvol, 0, 31) / 31.0f;
	memset(mix, 0, sizeof(mix));
	mix[0] = gain;
	mix[1] = gain;
	if (sound_started)
		ndspChnSetMix(MUSIC_CHANNEL, mix);
}

static void musicsetup(void)
{
	ndspChnReset(MUSIC_CHANNEL);
	ndspChnSetInterp(MUSIC_CHANNEL, NDSP_INTERP_NONE);
	ndspChnSetFormat(MUSIC_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
	ndspChnSetRate(MUSIC_CHANNEL, (float)MUSIC_RATE);
	musicvolume();
}

static void updatevoicestep(musvoice_t *voice)
{
	u32 bend;
	if (!voice || !voice->active)
		return;
	bend = bendtable[muschannels[voice->channel].pitch];
	voice->step = (u32)(((u64)notesteps[voice->note] * bend) >> 16);
}

static void updatevoicegain(musvoice_t *voice)
{
	muschan_t *ch;
	u64 scaled;
	int base;
	int pan;
	if (!voice || !voice->active)
		return;
	ch = &muschannels[voice->channel];
	scaled = (u64)voice->velocity * ch->volume * ch->expression * 8192u;
	base = (int)(scaled / (127u * 127u * 127u));
	pan = ch->pan;
	voice->leftgain = (base * (127 - pan)) / 127;
	voice->rightgain = (base * pan) / 127;
}

static void updatechannelvoices(int channel, boolean step, boolean gain)
{
	int i;
	for (i = 0; i < MUSIC_VOICES; ++i)
	{
		if (!musvoices[i].active || musvoices[i].channel != channel)
			continue;
		if (step)
			updatevoicestep(&musvoices[i]);
		if (gain)
			updatevoicegain(&musvoices[i]);
	}
}

static int programwave(int program)
{
	if (program < 8) return WAVE_TRIANGLE;
	if (program < 24) return WAVE_SQUARE;
	if (program < 40) return WAVE_SAW;
	if (program < 56) return WAVE_TRIANGLE;
	if (program < 72) return WAVE_SQUARE;
	if (program < 88) return WAVE_TRIANGLE;
	if (program < 104) return WAVE_PULSE;
	return WAVE_SAW;
}

static musvoice_t *allocvoice(void)
{
	int i;
	int quietest = 0;
	u32 quietest_env = 0xffffffffu;
	for (i = 0; i < MUSIC_VOICES; ++i)
	{
		if (!musvoices[i].active)
			return &musvoices[i];
		if (musvoices[i].envelope < quietest_env)
		{
			quietest = i;
			quietest_env = musvoices[i].envelope;
		}
	}
	return &musvoices[quietest];
}

static void noteon(int channel, int note, int velocity)
{
	musvoice_t *voice;
	if (channel < 0 || channel >= 16 || note < 0 || note > 127)
		return;
	voice = allocvoice();
	memset(voice, 0, sizeof(*voice));
	voice->active = true;
	voice->channel = (u8)channel;
	voice->note = (u8)note;
	voice->velocity = (u8)clamp(velocity, 1, 127);
	voice->phase = 0;
	voice->envelope = 0;
	voice->releasestep = 48;
	voice->waveform = (u8)programwave(muschannels[channel].program);

	if ((ismidi && channel == 9) || (!ismidi && channel == 15))
	{
		voice->envelope = 65535;
		voice->releasing = true;
		if (note == 35 || note == 36)
		{
			voice->waveform = WAVE_TRIANGLE;
			voice->releasestep = 18;
		}
		else
		{
			voice->waveform = WAVE_NOISE;
			voice->releasestep = (note >= 42 && note <= 46) ? 96 : 32;
		}
	}

	updatevoicestep(voice);
	updatevoicegain(voice);
}

static void noteoff(int channel, int note)
{
	int i;
	for (i = 0; i < MUSIC_VOICES; ++i)
	{
		if (musvoices[i].active && musvoices[i].channel == channel
			&& musvoices[i].note == note)
			musvoices[i].releasing = true;
	}
}

static void allnotes(int channel, boolean immediate)
{
	int i;
	for (i = 0; i < MUSIC_VOICES; ++i)
	{
		if (!musvoices[i].active || musvoices[i].channel != channel)
			continue;
		if (immediate)
			musvoices[i].active = false;
		else
			musvoices[i].releasing = true;
	}
}

static void resetmusic(void)
{
	int i;
	memset(musvoices, 0, sizeof (musvoices));
	memset(muschannels, 0, sizeof (muschannels));
	for (i = 0; i < 16; ++i)
	{
		muschannels[i].volume = 100;
		muschannels[i].expression = 127;
		muschannels[i].pan = 64;
		muschannels[i].program = 0;
		muschannels[i].pitch = 128;
		muschannels[i].lastvelocity = 64;
	}
	scorepos = scorestart;
	samplestoevent = 0;
	tickremainder = 0;
	midieventindex = 0;
	midicurrenttick = 0;
	miditempo = 500000;
	midisamplerem = 0;
	musicended = false;
}

static void endmusic(void);

static boolean readvarlen(const u8 **cursor, const u8 *end, u32 *value)
{
	u32 result = 0;
	u8 b;
	int count = 0;
	if (!cursor || !*cursor || !value)
		return false;
	do
	{
		if (*cursor >= end || ++count > 4)
			return false;
		b = *(*cursor)++;
		result = (result << 7) | (b & 0x7f);
	} while (b & 0x80);
	*value = result;
	return true;
}

static boolean addevent(u32 tick, u32 order, u8 type,
	u8 channel, u8 a, u8 b, u32 value)
{
	midievent_t *grown;
	size_t capacity;
	if (nummidievents >= maxmidievents)
	{
		capacity = maxmidievents ? maxmidievents * 2u : 1024u;
		if (capacity < maxmidievents
			|| capacity > ((size_t)-1) / sizeof(*midievents))
			return false;
		grown = (midievent_t *)realloc(midievents,
			capacity * sizeof(*midievents));
		if (!grown)
			return false;
		midievents = grown;
		maxmidievents = capacity;
	}
	midievents[nummidievents].tick = tick;
	midievents[nummidievents].order = order;
	midievents[nummidievents].value = value;
	midievents[nummidievents].type = type;
	midievents[nummidievents].channel = channel;
	midievents[nummidievents].a = a;
	midievents[nummidievents].b = b;
	++nummidievents;
	return true;
}

static int compareevents(const void *left, const void *right)
{
	const midievent_t *a = (const midievent_t *)left;
	const midievent_t *b = (const midievent_t *)right;
	if (a->tick < b->tick) return -1;
	if (a->tick > b->tick) return 1;
	if (a->order < b->order) return -1;
	if (a->order > b->order) return 1;
	return 0;
}

static boolean parsetrack(const u8 *cursor, const u8 *end,
	u32 *order)
{
	u32 tick = 0;
	u8 running = 0;
	while (cursor < end)
	{
		u32 delta;
		u8 status;
		u8 a = 0, b = 0;
		u8 kind, channel;
		boolean have_a = false;
		if (!readvarlen(&cursor, end, &delta))
			return false;
		if (0xffffffffu - tick < delta)
			return false;
		tick += delta;
		if (cursor >= end)
			return false;
		status = *cursor++;
		if (status < 0x80)
		{
			if (!running)
				return false;
			a = status;
			have_a = true;
			status = running;
		}
		else if (status < 0xf0)
			running = status;
		else
			running = 0;

		if (status == 0xff)
		{
			u8 meta;
			u32 length;
			if (cursor >= end)
				return false;
			meta = *cursor++;
			if (!readvarlen(&cursor, end, &length)
				|| (size_t)(end - cursor) < length)
				return false;
			if (meta == 0x51 && length == 3)
			{
				u32 tempo = ((u32)cursor[0] << 16)
					| ((u32)cursor[1] << 8) | cursor[2];
				if (tempo && !addevent(tick, (*order)++, MIDI_EVENT_TEMPO,
					0, 0, 0, tempo))
					return false;
			}
			cursor += length;
			if (meta == 0x2f)
				break;
			continue;
		}
		if (status == 0xf0 || status == 0xf7)
		{
			u32 length;
			if (!readvarlen(&cursor, end, &length)
				|| (size_t)(end - cursor) < length)
				return false;
			cursor += length;
			continue;
		}
		if (status >= 0xf0)
			return false;

		kind = status & 0xf0;
		channel = status & 0x0f;
		if (kind == 0xc0 || kind == 0xd0)
		{
			if (!have_a)
			{
				if (cursor >= end)
					return false;
				a = *cursor++;
			}
		}
		else
		{
			if (!have_a)
			{
				if (cursor >= end)
					return false;
				a = *cursor++;
			}
			if (cursor >= end)
				return false;
			b = *cursor++;
		}
		a &= 0x7f;
		b &= 0x7f;

		switch (kind)
		{
			case 0x80:
				if (!addevent(tick, (*order)++, MIDI_EVENT_NOTE_OFF,
					channel, a, b, 0)) return false;
				break;
			case 0x90:
				if (!addevent(tick, (*order)++, b ? MIDI_EVENT_NOTE_ON
					: MIDI_EVENT_NOTE_OFF, channel, a, b, 0)) return false;
				break;
			case 0xb0:
				if (!addevent(tick, (*order)++, MIDI_EVENT_CONTROL,
					channel, a, b, 0)) return false;
				break;
			case 0xc0:
				if (!addevent(tick, (*order)++, MIDI_EVENT_PROGRAM,
					channel, a, 0, 0)) return false;
				break;
			case 0xe0:
				if (!addevent(tick, (*order)++, MIDI_EVENT_PITCH,
					channel, a, b, 0)) return false;
				break;
			default:
				break;
		}
	}
	return true;
}

static boolean parsemidi(const u8 *data, size_t len)
{
	const u8 *cursor;
	const u8 *end;
	u32 header_length;
	u16 format, tracks, division;
	u32 order = 0;
	u16 track;
	if (!data || len < 14 || memcmp(data, "MThd", 4) != 0)
		return false;
	header_length = readbe32(data + 4);
	if (header_length < 6 || (size_t)header_length > len - 8)
		return false;
	format = readbe16(data + 8);
	tracks = readbe16(data + 10);
	division = readbe16(data + 12);
	if ((format != 0 && format != 1) || tracks == 0 || (division & 0x8000))
		return false;
	mididivision = division;
	cursor = data + 8 + header_length;
	end = data + len;
	for (track = 0; track < tracks; ++track)
	{
		u32 track_length;
		const u8 *track_end;
		while ((size_t)(end - cursor) >= 8 && memcmp(cursor, "MTrk", 4) != 0)
		{
			u32 chunk_length = readbe32(cursor + 4);
			if ((size_t)(end - cursor) < 8u + chunk_length)
				return false;
			cursor += 8u + chunk_length;
		}
		if ((size_t)(end - cursor) < 8 || memcmp(cursor, "MTrk", 4) != 0)
			return false;
		track_length = readbe32(cursor + 4);
		cursor += 8;
		if ((size_t)(end - cursor) < track_length)
			return false;
		track_end = cursor + track_length;
		if (!parsetrack(cursor, track_end, &order))
			return false;
		cursor = track_end;
	}
	if (!nummidievents)
		return false;
	qsort(midievents, nummidievents, sizeof(*midievents), compareevents);
	return true;
}

static void domidievent(const midievent_t *event)
{
	int channel;
	int pitch;
	if (!event)
		return;
	channel = event->channel;
	switch (event->type)
	{
		case MIDI_EVENT_NOTE_OFF:
			noteoff(channel, event->a);
			break;
		case MIDI_EVENT_NOTE_ON:
			noteon(channel, event->a, event->b);
			break;
		case MIDI_EVENT_CONTROL:
			switch (event->a)
			{
				case 7:
					muschannels[channel].volume = event->b;
					updatechannelvoices(channel, false, true);
					break;
				case 10:
					muschannels[channel].pan = event->b;
					updatechannelvoices(channel, false, true);
					break;
				case 11:
					muschannels[channel].expression = event->b;
					updatechannelvoices(channel, false, true);
					break;
				case 120:
					allnotes(channel, true);
					break;
				case 121:
					muschannels[channel].volume = 100;
					muschannels[channel].expression = 127;
					muschannels[channel].pan = 64;
					muschannels[channel].pitch = 128;
					updatechannelvoices(channel, true, true);
					break;
				case 123:
					allnotes(channel, false);
					break;
				default:
					break;
			}
			break;
		case MIDI_EVENT_PROGRAM:
			muschannels[channel].program = event->a;
			break;
		case MIDI_EVENT_PITCH:
			pitch = (((int)event->b << 7) | event->a) - 8192;
			pitch = 128 + pitch / 64;
			muschannels[channel].pitch = (u8)clamp(pitch, 0, 255);
			updatechannelvoices(channel, true, false);
			break;
		case MIDI_EVENT_TEMPO:
			if (event->value)
				miditempo = event->value;
			break;
		default:
			break;
	}
}

static void domidievents(void)
{
	int guard = 0;
	while (musicplaying && samplestoevent <= 0 && guard++ < 4096)
	{
		u32 event_tick;
		if (midieventindex >= nummidievents)
		{
			endmusic();
			return;
		}
		event_tick = midievents[midieventindex].tick;
		if (event_tick > midicurrenttick)
		{
			u32 delta = event_tick - midicurrenttick;
			u64 numerator = (u64)delta * MUSIC_RATE * miditempo
				+ midisamplerem;
			u64 denominator = (u64)mididivision * 1000000u;
			samplestoevent = (int)(numerator / denominator);
			midisamplerem = numerator % denominator;
			midicurrenttick = event_tick;
			if (samplestoevent <= 0)
				samplestoevent = 1;
			return;
		}
		while (midieventindex < nummidievents
			&& midievents[midieventindex].tick == midicurrenttick)
		{
			domidievent(&midievents[midieventindex]);
			++midieventindex;
		}
	}
}

static boolean readmusicbyte(u8 *out)
{
	if (!out || scorepos >= scoreend)
		return false;
	*out = *scorepos++;
	return true;
}

static boolean readmusicdelay(u32 *delay)
{
	u32 value = 0;
	u8 b;
	int count = 0;
	if (!delay)
		return false;
	do
	{
		if (!readmusicbyte(&b) || ++count > 5)
			return false;
		value = (value << 7) | (b & 0x7f);
	} while (b & 0x80);
	*delay = value;
	return true;
}

static void restartmusic(void)
{
	resetmusic();
}

static void endmusic(void)
{
	if (musiclooping)
		restartmusic();
	else
	{
		musicended = true;
		musicplaying = false;
	}
}

static void domusevents(void)
{
	int guard = 0;
	if (ismidi)
	{
		domidievents();
		return;
	}
	while (musicplaying && samplestoevent <= 0 && guard++ < 4096)
	{
		u8 event;
		boolean last;
		u32 delay;

		do
		{
			u8 type, channel, a, b;
			if (!readmusicbyte(&event))
			{
				endmusic();
				return;
			}
			last = (event & 0x80) != 0;
			type = (event >> 4) & 7;
			channel = event & 15;

			switch (type)
			{
				case 0: /* release note */
					if (!readmusicbyte(&a)) { endmusic(); return; }
					noteoff(channel, a & 0x7f);
					break;

				case 1: /* play note */
					if (!readmusicbyte(&a)) { endmusic(); return; }
					b = muschannels[channel].lastvelocity;
					if (a & 0x80)
					{
						if (!readmusicbyte(&b)) { endmusic(); return; }
						b &= 0x7f;
						muschannels[channel].lastvelocity = b;
					}
					noteon(channel, a & 0x7f, b);
					break;

				case 2: /* pitch wheel */
					if (!readmusicbyte(&a)) { endmusic(); return; }
					muschannels[channel].pitch = a;
					updatechannelvoices(channel, true, false);
					break;

				case 3: /* system event */
					if (!readmusicbyte(&a)) { endmusic(); return; }
					if (a == 10)
						allnotes(channel, true);
					else if (a == 11)
						allnotes(channel, false);
					break;

				case 4: /* controller or program */
					if (!readmusicbyte(&a) || !readmusicbyte(&b))
					{
						endmusic();
						return;
					}
					b &= 0x7f;
					if (a == 0)
						muschannels[channel].program = b;
					else if (a == 3)
					{
						muschannels[channel].volume = b;
						updatechannelvoices(channel, false, true);
					}
					else if (a == 4)
					{
						muschannels[channel].pan = b;
						updatechannelvoices(channel, false, true);
					}
					else if (a == 5)
					{
						muschannels[channel].expression = b;
						updatechannelvoices(channel, false, true);
					}
					break;

				case 6: /* score end */
					endmusic();
					return;

				default:
					endmusic();
					return;
			}
		} while (!last && musicplaying);

		if (!musicplaying)
			return;
		if (!readmusicdelay(&delay))
		{
			endmusic();
			return;
		}
		{
			u64 scaled = (u64)delay * MUSIC_RATE + tickremainder;
			samplestoevent = (int)(scaled / MUS_TICK_RATE);
			tickremainder = (u32)(scaled % MUS_TICK_RATE);
		}
	}
}

static int voicesample(musvoice_t *voice)
{
	u32 p;
	int sample;
	if (!voice->active)
		return 0;

	voice->phase += voice->step;
	p = voice->phase >> 16;

	switch (voice->waveform)
	{
		case WAVE_SQUARE:
			sample = (p < 32768) ? 26000 : -26000;
			break;
		case WAVE_SAW:
			sample = (int)p - 32768;
			break;
		case WAVE_PULSE:
			sample = (p < 16384) ? 28000 : -18000;
			break;
		case WAVE_NOISE:
			noisestate = noisestate * 1664525u + 1013904223u;
			sample = (short)(noisestate >> 16);
			break;
		case WAVE_TRIANGLE:
		default:
			sample = (p < 32768) ? (int)p : (65535 - (int)p);
			sample = sample * 2 - 32767;
			break;
	}

	if (!voice->releasing)
	{
		if (voice->envelope < 64511)
			voice->envelope += 1024;
		else
			voice->envelope = 65535;
	}
	else
	{
		if (voice->envelope <= voice->releasestep)
		{
			voice->active = false;
			return 0;
		}
		voice->envelope -= voice->releasestep;
	}

	return (sample * (int)(voice->envelope >> 8)) >> 8;
}

static void rendermusic(short *dst, int frames)
{
	int frame;
	if (!dst || frames <= 0)
		return;

	for (frame = 0; frame < frames; ++frame)
	{
		int left = 0;
		int right = 0;
		int i;

		if (musicplaying && !musicpaused)
		{
			if (samplestoevent <= 0)
				domusevents();

			for (i = 0; i < MUSIC_VOICES; ++i)
			{
				int sample;
				if (!musvoices[i].active)
					continue;
				sample = voicesample(&musvoices[i]);
				left += (sample * musvoices[i].leftgain) >> 13;
				right += (sample * musvoices[i].rightgain) >> 13;
			}

			if (samplestoevent > 0)
				--samplestoevent;
		}

		dst[frame * 2] = (short)clampsample(left);
		dst[frame * 2 + 1] = (short)clampsample(right);
	}
}

static void queuemusic(int index)
{
	short *buffer;
	if (!musicpcm || index < 0 || index >= MUSIC_BUFFERS)
		return;
	buffer = musicpcm + index * MUSIC_FRAMES * 2;
	rendermusic(buffer, MUSIC_FRAMES);
	DSP_FlushDataCache(buffer, MUSIC_FRAMES * 2 * sizeof(*buffer));
	memset(&musicwaves[index], 0, sizeof(musicwaves[index]));
	musicwaves[index].data_pcm16 = buffer;
	musicwaves[index].nsamples = MUSIC_FRAMES;
	musicwaves[index].looping = false;
	ndspChnWaveBufAdd(MUSIC_CHANNEL, &musicwaves[index]);
}


// DIGMUSIC I/O

static size_t oggread(void *ptr, size_t size, size_t nmemb, void *datasource)
{
	digmemfile_t *source = (digmemfile_t *)datasource;
	size_t available, items, bytes;
	if (!source || !ptr || !size || !nmemb)
		return 0;
	available = source->pos < source->size ? source->size - source->pos : 0;
	items = available / size;
	if (items > nmemb)
		items = nmemb;
	bytes = items * size;
	if (bytes)
	{
		memcpy(ptr, source->data + source->pos, bytes);
		source->pos += bytes;
	}
	return items;
}

static int oggseek(void *datasource, ogg_int64_t offset, int whence)
{
	digmemfile_t *source = (digmemfile_t *)datasource;
	ogg_int64_t position;
	if (!source)
		return -1;
	switch (whence)
	{
		case SEEK_SET: position = offset; break;
		case SEEK_CUR: position = (ogg_int64_t)source->pos + offset; break;
		case SEEK_END: position = (ogg_int64_t)source->size + offset; break;
		default: return -1;
	}
	if (position < 0 || (u64)position > (u64)source->size)
		return -1;
	source->pos = (size_t)position;
	return 0;
}

static int oggclose(void *datasource)
{
	(void)datasource;
	return 0;
}

static long oggtell(void *datasource)
{
	digmemfile_t *source = (digmemfile_t *)datasource;
	if (!source || source->pos > LONG_MAX)
		return -1;
	return (long)source->pos;
}

static void digsetvolume(void)
{
	float mix[12];
	float gain = (float)clamp(digvolume, 0, 31) / 31.0f;
	memset(mix, 0, sizeof(mix));
	mix[0] = gain;
	mix[1] = gain;
	ndspChnSetMix(MUSIC_CHANNEL, mix);
}

static void digsetup(void)
{
	ndspChnReset(MUSIC_CHANNEL);
	ndspChnSetInterp(MUSIC_CHANNEL, NDSP_INTERP_LINEAR);
	ndspChnSetFormat(MUSIC_CHANNEL, NDSP_FORMAT_MONO_PCM16);
	ndspChnSetRate(MUSIC_CHANNEL, (float)digrate);
	digsetvolume();
}

static void digstop(void)
{
	if (musicbackend == MUSIC_BACKEND_DIGITAL && sound_started)
		ndspChnWaveBufClear(MUSIC_CHANNEL);
	digplaying = false;
	digpaused = false;
	oggeof = false;
	memset(&digwave, 0, sizeof(digwave));
	memset(oggwaves, 0, sizeof(oggwaves));
	if (musicbackend == MUSIC_BACKEND_DIGITAL)
		musicbackend = MUSIC_BACKEND_NONE;
}

static void closeogg(void)
{
	int i;
	if (oggopen)
	{
		ov_clear(&oggfile);
		oggopen = false;
	}
	if (oggdata && oggdataowned)
	{
		if (oggdatalinear)
			linearFree(oggdata);
		else
			free(oggdata);
	}
	oggdata = NULL;
	oggdatalen = 0;
	oggdatalinear = false;
	oggdataowned = false;
	memset(&oggsource, 0, sizeof(oggsource));
	for (i = 0; i < DIG_OGG_STREAM_BUFFERS; ++i)
	{
		if (oggpcm[i])
			linearFree(oggpcm[i]);
		oggpcm[i] = NULL;
	}
}

static void unloaddig(void)
{
	digstop();
	if (digpcm)
		linearFree(digpcm);
	digpcm = NULL;
	closeogg();
	digbytes = 0;
	digrate = MUSIC_RATE;
	digsamples = 0;
	digloaded = false;
	digisogg = false;
}

static boolean loadwav(const u8 *raw, size_t rawlen,
	const u8 **data, u32 *rate, u32 *samples,
	u16 *bits, u16 *channels)
{
	size_t pos = 12;
	u16 format = 0, ch = 0, bps = 0;
	u32 samplerate = 0, datasize = 0;
	const u8 *pcm = NULL;
	if (!raw || rawlen < 44 || memcmp(raw, "RIFF", 4)
		|| memcmp(raw + 8, "WAVE", 4))
		return false;
	while (pos + 8 <= rawlen)
	{
		const u8 *chunk = raw + pos;
		u32 len = (u32)chunk[4] | ((u32)chunk[5] << 8)
			| ((u32)chunk[6] << 16) | ((u32)chunk[7] << 24);
		pos += 8;
		if ((size_t)len > rawlen - pos)
			break;
		if (!memcmp(chunk, "fmt ", 4) && len >= 16)
		{
			format = readle16(raw + pos);
			ch = readle16(raw + pos + 2);
			samplerate = (u32)raw[pos + 4] | ((u32)raw[pos + 5] << 8)
				| ((u32)raw[pos + 6] << 16) | ((u32)raw[pos + 7] << 24);
			bps = readle16(raw + pos + 14);
		}
		else if (!memcmp(chunk, "data", 4))
		{
			pcm = raw + pos;
			datasize = len;
		}
		pos += ((size_t)len + 1u) & ~(size_t)1u;
	}
	if (format != 1 || !pcm || !datasize || !samplerate
		|| (ch != 1 && ch != 2) || (bps != 8 && bps != 16))
		return false;
	*data = pcm;
	*rate = samplerate;
	*bits = bps;
	*channels = ch;
	*samples = datasize / (ch * (bps / 8));
	return *samples != 0;
}

static boolean openogg(const u8 *raw, size_t rawlen)
{
	ov_callbacks callbacks;
	vorbis_info *info;
	int i;
	if (!raw || rawlen < 16)
		return false;

	oggdata = (u8 *)raw;
	oggdatalen = rawlen;
	oggdatalinear = false;
	oggdataowned = false;
	oggsource.data = raw;
	oggsource.size = rawlen;
	oggsource.pos = 0;
	callbacks.read_func = oggread;
	callbacks.seek_func = oggseek;
	callbacks.close_func = oggclose;
	callbacks.tell_func = oggtell;
	if (ov_open_callbacks(&oggsource, &oggfile, NULL, 0, callbacks) < 0)
	{
		closeogg();
		return false;
	}
	oggopen = true;
	info = ov_info(&oggfile, -1);
	if (!info || info->rate < 8000 || info->rate > 48000
		|| info->channels < 1 || info->channels > 2)
	{
		closeogg();
		return false;
	}
	for (i = 0; i < DIG_OGG_STREAM_BUFFERS; ++i)
	{
		oggpcm[i] = (short *)linearAlloc(DIG_OGG_BUFFER_SAMPLES * sizeof(short));
		if (!oggpcm[i])
		{
			closeogg();
			return false;
		}
	}
	digrate = (u32)info->rate;
	digsamples = 0;
	digbytes = rawlen + DIG_OGG_STREAM_BUFFERS
		* DIG_OGG_BUFFER_SAMPLES * sizeof(short);
	digloaded = true;
	digisogg = true;
	oggeof = false;
	oggdataowned = true;
	return true;
}

static boolean loaddig(const u8 *raw, size_t rawlen)
{
	const u8 *data = NULL;
	u32 rate = MUSIC_RATE, samples = 0, i;
	u16 bits = 8, channels = 1;
	size_t bytes;
	short *pcm;
	unloaddig();
	if (!raw || rawlen < 16)
		return false;
	if (!memcmp(raw, "OggS", 4))
		return openogg(raw, rawlen);
	if (!loadwav(raw, rawlen, &data, &rate, &samples, &bits, &channels))
	{
		return false;
	}
	if (samples > DIG_MAX_PCM_SAMPLES)
		samples = DIG_MAX_PCM_SAMPLES;
	if (rate < 8000 || rate > 48000)
		rate = MUSIC_RATE;
	bytes = (size_t)samples * sizeof(short);
	pcm = (short *)linearAlloc(bytes);
	if (!pcm)
	{
		return false;
	}
	for (i = 0; i < samples; ++i)
	{
		int sample;
		if (bits == 8)
		{
			if (channels == 2)
				sample = (((int)data[i * 2] - 128) + ((int)data[i * 2 + 1] - 128)) / 2;
			else
				sample = (int)data[i] - 128;
			sample <<= 8;
		}
		else if (channels == 2)
		{
			short left = (short)readle16(data + i * 4);
			short right = (short)readle16(data + i * 4 + 2);
			sample = ((int)left + (int)right) / 2;
		}
		else
			sample = (short)readle16(data + i * 2);
		pcm[i] = (short)sample;
	}
	DSP_FlushDataCache(pcm, bytes);
	digpcm = pcm;
	digbytes = bytes;
	digrate = rate;
	digsamples = samples;
	digloaded = true;
	digisogg = false;
	return true;
}

static u32 filloggbuffer(int slot, boolean looping)
{
	short temp[DIG_OGG_TEMP_BYTES / sizeof(short)];
	u32 written = 0;
	int bitstream = 0;
	int decode_errors = 0;
	if (!oggopen || slot < 0 || slot >= DIG_OGG_STREAM_BUFFERS
		|| !oggpcm[slot])
		return 0;
	while (written < DIG_OGG_BUFFER_SAMPLES)
	{
		vorbis_info *info = ov_info(&oggfile, -1);
		long got;
		u32 framebytes, frames, frame;
		int channels;
		if (!info || info->channels < 1)
			break;
		channels = info->channels > 2 ? 2 : info->channels;
		got = ov_read(&oggfile, (char *)temp, sizeof(temp), &bitstream);
		if (got == 0)
		{
			if (looping && ov_pcm_seek(&oggfile, 0) == 0)
				continue;
			oggeof = true;
			break;
		}
		if (got < 0)
		{
			if (++decode_errors >= 8)
				break;
			continue;
		}
		decode_errors = 0;
		framebytes = (u32)channels * sizeof(short);
		frames = (u32)got / framebytes;
		for (frame = 0; frame < frames && written < DIG_OGG_BUFFER_SAMPLES; ++frame)
		{
			short *samples = (short *)((u8 *)temp + frame * framebytes);
			int sample = channels == 2
				? ((int)samples[0] + (int)samples[1]) / 2 : samples[0];
			oggpcm[slot][written++] = (short)sample;
		}
	}
	if (written)
		DSP_FlushDataCache(oggpcm[slot], written * sizeof(short));
	return written;
}

static void updatedigstream(void)
{
	int i;
	boolean active = false;
	if (musicbackend != MUSIC_BACKEND_DIGITAL
		|| !digplaying || digpaused)
		return;
	if (!digisogg)
	{
		if (!diglooping && (digwave.status == NDSP_WBUF_DONE
			|| digwave.status == NDSP_WBUF_FREE))
			digplaying = false;
		return;
	}
	for (i = 0; i < DIG_OGG_STREAM_BUFFERS; ++i)
	{
		if (oggwaves[i].status == NDSP_WBUF_DONE
			|| oggwaves[i].status == NDSP_WBUF_FREE)
		{
			u32 got = filloggbuffer(i, diglooping);
			if (got)
			{
				memset(&oggwaves[i], 0, sizeof(oggwaves[i]));
				oggwaves[i].data_pcm16 = oggpcm[i];
				oggwaves[i].nsamples = got;
				ndspChnWaveBufAdd(MUSIC_CHANNEL, &oggwaves[i]);
				active = true;
			}
		}
		else
			active = true;
	}
	if (!active && oggeof && !diglooping)
		digplaying = false;
}

void I_UpdateMusic(void)
{
	int i;
	if (audiosuspended)
		return;
	if (musicbackend == MUSIC_BACKEND_DIGITAL)
	{
		updatedigstream();
		return;
	}
	if (musicbackend != MUSIC_BACKEND_MIDI
		|| !musicready || !musicplaying || musicpaused)
		return;
	for (i = 0; i < MUSIC_BUFFERS; ++i)
	{
		if (musicwaves[i].status == NDSP_WBUF_DONE
			|| musicwaves[i].status == NDSP_WBUF_FREE)
			queuemusic(i);
	}
}

void I_InitMusic(void)
{
	int i;
	if (!sound_started)
		return;
	music_started = true;
	digmusic_started = true;
	if (musicready)
		return;

	for (i = 0; i < 128; ++i)
	{
		double frequency = 440.0 * pow(2.0, ((double)i - 69.0) / 12.0);
		double step = frequency * 4294967296.0 / (double)MUSIC_RATE;
		if (step < 1.0) step = 1.0;
		if (step > 4294967295.0) step = 4294967295.0;
		notesteps[i] = (u32)step;
	}
	for (i = 0; i < 256; ++i)
	{
		double semitones = ((double)i - 128.0) / 64.0;
		double ratio = pow(2.0, semitones / 12.0);
		bendtable[i] = (u32)(ratio * 65536.0);
	}

	musicpcm = (short *)linearAlloc(MUSIC_BUFFERS * MUSIC_FRAMES * 2 * sizeof (*musicpcm));
	if (!musicpcm)
	{
		return;
	}
	memset(musicpcm, 0, MUSIC_BUFFERS * MUSIC_FRAMES * 2 * sizeof (*musicpcm));
	memset(musicwaves, 0, sizeof (musicwaves));
	musicsetup();
	musicready = true;
	music_started = true;
	midimusic_started = true;
	musicvol = 20;
	musicvolume();
}

void I_ShutdownMusic(void)
{
	unloaddig();
	if (sound_started)
		ndspChnWaveBufClear(MUSIC_CHANNEL);
	musicplaying = false;
	musicpaused = false;
	musicloaded = false;
	musicready = false;
	music_started = false;
	midimusic_started = false;
	digmusic_started = false;
	musicbackend = MUSIC_BACKEND_NONE;
	if (musicdata)
	{
		free(musicdata);
		musicdata = NULL;
	}
	musicdatalen = 0;
	free(midievents);
	midievents = NULL;
	nummidievents = 0;
	maxmidievents = 0;
	ismidi = false;
	if (musicpcm)
	{
		linearFree(musicpcm);
		musicpcm = NULL;
	}
}

void I_SetMIDIMusicVolume(int volume)
{
	musicvol = clamp(volume, 0, 31);
	musicvolume();
}

void I_InitMIDIMusic(void)
{
	if (!musicready)
		I_InitMusic();
	midimusic_started = musicready;
}

void I_ShutdownMIDIMusic(void)
{
	if (musicbackend == MUSIC_BACKEND_MIDI)
		I_StopSong(1);
	midimusic_started = false;
}

void I_PauseSong(int handle)
{
	(void)handle;
	if (musicbackend == MUSIC_BACKEND_DIGITAL)
	{
		if (!digplaying || digpaused)
			return;
		digpaused = true;
		ndspChnSetPaused(MUSIC_CHANNEL, true);
		return;
	}
	if (musicbackend != MUSIC_BACKEND_MIDI
		|| !musicready || !musicplaying || musicpaused)
		return;
	musicpaused = true;
	ndspChnSetPaused(MUSIC_CHANNEL, true);
}

void I_ResumeSong(int handle)
{
	(void)handle;
	if (musicbackend == MUSIC_BACKEND_DIGITAL)
	{
		if (!digplaying || !digpaused)
			return;
		digpaused = false;
		ndspChnSetPaused(MUSIC_CHANNEL, false);
		return;
	}
	if (musicbackend != MUSIC_BACKEND_MIDI
		|| !musicready || !musicplaying || !musicpaused)
		return;
	musicpaused = false;
	ndspChnSetPaused(MUSIC_CHANNEL, false);
}

static boolean loadmusic(const void *data, size_t len)
{
	u16 score_length;
	u16 score_start;
	if (!musicready || !data || len < 4 || len > 0xffffffffu)
		return false;

	if (musicbackend == MUSIC_BACKEND_DIGITAL)
		unloaddig();
	if (musicready && sound_started)
		ndspChnWaveBufClear(MUSIC_CHANNEL);
	musicplaying = false;
	musicpaused = false;
	memset(musvoices, 0, sizeof (musvoices));

	if (musicdata)
	{
		free(musicdata);
		musicdata = NULL;
	}
	musicdatalen = 0;
	scorestart = NULL;
	scoreend = NULL;
	scorepos = NULL;
	free(midievents);
	midievents = NULL;
	nummidievents = 0;
	maxmidievents = 0;
	midieventindex = 0;
	ismidi = false;
	musicloaded = false;

	musicdata = (u8 *)malloc(len);
	if (!musicdata)
		return false;
	memcpy(musicdata, data, len);
	musicdatalen = len;

	if (len >= 16 && memcmp(musicdata, "MUS\x1a", 4) == 0)
	{
		score_length = readle16(musicdata + 4);
		score_start = readle16(musicdata + 6);
		if (score_start >= len || score_length == 0
			|| (size_t)score_start + score_length > len)
		{
			goto fail;
		}
		ismidi = false;
		scorestart = musicdata + score_start;
		scoreend = scorestart + score_length;
	}
	else if (memcmp(musicdata, "MThd", 4) == 0)
	{
		ismidi = true;
		nummidievents = 0;
		maxmidievents = 0;
		if (!parsemidi(musicdata, len))
		{
			goto fail;
		}
		scorestart = NULL;
		scoreend = NULL;
	}
	else
	{
		goto fail;
	}

	musicloaded = true;
	resetmusic();
	return true;

fail:
	free(musicdata);
	musicdata = NULL;
	musicdatalen = 0;
	free(midievents);
	midievents = NULL;
	nummidievents = 0;
	maxmidievents = 0;
	ismidi = false;
	musicloaded = false;
	return false;
}

int I_RegisterSong(void *data, int len)
{
	if (len <= 0 || !loadmusic(data, (size_t)len))
		return 0;
	return 1;
}

boolean I_PlaySong(int handle, int looping)
{
	int i;
	if (handle != 1 || !musicready || !musicloaded)
		return false;
	musicsetup();
	memset(musicwaves, 0, sizeof (musicwaves));
	musiclooping = looping != 0;
	musicplaying = true;
	musicbackend = MUSIC_BACKEND_MIDI;
	musicpaused = false;
	resetmusic();
	for (i = 0; i < MUSIC_BUFFERS; ++i)
		queuemusic(i);
	return true;
}

void I_StopSong(int handle)
{
	(void)handle;
	if (musicbackend != MUSIC_BACKEND_MIDI)
		return;
	if (musicready && sound_started)
		ndspChnWaveBufClear(MUSIC_CHANNEL);
	musicplaying = false;
	musicpaused = false;
	musicbackend = MUSIC_BACKEND_NONE;
	memset(musvoices, 0, sizeof (musvoices));
}

void I_UnRegisterSong(int handle)
{
	(void)handle;
	I_StopSong(handle);
	if (musicdata)
	{
		free(musicdata);
		musicdata = NULL;
	}
	musicdatalen = 0;
	scorestart = NULL;
	scoreend = NULL;
	scorepos = NULL;
	free(midievents);
	midievents = NULL;
	nummidievents = 0;
	maxmidievents = 0;
	midieventindex = 0;
	ismidi = false;
	musicloaded = false;
}

void I_InitDigMusic(void)
{
	if (!sound_started)
		I_StartupSound();
	digmusic_started = sound_started;
	if (digmusic_started)
		music_started = true;
}

void I_ShutdownDigMusic(void)
{
	unloaddig();
	digmusic_started = false;
}

boolean I_StartDigSong(const char *musicname, int looping)
{
	char lumpname[9];
	int lumpnum, length, i;
	void *raw;
	boolean queued = false;
	if (!musicname || !*musicname)
		return false;
	if (!digmusic_started)
		I_InitDigMusic();
	if (!digmusic_started)
		return false;
	snprintf(lumpname, sizeof(lumpname), "o_%.6s", musicname);
	lumpnum = W_CheckNumForName(lumpname);
	if (lumpnum < 0)
		return false;
	length = W_LumpLength(lumpnum);
	if (length < 16)
		return false;
	raw = malloc((size_t)length);
	if (!raw)
		return false;
	W_ReadLump(lumpnum, raw);
	if (musicbackend == MUSIC_BACKEND_MIDI)
		I_StopSong(1);
	if (!loaddig((const u8 *)raw, (size_t)length))
	{
		free(raw);
		return false;
	}
	if (!digisogg)
		free(raw);
	digsetup();
	diglooping = looping != 0;
	digpaused = false;
	oggeof = false;
	if (digisogg)
	{
		if (ov_pcm_seek(&oggfile, 0) < 0)
		{
			unloaddig();
			return false;
		}
		for (i = 0; i < DIG_OGG_STREAM_BUFFERS; ++i)
		{
			u32 got = filloggbuffer(i, diglooping);
			if (!got)
				continue;
			memset(&oggwaves[i], 0, sizeof(oggwaves[i]));
			oggwaves[i].data_pcm16 = oggpcm[i];
			oggwaves[i].nsamples = got;
			ndspChnWaveBufAdd(MUSIC_CHANNEL, &oggwaves[i]);
			queued = true;
		}
	}
	else if (digpcm && digsamples)
	{
		memset(&digwave, 0, sizeof(digwave));
		digwave.data_pcm16 = digpcm;
		digwave.nsamples = digsamples;
		digwave.looping = diglooping;
		DSP_FlushDataCache(digpcm, digbytes);
		ndspChnWaveBufAdd(MUSIC_CHANNEL, &digwave);
		queued = true;
	}
	if (!queued)
	{
		unloaddig();
		return false;
	}
	digplaying = true;
	musicbackend = MUSIC_BACKEND_DIGITAL;
	return true;
}

void I_StopDigSong(void)
{
	if (musicbackend == MUSIC_BACKEND_DIGITAL || digloaded)
		unloaddig();
}

void I_SetDigMusicVolume(int volume)
{
	digvolume = clamp(volume, 0, 31);
	if (musicbackend == MUSIC_BACKEND_DIGITAL)
		digsetvolume();
}

void I_InitCD(void) { cdaudio_started = false; }
void I_StopCD(void) {}
void I_PauseCD(void) {}
void I_ResumeCD(void) {}
void I_ShutdownCD(void) { cdaudio_started = false; }
void I_UpdateCD(void) {}
void I_PlayCD(int track, boolean looping) { (void)track; (void)looping; }
int I_SetVolumeCD(int volume) { return volume; }
