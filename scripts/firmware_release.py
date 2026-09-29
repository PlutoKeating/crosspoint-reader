#!/usr/bin/env python3
"""
Package a StockStick firmware release for OTA.

Checks the identity in platformio.ini ([crosspoint] version / build /
min_install_build), optionally builds `gh_release`, cross-checks the image's
embedded StockStick descriptor, and writes:

    dist/firmware/<version>/
      stockstick-<version>.bin     OTA / SD-card image
      stockstick-<version>.elf     symbols for crash decoding (not published)
      manifest.json                identity, size, SHA-256, commit, notes
      lang/<CODE>.lang             SD-card language packs for this build
      catalogue.json               body for the admin firmware catalogue

Rules enforced (see docs/firmware-ota.md):
- version is semver, <= 31 bytes, accepted by the server catalogue regex;
- build strictly increases across releases in the output directory and,
  for final releases, equals major*10000 + minor*100 + patch;
- min_install_build <= build;
- the image descriptor matches platformio.ini and the git commit;
- the image fits the smallest OTA app partition in partitions.csv.

Usage:
    python3 scripts/firmware_release.py --build --notes RELEASE_NOTES.md \\
        --url-base https://<storage>/stockstick/firmware/
"""

from __future__ import annotations

import argparse
import configparser
import csv
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENV = "gh_release"
SERVER_VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(?:[-.][a-zA-Z0-9.-]+)?$")
SEMVER_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$")

# Must match lib/ProjectStick/StickFirmware.h.
DESCRIPTOR_OFFSET = 24 + 8 + 256
DESCRIPTOR_MAGIC = 0x57465353
DESCRIPTOR_FORMAT = "<IHHII16s32s16s16s"
BOARD_NAMES = {1: "x3", 2: "x4"}


def fail(message: str) -> None:
    print(f"release error: {message}", file=sys.stderr)
    sys.exit(1)


def read_identity() -> dict:
    config = configparser.ConfigParser()
    config.read(ROOT / "platformio.ini")
    section = config["crosspoint"]
    identity = {
        "version": section["version"].strip(),
        "build": int(section["build"]),
        "min_install_build": int(section["min_install_build"]),
    }
    version = identity["version"]
    match = SEMVER_RE.match(version)
    if not match or not SERVER_VERSION_RE.match(version) or len(version.encode()) > 31:
        fail(f"version {version!r} must be semver, accepted by the catalogue and at most 31 bytes")
    if not match.group(4):
        expected = int(match.group(1)) * 10000 + int(match.group(2)) * 100 + int(match.group(3))
        if identity["build"] != expected:
            fail(f"build {identity['build']} must be {expected} for release {version}")
    if identity["min_install_build"] > identity["build"]:
        fail("min_install_build cannot exceed build")
    return identity


def app_partition_limit() -> int:
    limits = []
    for row in csv.reader((ROOT / "partitions.csv").read_text().splitlines()):
        if len(row) >= 5 and not row[0].strip().startswith("#") and row[1].strip() == "app":
            limits.append(int(row[4].strip(), 0))
    if not limits:
        fail("no app partition in partitions.csv")
    return min(limits)


def parse_descriptor(image: bytes) -> dict:
    if len(image) < DESCRIPTOR_OFFSET + struct.calcsize(DESCRIPTOR_FORMAT) or image[0] != 0xE9:
        fail("image is not an ESP application")
    fields = struct.unpack_from(DESCRIPTOR_FORMAT, image, DESCRIPTOR_OFFSET)
    magic, desc_version, desc_size, build, boards, product, version, commit, _ = fields
    if magic != DESCRIPTOR_MAGIC or desc_version != 1 or desc_size != 96:
        fail("image has no StockStick descriptor (is it a StockStick build?)")

    def text(raw: bytes) -> str:
        return raw.split(b"\0", 1)[0].decode("ascii")

    return {
        "product": text(product),
        "version": text(version),
        "build": build,
        "boards": [name for bit, name in BOARD_NAMES.items() if boards & bit],
        "commit": text(commit),
        "chip_id": struct.unpack_from("<H", image, 12)[0],
    }


def git(*args: str) -> str:
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def previous_builds(out_root: Path, current_version: str) -> list[tuple[str, int]]:
    builds = []
    for manifest in out_root.glob("*/manifest.json"):
        data = json.loads(manifest.read_text())
        if data.get("version") != current_version:
            builds.append((data.get("version", "?"), int(data.get("build", 0))))
    return builds


def main() -> int:
    parser = argparse.ArgumentParser(description="Package a StockStick OTA firmware release")
    parser.add_argument("--build", action="store_true", help=f"run `pio run -e {ENV}` first")
    parser.add_argument("--notes", type=Path, help="release notes (UTF-8, <= 4000 chars) shown to owners")
    parser.add_argument("--url-base", default="", help="public HTTPS directory the .bin will be uploaded to")
    parser.add_argument("--out", type=Path, default=ROOT / "dist" / "firmware")
    parser.add_argument("--allow-dirty", action="store_true", help="package an uncommitted tree (testing only)")
    args = parser.parse_args()

    identity = read_identity()
    if git("status", "--porcelain", "--untracked-files=no") and not args.allow_dirty:
        fail("working tree has uncommitted changes; commit them or pass --allow-dirty")
    if args.build:
        subprocess.run(["pio", "run", "-e", ENV], cwd=ROOT, check=True)

    build_dir = ROOT / ".pio" / "build" / ENV
    image = (build_dir / "firmware.bin").read_bytes()
    descriptor = parse_descriptor(image)
    head = git("rev-parse", "--short", "HEAD")
    if descriptor["product"] != "stockstick":
        fail(f"descriptor product is {descriptor['product']!r}")
    if descriptor["version"] != identity["version"] or descriptor["build"] != identity["build"]:
        fail(
            f"image is {descriptor['version']} ({descriptor['build']}) but platformio.ini says "
            f"{identity['version']} ({identity['build']}); rebuild with --build"
        )
    if not head.startswith(descriptor["commit"]) and not descriptor["commit"].startswith(head):
        fail(f"image was built from {descriptor['commit']}, HEAD is {head}; rebuild with --build")
    limit = app_partition_limit()
    if len(image) > limit:
        fail(f"image is {len(image)} bytes, OTA partition holds {limit}")

    for version, build in previous_builds(args.out, identity["version"]):
        if build >= identity["build"]:
            fail(f"build {identity['build']} does not exceed {build} of release {version}")

    notes = args.notes.read_text(encoding="utf-8").strip() if args.notes else ""
    if len(notes) > 4000:
        fail("release notes exceed 4000 characters (catalogue limit)")

    version = identity["version"]
    out = args.out / version
    if out.exists():
        shutil.rmtree(out)
    (out / "lang").mkdir(parents=True)
    binary_name = f"stockstick-{version}.bin"
    (out / binary_name).write_bytes(image)
    shutil.copyfile(build_dir / "firmware.elf", out / f"stockstick-{version}.elf")
    subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "build_lang_pack.py"), "--out", str(out / "lang")],
        cwd=ROOT,
        check=True,
        stdout=subprocess.DEVNULL,
    )

    sha256 = hashlib.sha256(image).hexdigest()
    manifest = {
        "product": "stockstick",
        "version": version,
        "build": identity["build"],
        "min_install_build": identity["min_install_build"],
        "boards": descriptor["boards"],
        "chip_id": descriptor["chip_id"],
        "file": binary_name,
        "bytes": len(image),
        "sha256": sha256,
        "partition_bytes": limit,
        "commit": git("rev-parse", "HEAD"),
        "built_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "notes": notes,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    catalogue = {
        "kind": "firmware",
        "version": version,
        "url": (args.url_base.rstrip("/") + "/" + binary_name) if args.url_base else "",
        "sha256": sha256,
        "bytes": len(image),
        "notes": notes,
    }
    (out / "catalogue.json").write_text(json.dumps(catalogue, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print(f"StockStick {version} (build {identity['build']}) -> {out}")
    print(f"  {binary_name}: {len(image)} / {limit} bytes, sha256 {sha256}")
    print("  Upload the .bin to HTTPS storage, then register catalogue.json in /console/studio.")
    if not catalogue["url"]:
        print("  (catalogue.json has no url: pass --url-base once the storage location is known)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
