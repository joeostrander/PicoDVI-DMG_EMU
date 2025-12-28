# PicoDVI-DMG_EMU

Game Boy emulation on the RP2350 with full HDMI video (via libdvi) and MiniGB APU audio. The original PicoDVI DMG capture pipeline has been replaced with an embedded Peanut-GB core, so the RP2350 now runs the entire emulator, renders frames directly into the TMDS encoder, and streams stereo audio over HDMI.

![gameplay preview](./images/gameplay.gif?raw=true)

## Key Features

- **Peanut-GB CPU/PPU** with MiniGB APU for accurate DMG simulation at 60 Hz.
- **Direct HDMI output** at 640×480 or 800×600
  - set `RESOLUTION_800x600` in `software/apps/dmg_emu/CMakeLists.txt`
  - 640x480 is stretched slightly horizontally, but is a valid HDMI resolution
  - 800x600 is pixel accurate, but may only work on monitors or capture cards
- **SD Card ROMs** - load/stream ROMs directly from SD Card
- **Embedded ROM (optional)** powered by `software/scripts/gb_rom_to_header.py` (and the `convert_rom.bat` helper) so ROMs live in flash; no SD card or USB streaming required.
- **HDMI audio** sourced from the emulated APU using libdvi’s audio ring, synced to 32 768 Hz with per-frame pacing.
- **Palette selection** with `SELECT + LEFT/RIGHT` and optional color schemes (`colors.c`).
- **NES Classic controller input** over I²C with non-blocking startup/retry handling.

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

On Windows you can substitute `-G "MinGW Makefiles"` or use the VS Code CMake extension. The resulting UF2 lives under `software/apps/dmg_emu/` inside the build tree (follow the CMake output for the exact path).

## Flashing

1. Hold `BOOTSEL` on the Pico while plugging it in.
2. Copy the generated `dmg_emu.uf2` onto the exposed `RPI-RP2` drive.

## Controls / Shortcuts

- **NES Classic controller** maps to the standard Game Boy buttons.
- **SELECT + LEFT/RIGHT** cycles palette schemes.
- **SELECT + START** exit game (restart Pico)
- **SELECT + HOME** exit game (restart Pico)
- **SELECT + DOWN** save settings

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
