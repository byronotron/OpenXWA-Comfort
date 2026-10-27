# OpenXWA Comfort: first baseline

The baseline history below is retained for comparison. The current test build is
**0.2.0-adaptive-mask**, documented in [PERIPHERAL-MASK.md](PERIPHERAL-MASK.md).
It adds an Off-by-default peripheral mask driven by ship rotation, with separate
strength and sensitivity controls. Impact shake remains Original by default;
see [CAMERA-AUDIT.md](CAMERA-AUDIT.md). Headset flight and subjective comfort
validation of the mask remain pending.

This branch starts from OpenXWA Quest **v1.0.1**, commit
`22c3fd3373c4b1a45bffb2072b131e242dd322d2`, with Aeron pinned to
`2e813e81c3384422792a7637c81516e6506cdb05`. See `lock.json`.

## Native baseline

`build/artifacts/OpenXWA-Comfort-0.1.0-native-baseline.apk` is the first native
source baseline. The game/engine (`libmain.so`) and Vulkan foveation shim were
recompiled from the pinned source with Android NDK r27c. It uses a separate
application ID, label and signing identity. It has **no comfort feature changes**.

The other 124 upstream DEX, library and asset entries are byte-identical,
including SDL, FFmpeg, voice libraries/models, VR artwork, and shaders. This is
not a rebuild of every third-party library or a fresh HLSL-to-SPIR-V compilation.
The original host shader-manifest emitter runs against its pinned YAML input;
cached shaders are allowed only while their source and generator remain pinned.
Build adaptations are confined to an ignored source mirror; the Aeron submodule
and C/C++ runtime source remain unmodified.

Desktop checks passed: APK signature, package/provider isolation, retained
payload hashes, ARM64 ELF ABI, 16 KiB segment alignment, shared-library presence,
and strong symbol resolution against packaged libraries and Android API 29 stubs.
These checks do not establish startup or gameplay correctness on the headset.
The adjacent `.validation.json` and `.native-validation.json` files record them.

## Identity-only comparison APK

`build/artifacts/OpenXWA-Comfort-0.1.0-baseline.apk` is an **identity-only repack**
of the verified upstream release. It is **not a native source rebuild** and has
no camera, shader, physics, tracking, graphics-default, or comfort changes.
All 126 upstream DEX, native library, and asset entries are byte-identical.
An added metadata asset records provenance; packaging and Android resources
are regenerated from the source manifest and resources.

| Item | Original | Baseline |
| --- | --- | --- |
| Android package | `org.openxwa.quest` | `org.openxwa.comfort` |
| Library label | OpenXWA | OpenXWA Comfort (Baseline) |
| Import provider | `org.openxwa.quest.setup` | `org.openxwa.comfort.setup` |
| Signing certificate | Upstream release | Separate local RSA key |
| External files | Original package's files | Comfort package's files |
| Internal pilots/settings | Original app sandbox | Comfort app sandbox |

The native baseline uses the same Comfort package/key and versionCode 2, with the
label **OpenXWA Comfort (Native baseline)**. The identity-only comparison has
versionCode 1. Either can coexist with the original OpenXWA. The two Comfort
variants are versions of the same app, so install the native baseline for the
first source-build comparison. Avoid uninstalling either app to preserve saves.

## Rebuild from a clean checkout on Windows

Prerequisites: Python 3.11+, JDK 17, Git, CMake 3.20+, Ninja, Windows `tar`,
Android SDK Build Tools 34.0.0 and Platform android-34. Recursively initialize
the pinned `aeron` submodule. Downloads use the URLs and SHA-256 hashes in
`lock.json`; NDK and Zig are stored outside the repository and OneDrive.

```powershell
git submodule update --init --recursive
python comfort/prepare_native.py
python comfort/build_native.py --ndk "$env:LOCALAPPDATA/OpenXWA-Comfort/toolchains/android-ndk-r27c" --zig "$env:LOCALAPPDATA/OpenXWA-Comfort/toolchains/zig-windows-x86_64-0.13.0/zig.exe"
python comfort/build_baseline.py --base-apk build/upstream-release/OpenXWA-quest-1.0.1.apk --native-build build/comfort-native/packaged
python comfort/verify_native.py --apk build/artifacts/OpenXWA-Comfort-0.1.0-native-baseline.apk --ndk "$env:LOCALAPPDATA/OpenXWA-Comfort/toolchains/android-ndk-r27c"
```

If the existing release is already present beside this checkout, preparation
reuses `../OpenXWA-Quest-1.0.1/OpenXWA-quest-1.0.1.apk`; supply that path to the
packaging command instead. Build-only host-tool adaptations skip compilation of
unchanged cached shaders; they do not fabricate reflection data or certify
modified shaders. A shader development build must add a real host compiler.

The Java class names remain `org.openxwa.quest.*`. Fully qualified class names
do not determine Android's application ID or its storage sandbox. The manifest's
provider authority is parameterized to prevent installation conflicts.

## Build the baseline

Prerequisites: Python 3, JDK 17 with `java` and `keytool` on PATH, Android SDK
Build Tools 34.0.0 and Platform android-34. No game data is included in the APK.

```powershell
python comfort/build_baseline.py --base-apk ../OpenXWA-Quest-1.0.1/OpenXWA-quest-1.0.1.apk --sdk "$env:LOCALAPPDATA/Android/Sdk"
```

The base APK must match the SHA-256 in `lock.json`. The build checks the signed
APK, its application ID, label, isolated provider authority, and every retained
payload hash. The adjacent `.validation.json` records results. Signing material
is kept in `%LOCALAPPDATA%/OpenXWA-Comfort/signing/`, outside Git and OneDrive.
Back up that folder securely if future APKs must update this installation.

## Install when the Quest is connected

Enable USB debugging and leave the headset awake. From this repository:

```powershell
python comfort/install.py --game-zip ../downloads/xwa.zip --adb "$env:LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe" --launch
```

Or create a fresh game archive directly from your own Steam installation:

```powershell
python comfort/install.py --game-dir 'C:/Program Files (x86)/Steam/steamapps/common/Star Wars X-Wing Alliance' --adb "$env:LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe" --launch
```

Add `--pilot-dir 'C:/path/to/exported-pilots'` to import `.plt` files using the
engine's existing pilot-import mechanism. Existing Comfort pilots take
precedence. The installer never changes or uninstalls the original package;
it compares the original APK fingerprint before and after installation.
It checks the imported archive's file count and uses the app-owned importer
rather than writing game data directly into app storage.

The original app's current pilots are in its **private internal storage**.
An ordinary ADB connection cannot assume access to them. This baseline supports
PC or exported pilot files; migration of private Quest saves still needs an
explicit export path in the original app or a supported device backup route.
The original working installation and its progress remain the comparison copy.

## Current acceptance status

- Source and Aeron revisions pinned: complete.
- Separate identity and signing key: complete.
- Identity-only baseline APK and payload/signature checks: complete.
- Game/pilot import installer: implemented; headset validation pending.
- Native C/C++ source rebuild and desktop compatibility checks: complete.
- Side-by-side installation, startup, pilot import, and mission test: pending
  the next USB headset connection.
- Performance and comfort measurements: not started.

Do not describe this APK as optimized, comfortable, campaign-validated, or a
90 Hz build. It preserves the original runtime defaults for the first comparison.

## Next engineering gate

The repeatable native baseline build is established on the PC. Its next gate is
side-by-side startup and mission equivalence on Quest 3. The source release's
Linux-only packaging assumptions are bypassed by the isolated Windows build;
all runtime payloads are retained. SDL matches the release revision `ce3af25d4`
(SDL 3.5.0), which supplies its OpenXR APIs. Films, voice, and VR remain enabled.

After native build/startup equivalence, add performance instrumentation, record
a reproducible combat baseline, and audit camera pivots and artificial shake.
90 Hz is a hypothesis to measure, not an acceptance result. Keep live headset
tracking direct and all comfort effects render-only and independently optional.

GPL-3.0: see the repository `LICENSE`. Existing third-party notices are retained
inside the APK. Do not include owned game data or signing material in commits.
