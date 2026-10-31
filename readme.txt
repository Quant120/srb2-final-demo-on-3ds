# SRB2 Final Demo v1.09.4a - Nintendo 3DS

This is a Nintendo 3DS port of SRB2 Final Demo v1.09.4a. (not made by me but by someone else)

## Build

Install devkitPro with devkitARM, libctru, 3ds-zlib, libogg and Tremor/libvorbisidec, then run:


./build-3ds.sh clean all


## Install

Put the 3DSX beside the Final Demo data files, or use `sdmc:/3ds/srb2_fd109/`.
The 3DS build uses `config3ds.cfg`. A renderer picker appears on the bottom screen before the game boots. Use UP/DOWN and A to choose Software or experimental Citro3D HWR; B starts Software. Nothing to edit on the SD card.

## Networking

Networking uses the normal Final Demo UDP driver and port 5029. PC and 3DS players must use the same v1.09.4a game/data files and matching add-ons.


The networking is highly unstable
**Join Game (Specify IP)** opens the 3DS software keyboard. The custom `.` key can be used to enter an IPv4 address.

## Add-ons

Put Final Demo-compatible `.wad` or `.soc` files in the `addons` directory. Use **ADD-ONS** on the main menu to load them.
