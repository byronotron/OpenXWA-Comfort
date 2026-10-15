# 3D ship previews (VR)

On the headset the rotating ship previews on the game's 2D screens (mission
setup, tech room) are shown in stereo: each eye gets its own camera on the
ship, so it has real depth. The rest of the 2D screens stays flat on the
virtual screen. VR Options → **3D Ship Previews**: Off (flat), Subtle, Normal
(default), Strong. Config key `XWA_XR_SHIP_3D` (0, 0.5, 1, 1.6; any value up
to 3).

## How

The HD frontend is rebuilt from the game's draw commands
(`xwa_remaster/frontend.c`). While a screen shows a 3D preview, the
reconstruction runs twice per frontend tick, once per eye, into two sets of
persistent targets. The right eye's set starts as a copy of the left one (the
2D screen is the same for both); only the previews differ.

Each preview is rendered once per eye with the camera moved half the eye
distance sideways at the scale the ship has on the screen, then each image is
moved back so the ship's centre sits on the screen plane, clipped to its
window. The shift per eye is `ipd/2` at that scale (`XWA_XR_SHIP_3D_IPD`
overrides the 64 mm eye distance).

Stereo ends 90 frontend ticks after the last 3D preview, so a tick without a
preview record does not flatten the ship.

Aeron shows the two images through `AeronTextureLayerDesc.right_eye_texture`:
when a frame's layers differ per eye, the XR virtual screen is composed a
second time for the right eye into its own swapchain, and the frame submits
one screen quad per eye (`XR_EYE_VISIBILITY_LEFT` / `RIGHT`). The desktop
window shows the left eye.

## Limits

- Wireframe previews (briefing hologram) stay flat.
- Cutscenes and flight are unchanged.
