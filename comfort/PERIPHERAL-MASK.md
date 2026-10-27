# Adaptive peripheral mask: 0.2.5

In **VR Options → Comfort**, Peripheral Mask defaults to **Off**. Switch it On
to adjust Mask Strength and Mask Sensitivity independently. Changes save through
the existing configuration system and take effect without restarting. Impact
Camera Shake still defaults to Original.

| Control | Low | Medium (initial value) | High |
| --- | --- | --- | --- |
| Strength: maximum black blend (linear RGB) | 65% | 100% | 100% |
| Clear central cone (total angle) | 40° | 40° | 26.7° |
| Feather reaches full coverage (angle from center) | 35° | 35° | 23.3° |
| Sensitivity: rotation-rate multiplier | 0.65 | 1.0 | 1.5 |

Low is the previous Medium; Medium is exactly the previous High. High increases
coverage by narrowing the aperture 1.5×, while keeping opacity capped at 100%.
At Low/Medium the center remains clear within 20 degrees of head-forward; at
High it remains clear within 13.3 degrees. The mask feathers smoothly outward.
At Medium sensitivity, activation begins at
5 degrees/second and reaches full at 55 degrees/second. Higher sensitivity
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
parameters or the selected aperture change. The final resolved eye images receive one RGB-only LOAD
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
`build/artifacts/OpenXWA-Comfort-0.2.5-mask-strength.apk` (versionCode 10).
Native validation checks ARM64 ABI, 16 KiB alignment and strong symbol resolution;
packaging checks signature, package isolation and 124 unchanged upstream payloads.
These tests do not establish visual alignment, frame cost or comfort in flight.

For headset acceptance: confirm Off initially; enable Medium/Medium; turn the
ship separately in yaw, pitch and roll; then stop and turn only your head.
Verify a smooth fade on ship turns, a clear center, matching eyes, readable HUD,
release at rest, and no mask in menus or other excluded views. Compare all
strength/sensitivity levels and verify Off immediately removes it. Stop the
test if uncomfortable. Results are pending; see `MASK-VALIDATION.json`.

## Visibility correction after headset testing

The user could see the controls but could not perceive the 0.2.0 mask, even at
High strength and sensitivity. Diagnostic builds 0.2.1–0.2.3 established that
settings reached the renderer, ship turns produced activation, textures were
ready, and draw calls executed. A real Quest GPU test at 80% blend read back
darkened outer pixels in both eyes (124/139 RGB versus 255 at the center), with
destination alpha preserved at 64. A captured flight frame showed 1,268,480
changed pixels and a maximum summed RGB difference of 170 across the mask pass.
The before/after captures showed that the old wide 35–55-degree feather mostly
dimmed the far periphery and was barely apparent in dark space.

0.2.4 moves the feather inward to 20–35 degrees and lowers the response band
from 35–100 to 5–55 degrees/second at Medium sensitivity. High sensitivity now
reaches full activation at about 37 degrees/second, matching ordinary turns
observed in the flight logs. Strength values and Off default remain unchanged.
A regression test checks sustained 40-degree/second turns at every sensitivity.
Temporary GPU test/capture code is removed from this build. Visual acceptance
of 0.2.4 was confirmed by the user: the mask is visible and fades away at rest.

The user requested that its High become Medium and the new High be stronger.
0.2.5 uses strength values 0.65 / 1 / 1.5 (initial Medium = 1). Existing saved
value 1 therefore becomes Medium without changing its effect. High changes the
angular profile, not sensitivity or fade timing. Tests compare Medium with the
previous angular profile, check increased coverage at High, verify opacity never
exceeds one, and check both texture-cache transitions.

On 2026-10-27 the Quest 3 user confirmed that High is strong enough and Medium
feels like the previous High, and asked to retain these presets. They reported
being able to play and score several kills without feeling instantly sick.
This is encouraging individual playtest feedback; duration and objective
comfort measurements were not recorded. The 0.2.5 presets are the accepted
baseline for subsequent comfort work. Off remains the default.
