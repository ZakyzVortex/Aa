#!/usr/bin/env python3
"""Packages JNGS Frame Gen into a validated LeviLaunchroid .levipack archive.

Usage:
    python3 scripts/package_levipack.py --library build/libjngsframegen.so \
        --icon assets/icon.png --output FrameGen.levipack
"""

import argparse
import json
import sys
import zipfile
from pathlib import Path

MOD_ID = "jngsframegen"
MOD_NAME = "JNGS Frame Gen"
MOD_AUTHOR = "JNGS"
MOD_VERSION = "3.3.5"
MOD_DESCRIPTION = (
    "JNGS Frame Gen v3.3.5 - Fixes misaligned/desynchronized generated frames: perspective-correct camera warp, working mismatch guard, safe GL state in hooks, correct previous-output capture."
)
ENTRY = "libjngsframegen.so"


def build_manifest(has_icon: bool) -> dict:
    return {
        "schema_version": 1,
        "id": MOD_ID,
        "name": MOD_NAME,
        "author": MOD_AUTHOR,
        "version": MOD_VERSION,
        "description": MOD_DESCRIPTION,
        "type": "preload-native",
        "entry": ENTRY,
        "minecraft_versions": [],
        "icon": "icon.png" if has_icon else "",
    }


def write_package(library: Path, icon: Path | None, output: Path) -> None:
    if not library.is_file():
        raise FileNotFoundError(f"Shared library not found: {library}")

    has_icon = bool(icon and icon.is_file())
    manifest = build_manifest(has_icon)

    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()

    with zipfile.ZipFile(
        output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        archive.writestr(
            "manifest.json",
            json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        )
        archive.write(library, ENTRY)
        if has_icon and icon is not None:
            archive.write(icon, "icon.png")
        archive.writestr(
            f"CHANGELOG_v{MOD_VERSION}.txt",
            "JNGS Frame Gen " + MOD_VERSION + "\n\n"
            "=== Major Features ===\n"
            "- Synthesis Resolution Scaling: new config 'synthesis_scale' (0.25x-1.0x) "
            "renders frame synthesis at lower resolution for massive GPU savings. "
            "0.5x = 4x fewer pixels, 0.75x = ~2x fewer pixels.\n"
            "- Performance Preset: new 'Performance' quality preset (SingleSwap, "
            "2x multiplier, 0.5x synthesis scale, minimal HUD protection) targets "
            "<5% overhead on mid-range ARM64 devices.\n"
            "- SingleSwap Default: frameGenMode now defaults to SingleSwapSynthesis "
            "(visual synthesis only, 1 eglSwapBuffers/frame). MultiSwapInterpolation "
            "is opt-in for users who want additional presentation submissions.\n"
            "- FPS Drop Fixes: addresses major mobile bottlenecks — glCopyTexSubImage2D "
            "readback at scaled resolution, full-res shader ALU reduction, "
            "GL state caching, frame pacing off render thread.\n\n"
            "=== Shader Improvements ===\n"
            "- UV scaling for synthesis textures (uSynthScale uniform) — correctly "
            "samples from lower-resolution synthesis textures.\n"
            "- HUD texture remains full resolution for clean overlay.\n\n"
            "=== Config Changes ===\n"
            "- New key: synthesis_scale (0.25 .. 1.0, default 1.0)\n"
            "- New preset: Performance (index 4)\n"
            "- Cycle preset now cycles through 5 presets (PowerSaver, Balanced, "
            "Smooth, Ultra, Performance)\n"
        )

    print(f"Packaged {output.resolve()} ({output.stat().st_size} bytes)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--icon", type=Path, default=None)
    args = parser.parse_args()
    try:
        write_package(args.library.resolve(), args.icon, args.output.resolve())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
