#ifndef XWA_REMASTER_VR_CONSOLE_PROTOCOL_H
#define XWA_REMASTER_VR_CONSOLE_PROTOCOL_H

/*
 * Data shared between the VR lap console (xwa_remaster, which draws it and
 * handles the hands) and the game side (xwa_runtime, which knows the ship
 * and turns console actions into the game's own key actions). Plain data,
 * no dependencies; the host passes it across each frame.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XwaVrConsoleAction {
	XWA_VRC_NONE = 0,
	/* Energy (arg: level 0 off .. 4 full, or balance 0 front, 1 even, 2 rear) */
	XWA_VRC_LASER_LEVEL,
	XWA_VRC_SHIELD_LEVEL,
	XWA_VRC_BEAM_LEVEL,
	XWA_VRC_SHIELD_BALANCE,
	XWA_VRC_LASERS_TO_SHIELDS,
	XWA_VRC_SHIELDS_TO_LASERS,
	XWA_VRC_PRESET_RECALL, /* arg 0/1 */
	XWA_VRC_PRESET_STORE,
	/* Targeting */
	XWA_VRC_TARGET_NEAREST_ENEMY,
	XWA_VRC_TARGET_ATTACKER,
	XWA_VRC_MATCH_SPEED,
	XWA_VRC_TARGET_FRIENDLY,
	XWA_VRC_TARGET_NEUTRAL,
	XWA_VRC_TARGET_ENEMY,
	XWA_VRC_SUBSYSTEM_PREV,
	XWA_VRC_SUBSYSTEM_NEXT,
	XWA_VRC_MEMORY_RECALL, /* arg 0..2 */
	XWA_VRC_MEMORY_STORE,
	XWA_VRC_TARGET_NEWEST,
	XWA_VRC_TARGET_OBJECTIVE,
	XWA_VRC_TARGET_CLEAR,
	/* Weapons and ship */
	XWA_VRC_FIRE_LINK,
	XWA_VRC_CONVERGENCE,
	XWA_VRC_SFOILS,
	XWA_VRC_MAP,
	XWA_VRC_DOCK,
	XWA_VRC_PICKUP,
	XWA_VRC_RELEASE,
	XWA_VRC_GUNNER,
	XWA_VRC_AUTO_GUNNER,
	XWA_VRC_COUNTERMEASURE,
	XWA_VRC_EJECT,
	/* Wingmen */
	XWA_VRC_WING_ATTACK,
	XWA_VRC_WING_COVER,
	XWA_VRC_WING_IGNORE,
	XWA_VRC_WING_EVADE,
	XWA_VRC_WING_WAIT,
	XWA_VRC_WING_GO_AHEAD,
	XWA_VRC_WING_HOME,
	XWA_VRC_WING_REPORT,
	XWA_VRC_WING_RESUPPLY,
	XWA_VRC_WING_REINFORCE,
	XWA_VRC_COUNT
} XwaVrConsoleAction;

/* Live ship state the console shows. */
typedef struct XwaVrConsoleState {
	int valid; /* flying a craft (else the console is hidden) */
	int laser_level, shield_level, beam_level; /* 0 off .. 4 full */
	int has_shields, has_beam, has_sfoils;
	int shield_balance; /* 0 front, 1 even, 2 rear */
	int sfoils_closed;
	int has_target;
	int memory_set[3];
	/* Changes when a new mission starts (the button card returns to its
	 * spot on the right). */
	unsigned int mission_serial;
} XwaVrConsoleState;

#ifdef __cplusplus
}
#endif

#endif
