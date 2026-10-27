#!/usr/bin/env python3
"""Build the isolated baseline using byte-identical upstream engine payloads.

This is an identity-only repack, not a native source rebuild. No comfort changes
or performance claims are made. The source fork is pinned separately in lock.json.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import tempfile
import zipfile
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "org.openxwa.comfort"
VERSION = "0.1.0-baseline"


def run(args, env=None):
    print("+", " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, env=env)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-apk", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, default=Path(os.environ.get("ANDROID_HOME", "")))
    parser.add_argument("--native-build", type=Path, help="Verified native packaged directory from build_native.py")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    version = VERSION
    label = "OpenXWA Comfort (Baseline)"
    kind = "identity-only-baseline"
    native = None
    if args.native_build:
        native = json.loads((args.native_build / "native-build.json").read_text())
        for name, digest in native["native_libraries"].items():
            if sha((args.native_build / name).read_bytes()) != digest:
                raise SystemExit(f"Native library does not match its build record: {name}")
        version = "0.1.0-native-baseline"
        label = "OpenXWA Comfort (Native baseline)"
        kind = "native-source-baseline-with-upstream-dependencies"
        if native.get("comfort_changes"):
            version = native["app_version"]
            label = native["app_label"]
            kind = native["kind"]
    if args.out is None:
        args.out = ROOT / f"build/artifacts/OpenXWA-Comfort-{version}.apk"
    lock = json.loads((ROOT / "comfort/lock.json").read_text())
    if sha(args.base_apk.read_bytes()) != lock["baseline_apk_sha256"]:
        raise SystemExit("Base APK differs from the pinned upstream 1.0.1 APK")
    sdk = args.sdk.resolve()
    tools = sdk / "build-tools/34.0.0"
    android_jar = sdk / "platforms/android-34/android.jar"
    aapt = tools / ("aapt2.exe" if os.name == "nt" else "aapt2")
    signer = tools / "lib/apksigner.jar"
    for path in (aapt, signer, android_jar):
        if not path.is_file():
            raise SystemExit(f"Missing Android build tool: {path}")
    for command in ("java", "keytool"):
        if not shutil.which(command):
            raise SystemExit(f"JDK executable missing from PATH: {command}")
    # Keep the independent signing identity outside both Git and OneDrive.
    key_dir = Path(os.environ.get("LOCALAPPDATA", str(Path.home() / ".local/share"))) / "OpenXWA-Comfort/signing"
    key_dir.mkdir(parents=True, exist_ok=True)
    credentials = key_dir / "baseline-signing.json"
    keystore = key_dir / "baseline.p12"
    if not credentials.exists():
        if keystore.exists():
            raise SystemExit("Keystore exists without its password file; refusing to replace it")
        credentials.write_text(json.dumps({"password": secrets.token_urlsafe(32)}))
    env = dict(os.environ)
    env["XWA_COMFORT_STOREPASS"] = json.loads(credentials.read_text())["password"]
    if not keystore.exists():
        run(["keytool", "-genkeypair", "-keystore", keystore, "-storetype", "PKCS12",
             "-storepass:env", "XWA_COMFORT_STOREPASS", "-alias", "comfort", "-keyalg", "RSA",
             "-keysize", "3072", "-validity", "10000", "-dname", "CN=OpenXWA Comfort local baseline"], env)
    args.out = args.out.resolve()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("upstream_packager", ROOT / "packaging/quest/build_apk.py")
    upstream = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(upstream)
    with tempfile.TemporaryDirectory(prefix="openxwa-comfort-") as directory:
        work = Path(directory)
        resources = work / "res"
        shutil.copytree(ROOT / "packaging/quest/res", resources)
        (resources / "values/strings.xml").write_text(
            '<resources><string name="app_name">' + escape(label) + '</string></resources>', encoding="utf-8")
        manifest = (ROOT / "packaging/quest/AndroidManifest.xml").read_text(encoding="utf-8")
        version_code = str(native.get("app_version_code", 2)) if native else "1"
        manifest = manifest.replace("@PACKAGE@", PACKAGE).replace("@VERSION_NAME@", version).replace("@VERSION_CODE@", version_code)
        (work / "AndroidManifest.xml").write_text(manifest, encoding="utf-8")
        run([aapt, "compile", "--dir", resources, "-o", work / "res.zip"])
        run([aapt, "link", "-o", work / "base.apk", "--manifest", work / "AndroidManifest.xml",
             "-I", android_jar, work / "res.zip", "--min-sdk-version", "29", "--target-sdk-version", "32"])
        shutil.copyfile(work / "base.apk", work / "unaligned.apk")
        retained = {}
        with zipfile.ZipFile(args.base_apk) as original, zipfile.ZipFile(work / "unaligned.apk", "a") as output:
            for info in original.infolist():
                name = info.filename
                if name.startswith(("lib/", "assets/")) or (name.startswith("classes") and name.endswith(".dex")):
                    original_data = original.read(name)
                    data = original_data
                    if native and name.startswith("lib/arm64-v8a/") and Path(name).name in native["native_libraries"]:
                        data = (args.native_build / Path(name).name).read_bytes()
                    output.writestr(info, data)
                    if data == original_data:
                        retained[name] = sha(data)
            metadata = {"package": PACKAGE, "version": version, "kind": kind,
                        "source": lock, "unchanged_payload_count": len(retained)}
            if native:
                metadata["native_build"] = native
            output.writestr("assets/comfort-build.json", json.dumps(metadata, indent=2))
            output.writestr("assets/comfort-LICENSE.txt", (ROOT / "LICENSE").read_bytes())
        upstream.align_zip(work / "unaligned.apk", work / "aligned.apk")
        run(["java", "-jar", signer, "sign", "--ks", keystore, "--ks-pass", "env:XWA_COMFORT_STOREPASS",
             "--ks-key-alias", "comfort", "--min-sdk-version", "29", "--v1-signing-enabled", "false",
             "--v2-signing-enabled", "true", "--v3-signing-enabled", "true", "--out", args.out, work / "aligned.apk"], env)
        run(["java", "-jar", signer, "verify", "--verbose", "--print-certs", args.out])
        with zipfile.ZipFile(args.out) as output:
            for name, digest in retained.items():
                if sha(output.read(name)) != digest:
                    raise SystemExit(f"Baseline payload changed: {name}")
            if native:
                for name, digest in native["native_libraries"].items():
                    if sha(output.read("lib/arm64-v8a/" + name)) != digest:
                        raise SystemExit(f"Packaged native library differs from the build: {name}")
        badging = subprocess.check_output([str(aapt), "dump", "badging", str(args.out)], text=True)
        if f"package: name='{PACKAGE}'" not in badging or label not in badging:
            raise SystemExit("Packaged identity or label is incorrect")
        manifest_dump = subprocess.check_output([str(aapt), "dump", "xmltree", str(args.out), "--file", "AndroidManifest.xml"], text=True)
        if f'"{PACKAGE}.setup"' not in manifest_dump or '"org.openxwa.quest.setup"' in manifest_dump:
            raise SystemExit("Setup provider authority does not isolate the baseline")
        report = {**metadata, "apk_sha256": sha(args.out.read_bytes()), "unchanged_payloads": retained,
                  "validation": {"signature": "verified", "payloads": "byte-identical", "package": PACKAGE,
                                 "provider": PACKAGE + ".setup", "headset_runtime": "not yet tested"}}
        args.out.with_suffix(".validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"Verified {len(retained)} unchanged upstream payloads. APK: {args.out}")


if __name__ == "__main__":
    main()
