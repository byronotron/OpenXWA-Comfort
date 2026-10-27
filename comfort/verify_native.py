#!/usr/bin/env python3
"""Check rebuilt ARM64 ELF headers, dependencies, and strong symbol resolution."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--ndk", type=Path, required=True)
    args = parser.parse_args()
    llvm = args.ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe"
    system = args.ndk / "toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/29"

    def readelf(path, *options):
        return subprocess.check_output([str(llvm), *options, str(path)], text=True)

    def symbols(path):
        defined, undefined = set(), set()
        for line in readelf(path, "--dyn-symbols", "--wide").splitlines():
            fields = line.split()
            if len(fields) < 8 or not fields[0].endswith(":"):
                continue
            name = fields[7].split("@")[0]
            if fields[6] == "UND" and fields[4] != "WEAK":
                undefined.add(name)
            elif fields[6] != "UND":
                defined.add(name)
        return defined, undefined

    with tempfile.TemporaryDirectory(prefix="comfort-elf-") as directory:
        work = Path(directory)
        with zipfile.ZipFile(args.apk) as archive:
            metadata = json.loads(archive.read("assets/comfort-build.json"))
            for name in archive.namelist():
                if name.startswith("lib/arm64-v8a/") and name.endswith(".so"):
                    (work / Path(name).name).write_bytes(archive.read(name))
        providers = set()
        for path in work.glob("*.so"):
            providers.update(symbols(path)[0])
        # The Android API stubs provide the system symbols available at minSdk 29.
        for path in system.glob("*.so"):
            with path.open("rb") as handle:
                is_elf = handle.read(4) == b"\x7fELF"
            if is_elf:
                providers.update(symbols(path)[0])
        results = {}
        for name in metadata["native_build"]["native_libraries"]:
            path = work / name
            header = readelf(path, "-h")
            if "AArch64" not in header or "ELF64" not in header:
                raise SystemExit(f"Wrong native ABI: {name}")
            segments = readelf(path, "-l", "--wide")
            loads = [line for line in segments.splitlines() if line.strip().startswith("LOAD")]
            if not loads or any(int(line.split()[-1], 16) < 16384 for line in loads):
                raise SystemExit(f"Missing 16 KiB ELF segment alignment: {name}")
            dependencies = re.findall(r"\(NEEDED\).*?\[([^]]+)\]", readelf(path, "-d"))
            for dependency in dependencies:
                if not (work / dependency).exists() and not (system / dependency).exists():
                    raise SystemExit(f"Missing dependency: {name} -> {dependency}")
            missing = symbols(path)[1] - providers
            if missing:
                raise SystemExit(f"Unresolved strong symbols in {name}: {sorted(missing)}")
            results[name] = {"abi": "arm64-v8a", "elf_alignment": 16384,
                             "dependencies": dependencies, "unresolved_strong_symbols": []}
        out = args.apk.with_suffix(".native-validation.json")
        out.write_text(json.dumps({"apk_sha256": hashlib.sha256(args.apk.read_bytes()).hexdigest(),
                                   "checks": results, "headset_runtime": "not yet tested"}, indent=2))
        print(f"Verified ARM64 ABI, 16 KiB alignment, dependencies and strong symbols: {out}")


if __name__ == "__main__":
    main()
