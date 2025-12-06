# PicoDVI-DMG_EMU

Modernized Game Boy emulation on the RP2040 with full HDMI video (via libdvi) and MiniGB APU audio. The original PicoDVI DMG capture pipeline has been replaced with an embedded Peanut-GB core, so the RP2040 now runs the entire emulator, renders frames directly into the TMDS encoder, and streams stereo audio over HDMI.

![gameplay preview](./images/gameplay.gif?raw=true)

## Key Features

- **Peanut-GB CPU/PPU** with MiniGB APU for accurate DMG simulation at 60 Hz.
- **Direct HDMI output** at 640×480 or 800×600 (set `RESOLUTION_800x600` in `software/apps/dmg_emu/CMakeLists.txt`).
- **Embedded ROM workflow** powered by `software/scripts/gb_rom_to_header.py` (and the `convert_rom.bat` helper) so ROMs live in flash; no SD card or USB streaming required.
- **HDMI audio** sourced from the emulated APU using libdvi’s audio ring, synced to 32 768 Hz with per-frame pacing.
- **Palette selection** with `SELECT + LEFT/RIGHT` and optional color schemes (`colors.c`).
- **NES Classic controller input** over I²C with non-blocking startup/retry handling.

## Project Layout

```
software/
	apps/dmg_emu/         # Emulator sources (main loop, palettes, Peanut-GB glue)
	scripts/              # ROM conversion utilities
	libdvi/, libsprite/   # DVI/graphics support libraries
ROMS/                   # Drop raw .gb/.gbc files here
images/                 # Reference photos / GIFs
```

## Converting ROMs

1. Copy your `.gb`/`.gbc` into the `ROMS/` folder.
2. Run the helper script (Python 3.11 path can be adjusted if needed):

	 ```powershell
	 software\scripts\convert_rom.bat ROMS\tetris.gb
	 ```

	 or

	 ```powershell
	 C:\Python311\python.exe software\scripts\gb_rom_to_header.py ROMS\tetris.gb
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

```bash
mkdir build
cd build
cmake -DPICO_SDK_PATH=/path/to/pico-sdk -DPICO_COPY_TO_RAM=1 ..
cmake --build . -j$(nproc)
```

On Windows you can substitute `-G "MinGW Makefiles"` or use the VS Code CMake extension. The resulting UF2 lives under `software/apps/dmg_emu/` inside the build tree (follow the CMake output for the exact path).

## Flashing

1. Hold `BOOTSEL` on the Pico while plugging it in.
2. Copy the generated `dmg_emu.uf2` onto the exposed `RPI-RP2` drive.

## Controls / Shortcuts

- **NES Classic controller** maps to the standard Game Boy buttons.
- **SELECT + LEFT/RIGHT** cycles palette schemes.
- (Optional) OSD hooks are still in the codebase but disabled; set `ENABLE_OSD` once the feature lands.

## Credits

- Peanut-GB by **deltabeard** (https://github.com/deltabeard/Peanut-GB)
- MiniGB APU by **Alex Baines** / **fty94**
- libdvi / PicoDVI by **Wren6991**, plus prior PicoDVI-N64 inspiration from **kbeckmann**
- Original DMG capture hardware concept by **Andy West** (element14)

## License

See `LICENSE` for details. The ROM conversion script and emulator glue are MIT; Peanut-GB and MiniGB APU retain their respective upstream licenses (bundled in the tree).
