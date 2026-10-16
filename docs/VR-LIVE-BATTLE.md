# Live Battle menu scene (VR)

VR Options → **Menu Scene → Live Battle** (`XWA_XR_ENVIRONMENT=battle`; also
one of the scenes `random` picks). The menus float in a battle that goes on
around the screen, cast from the mission you flew last.

## What comes from the last mission

The mission flown last (this session's, else the one remembered in
`vr_last_mission.txt` in the user folder; `XWA_XR_BATTLE_MISSION` names one
instead) is read for:

- your craft, flying with a wingman (never shot down);
- up to three fighter types a side, by how many the mission has;
- up to two capital ships a side and one station, on that side's flank;
- the biggest hostile capital ship, burning;
- the mission's planets, nebulae and suns, the main world beside the screen,
  the suns over your shoulder lighting the craft in their colour.

Sides follow the mission's teams and their allegiances; neutral craft, mines,
buoys and containers stay out. Before any mission, rebels fight the Empire
over a random world.

## How it moves

`vr_env.c`, the "Live Battle" section: a light steering director, not the
flight engine. Fighters settle on a course over about half a second, turn
gently for small corrections and at most 23 to 46 degrees a second by size,
and bank by how hard they actually turn. They chase with lead and fire in
bursts when lined up, break off past their target in a long sweep, and jump
in from hyperspace on their side's flank when they rejoin. The fight spreads
over about 110 m to 1.2 km (mean about 390 m). Hits flash; shot-down craft
burst (Explosions.dat group 2005, animated) and return 4 to 9 s later.
About five fighters go down a minute. Capital ships, 2.6 km and more out,
creep forward and trade turbolaser fire about once a second each.

Seven asteroids (three of the game's AsteroidHR models, named in the craft
lists) tumble slowly 200 m to 1 km out, 25 to 160 m across; fighters steer
round them (jump-ins are aimed clear of them) and bolts strike them.

Comfort: you never move. Courses that would pass within 80 m of you are
turned away (closest seen in tests: about 50 m), jump-ins are aimed well
clear of you, and craft are pushed out of the cone the screen covers (they
spend about 15 % of the time there).

## Limits

- No sound.
- Craft do not collide; bolts only hit their targets' bounding spheres.
- The mission's region layout is not kept: backdrops are turned so the main
  world sits beside the screen.
