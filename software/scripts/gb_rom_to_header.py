#!/usr/bin/env python3
"""Convert a Game Boy ROM binary into a C header living in flash."""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

BYTES_PER_LINE = 16
SCRIPT_PATH = pathlib.Path(__file__).resolve()
SOFTWARE_DIR = SCRIPT_PATH.parent.parent
DEFAULT_HEADER_DIR = SOFTWARE_DIR / "apps" / "dmg_emu" / "roms"


def sanitize_symbol(name: str) -> str:
    """Return a valid lowercase C identifier derived from the ROM stem."""
    normalized = name.lower().replace(" ", "_")
    cleaned = re.sub(r"[^0-9a-z_]", "_", normalized)
    cleaned = re.sub(r"_+", "_", cleaned).strip("_")
    if not cleaned:
        cleaned = "rom"
    if cleaned[0].isdigit():
        cleaned = f"_{cleaned}"
    return cleaned


def derive_paths(rom_path: pathlib.Path) -> tuple[pathlib.Path, str, str]:
    base_name = sanitize_symbol(rom_path.stem)
    symbol = f"{base_name}_rom"
    section = f"rom_data_{base_name}"
    header_dir = DEFAULT_HEADER_DIR
    header_dir.mkdir(parents=True, exist_ok=True)
    header_path = header_dir / f"{symbol}.h"
    return header_path, symbol, section


def emit_header(data: bytes, symbol: str, section: str, source_name: str) -> str:
    hex_rows = []
    for index in range(0, len(data), BYTES_PER_LINE):
        chunk = data[index : index + BYTES_PER_LINE]
        line = ", ".join(f"0x{byte:02x}" for byte in chunk)
        hex_rows.append(f"    {line},")

    lines = [
        f"// Auto-generated from {source_name}; size {len(data)} bytes",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"static const unsigned int {symbol}_len = {len(data)}u;",
        f"static const unsigned char __in_flash(\"{section}\") {symbol}[] = {{",
    ]

    lines.extend(hex_rows)
    lines.append(";")
    lines.append("")
    lines.append(f"#define ACTIVE_ROM_DATA {symbol}")
    lines.append(f"#define ACTIVE_ROM_LEN  {symbol}_len")
    lines.append("")

    return "\n".join(lines)


def convert_rom(rom_path: pathlib.Path) -> pathlib.Path:
    header_path, symbol, section = derive_paths(rom_path)
    rom_bytes = rom_path.read_bytes()
    header = emit_header(rom_bytes, symbol, section, rom_path.name)
    header_path.write_text(header, encoding="utf-8")
    return header_path



def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=pathlib.Path, help="Path to the .gb or .gbc file")
    args = parser.parse_args(argv)

    header_path = convert_rom(args.rom)
    print(f"Wrote {header_path} ({args.rom.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
