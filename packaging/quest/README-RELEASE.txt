OpenXWA VR @VERSION@ - X-Wing Alliance in standalone VR
=========================================================

Play your own copy of Star Wars: X-Wing Alliance on Meta Quest 3 / 3S.
Pico and Steam Frame are supported but not tested yet.

You need your own installed X-Wing Alliance (Steam, GOG or the original
CD version) on a Windows PC. No game files are included in this download.


Before you start (once)
-----------------------
1. Turn on Developer Mode for the headset:
   Meta Horizon phone app -> Devices -> your headset -> Headset settings ->
   Developer Mode (needs a free Meta developer account).
2. Have a USB-C cable that carries data (not a charge-only cable).


Install (and update)
--------------------
1. Unzip this folder anywhere on the PC.
2. Connect the headset to the PC with the USB cable.
3. Double-click OpenXWA-Launcher.exe and follow the steps in the window.
   It finds your X-Wing Alliance, installs the app on the headset, copies
   the game files (about 1 GB, a few minutes), checks every file arrived
   and asks whether to bring your PC pilots along.
   Put on the headset when it asks you to allow USB debugging (tick
   "Always allow from this computer").
4. Put on the headset: Library -> filter "Unknown sources" -> OpenXWA.

Keep the headset on your head when the launcher asks you to: OpenXWA has
to start once on the headset to create its own folder.

Steam Frame: turn on Developer Mode, start "Lepton Development" from your
Steam Library and keep it running; the launcher then connects to it over
the USB cable (or Wi-Fi). OpenXWA appears in the Lepton window's Android
home screen. Not tested on a Steam Frame yet.

Windows may say "Windows protected your PC": the launcher is not
code-signed yet. Click "More info", then "Run anyway".

The launcher needs Google's adb tool. If it can't find one (SideQuest and
Meta Quest Developer Hub bring their own), it offers to download it from
Google; you accept Google's licence terms yourself.

Updating: download the new release and run its launcher. It updates the
app and copies only missing or changed files; your pilots and settings
stay. The launcher also tells you when a new version is out.

If the game was found somewhere unusual: drag the X-Wing Alliance folder
onto OpenXWA-Launcher.exe, or paste its path when the launcher asks.


Controls
--------
  Right stick: pitch / yaw          Left stick: roll (X), throttle (Y)
  Right trigger: fire               Right stick click: target in sights
  Right grip: countermeasures       A / B: next / previous target
  X: target closest enemy           Y: target my attacker
  Left trigger: change weapon       Left stick click: link weapons
  Left grip (hold): radial menu     Left menu button: game menu (Esc)
  In menus the right controller is a laser pointer.
  Taking the headset off or opening the Meta menu pauses the game.
  Side panel on your right: tactical map, voice commands and the button
  mapping; touch a tab (or point and pull the trigger). Touch the map to
  zoom.
  VR settings and help: game menu -> VR Options.


Problems?
---------
Run the launcher and choose "collect logs" at the end (or start it with
--logs): it copies the game's log files into a folder next to the
launcher. Please send those along with your report.

Uninstalling the app (Settings or adb uninstall) also deletes the copied
game files and your pilots on the headset.


Manual install without the launcher
-----------------------------------
  adb install -r OpenXWA-quest-@VERSION@.apk
  (start the app once, then)
  adb push "<your X-Wing Alliance folder>\." /sdcard/Android/data/org.openxwa.quest/files/XWA
RESDATA.TXT must end up directly in .../files/XWA. Large copies with adb
push can silently skip files, which is why the launcher exists.


Licence
-------
OpenXWA and this VR port are free software under the GNU GPL version 3
(LICENSE.txt). Source: https://github.com/neitey/OpenXWA-Quest
Third-party components and their licences: THIRD-PARTY-NOTICES.txt.
Star Wars and X-Wing Alliance are trademarks of their owners; this is an
unofficial fan project and contains no game data.
