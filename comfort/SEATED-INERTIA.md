# Seated pilot inertia: 0.4.0

**VR Options → Comfort → Pilot Inertia (experimental)** now moves the viewpoint
relative to the cockpit. It is Off on first upgrade as well as fresh installs.
Low / Medium / High retain total displacement caps of 1 / 2 / 3.5 cm.
Pilot-Centered Rotation remains a separate setting. Mask presets are unchanged.

The 0.3.1 exterior-only prototype was active in headset logs but imperceptible
to the user. This revision separates the simulated pilot from the cockpit rig.
It is inspired by the eye/body displacement described in Motive's Squadrons
development article; it does not add a pilot-body mesh, arm IK or camera roll.
Subjective benefit still needs testing.

## Camera and cockpit

Let P be the normal base camera plus any pilot-centering correction, and I be
the bounded inertia translation in world units:

```
cockpit rig origin = P
simulated eye base = P + I
rendered eye       = P + I + current tracked eye translation
```

The cockpit mesh, static seat shifts, cockpit scaling and virtual stick use
the rig origin. World projection and both eye cameras use the simulated eye
base. This leaves a real relative displacement of -I in the visible cockpit.
The existing tracked-eye composition function and tracked orientation are
unchanged. Head tracking is never smoothed or delayed by the inertia filter.

The acceleration detector and smoothing retain the previous validated policy:
ship-origin positions, simulation-clock differentiation across velocity windows
of at least 80 ms, 120 ms acceleration filtering, 0.002 s² acceleration gain,
180 ms displacement attack and 450 ms release. Caps apply to total vector
length, not each axis independently. Constant-speed arrival/position noise,
held simulation, clock discontinuities and teleports have regression tests.
Only ship translational acceleration drives it; a stationary ship rolling in
place does not create inertia. Natural head movement does not drive it.

## A common coordinate frame for interactions

`vr_pilot_space.c` holds the current simulated body translation D in cockpit
LOCAL axes (meters). It is updated on the render thread. Runtime tracking
records are never overwritten:

```
tracked hand in cockpit coordinates = raw tracked hand + D
tracked eye in cockpit coordinates  = raw tracked eye + D
cockpit panel for compositor         = cockpit panel - D
```

Aim and valid grip positions are translated in copies; orientations, buttons,
triggers and grip values are untouched. Runtime controller model root poses
receive the same translation; relative animation-node poses do not. Gloves and
controllers are drawn from the rig origin using these cockpit-space poses,
so their visible placement still follows the real controllers immediately.

Cockpit HUD panel grabs, console/card interaction, pointer rays and labels use
the same cockpit coordinates. Presentation copies of the panels and pointer
beams are translated back to tracking coordinates. Saved panel placements stay
in cockpit coordinates; transient presentation offsets are not saved. World
targeting and helmet HUD quads retain viewer coordinates for their existing
world-projection/head-follow behavior. In the legacy fixed HUD layout, the
single HUD sheet retains that existing viewer-relative projection as well.

Hand-overlay meshes and eye copies are both evaluated in cockpit coordinates.
Cockpit-fixed layers are cached in cockpit coordinates; viewer-fixed layers
stay in tracking coordinates and are converted using the current body offset.
This prevents a changing offset from dragging static panels in the hand masks.
Cockpit HUD occlusion uses the rig origin and the matching eye/quad coordinates.
No new compositor layer or shader is added.

The stick's grab and lever hit tests use cockpit-space hands. Its deflection
and placement calculations subtract the *change* in simulated body translation
since the grab. Inertia alone therefore cannot steer the craft, walk the stick
around the cockpit or alter its saved placement. Real controller movement is
still effective. Hands are submitted before stick hit tests so the palm
capsules describe the current frame. No arm/hand IK is added to hold the visual
fingers precisely against a stationary stick under a body displacement.

## Defaults and resets

The new setting is `XWA_COMFORT_SEAT_INERTIA`, default `0`. The old
`XWA_COMFORT_PILOT_INERTIA` exterior-only key is ignored: its saved value must
not silently activate the new relative-camera effect on upgrade.
Existing mask, shake and pilot-centering selections are preserved.

The existing eligibility/reset policy remains: focused stereo own-ship flight,
valid visible cockpit and anchor; no menus, external view, films, maps,
hangars or hyperspace. Entering menu environments and shutdown also clear the
shared pilot-space offset. Off produces zero inertia displacement and identity
pose/panel adapters. Flight physics, weapon origins and collision behavior are
unchanged. Pilot centering retains its separately documented render-only scope.

## Checks and headset acceptance

- `test_pilot_motion.py`: policy plus extracted production camera functions;
  includes 75 relative eye/cockpit configurations, stereo tracking invariants,
  Off byte identity and retirement of the old setting key.
- `test_seated_inertia.py`: actual pose-adapter module and extracted production
  virtual-stick code; copied aim/grip/runtime model poses, unchanged rotations
  and button values, matching stereo eye/hand/panel rays, zero-offset identity,
  invalid tracking, and 100 changing body offsets with a held stick and during
  placement. Real hand movement remains effective; placement does not drift.
- Existing camera and peripheral-mask suites pass.
- APK signature, isolated package/provider, 124 unchanged retained upstream
  payloads, ARM64 ELF ABI, 16 KiB alignment, dependencies and strong symbols pass.

Use build profile `pilot-motion`. APK:
`build/artifacts/OpenXWA-Comfort-0.4.0-seated-inertia.apk`, versionCode 13.
Detailed evidence is in `SEATED-INERTIA-VALIDATION.json`.

Start with Low and change throttle or make a gentle moving turn while watching
the dashboard/canopy frame. Check that the viewpoint shifts relative to it and
returns to neutral, the two eyes agree, and physical head movement stays direct.
Then check cockpit instrument placement, controller models/gloves, virtual-stick
grabs, console pointing/touch, panel grabs, labels, and mask combinations.
Compare Medium/High only after Low behaves correctly. Headset appearance,
interaction acceptance, enabled frame cost and comfort remain pending testing.
