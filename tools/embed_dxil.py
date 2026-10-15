#!/usr/bin/env python3
"""Convert a DXC library binary to the small C++ header used by the DXR PSO."""

from __future__ import annotations

import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: embed_dxil.py INPUT.dxil OUTPUT.h", file=sys.stderr)
        return 2

    source = Path(sys.argv[1])
    target = Path(sys.argv[2])
    data = source.read_bytes()
    if not data:
        print(f"error: {source} is empty", file=sys.stderr)
        return 1

    rows = []
    for offset in range(0, len(data), 12):
        row = data[offset : offset + 12]
        rows.append("    " + ", ".join(f"0x{byte:02x}" for byte in row) + ",")

    target.parent.mkdir(parents=True, exist_ok=True)
    symbol = target.stem.removesuffix("_Bytecode")
    target.write_text(
        "#pragma once\n"
        "\n"
        f"// Generated from {source.name} by DXC. Do not edit.\n"
        f"static const unsigned char g_{symbol}_Bytecode[] = {{\n"
        + "\n".join(rows)
        + "\n};\n",
        encoding="ascii",
    )
    print(f"Embedded {len(data):,} DXIL bytes into {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
