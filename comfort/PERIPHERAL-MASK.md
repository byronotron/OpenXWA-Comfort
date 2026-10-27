# Adaptive peripheral mask: 0.2.0

In **VR Options → Comfort**, Peripheral Mask defaults to **Off**. Switch it On
to adjust Mask Strength and Mask Sensitivity independently. Changes save through
the existing configuration system and take effect without restarting. Impact
Camera Shake still defaults to Original.

| Control | Low | Medium (initial value) | High |
| --- | --- | --- | --- |
| Strength: maximum outer-edge darkness | 35% | 65% | 100% |
| Sensitivity: rotation-rate multiplier | 0.65 | 1.0 | 1.5 |

Strength controls opacity, not aperture size. The center remains clear within
35 degrees of head-forward (a 70-degree cone); a smooth feather reaches full
mask coverage at 55 degrees. At Medium sensitivity, activation begins at
35 degrees/second and reaches full at 100 degrees/second. Higher sensitivity
responds to slower ship turns. Opacity follows an exponential fade with a
0.12-second attack and 0.45-second release time constant.

## Motion and rendering

The detector compares normalized quaternions from the player's ship-body
rotation matrix. Yaw, pitch and roll contribute equally. Head tracking,
turret aim, camera shake and translational speed do not enter the detector.
Sampling follows simulation advances using elapsed host time. Held simulation
releases after 100 ms; focus loss, excluded views, ship/seat changes, clock
discontinuities and large pose jumps reset the detector. Menu environments,
external views, film, map, hangar and hyperspace do not activate it.

Both eyes use the same opacity and angular bounds, accounting for asymmetric
eye FOV and relative eye cant. Head rotation cancels from the mask alignment.
Two cached 128×128 alpha textures are regenerated only when those view
parameters change. The final resolved eye images receive one RGB-only LOAD
pass each when opacity is visible. Destination alpha is preserved, including
the existing hand-overlay coverage. HUD, console and control compositor layers
remain visible. The first implementation can also dim cockpit edges and hands;
it does not separate the outside world from cockpit geometry.

Off allocates, uploads and draws nothing. Enabling prewarms the resources, even
while stationary. Rendering reuses pinned upstream blit SPIR-V and adds no
OpenXR layer. Enabled performance and subjective benefit need headset testing.

## Verification and reproduction

`comfort/test_mask.py --zig <zig.exe>` compiles the production detector, GPU
module and extracted ship-basis functions against a recording GPU backend.
It covers all rotation axes, cached/Euler matrix equivalence, quaternion sign
and wrap, 600 head-only poses, sensitivity, pause/discontinuity resets, stereo
angles, cached uploads, RGB-only blending and Off doing zero GPU work. Upload
failure skips both eyes. The existing camera suite also passes (567 eye poses,
24 cockpit/turret poses, Original matching upstream).

Build with the existing instructions, selecting `--profile adaptive-mask` for
`comfort/build_native.py`, then package with `comfort/build_baseline.py` and
validate with `comfort/verify_native.py`. The APK is
`build/artifacts/OpenXWA-Comfort-0.2.0-adaptive-mask.apk` (versionCode 5).
Native validation checks ARM64 ABI, 16 KiB alignment and strong symbol resolution;
packaging checks signature, package isolation and 124 unchanged upstream payloads.
These tests do not establish visual alignment, frame cost or comfort in flight.

For headset acceptance: confirm Off initially; enable Medium/Medium; turn the
ship separately in yaw, pitch and roll; then stop and turn only your head.
Verify a smooth fade on ship turns, a clear center, matching eyes, readable HUD,
release at rest, and no mask in menus or other excluded views. Compare all
strength/sensitivity levels and verify Off immediately removes it. Stop the
test if uncomfortable. Results are pending; see `MASK-VALIDATION.json`.
