# SRB2 Final Demo v1.09.4a - Nintendo 3DS

This is a Nintendo 3DS port of SRB2 Final Demo v1.09.4a. (not made by me but by someone else)

## MASSIVE UPDATE - OpenGL rendering guaranteed +1 FPS over software

Hardware rendering is finally here. (not actually OpenGL, it uses Citro3D)

This update adds:

- Experimental Citro3D hardware rendering.
- The option to use software or hardware rendering.
- Fixed sky rendering.
- A loading screen that preloads level textures and sprites.
- A LOT of FPS optimisations and it still runs like my poo sometimes.
- Some other stuff I forgot.

Hardware rendering is still experimental so don't expect miracles.

Mostly tested in Citra so uhhhh good luck on a real 3DS.

## How to NOT build

Install devkitPro with devkitARM, libctru, 3ds-zlib, libogg and Tremor/libvorbisidec.

Open devkitPro MSYS2 in the project directory and run:

**CIA:**

```bash
./build-finaldemo-cia.sh
```

**3DSX:**

```bash
./build-3ds.sh all
```

The CIA should be in `src/srb2_fd1094a_80mb.cia`.

Why I called it that is cuz i kept doing 64mb AND I HAD ENOUGH 

If it doesn't build then uhhhh good luck.

## Install

Put the 3DSX beside the Final Demo data files, or use `sdmc:/3ds/srb2_fd109/`.

The 3DS build uses `config3ds.cfg`. A renderer picker appears on the bottom screen before the game boots.

Use UP/DOWN and A to choose Software or experimental Citro3D HWR; B starts Software.

Nothing to edit on the SD card.

## Networking

Networking uses the normal Final Demo UDP driver and port 5029. PC and 3DS players must use the same v1.09.4a game/data files and matching add-ons.

The networking is highly unstable.

**Join Game (Specify IP)** opens the 3DS software keyboard. The custom `.` key can be used to enter an IPv4 address.

## Add-ons

Put Final Demo-compatible `.wad` or `.soc` files in the `addons` directory. Use **ADD-ONS** on the main menu to load them.

## Known issues

- Hardware rendering can run very slowly in some areas.
- MD2 models are invisible for now. Use `gr_md2 off`.
- Networking is still unstable.
- Hardware rendering hasn't been tested much on real hardware.
- Probably some other bugs I haven't found yet.

## yeah

I have spent way too much time trying to make the 3DS run this game faster.

It still struggles.

Enjoy
