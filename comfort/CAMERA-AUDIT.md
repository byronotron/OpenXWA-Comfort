# Camera audit and impact shake: 0.1.2 test build

## Available option

Open **VR Options → Comfort → Impact Camera Shake**:

| Value | Rendered impact pan |
| --- | --- |
| Off | Removes the captured artificial translation |
| Reduced (25%) | One quarter of the original strength |
| Original | Full upstream strength; default |

Version 0.1.2 makes Original the default for an unset/invalid setting, a fresh
installation and Restore VR Defaults. Existing saved choices remain selected.
The initial 0.1.1 test build used Reduced as its default; user feedback favored
keeping the original experience as the starting point.

Changes apply immediately and save through the existing VR settings mechanism.
They apply to an active stereo VR session, internal view of the player's own
ship, in normal flight. External cameras, maps, hangar, films, hyperspace and
flat presentation retain their existing behavior. Real head movement, ship
rotation/translation, collision physics, RNG, audio and haptics are unchanged.
No shaders or OpenXR compositor layers were added.

This option reduces the **translational impact effect**. It does not stabilize
the horizon, counter-rotate the world, reduce actual spacecraft motion, or
change hyperspace effects. Improved comfort has not yet been established by
headset testing.

## What the pivot audit found

1. `ComputeHardpointWorldPos` in `src/xwa/flight/flight_view.c` resolves the
   pilot's primary hardpoint or the active turret-seat hardpoint, including its
   mount/aim pivots, and rotates it into world space.
2. The internal own-ship branch of `FlightView_UpdatePlayerCamera` sets the
   camera anchor to the ship position **plus that seat hardpoint**, then adds
   `cameraPanDelta* >> 4`. It is already a seat-based camera; replacing it with
   the spacecraft center would move the viewer to the wrong place.
3. `fl_xr_eye_camera` in `src/xwa_remaster/flight.c` composes the current OpenXR
   eye orientation and position with that base camera. Head orientation changes
   the view basis; it does not rotate the base camera position around the ship
   center. No tracking smoothing or delay was found or added.
4. `fl_cockpit_model_matrix` accounts for the same seat hardpoint and pan. The
   rear turret (seat 2) also flips the cockpit about its hardpoint. Reducing
   only the world camera would leave an inconsistent cockpit offset, so both
   render paths receive the same shake scale.
5. The upstream renderer intentionally shifts the Sabra/YT-1300 cockpit to
   provide a 20 cm leftward pilot seat and scales Y-wing/A-wing cockpit models
   to 90% about the eye. Those static cockpit adjustments remain intact.

A seat offset physically rotates with the spacecraft hull; its world-space
trajectory around the ship center is expected. That differs from incorrectly
using the ship center as the head-tracking pivot. The source audit found no
evidence requiring a new global pivot correction. Per-craft headset validation
is still needed, especially for turret mounts and existing cockpit offsets.

## Artificial movement isolated

`collide_damagecraft`'s impact-feedback path in
`src/xwa/flight/object/collision.c` fills `cameraPanDeltaX/Y/Z`, using either
random or directional impact offsets. The object-update movement path in
`src/xwa/flight/object/object.c` decays those fields.
The existing angular camera-delta fields are reset/decayed, but this source
revision has no nonzero impulse writer for them. No angular filter was added.

Snapshot capture now observes the **exact signed integer pan shift** used by
the classic world camera. The renderer subtracts the unwanted portion from
its floating-point camera position before projection/eye composition. The
cockpit uses the scaled fractional pan in its existing model transform. This
preserves the classic integer/fractional distinction and makes Off remove the
cockpit translation as well as the world-camera impulse. Simulation fields and
snapshot inputs are never overwritten.

## Audit output

The log records the cockpit model, seat, selected shake scale, original impact
pan and the camera's residual from `ship + seat hardpoint + integer pan` once
per cockpit/seat/setting change. Integer truncation of the seat hardpoint can
leave a residual of less than one engine unit per axis; a large residual needs
investigation before any pivot correction is made.

For repeated sampling while testing pitch/yaw/roll, add
`XWA_COMFORT_PIVOT_AUDIT=1` to the Comfort app's `openxwa-quest.cfg` using the
app-owned import route, then restart. Samples are throttled to once every two
seconds. This is a diagnostic setting, not a camera adjustment.

## Completed checks

- Host tests compile the **actual production function bodies**, with the real
  Aeron camera/view structs. The tracked-eye composition and quaternion helpers
  are also checked against upstream source to ensure they are unchanged.
- 567 ship/head orientation combinations: head-only rotations do not move the
  anchor; supplied head translations and eye separation remain direct; poses
  are deterministic and input structs remain unchanged.
- 24 pilot/turret poses: Original cockpit transforms match upstream byte for
  byte; Off matches the upstream no-pan cockpit relative to the corrected eye.
- Policy checks cover defaults, Off/Reduced/Original, invalid/nonfinite values,
  clamping and excluded view modes. Signed integer pan contributions are tested.
- ARM64 build, APK signing, package/provider isolation, retained upstream
  payload hashes, 16 KiB ELF alignment, dependency presence and strong symbol
  resolution passed.
- Updated `org.openxwa.comfort` in place on the Quest. Its settings-file hash
  and the original `org.openxwa.quest` APK hash were unchanged by installation.

Run the focused tests:

```powershell
python comfort/test_camera.py --zig "$env:LOCALAPPDATA/OpenXWA-Comfort/toolchains/zig-windows-x86_64-0.13.0/zig.exe"
```

Build/package/verify (native prerequisites are described in `README.md`):

```powershell
python comfort/build_native.py --profile comfort-shake --ndk "$env:LOCALAPPDATA/OpenXWA-Comfort/toolchains/android-ndk-r27c" --zig "$env:LOCALAPPDATA/OpenXWA-Comfort/toolchains/zig-windows-x86_64-0.13.0/zig.exe"
python comfort/build_baseline.py --base-apk ../OpenXWA-Quest-1.0.1/OpenXWA-quest-1.0.1.apk --native-build build/comfort-native/packaged
python comfort/verify_native.py --apk build/artifacts/OpenXWA-Comfort-0.1.2-original-default.apk --ndk "$env:LOCALAPPDATA/OpenXWA-Comfort/toolchains/android-ndk-r27c"
```

The baseline profile still rejects modified source. The `comfort-shake` profile
records changed source-file hashes and marks the APK as a comfort test build.
Both profiles require unchanged shader inputs, upstream libraries and Aeron.

## Remaining headset checks

Confirm the Comfort menu and persistence across restart, then compare the same
hit/collision in Off, Reduced and Original modes. Check X-wing, Y-wing,
Corellian transport and both turret seats while turning and moving the head.
Capture the residual logs, verify targeting and cockpit controls, and exercise
save/load and mission transitions. None of those flight checks are claimed
complete by the desktop tests. Measure sustained frame pacing separately.
