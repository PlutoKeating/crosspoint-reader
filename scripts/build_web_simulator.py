#!/usr/bin/env python3
"""Build the browser (WebAssembly) X3 simulator from this firmware source tree.

The browser simulator is the same native simulator build as `env:simulator`
(crosspoint-simulator HAL shims) compiled with Emscripten instead of the host
compiler. PlatformIO resolves the exact source set, include paths and defines
(`compiledb`); this script re-targets those commands to em++ and links
`stockstick-<version>-web.js` + `.wasm`.

Usage:
  python3 scripts/build_web_simulator.py [--simulator ../crosspoint-simulator]
      [--out dist/web] [--jobs N] [--emsdk ~/emsdk]

The output is published next to the OTA image by scripts/firmware_release.py.
The web page only loads these two files; it contains no firmware logic.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import configparser
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Host-only flags from the native simulator build that Emscripten must not see.
DROP_FLAGS = {"-lssl", "-lcrypto", "-lSDL2", "-D_REENTRANT", "-pthread"}


def firmware_identity() -> tuple[str, str, str]:
    parser = configparser.ConfigParser(inline_comment_prefixes=(";",), interpolation=None)
    parser.read(ROOT / "platformio.ini")
    section = parser["crosspoint"]
    return (section["version"].strip(), section["build"].strip(),
            section["min_install_build"].strip())


def project_config(simulator: Path, version: str, build: str, min_build: str) -> str:
    # Mirrors env:simulator (see platformio.local.ini sample in the docs) with
    # the X3 profile. SDL comes from the simulator's web shim, so no
    # sdl2-config call.
    return f"""
[platformio]
src_dir = src
lib_dir = lib

[env:web]
platform = native
lib_ldf_mode = deep+
build_src_filter =
  +<*>
  -<network/FirmwareFlasher.cpp>
  -<network/OtaBootSwitch.cpp>
  -<network/OtaTrial.cpp>
  -<platform/skip_efuse_blk_check.c>
build_flags =
  -std=gnu++2a
  -Wno-deprecated-declarations
  -Wno-narrowing
  -DSIMULATOR
  -DSIMULATOR_DEVICE_X3
  -DSIMULATOR_WEB
  -DCROSSPOINT_VERSION=\\"{version}\\"
  -DSTOCKSTICK_FW_BUILD={build}
  -DSTOCKSTICK_FW_MIN_INSTALL_BUILD={min_build}
  -DPROJECT_STICK_BASE_URL=\\"\\"
  -DENABLE_SERIAL_LOG
  -DLOG_LEVEL=2
  -DEINK_DISPLAY_SINGLE_BUFFER_MODE=1
  -DUSE_UTF8_LONG_NAMES=1
  -DDISABLE_FS_H_WARNING=1
  -DDESTRUCTOR_CLOSES_FILE=1
  -Isrc
  -I{simulator}/src
  -Ifreeink-sdk/libs/assets/Icons/include
lib_ignore = hal
extra_scripts =
  pre:scripts/gen_i18n.py
  pre:scripts/git_branch.py
lib_deps =
  simulator=symlink://{simulator}
  FreeInkUI=symlink://freeink-sdk/libs/ui/FreeInkUI
  bblanchon/ArduinoJson @ 7.4.2
  ricmoo/QRCode @ ^0.0.1
"""


def emscripten_env(emsdk: Path) -> dict[str, str]:
    env = dict(os.environ)
    if shutil.which("em++", path=env.get("PATH")):
        return env
    script = emsdk / "emsdk_env.sh"
    if not script.exists():
        sys.exit(f"em++ not found and {script} missing; install emsdk first")
    out = subprocess.run(
        ["bash", "-c", f"source {shlex.quote(str(script))} >/dev/null 2>&1 && env -0"],
        check=True, capture_output=True)
    for item in out.stdout.split(b"\0"):
        if b"=" in item:
            key, value = item.split(b"=", 1)
            env[key.decode()] = value.decode()
    return env


def compile_commands(simulator: Path, version: str, build: str, min_build: str) -> list[dict]:
    with tempfile.TemporaryDirectory(prefix="stockstick-web-conf-") as tmp:
        conf = Path(tmp) / "platformio.ini"
        conf.write_text(project_config(simulator, version, build, min_build))
        subprocess.run(["pio", "run", "--project-dir", str(ROOT), "--project-conf", str(conf),
                        "-e", "web", "-t", "compiledb"], check=True, cwd=ROOT)
    return json.loads((ROOT / "compile_commands.json").read_text())


def retarget(entry: dict, simulator: Path, objdir: Path) -> tuple[list[str], Path]:
    args = entry.get("arguments") or shlex.split(entry["command"])
    source = entry["file"]
    is_c = source.endswith(".c")
    out = objdir / (hashlib.sha1(source.encode()).hexdigest()[:16] + ".o")
    result = ["emcc" if is_c else "em++"]
    skip = False
    for arg in args[1:]:
        if skip:
            skip = False
            continue
        if arg == "-o":
            skip = True
            continue
        if arg == "-c" or arg == source or arg in DROP_FLAGS or arg.startswith("-I/usr/"):
            continue
        if is_c and arg.startswith("-std=gnu++"):
            continue
        result.append(arg)
    result[1:1] = [f"-I{simulator}/src/web"]
    result += ["-pthread", "-O2", "-fexceptions", "-c", source, "-o", str(out)]
    return result, out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--simulator", default=str(ROOT.parent / "crosspoint-simulator"))
    parser.add_argument("--out", default=str(ROOT / "dist" / "web"))
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--emsdk", default=str(Path.home() / "emsdk"))
    args = parser.parse_args()

    simulator = Path(args.simulator).resolve()
    version, build, min_build = firmware_identity()
    env = emscripten_env(Path(args.emsdk).expanduser())
    entries = compile_commands(simulator, version, build, min_build)

    objdir = ROOT / ".pio" / "build" / "web-emscripten"
    objdir.mkdir(parents=True, exist_ok=True)
    jobs = [retarget(e, simulator, objdir) for e in entries]

    def run(job: tuple[list[str], Path]) -> Path:
        command, out = job
        proc = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True)
        if proc.returncode:
            raise RuntimeError(f"{' '.join(command)}\n{proc.stderr}")
        return out

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        objects = list(pool.map(run, jobs))

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = f"stockstick-{version}-web"
    link = [
        "em++", *map(str, objects), "-o", str(out_dir / f"{stem}.js"),
        "-O2", "-pthread", "-fexceptions",
        "-sPROXY_TO_PTHREAD=1", "-sPTHREAD_POOL_SIZE=8",
        "-sINITIAL_MEMORY=268435456", "-sALLOW_MEMORY_GROWTH=0", "-sSTACK_SIZE=1048576",
        "-sDEFAULT_PTHREAD_STACK_SIZE=1048576",
        "-sFETCH=1", "-lidbfs.js", "-sFORCE_FILESYSTEM=1",
        "-sMODULARIZE=1", "-sEXPORT_NAME=createStockStickSimulator",
        "-sENVIRONMENT=web,worker",
        "-sEXPORTED_RUNTIME_METHODS=['FS','IDBFS','ENV','HEAPU8','addRunDependency','removeRunDependency']",
        "-sEXPORTED_FUNCTIONS=['_main','_sim_web_key','_sim_web_mouse','_sim_web_quit',"
        "'_sim_web_frame_seq','_sim_web_frame_ptr','_sim_web_frame_width','_sim_web_frame_height']",
        "-sEXIT_RUNTIME=0",
        # Module options the loader page passes in (the page supplies the
        # pre-downloaded wasm so it can show progress).
        "-sINCOMING_MODULE_JS_API=['wasmBinary','locateFile','print','printErr','preRun',"
        "'mainScriptUrlOrBlob','onAbort']",
    ]
    proc = subprocess.run(link, cwd=ROOT, env=env, capture_output=True, text=True)
    if proc.returncode:
        sys.exit(proc.stderr)
    info = {"version": version, "build": int(build), "js": f"{stem}.js", "wasm": f"{stem}.wasm"}
    (out_dir / f"{stem}.json").write_text(json.dumps(info, indent=2) + "\n")
    for name in (f"{stem}.js", f"{stem}.wasm"):
        print(f"{name}: {(out_dir / name).stat().st_size} bytes")


if __name__ == "__main__":
    main()
