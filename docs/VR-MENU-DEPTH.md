# 3D depth for the 2D screens (VR)

On the headset the game's 2D screens (concourse, combat chamber, mission setup,
briefing, debrief, tech room, loading screens) are drawn once per eye with
depth: the room pictures sit behind the virtual screen, text and buttons stay
on it. VR Options → **Menu 3D Depth**: Off (flat), Subtle, Normal (default),
Strong. Config key `XWA_XR_MENU_DEPTH` (0, 0.5, 1, 1.6; any value up to 3).

## How

The HD frontend is rebuilt from the game's draw commands (`xwa_remaster/frontend.c`),
so every sprite, line and letter is known. With depth on, the reconstruction
runs twice per frontend tick into two sets of persistent targets, and each
command is drawn at the depth of its layer, shifted sideways per eye:

| layer | what | depth behind the screen (Normal) |
|-------|------|----------------------------------|
| UI    | text, buttons, panels, everything else | 0 (on the screen, so the pointer lines up) |
| map   | the briefing map (its viewport clip)   | 0.35 m |
| room  | full-screen pictures, room animations (doors, pods, concourse doors and Emkay, tech globe, medal case, Death Star briefing) | 1.5 m |
| far   | the concourse planet and stars         | 6 m |

The shift per eye is `ipd/2 · z/(D+z)` on a screen at distance `D`
(`XWA_XR_MENU_IPD` overrides the 64 mm eye distance). Layers behind the screen
are widened by twice their shift so full-screen pictures still cover both
edges.

Ship previews (mission setup, tech room) are rendered once per eye with the
camera moved half the eye distance at the scale the ship has on the screen,
then each image is moved back so the ship's centre sits on the screen plane:
the ship has real depth around it.

Aeron shows the two images through `AeronTextureLayerDesc.right_eye_texture`:
when a frame's layers differ per eye, the XR virtual screen is composed a
second time for the right eye into its own swapchain, and the frame submits
one screen quad per eye (`XR_EYE_VISIBILITY_LEFT` / `RIGHT`). The desktop
window shows the left eye.

## Limits

- Layers are flat cards; only the ship previews have real shape.
- Pictures not in the room list sit on the screen plane. Add their sprite
  names to `rm_draw_layer` (frontend.c) to move them back.
- The depth is chosen as a screen starts (its pictures are drawn once then)
  and kept for that screen: a new Menu 3D Depth setting shows from the next
  screen on.
- Cutscenes and flight are unchanged.
