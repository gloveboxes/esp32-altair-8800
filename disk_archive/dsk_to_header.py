#!/usr/bin/env python3
"""Convert a CP/M disk image into a C header with a byte array."""

from __future__ import annotations

import argparse
from pathlib import Path
from textwrap import dedent


def format_bytes(data: bytes, width: int = 16, indent: str = "") -> str:
    if not data:
        return ""
    lines = []
    for idx in range(0, len(data), width):
        chunk = data[idx : idx + width]
        line = ", ".join(f"0x{byte:02x}" for byte in chunk)
        lines.append(f"{indent}{line},")
    lines[-1] = lines[-1].rstrip(",")
    return "\n".join(lines)


def infer_symbol(path: Path) -> str:
    stem = path.stem.replace("-", "_").replace(" ", "_")
    return stem.lower()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Convert a binary CP/M disk image into a C header containing a byte array."
    )
    parser.add_argument("input", nargs="?", type=Path, help="Path to the .dsk image")
    parser.add_argument("output", nargs="?", type=Path, help="Destination header file")
    parser.add_argument(
        "-i",
        "--input",
        dest="input_opt",
        type=Path,
        help="Path to the .dsk image (alternative to positional argument)",
    )
    parser.add_argument(
        "-o",
        "--output",
        dest="output_opt",
        type=Path,
        help="Destination header file (alternative to positional argument)",
    )
    parser.add_argument(
        "--symbol",
        type=str,
        default=None,
        help="Base symbol name for the generated array (defaults to the input stem)",
    )
    parser.add_argument(
        "--width",
        type=int,
        default=12,
        help="Number of bytes per line in the generated array (default: 16)",
    )

    args = parser.parse_args()

    input_path = args.input_opt or args.input
    output_path = args.output_opt or args.output

    if input_path is None or output_path is None:
        parser.error("input and output paths are required (either positional or via -i/-o)")

    data = input_path.read_bytes()
    symbol = args.symbol or infer_symbol(input_path)
    formatted = format_bytes(data, width=args.width)

    header = (
        f"const unsigned char {symbol}[] = {{\n"
        f"{formatted}\n"
        f"}};\n"
        f"const unsigned int {symbol}_len = {len(data)};\n"
    )

    output_path.write_text(header, encoding="ascii")


if __name__ == "__main__":
    main()
