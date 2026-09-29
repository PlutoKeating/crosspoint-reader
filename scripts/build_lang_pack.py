#!/usr/bin/env python3
"""
Build SD-card language packs (.lang) from lib/I18n/translations/*.yaml.

The firmware only compiles the built-in catalogue (see gen_i18n.py). Copy a
generated `<CODE>.lang` to `/.crosspoint/lang/` on the SD card and pick it in
Settings > System > Language. Entries are keyed by the FNV-1a hash of the key
name, so a pack stays valid across firmware updates; keys the firmware does
not know are ignored and keys missing from the pack use the built-in text.

Binary layout (little-endian):
    char[4]  magic "SLNG"
    u16      format version (1)
    u16      entry count N
    char[8]  language code, NUL padded (e.g. "EN")
    char[32] native language name, UTF-8, NUL padded
    N x { u32 key hash, u32 string offset }   sorted by key hash
    UTF-8 strings, each NUL terminated (offsets relative to this area)

Only keys present in the built-in catalogue are emitted, which keeps packs
small enough to hold in RAM on the ESP32-C3 (limit: 32 KiB per pack).

Usage:
    python scripts/build_lang_pack.py [--out dist/lang] [CODE ...]
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_i18n  # noqa: E402

MAX_PACK_BYTES = 32 * 1024


def build_pack(catalogue: dict, builtin_keys: list, fallback: dict | None = None) -> bytes:
    code = catalogue["_language_code"]
    name = catalogue["_language_name"]
    entries = []
    strings = bytearray()
    for key in builtin_keys:
        value = catalogue.get(key)
        if value is None and fallback is not None:
            value = fallback.get(key)
        if value is None:
            continue
        entries.append((gen_i18n.fnv1a32(key), len(strings)))
        strings += value.encode("utf-8") + b"\0"
    entries.sort()
    header = gen_i18n.PACK_MAGIC + struct.pack("<HH", gen_i18n.PACK_VERSION, len(entries))
    header += code.encode("ascii").ljust(gen_i18n.PACK_CODE_BYTES, b"\0")
    header += name.encode("utf-8").ljust(gen_i18n.PACK_NAME_BYTES, b"\0")
    table = b"".join(struct.pack("<II", key_hash, offset) for key_hash, offset in entries)
    data = header + table + bytes(strings)
    if len(data) > MAX_PACK_BYTES:
        raise ValueError(f"{code}: pack is {len(data)} bytes, limit {MAX_PACK_BYTES}")
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description="Build StockStick SD-card language packs")
    parser.add_argument("codes", nargs="*", help="language codes to build (default: all non-built-in)")
    parser.add_argument("--translations", default="lib/I18n/translations")
    parser.add_argument("--out", default="dist/lang")
    parser.add_argument(
        "--fallback",
        default="EN",
        help="pack whose text fills keys a translation lacks (default EN; 'none' to use the built-in text)",
    )
    args = parser.parse_args()

    builtin, others = gen_i18n.load_catalogues(Path(args.translations))
    builtin_keys = gen_i18n.string_keys(builtin)
    fallback = None
    if args.fallback.lower() != "none":
        fallback = next((c for _p, c in others if c["_language_code"] == args.fallback.upper()), None)
        if fallback is None:
            print(f"fallback language {args.fallback} not found", file=sys.stderr)
            return 1
    wanted = {code.upper() for code in args.codes}
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    built = 0
    for path, catalogue in others:
        code = catalogue["_language_code"]
        if wanted and code not in wanted:
            continue
        pack_fallback = None if catalogue is fallback else fallback
        data = build_pack(catalogue, builtin_keys, pack_fallback)
        missing = [key for key in builtin_keys if key not in catalogue]
        (out / f"{code}.lang").write_bytes(data)
        source = fallback["_language_code"] if pack_fallback else builtin["_language_code"]
        note = f", {len(missing)} keys from {source}" if missing else ""
        print(f"{code}.lang  {len(data):6d} bytes  ({path.name}{note})")
        built += 1
    if wanted and built != len(wanted):
        print("some requested language codes were not found", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
