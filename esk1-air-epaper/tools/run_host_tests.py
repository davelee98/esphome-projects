"""Build and run the host tests for components/opendisplay.

    python tools/run_host_tests.py [filter]

Compiles tests/host/*.cpp together with the ESPHome-free sources of both
components: opendisplay (protocol session, config parser) and aqi_dashboard
(model, renderer, scheduler and the generated assets) using zig's bundled Clang (`zig c++`), fetched with uv
on first use. Then:

  * runs the tests (an optional filter runs only matching test names),
  * writes the renderer's scenario images to output/host-render/*.ppm,
  * checks every glyph row in dashboard_assets.cpp against the Terminus BDF
    sources, byte for byte, with a BDF reader independent of the generator.

Exit status 0 only if everything passes. No Pillow or other packages needed.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "opendisplay"
DASHBOARD = ROOT / "components" / "aqi_dashboard"
TESTS = ROOT / "tests" / "host"
BUILD = TESTS / ".build"
RENDERS = ROOT.parent / "output" / "host-render"

SOURCES = [
    COMPONENT / "od_config.cpp",
    COMPONENT / "od_session.cpp",
    DASHBOARD / "dashboard_model.cpp",
    DASHBOARD / "dashboard_render.cpp",
    DASHBOARD / "dashboard_scheduler.cpp",
    DASHBOARD / "dashboard_assets.cpp",
]


def compiler():
    """zig c++ through uv, or a system clang++ / g++ if present."""
    for name in ("clang++", "g++"):
        path = shutil.which(name)
        if path:
            return [path]
    uv = shutil.which("uvx") or shutil.which("uv")
    if uv is None:
        sys.exit("No C++ compiler: install uv (for zig) or clang++ / g++")
    if Path(uv).stem == "uv":
        return [uv, "tool", "run", "--from", "ziglang", "python", "-m", "ziglang", "c++"]
    return [uv, "--from", "ziglang", "python", "-m", "ziglang", "c++"]


def build():
    BUILD.mkdir(parents=True, exist_ok=True)
    exe = BUILD / ("host_tests.exe" if os.name == "nt" else "host_tests")
    cmd = compiler() + [
        "-std=c++17",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Wno-missing-field-initializers",
        "-fsanitize=undefined",
        "-fno-sanitize-recover=undefined",
        "-o",
        str(exe),
        *map(str, sorted(TESTS.glob("*.cpp"))),
        *map(str, SOURCES),
    ]
    print("Building host tests ...", flush=True)
    result = subprocess.run(cmd, capture_output=True, text=True)
    output = (result.stdout + result.stderr).strip()
    if output:
        print(output)
    if result.returncode != 0:
        sys.exit("Build failed")
    warnings = output.count("warning:")
    return exe, warnings


# ---- Independent glyph check ----

def read_bdf(path):
    """{codepoint: (width, height, left, top, advance, rows bytes)}."""
    glyphs, size, ascent = {}, None, None
    lines = path.read_text(encoding="ascii").splitlines()
    i = 0
    while i < len(lines):
        parts = lines[i].split()
        if parts and parts[0] == "PIXEL_SIZE":
            size = int(parts[1])
        elif parts and parts[0] == "FONT_ASCENT":
            ascent = int(parts[1])
        elif parts and parts[0] == "STARTCHAR":
            g = {}
            i += 1
            while not lines[i].startswith("ENDCHAR"):
                p = lines[i].split()
                if p[0] == "ENCODING":
                    g["cp"] = int(p[1])
                elif p[0] == "DWIDTH":
                    g["adv"] = int(p[1])
                elif p[0] == "BBX":
                    g["w"], g["h"], g["x"], g["y"] = map(int, p[1:5])
                elif p[0] == "BITMAP":
                    rows = []
                    for _ in range(g["h"]):
                        i += 1
                        rows.append(bytes.fromhex(lines[i].strip()))
                    g["rows"] = b"".join(rows)
                i += 1
            if g.get("cp", -1) >= 0:
                glyphs[g["cp"]] = (g["w"], g["h"], g["x"], -g["h"] - g["y"], g["adv"], g["rows"])
        i += 1
    return size, ascent, glyphs


def parse_assets(text, name):
    m = re.search(rf"static const uint8_t {name}_BITMAP\[\] = \{{(.*?)\}};", text, re.S)
    bitmap = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", m.group(1)))
    m = re.search(rf"static const Glyph {name}_GLYPHS\[\] = \{{(.*?)\n\}};", text, re.S)
    glyphs = [tuple(map(int, g.split(","))) for g in re.findall(r"\{([-0-9, ]+)\}", m.group(1))]
    m = re.search(rf"const Font FONT_{name} = \{{(\d+), (\d+), (\d+),", text)
    return bitmap, glyphs, tuple(map(int, m.groups()))


def check_assets():
    text = (DASHBOARD / "dashboard_assets.cpp").read_text(encoding="utf-8")
    fonts = ROOT / "fonts" / "terminus"
    problems = 0
    expected = {
        "TEXT14": ("ter-u14n.bdf", [*range(0x20, 0x7F), 0xB0, 0xB5, 0xB3], 1372),
        "BOLD32": ("ter-u32b.bdf", [ord(c) for c in "0123456789.- °FC%"], 1088),
    }
    for name, (bdf, codepoints, total) in expected.items():
        size, ascent, src = read_bdf(fonts / bdf)
        bitmap, glyphs, (fsize, baseline, count) = parse_assets(text, name)
        if (fsize, baseline, count) != (size, ascent, len(set(codepoints))):
            print(f"  FAIL {name}: font header {fsize}/{baseline}/{count}")
            problems += 1
        if len(bitmap) != total:
            print(f"  FAIL {name}: {len(bitmap)} bitmap bytes, expected {total}")
            problems += 1
        if [g[0] for g in glyphs] != sorted(set(codepoints)):
            print(f"  FAIL {name}: glyph set or order differs")
            problems += 1
        for cp, offset, w, h, adv, left, top in glyphs:
            sw, sh, sx, stop, sadv, rows = src[cp]
            if (w, h, adv, left, top) != (sw, sh, sadv, sx, stop) or bitmap[offset:offset + len(rows)] != rows:
                print(f"  FAIL {name}: U+{cp:04X} differs from {bdf}")
                problems += 1
    m = re.search(r"const Sprite SPRITE_PM_UNITS = \{(\d+), (\d+), (\d+),", text)
    if m is None or tuple(map(int, m.groups())) != (30, 12, 4):
        print("  FAIL PM sprite is not 30 x 12, stride 4")
        problems += 1
    print(f"{'ok  ' if problems == 0 else 'FAIL'} assets match the Terminus BDF sources byte for byte")
    return problems


def main():
    exe, warnings = build()
    RENDERS.mkdir(parents=True, exist_ok=True)
    args = [str(exe), str(RENDERS)] + sys.argv[1:2]
    code = subprocess.run(args).returncode
    problems = check_assets()
    print(f"\nCompiler warnings: {warnings}; renders in {RENDERS}")
    sys.exit(0 if code == 0 and problems == 0 and warnings == 0 else 1)


if __name__ == "__main__":
    main()
