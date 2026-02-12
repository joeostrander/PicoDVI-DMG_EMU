# PicoDVI-DMG_EMU

Play Gameboy DMG games with audio over HDMI with a Raspbery Pi Pico 2 (rp2350).  

**Updates**:  
- 2026.02.11 
  - Added instructions, 3D files, images, etc. for using a NESPi case  
	[**Click here for NESPi case instructions**](./NESPi_Metro2350.md)
- 2026.02.08 
  - added support for the Adafruit Metro RP2350 + HSTX to DVI Adapter.  (set USE_METRO_RP2350 = 1 in board_defs.h)
	- added support for original NES controller



Original (non-emulated) PicoDVI DMG capture project:
https://github.com/joeostrander/PicoDVI-DMG


![gameplay preview](./images/gameplay.gif?raw=true)

Using Adafruit Metro RP2350 (wireless option):  
![metro rp2350 wireless](./images/instructions/option1_internal_wireless.jpg?raw=true)  
Using Adafruit Metro RP2350 (wired option):  
![metro rp2350 wired](./images/instructions/option2_external.jpg?raw=true)  



Another option:  Using my custom PCB (not designed for NESPi):  
![pcb](./images/pcb_v1.png?raw=true)

![osd](./images/osd.jpg?raw=true) 


![metro_nespi](./images/instructions/assembled_front.jpg?raw=true) 


## [**Click here for NESPi case instructions**](./NESPi_Metro2350.md)

## Key Features

- **Peanut-GB CPU/PPU** with MiniGB APU for accurate DMG simulation at 60 Hz.
- **Direct HDMI output** at 640×480 or 800×600
	- Set resolution via cmake parameter:  -DRESOLUTION_MODE=2  
  - Modes:  
	- 0: 640x480, horizontally scaled x4, vertically x3 --> 640x480 Full screen  
	- 1: 800x600, horizontally scaled x4, vertically x4 --> 640x576 window
	- 2: 640x480, horizontally scaled x2, vertically x2 --> 320x288 window
	- Note: TVs might not support 800x600
- **SD Card ROMs** - load/stream ROMs directly from SD Card
- **Embedded ROM (optional)** powered by `software/scripts/gb_rom_to_header.py` (and the `convert_rom.bat` helper) so ROMs live in flash; no SD card or USB streaming required.
- **HDMI audio** sourced from the emulated APU using libdvi’s audio ring, synced to 32KHz with per-frame pacing.
- **On-Screen Display** - change color schemes, toggle frame blending, etc.
- **NES Classic controller input** over I²C with non-blocking startup/retry handling.
- **NES Original controller input** shift-register-based polling  

## Converting ROMs

1. Copy your `.gb`/`.gbc` into the `ROMS/` folder.
2. Run the helper script:

	 ```
	 software\scripts\convert_rom.bat ROMS\tetris.gb
	 ```

	 or

	 ```
	 py software\scripts\gb_rom_to_header.py ROMS\tetris.gb
	 ```

3. The script creates `software/apps/dmg_emu/roms/tetris_rom.h`, defines `tetris_rom_len`, places the data in a named flash section, and emits `#define ACTIVE_ROM_DATA`/`ACTIVE_ROM_LEN` macros.
4. Include the generated header in `software/apps/dmg_emu/main.c` (uncomment / switch the relevant `#include "roms/..."` line).

Symbols are auto-generated from the filename: lowercase, spaces to underscores, unsafe characters replaced, and prefixed if they would start with a digit.

## Building

Requirements:

- Pico SDK (1.5+ recommended) with submodules for libdvi/libsprite
- CMake 3.23+
- `arm-none-eabi-gcc` toolchain
- Python 3.11 (for ROM conversion) if you use the scripts

Build steps:

```
mkdir build
cd build
cmake -G "MinGW Makefiles" -DPICO_COPY_TO_RAM=1 -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 ..
cmake --build . -j4
```
or see build_clean.cmd for what I use

On Windows you can substitute `-G "MinGW Makefiles"` or use the VS Code CMake extension. The resulting UF2 lives under `software/apps/dmg_emu/` inside the build tree (follow the CMake output for the exact path).

## Flashing

1. Hold `BOOTSEL` on the Pico while plugging it in.
2. Copy the generated `dmg_emu.uf2` onto the exposed `RPI-RP2` drive.

## Controls / Shortcuts

- **NES Classic controller** maps to the standard Game Boy buttons.
- **Home** launch OSD
- **SELECT + START** launch OSD

## Credits

- Peanut-GB - **deltabeard** (https://github.com/deltabeard/Peanut-GB)
- RP2040-GB - **deltabeard** (https://github.com/deltabeard/RP2040-GB)
- Pico-GB - **YouMakeTech** (https://github.com/YouMakeTech/Pico-GB)
- MiniGB APU by **Alex Baines** / **fty94**
- libdvi / PicoDVI - **Wren6991**, plus prior PicoDVI-N64 inspiration from **kbeckmann**
- PicoDVI Audio support - **mlorenzati**
- Various Gameboy projects by **Andy West** (https://github.com/andy-west)
- GPT-5.1-Codex Max - helped me finally get the HDMI audio working and various other things


## License

See `LICENSE` for details. The ROM conversion script and emulator glue are MIT; Peanut-GB and MiniGB APU retain their respective upstream licenses (bundled in the tree).
