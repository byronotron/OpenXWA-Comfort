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
flight engine. Fighters turn at a rate set by their size and bank into the
turn, chase with lead and fire in bursts when lined up, break off past their
target, and jump in from hyperspace on their side's flank when they rejoin.
Hits flash; shot-down craft burst (Explosions.dat group 2005, animated) and
return 4 to 9 s later. Capital ships creep forward and trade turbolaser
fire. About six fighters go down a minute.

Comfort: you never move. Courses that would pass within 60 m of you are
turned away (closest seen in tests: about 40 m), jump-ins are aimed well
clear of you, and craft are pushed out of the cone the screen covers (they
spend about 15 % of the time there).

## Limits

- No sound.
- Craft do not collide; bolts only hit their targets' bounding spheres.
- The mission's region layout is not kept: backdrops are turned so the main
  world sits beside the screen.
