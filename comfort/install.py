#!/usr/bin/env python3
"""Install only org.openxwa.comfort; import owned game files and optional pilots."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "org.openxwa.comfort"
ORIGINAL = "org.openxwa.quest"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", type=Path, default=ROOT / "build/artifacts/OpenXWA-Comfort-0.1.0-native-baseline.apk")
    game = parser.add_mutually_exclusive_group(required=True)
    game.add_argument("--game-dir", type=Path)
    game.add_argument("--game-zip", type=Path)
    parser.add_argument("--pilot-dir", type=Path, help="Folder containing PC/exported .plt files")
    parser.add_argument("--serial", help="Required if more than one Android device is connected")
    parser.add_argument("--adb", default=shutil.which("adb") or str(Path(os.environ.get("ANDROID_HOME", "")) / "platform-tools/adb.exe"))
    parser.add_argument("--launch", action="store_true")
    args = parser.parse_args()
    args.apk = args.apk.resolve()
    # Bind installation to the reviewed artifact and its validation report.
    report = json.loads(args.apk.with_suffix(".validation.json").read_text(encoding="utf-8"))
    digest = hashlib.sha256(args.apk.read_bytes()).hexdigest()
    if report.get("package") != PACKAGE or report.get("apk_sha256") != digest:
        raise SystemExit("APK does not match its Comfort validation report")
    with zipfile.ZipFile(args.apk) as apk:
        metadata = json.loads(apk.read("assets/comfort-build.json"))
        if metadata.get("package") != PACKAGE:
            raise SystemExit("Refusing to install an APK without the Comfort identity")
    devices = subprocess.check_output([args.adb, "devices"], text=True)
    connected = [line.split()[0] for line in devices.splitlines()[1:] if len(line.split()) > 1 and line.split()[1] == "device"]
    if args.serial:
        if args.serial not in connected:
            raise SystemExit("Selected device is disconnected or USB debugging is unauthorized")
    elif len(connected) == 1:
        args.serial = connected[0]
    else:
        raise SystemExit("Connect and authorize exactly one headset, or supply --serial")
    adb = [args.adb, "-s", args.serial]

    def command(*parts):
        result = subprocess.run(adb + list(map(str, parts)), check=True, capture_output=True, text=True)
        return result.stdout.strip()

    def original_fingerprint():
        paths = command("shell", "pm", "path", ORIGINAL).splitlines()
        return [command("shell", "sha256sum", path.removeprefix("package:")) for path in paths if path.startswith("package:")]

    def import_zip(path, destination):
        authority = f"content://{PACKAGE}.setup"
        result = command("shell", "content", "call", "--uri", authority, "--method", "mkdirs", "--arg", destination)
        if "error=" in result or "made=1" not in result:
            raise RuntimeError(f"Could not prepare app-owned storage: {result}")
        print(command("push", path, f"/sdcard/Android/obb/{PACKAGE}/openxwa-import.zip"), flush=True)
        result = command("shell", "content", "call", "--uri", authority, "--method", "unzip", "--arg", destination)
        print(result, flush=True)
        with zipfile.ZipFile(path) as archive:
            count = sum(not info.is_dir() for info in archive.infolist())
        match = re.search(r"\bfiles=(\d+)\b", result)
        if "error=" in result or "skipped=0" not in result or not match or int(match[1]) != count:
            raise RuntimeError("Import did not unpack every expected file")
        return count

    def validate_game_zip(path):
        with zipfile.ZipFile(path) as archive:
            names = [info.filename for info in archive.infolist() if not info.is_dir()]
            if not any(name.upper() == "RESDATA.TXT" for name in names):
                raise RuntimeError("Game archive must contain RESDATA.TXT at its root")
            for name in names:
                # Reject both POSIX and Windows forms before invoking the app's importer.
                normalized = name.replace("\\", "/")
                if normalized.startswith("/") or ":" in normalized or ".." in normalized.split("/"):
                    raise RuntimeError(f"Unsafe game archive path: {name}")

    with tempfile.TemporaryDirectory(prefix="comfort-import-") as directory:
        work = Path(directory)
        if args.game_dir:
            folder = args.game_dir.resolve(strict=True)
            if not folder.is_dir():
                raise SystemExit("Game source must be a directory")
            game_zip = work / "game.zip"
            with zipfile.ZipFile(game_zip, "w", zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
                for path in sorted(folder.rglob("*")):
                    if path.is_file():
                        if path.is_symlink():
                            raise SystemExit(f"Game source contains a symlink: {path}")
                        archive.write(path, path.relative_to(folder).as_posix())
        else:
            game_zip = args.game_zip.resolve(strict=True)
        validate_game_zip(game_zip)
        pilots = []
        if args.pilot_dir:
            pilots = [path for path in args.pilot_dir.resolve(strict=True).iterdir()
                      if path.is_file() and path.suffix.lower() == ".plt" and len(path.name) <= 64]
            if not pilots:
                raise SystemExit("No .plt pilot files found; installation has not started")
        # Updating this package preserves its internal pilots. The provider imports
        # pilots through the engine's existing 'keep existing progress' mechanism.
        before = original_fingerprint()
        try:
            print(command("install", "-r", args.apk), flush=True)
            game_count = import_zip(game_zip, "XWA")
            if pilots:
                pilot_zip = work / "pilots.zip"
                with zipfile.ZipFile(pilot_zip, "w", zipfile.ZIP_DEFLATED) as archive:
                    for path in pilots:
                        archive.write(path, path.name)
                import_zip(pilot_zip, "pilot-import")
            if args.launch:
                print(command("shell", "am", "start", "-n", PACKAGE + "/org.openxwa.quest.OpenXwaActivity"))
        finally:
            if original_fingerprint() != before:
                raise RuntimeError("Original OpenXWA APK fingerprint changed during installation")
        print(f"Imported {game_count} game files and {len(pilots)} pilots into {PACKAGE}.")
        print("Original OpenXWA APK fingerprint is unchanged.")


if __name__ == "__main__":
    main()
