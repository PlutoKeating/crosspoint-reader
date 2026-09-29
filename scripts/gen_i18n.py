#!/usr/bin/env python3
"""
Generate the built-in I18n catalogue from lib/I18n/translations.

StockStick compiles exactly one language into the firmware: the catalogue
whose `_order` is "0" (currently chinese.yaml). Every other YAML file in the
directory is a source for an SD-card language pack built by
scripts/build_lang_pack.py; the firmware loads those at runtime and falls back
to the built-in text for any key a pack does not provide.

Packs are matched by key *name* (FNV-1a hash of "STR_..."), never by StrId
number, so a pack keeps working when a firmware update adds or removes keys.

Outputs (all gitignored, regenerated on every build):
- I18nKeys.h:     StrId enum, built-in language identity, pack format constants
- I18nStrings.h:  declarations of the built-in blob, offsets and key hashes
- I18nStrings.cpp: definitions

YAML format (flat, no PyYAML dependency):
    _language_name: "Native name"
    _language_code: "ZH"
    _order: "0"
    # comment
    STR_KEY: "value"      escapes: \\\\  \\"  \\n

Usage:
    python scripts/gen_i18n.py [translations_dir output_dir] [--src-dirs src lib]
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path
from typing import Dict, List, Set, Tuple

BUILTIN_ORDER = "0"
# Keep in sync with lib/I18n/I18n.cpp and scripts/build_lang_pack.py.
PACK_MAGIC = b"SLNG"
PACK_VERSION = 1
PACK_CODE_BYTES = 8
PACK_NAME_BYTES = 32

_LINE = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)\s*:\s*"(.*)"$')


def fnv1a32(text: str) -> int:
    value = 0x811C9DC5
    for byte in text.encode("utf-8"):
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def _unescape(raw: str, where: str) -> str:
    out: List[str] = []
    i = 0
    while i < len(raw):
        if raw[i] == "\\" and i + 1 < len(raw):
            nxt = raw[i + 1]
            if nxt == "\\":
                out.append("\\")
            elif nxt == '"':
                out.append('"')
            elif nxt == "n":
                out.append("\n")
            else:
                raise ValueError(f"{where}: unknown escape '\\{nxt}'")
            i += 2
        else:
            out.append(raw[i])
            i += 1
    return "".join(out)


def parse_catalogue(path: Path) -> Dict[str, str]:
    result: Dict[str, str] = {}
    with open(path, "r", encoding="utf-8") as handle:
        for number, raw in enumerate(handle, start=1):
            line = raw.rstrip("\r\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            match = _LINE.match(line)
            where = f"{path}:{number}"
            if not match:
                raise ValueError(f'{where}: expected KEY: "value", got {line!r}')
            key = match.group(1)
            if key in result:
                raise ValueError(f"{where}: duplicate key {key}")
            result[key] = _unescape(match.group(2), where)
    for meta in ("_language_name", "_language_code", "_order"):
        if meta not in result:
            raise ValueError(f"{path}: missing {meta}")
    code = result["_language_code"]
    if not re.fullmatch(r"[A-Z][A-Z0-9_]{1,6}", code):
        raise ValueError(f"{path}: _language_code must be 2-7 uppercase ASCII characters")
    if len(result["_language_name"].encode("utf-8")) >= PACK_NAME_BYTES:
        raise ValueError(f"{path}: _language_name must be shorter than {PACK_NAME_BYTES} bytes")
    return result


def load_catalogues(directory: Path) -> Tuple[Dict[str, str], List[Tuple[Path, Dict[str, str]]]]:
    builtin = None
    others: List[Tuple[Path, Dict[str, str]]] = []
    for path in sorted(directory.glob("*.yaml")):
        catalogue = parse_catalogue(path)
        if catalogue["_order"] == BUILTIN_ORDER:
            if builtin is not None:
                raise ValueError(f"more than one built-in catalogue (_order {BUILTIN_ORDER})")
            builtin = catalogue
        else:
            others.append((path, catalogue))
    if builtin is None:
        raise ValueError(f"no built-in catalogue with _order {BUILTIN_ORDER} in {directory}")
    return builtin, others


def string_keys(catalogue: Dict[str, str]) -> List[str]:
    return [key for key in catalogue if key.startswith("STR_")]


def referenced_keys(src_dirs: List[str]) -> Set[str]:
    pattern = re.compile(r"\bSTR_[A-Z0-9_]+\b")
    found: Set[str] = set()
    for src in src_dirs:
        for root, _dirs, files in os.walk(src):
            if os.path.normpath(root).startswith(os.path.normpath("lib/I18n")):
                continue
            for name in files:
                if name.endswith((".c", ".cpp", ".h", ".hpp")):
                    with open(os.path.join(root, name), encoding="utf-8", errors="replace") as handle:
                        found.update(pattern.findall(handle.read()))
    return found


def check_hashes(keys: List[str]) -> None:
    seen: Dict[int, str] = {}
    for key in keys:
        value = fnv1a32(key)
        if value in seen:
            raise ValueError(f"key hash collision: {key} and {seen[value]}")
        seen[value] = key


def _c_string(value: str) -> str:
    # Escape as bytes; split after hex escapes so following hex digits are not absorbed.
    parts: List[str] = []
    for byte in value.encode("utf-8"):
        char = chr(byte)
        if char == "\\":
            parts.append("\\\\")
        elif char == '"':
            parts.append('\\"')
        elif char == "\n":
            parts.append("\\n")
        elif 0x20 <= byte < 0x7F:
            parts.append(char)
        else:
            parts.append(f'\\x{byte:02X}""')
    return "".join(parts)


def generate(builtin: Dict[str, str], output_dir: Path) -> None:
    keys = string_keys(builtin)
    check_hashes(keys)
    code = builtin["_language_code"]
    name = builtin["_language_name"]

    offsets: List[int] = []
    blob = bytearray()
    for key in keys:
        offsets.append(len(blob))
        blob += builtin[key].encode("utf-8") + b"\0"
    if len(blob) > 0xFFFF:
        raise ValueError("built-in catalogue exceeds 64 KiB")

    header = [
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "// THIS FILE IS AUTO-GENERATED BY scripts/gen_i18n.py. DO NOT EDIT.",
        "// clang-format off",
        "",
        "enum class StrId : uint16_t {",
    ]
    header += [f"  {key}," for key in keys]
    header += [
        "  // Sentinel - must be last",
        "  _COUNT",
        "};",
        "",
        "namespace i18n_catalogue {",
        f'constexpr const char* BUILTIN_CODE = "{code}";',
        f'constexpr const char* BUILTIN_NAME = "{_c_string(name)}";',
        f"constexpr size_t KEY_COUNT = {len(keys)};",
        f'constexpr char PACK_MAGIC[4] = {{{", ".join(repr(chr(b)) for b in PACK_MAGIC)}}};',
        f"constexpr uint16_t PACK_VERSION = {PACK_VERSION};",
        f"constexpr size_t PACK_CODE_BYTES = {PACK_CODE_BYTES};",
        f"constexpr size_t PACK_NAME_BYTES = {PACK_NAME_BYTES};",
        "}  // namespace i18n_catalogue",
        "",
    ]
    (output_dir / "I18nKeys.h").write_text("\n".join(header), encoding="utf-8")

    strings_h = [
        "#pragma once",
        '#include "I18nKeys.h"',
        "",
        "// THIS FILE IS AUTO-GENERATED BY scripts/gen_i18n.py. DO NOT EDIT.",
        "",
        "namespace i18n_catalogue {",
        "extern const char BUILTIN_DATA[];",
        "extern const uint16_t BUILTIN_OFFSETS[];",
        "// FNV-1a 32 of each key name, indexed by StrId.",
        "extern const uint32_t KEY_HASHES[];",
        "}  // namespace i18n_catalogue",
        "",
    ]
    (output_dir / "I18nStrings.h").write_text("\n".join(strings_h), encoding="utf-8")

    cpp = [
        '#include "I18nStrings.h"',
        "",
        "// THIS FILE IS AUTO-GENERATED BY scripts/gen_i18n.py. DO NOT EDIT.",
        "// clang-format off",
        "",
        "namespace i18n_catalogue {",
        "",
        "const char BUILTIN_DATA[] =",
    ]
    cpp += [f'  "{_c_string(builtin[key])}\\0"  // {key}' for key in keys]
    cpp += [";", "", "const uint16_t BUILTIN_OFFSETS[] = {"]
    cpp += [f"  {offset}," for offset in offsets]
    cpp += ["};", "", "const uint32_t KEY_HASHES[] = {"]
    cpp += [f"  0x{fnv1a32(key):08X}u,  // {key}" for key in keys]
    cpp += [
        "};",
        "",
        "static_assert(sizeof(BUILTIN_OFFSETS) / sizeof(BUILTIN_OFFSETS[0]) == KEY_COUNT, \"offset table size\");",
        "static_assert(sizeof(KEY_HASHES) / sizeof(KEY_HASHES[0]) == KEY_COUNT, \"hash table size\");",
        "",
        "}  // namespace i18n_catalogue",
        "",
    ]
    (output_dir / "I18nStrings.cpp").write_text("\n".join(cpp), encoding="utf-8")
    print(f"I18n: built-in {code} catalogue with {len(keys)} keys ({len(blob)} bytes)")


def main(translations_dir: str, output_dir: str, src_dirs: List[str]) -> None:
    builtin, others = load_catalogues(Path(translations_dir))
    keys = set(string_keys(builtin))
    used = referenced_keys(src_dirs)
    missing = sorted(used - keys)
    if missing:
        raise ValueError(
            "keys referenced in source but missing from the built-in catalogue: " + ", ".join(missing)
        )
    unused = sorted(keys - used)
    if unused:
        print(f"I18n: warning: {len(unused)} built-in keys are never referenced: {', '.join(unused)}")
    for path, catalogue in others:
        extra = sorted(set(string_keys(catalogue)) - keys)
        if extra:
            print(f"I18n: note: {path.name} has {len(extra)} keys the firmware no longer uses (ignored in packs)")
    generate(builtin, Path(output_dir))


def _cli() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("translations_dir", nargs="?", default="lib/I18n/translations")
    parser.add_argument("output_dir", nargs="?", default="lib/I18n")
    parser.add_argument("--src-dirs", nargs="+", default=["src", "lib"])
    args = parser.parse_args()
    try:
        main(args.translations_dir, args.output_dir, args.src_dirs)
    except ValueError as error:
        print(f"I18n error: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    _cli()
else:
    try:
        Import("env")  # noqa: F821  # type: ignore[name-defined]
        _project = env["PROJECT_DIR"]  # noqa: F821  # type: ignore[name-defined]
        _cwd = os.getcwd()
        os.chdir(_project)
        try:
            main("lib/I18n/translations", "lib/I18n", ["src", "lib"])
        except ValueError as error:
            print(f"I18n error: {error}", file=sys.stderr)
            sys.exit(1)
        finally:
            os.chdir(_cwd)
    except NameError:
        pass
