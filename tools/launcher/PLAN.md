# OpenXWA-VR Launcher (PC) - plan

Windows PC app (one portable .exe, no install) that puts OpenXWA and the
user's own X-Wing Alliance game files on a standalone headset over USB.
Quest, Pico and Steam Frame use the same steps. Mac/Linux later.
Implemented in this folder (Go, standard library only; build.sh): a window UI
(embedded page in an Edge/Chrome app window, web/index.html) or --console.
This file records the original design; see the code for the current flow
(ownership checks, Steam Frame via Lepton).

Since 1.0.0-rc6 the files are not pushed: on Quest (Horizon OS 14) every
file and folder adb makes belongs to the "shell" user and the app cannot
read it. The APK has a helper (SetupProvider, see appimport.go) that adb
reaches without starting the game: the launcher zips the files while
streaming them into the app (exec-in content write), and the app unpacks
them itself (content call unzip), in batches of 256 MB. So the headset can
stay on the desk. Older apps without the helper fall back to pushing.

## Distribution

One zip per release on GitHub Releases (neitey/OpenXWA-Quest):

    OpenXWA-Quest-<version>.zip
      OpenXWA-VR-Launcher.exe
      OpenXWA-quest-<version>.apk
      README.txt
      THIRD-PARTY-NOTICES.txt
      LICENSE.txt               (GPL-3)

plus `SHA256SUMS.txt` as a separate release asset. The launcher installs
the APK next to it. No game files are ever included. Launcher source
lives in this repo (GPL-3). 1.0 ships unsigned (README explains the
SmartScreen warning); apply to SignPath Foundation for free signing,
which requires the .exe to be built by GitHub Actions from the public
repo. The APK keeps being signed locally; its key stays out of the repo.

## adb

1. Reuse an existing adb: PATH, SideQuest, Meta Quest Developer Hub,
   Android SDK platform-tools.
2. Otherwise download Google's platform-tools
   (https://dl.google.com/android/repository/platform-tools-latest-windows.zip)
   after showing the Android SDK License Agreement and getting the user's
   own "I agree". Unpack into the launcher's own folder. Never bundled
   (SDK licence section 3.4 forbids redistribution).

## Install flow

1. Find the game: Steam library folders, GOG registry entries, original
   CD registry entry; several found -> user picks; none -> "Browse...".
   Validate the folder (RESDATA.TXT and the data folders present).
2. Connect: wait for the headset, prompt "allow USB debugging" until
   authorised; read the device model (Quest / Pico / Steam Frame).
3. Install or update the APK (`adb install -r`). On a signature mismatch,
   stop and explain; never uninstall (that deletes the game files).
4. Let the app create its own folder (rc6+: through its helper).
5. Build the PC file list with sizes; skip files the port doesn't use
   (.exe, .dll).
6. Ask whether to bring existing PC pilot files over.
7. Create all folders on the headset first, in one shell call.
8. Push folder by folder with a progress bar.
9. Verify: list the headset folder with sizes, compare, re-push missing or
   wrong-size files individually, up to 3 tries; name any file that still
   fails.
10. Write a file list (manifest) on the headset; the app checks it at start
    and lists exactly which files are missing.
11. Done: button to start the game, or Library -> Unknown sources.

Running it again updates the app and copies only missing/changed files.
"Collect logs" pulls openxwa.log for bug reports.

## Update check

- At launcher start (and from a "Check for updates" button), request
  `https://api.github.com/repos/neitey/OpenXWA-Quest/releases/latest`.
  No account or token needed while the repo is public (GitHub allows 60
  unauthenticated requests per hour per IP; one per start is plenty).
- Compare the release tag (e.g. `v1.0.1`) with the launcher's own
  version. Release candidates (`-rcN`) are only offered when
  "Include test versions" is ticked.
- Newer version found: show version, date and the release notes, and ask
  "Download now?" [Download] [Later] [Skip this version].
- Download: the release zip into the user's Downloads folder with a
  progress bar; check its SHA-256 against `SHA256SUMS.txt` from the same
  release; extract to a new folder next to the zip.
- Then offer "Open the new version now". The new launcher continues with
  the normal install/update flow (app update + changed files only).
  The running launcher never replaces itself.
- No internet or GitHub unreachable: skip silently; the button reports
  the error. No telemetry: the only data sent is the request itself.
- Setting "Check for updates at start" (on by default).
