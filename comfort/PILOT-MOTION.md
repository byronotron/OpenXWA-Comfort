# Pilot motion experiments: 0.3.1

This document records the first, exterior-only inertia prototype. The current
relative eye/cockpit revision is described in [SEATED-INERTIA.md](SEATED-INERTIA.md).
The pilot-centering behavior below remains applicable.

Both new options are in **VR Options → Comfort**, default to **Off**, and save
immediately. Existing mask strength/sensitivity and impact shake choices are
preserved. The accepted 0.2.5 mask presets are unchanged.

## Pilot-Centered Rotation (experimental): Off / On

On entering an eligible flight, capture the current seat's world-space offset
from the player's ship origin. Hold that offset fixed while the ship rotates:

```
original viewpoint = ship position + rotating seat offset + impact pan
centered viewpoint = ship position + captured seat offset + impact pan
render correction = captured seat offset - current seat offset
```

The correction uses the exact integer camera contribution (camera minus ship
minus captured impact pan), avoiding fractional hardpoint rounding jitter.
It is computed directly each frame, not accumulated, so rotations do not
introduce integration drift. Ship translation remains intact. The cockpit rig
receives the same translation as the base camera, preserving the seated view;
tracked eyes are composed afterward with the existing unmodified function.

This is a **render-only approximation**, not Squadrons' ship-physics pivot
configuration. It changes the exterior viewpoint while leaving the simulation,
collisions, weapon origins and ship trajectories unchanged. Near-object parallax,
aiming alignment, capital-ship flybys, long cockpits and turret seats need real
headset evaluation. The view still rotates with the ship; this is not horizon
stabilization. Changing the option or re-entering a view re-primes its reference.

## Pilot Inertia (experimental): Off / Low / Medium / High

| Setting | Maximum displacement, total vector length |
| --- | --- |
| Off | 0 |
| Low | 1 cm |
| Medium | 2 cm |
| High | 3.5 cm |

The first prototype offsets the **exterior viewpoint**, retaining a stable
cockpit and controller reference. It does not animate a pilot body or shift
the eyes relative to the cockpit. It therefore differs from Squadrons' full
pilot/body inertia system; its comfort benefit is unproven.

Acceleration is estimated from ship-origin positions across velocity windows
of at least 80 ms, only when the simulation advances. Position differences are
computed in double precision before conversion to meters. Velocity and acceleration
use simulation timestamps, so snapshot-arrival jitter does not create false
g-forces. Displacement smoothing uses host time. An exponential
120 ms acceleration filter suppresses integer-position noise. The target
displacement opposes acceleration with a gain of 0.002 seconds squared, then
is capped by the selected total-vector limit. Displacement uses 180 ms attack
and 450 ms release time constants. There is no artificial camera rotation.

Turns that accelerate the ship's translational velocity can activate it;
rotation in place with a stationary ship origin does not. Head pose, impact
camera shake and turret aim do not drive the acceleration detector. A held
simulation releases the offset after 100 ms. These filters affect only the
simulated offset, never tracked head position or orientation.

## Integration and reset behavior

Effects require a focused stereo session, an eligible internal own-ship view,
a valid visible cockpit and a valid anchor. Menus, external views, film, maps,
hangars, hyperspace and the hyperspace preview reset them. Changes of ship,
seat, region, centering mode or world scale re-prime them; clock discontinuities,
long host gaps and position jumps over 250 m also clear history. Off returns
the original viewpoint; switching/re-priming can change the exterior parallax.

The translated base view is used for both eye cameras and HUD world projection,
including reduced-resolution HUD targets. Cockpit geometry, hands and the stick
use that same base, so LOCAL-space compositor controls retain their alignment.
Overlay sprites reconstructed from the classic view subtract the comfort
translation to retain their original world positions. Sky remains camera-centered.
No shader, OpenXR layer, tracking-function or flight-physics change is required.

## Validation

`comfort/test_pilot_motion.py --zig <zig.exe>` exercises the production policy
and extracts the actual integration/camera functions into a host harness. It
checks three-axis long-seat rotation, large world coordinates, immutable inputs,
bounded opposite-acceleration offsets, diagonal caps, constant-speed quantization
stability, pause/teleport/reset handling, runtime settings and excluded views.
It also checks Off byte identity, 75 cockpit/turret rig configurations and 150
tracked-eye poses with unchanged orientation, stereo separation and input poses.
The projection rebuild is recorded by a host stub; headset visual projection
still requires testing. Existing camera and mask suites also pass.

Build profile: `--profile pilot-motion`. APK:
`build/artifacts/OpenXWA-Comfort-0.3.1-pilot-motion.apk`, versionCode 12.
Signature/package isolation, 124 retained payload hashes, ARM64 ABI, 16 KiB
alignment, dependency presence and strong symbol checks passed. See
`PILOT-MOTION-VALIDATION.json` for installation and manual-test status.

Start with the accepted mask settings, enable **only Pilot-Centered Rotation**,
and compare gentle turns in a long ship. Check targeting and close geometry.
Then test **only Low inertia**, using throttle changes and gentle turns. Test
the combination only after each option behaves acceptably on its own. Comfort
and mission coverage remain pending individual headset testing.

## First individual playtest: 2026-10-27

The user tried both options and reported being unable to tell the difference.
Logs confirmed centering On with several-meter corrections during turns, then
Low inertia (1 cm) reaching near its displacement cap with centering still On.
The mask was saved Off at the time of the report. This establishes activation,
but no perceptible effect or comfort improvement for this user.

Exterior-only displacements have limited visual leverage against distant space
scenery. In particular, the current inertia prototype does not supply relative
eye-to-cockpit movement. A more faithful experiment would need to separate the
seat rig and simulated eye translation, with corresponding compositor HUD,
control, hand-overlay and interaction alignment work. Increasing the exterior
offset alone is not a validated substitute. Both options remain experimental
and Off by default.
