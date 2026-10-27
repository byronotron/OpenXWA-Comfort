#!/usr/bin/env python3
"""Download hash-locked native build tools into local app data and ignored build/."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool-dir", type=Path,
                        default=Path(os.environ.get("LOCALAPPDATA", str(Path.home() / ".local/share"))) / "OpenXWA-Comfort/toolchains")
    args = parser.parse_args()
    lock = json.loads((ROOT / "comfort/lock.json").read_text())
    deps = ROOT / "build/deps"
    deps.mkdir(parents=True, exist_ok=True)
    args.tool_dir.mkdir(parents=True, exist_ok=True)
    for name, item in lock["tool_downloads"].items():
        destination = (args.tool_dir if name in ("ndk", "zig") else deps) / item["file"]
        if not destination.exists() or hashlib.sha256(destination.read_bytes()).hexdigest() != item["sha256"]:
            partial = destination.with_suffix(destination.suffix + ".part")
            print("Downloading", item["url"], flush=True)
            urllib.request.urlretrieve(item["url"], partial)
            if hashlib.sha256(partial.read_bytes()).hexdigest() != item["sha256"]:
                raise SystemExit(f"Checksum mismatch: {name}")
            partial.replace(destination)
        print("Verified", destination, flush=True)
        if name in ("ndk", "zig"):
            directory = args.tool_dir / ("android-ndk-r27c" if name == "ndk" else "zig-windows-x86_64-0.13.0")
            marker = directory / ".comfort-extracted"
            if not marker.exists():
                with zipfile.ZipFile(destination) as archive:
                    archive.extractall(args.tool_dir)
                marker.write_text(item["sha256"])
        elif name == "ffmpeg" and not (deps / "ffmpeg-8.0.1/libavcodec/avcodec.h").exists():
            subprocess.run(["tar", "-xf", str(destination), "-C", str(deps)], check=True)
    sdl = deps / "SDL"
    if not sdl.exists():
        subprocess.run(["git", "clone", "--filter=blob:none", "--no-checkout", "https://github.com/libsdl-org/SDL.git", str(sdl)], check=True)
    subprocess.run(["git", "-C", str(sdl), "checkout", lock["sdl_revision"]], check=True)
    base = ROOT.parent / "OpenXWA-Quest-1.0.1/OpenXWA-quest-1.0.1.apk"
    if not base.exists():
        release_zip = deps / "OpenXWA-Quest-1.0.1.zip"
        release = lock["upstream_release_archive"]
        if not release_zip.exists() or hashlib.sha256(release_zip.read_bytes()).hexdigest() != release["sha256"]:
            urllib.request.urlretrieve(release["url"], release_zip)
        if hashlib.sha256(release_zip.read_bytes()).hexdigest() != release["sha256"]:
            raise SystemExit("Upstream release archive checksum mismatch")
        base = ROOT / "build/upstream-release/OpenXWA-quest-1.0.1.apk"
        base.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(release_zip) as archive:
            entries = [name for name in archive.namelist() if name.endswith("OpenXWA-quest-1.0.1.apk")]
            if len(entries) != 1:
                raise SystemExit("Upstream archive does not contain exactly one expected APK")
            base.write_bytes(archive.read(entries[0]))
    if hashlib.sha256(base.read_bytes()).hexdigest() != lock["baseline_apk_sha256"]:
        raise SystemExit("Pinned upstream APK missing or changed")
    with zipfile.ZipFile(base) as archive:
        archive.extractall(ROOT / "build/upstream-apk")
    print("Native prerequisites ready. See comfort/README.md for the build command.")


if __name__ == "__main__":
    main()
