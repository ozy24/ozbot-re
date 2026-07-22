/*
ozbot - self-learning q2dm1 bot

bot.h -- shared declarations for the bot subsystem.

NOTE: g_local.h has no include guard, so this header must NOT include it.
Every .c file that includes bot.h must include "g_local.h" first.
*/

#ifndef OZBOT_BOT_H
#define OZBOT_BOT_H

#define BOT_MAX_PATH	128

// 40Hz adaptation (Phase R3): per-tick dynamics were tuned at 10Hz ticks.
// BOT_TICK_RATIO converts per-tick constants to the actual tick rate --
// rates/caps/probabilities/steps multiply by it; exponential-approach gains
// ("fraction of remaining delta per tick") go through Bot_TickGain so the
// per-second convergence stays what it was tuned to be.  Both are exact
// no-ops at 10Hz.
#define BOT_TICK_RATIO	((float)(FRAMETIME * 10.0))
float Bot_TickGain (float gain10);

// base per-item avoidance after a bot abandons it (bot_main.c goal exits and
// bot_goal.c's escalating failure blacklist both build on this)
#define BOT_ITEM_COOLDOWN	10.0f

// bot_goalbudget constants (see the comment block in bot_main.c); shared so
// Goal_Select's bot_goalnode 2 fundability filter prices candidates with the
// exact budget formula the giveup clock uses
#define BOT_GOAL_BUDGET_BASE	6.0f	// seconds of slack regardless of route
#define BOT_GOAL_BUDGET_SPEED	100.0f	// effective travel speed (cost units/sec)
#define BOT_GOAL_BUDGET_MAX		20.0f	// cap fallback if bot_budgetcap <= 0

// bot behavior mode
#define BOT_MODE_EXPLORE	0	// wander, growing the nav graph
#define BOT_MODE_GOAL		1	// follow an A* path to a goal node

// lift controller states (bot_lift; see plans/lift-riding.md).  A lift ride
// needs deliberate waiting -- these states own the movement intent and keep
// the stuck/replan/budget machinery from punishing the stillness.
#define LIFT_NONE		0
#define LIFT_WAIT		1	// plat is away: hold clear of the footprint
#define LIFT_BOARD		2	// plat at bottom: walk onto it
#define LIFT_RIDE		3	// standing on the plat: let it carry us
#define LIFT_FAILED		4	// timed out this attempt: normal systems resume

// func_train riding (bot_train; bot_move.c).  A train shuttles horizontally
// between path_corners, so unlike the plat the bot boards only when the train
// is AT the near end and steps off at the far end -- otherwise it walks into
// the void where the endpoint node floats.  Mirrors LIFT_* / owns the intent.
#define TRAIN_NONE		0
#define TRAIN_WAIT		1	// train is away: hold on the ledge, don't advance onto air
#define TRAIN_BOARD		2	// train is at the near end: step onto it
#define TRAIN_RIDE		3	// standing on the train: let it carry us to the far end
#define TRAIN_FAILED	4	// timed out this attempt: normal systems resume

// playback controller states (bot_playbook; bot_playback.c).  A playbook
// entry replays a recorded input sequence; ALIGN gets the bot onto the
// recorded anchor first, FAILED sticks for the goal attempt like LIFT_FAILED.
#define PB_NONE			0
#define PB_ALIGN		1	// steering onto the anchor preconditions
#define PB_REPLAY		2	// feeding recorded usercmds, drift-monitored
#define PB_FAILED		3	// aborted this attempt: normal systems resume

// strafe-jump controller states (bot_strafejump).  Chained bunny hops with a
// forward+side wishdir and a per-tick yaw sweep build speed past the 300 run
// cap on long clear runways -- calibrated from a human input capture (see
// plans/wiggly-knitting-token.md / the Phase 19 memory).
#define SJ_NONE			0
#define SJ_ACTIVE		1

// per-bot runtime state, kept in a registry indexed by client slot
// (0-based: registry index i corresponds to edict g_edicts[i+1]).
typedef struct
{
	qboolean	inuse;			// this slot holds an active bot
	edict_t		*ent;			// the bot's edict
	int			id;				// stable bot id for logging/naming
	float		skill_ovr;		// per-bot skill override for bot_skilltest
								// head-to-head A/B; < 0 = use bot_skill global

	// movement intent (world space), decoupled from aim/facing so the bot can
	// move toward a goal while looking/shooting elsewhere
	vec3_t		move_dir;		// desired horizontal move direction (unit-ish)
	float		move_yaw;		// facing yaw to use when not in combat
	qboolean	want_jump;

	// wander steering (explore mode)
	float		desired_yaw;	// absolute yaw the bot is trying to face
	float		next_wander_time;	// level.time at which to pick a new yaw
	float		last_repick_time;	// throttle for stuck-triggered re-picks

	// navigation / path following
	int			mode;			// BOT_MODE_*
	int			cur_node;		// nav node the bot is currently at (-1 = none)
	int			prev_node;		// last node for link learning
	int			goal_node;		// destination node (-1 = none)
	edict_t		*goal_item;		// item entity we're heading for (NULL = roam)
	edict_t		*steer_item;	// bot_decisive: sticky Goal_NearestItem target
								// (explore steering hysteresis between goals)
	qboolean	goal_timing;	// pre-positioning for an item about to respawn
	int			path[BOT_MAX_PATH];
	int			path_len;
	int			path_idx;		// index of the next node to reach
	float		replan_time;	// next time we may replan
	float		goal_time;		// level.time the current goal was set
	float		goal_cost;		// A* g-cost of the committed route (0 = unknown)
	float		goal_best;		// closest we've come to the goal node this attempt
	int			pending_link;	// link type to record on next learn step
	// mid-attempt reroute (bot_reroutemid): watch path_idx for a pure-nav stall
	int			reroute_last_idx;	// path_idx at the last observed advance
	float		reroute_idx_time;	// level.time of that advance
	qboolean	reroute_fired;		// already penalized+repathed once this attempt

	// idle fidget (bot_fidget -- humanization)
	float		fidget_until;	// next fidget re-roll
	float		fidget_yaw;		// direction of the current micro-step
	float		fidget_mag;		// intent magnitude (0 = standing beat)

	// stuck / progress tracking
	vec3_t		last_pos;
	float		progress_time;	// last time we made progress
	qboolean	was_onground;
	qboolean	did_jump;		// issued a jump; used to learn jump links

	// lift riding (bot_lift)
	int			lift_state;		// LIFT_*
	edict_t		*lift_plat;		// the func_plat being waited for / ridden
	float		lift_deadline;	// level.time cap on the current lift state
	vec3_t		lift_move_pos;	// BOARD progress: last position...
	float		lift_move_time;	// ...and when we were there (geometry-block
								// detector -- boarding should never stall)

	// func_train riding (bot_train)
	int			train_state;	// TRAIN_*
	edict_t		*train_ent;		// the func_train being waited for / ridden
	float		train_deadline;	// level.time cap on the current train state

	// ladder climbing (bot_ladder)
	qboolean	on_ladder;		// the ladder controller owns movement this frame
	qboolean	slime_escaping;	// bot_slimeescape: submerged in slime, driving to safety

	// playback (bot_playbook -- recorded maneuvers, bot_playback.c)
	int			pb_state;		// PB_*
	int			pb_entry;		// index into the loaded playbook entries
	int			pb_tick;		// replay cursor
	int			pb_frame;		// frames since engage (time clock for the cursor)
	int			pb_hiwater;		// furthest tick ever matched (progress watchdog)
	float		pb_deadline;	// ALIGN timeout / replay progress deadline
	float		pb_dwell_start;	// time the bot first satisfied a dwell-gated
								// anchor (0 = not currently on it); rescue-only
								// entries engage only after sitting >= dwell
	short		pb_cmd_fwd;		// exact usercmd for this replay frame
	short		pb_cmd_side;	//   (Bot_ApplyMovement copies these verbatim,
	short		pb_cmd_up;		//    same contract as the sj_cmd_* fields)
	float		pb_yaw;			// facing this frame (the recording's view)
	float		pb_pitch;

	// strafe jumping (bot_strafejump)
	int			sj_state;		// SJ_*
	int			sj_side;		// strafe side this hop: +1 = right, -1 = left
	int			sj_end_idx;		// path index of the qualified runway's last node
	int			sj_air_ticks;	// ticks since takeoff (0 = grounded)
	float		sj_cooldown;	// no re-qualify before this level.time
	float		sj_next_check;	// qualification throttle
	float		sj_peak;		// telemetry: peak 2D speed this engage
	int			sj_hops;		// telemetry: hops this engage
	short		sj_cmd_fwd;		// controller's exact usercmd for this frame
	short		sj_cmd_side;	//   (Bot_ApplyMovement copies these verbatim --
	short		sj_cmd_up;		//   the unit-projection path caps diagonals)

	// gaze (bot_gaze / bot_turnrate -- humanization, plans/humanization.md)
	float		gaze_pitch_wander;	// slow pitch-noise state (OU)
	float		gaze_cap;		// this look's turn-speed cap (deg/tick)
	int			gaze_last_node;	// look-ahead node (new node = new turn speed)
	float		gaze_hold_yaw;	// fixation: the held look target
	float		gaze_hold_pitch;
	float		glance_until;	// an active glance expires at this time
	float		glance_yaw;		// world yaw / pitch of the active glance
	float		glance_pitch;
	float		next_glance_time;	// earliest start of the next glance

	// combat
	edict_t		*enemy;			// current target (NULL = none)
	edict_t		*threat_ent;	// bot_fov: who hurt us last (pain reflex)
	float		threat_time;	// ...and when
	vec3_t		aim;			// current aim angles (10Hz-committed target)
	vec3_t		aim_view;		// bot_aimsmooth: the actual sent view, gliding
								// toward aim_look at 40Hz (kills 10Hz view judder)
	vec3_t		aim_look;		// bot_gazelife: where the view is gliding toward --
								// b->aim (tracking) or a glance point of interest
	float		cglance_until;	// combat glance holds until this time (0 = none)
	float		cglance_next;	// earliest start of the next combat glance
	float		cglance_yaw;	// world yaw/pitch of the active combat glance
	float		cglance_pitch;
	vec3_t		aim_err;		// bot_aimtexture: wandering (OU) aim error
	float		aim_bearing_prev;	// last tick's target bearing (yaw)
	float		aim_sweep_sign;	// which way the bearing was sweeping
	float		aim_flip_time;	// last reversal-overshoot event (rate limit)
	float		reaction_until;	// earliest time we may fire after acquiring
	float		dodge_until;	// when to re-pick a strafe direction
	int			dodge_dir;		// -1 / +1 strafe
	float		dodge_flip_time;	// bot_hop: when the last reversal began
	float		dodge_rkt_until;	// bot_dodge: active rocket-dodge window end
	vec3_t		dodge_rkt_dir;		// bot_dodge: world-space step-away direction
	qboolean	flee;			// outmatched: retreat and fetch health/armor

	// enemy last-known-position pursuit (bot_pursuit).  While an enemy is
	// visible the combat keyframe stamps where they were; when sight is lost the
	// bot may spend a BOUNDED amount of travel investigating that spot before
	// returning to the item economy.  The bound (A* cost + wall clock + strength)
	// is the whole point: an unbounded chase is the reachable != completable
	// over-investment trap in a new costume.
	edict_t		*lkp_ent;		// whose last-known position this is (NULL = none)
	vec3_t		lkp_pos;		// their origin at the last sighting
	vec3_t		lkp_vel;		// ...and their velocity then (extrapolation seed)
	float		lkp_time;		// level.time of that sighting (0 = none/consumed)
	qboolean	pursuing;		// this GOAL leg is a chase, not an item route
	float		pursue_until;	// wall-clock cap on the current chase.  Advances
								// with goal_time whenever a traversal controller
								// (lift/train/playbook/ladder) owns the frame, so
								// a chase is not billed for a lift queue it is
								// standing in -- same freeze the goal budget gets
	float		pursue_began;	// level.time the chase started.  Kept separate
								// from goal_time precisely BECAUSE goal_time is
								// advanced by that freeze and is not a clock
	qboolean	lkp_noise;		// this sighting came from bot_hearing, not sight
								// (telemetry only -- the chase gates are identical)

	// engagement movement style (bot_combatmove)
	int			cm_style;		// CM_* currently applied (CM_NONE = stock blend)
	float		cm_until;		// style hysteresis: hold it until at least here

	// weapon-aware combat tactics (bot_wpntactic): 10Hz-committed preferred
	// engagement band + style bias for the held weapon (demo-calibrated),
	// consumed by the per-tick movement blend so range/style vary by weapon
	float		eng_lo;			// preferred-range band low edge (world units)
	float		eng_hi;			// ...high edge
	float		eng_bias;		// style: >0 press/close, <0 defensive/give-ground
	int			eng_intent;		// ENG_* engagement intent (bot_wpnlog telemetry)

	// transition tracking for event logging
	qboolean	was_dead;
	int			death_mod;		// MOD_* of the pending death (stashed by
								// Bot_NoteDeath from player_die, consumed by the
								// bot_hazard environmental-death nav feedback)
} bot_t;

//
// engine/game entry points the bot drives (defined in p_client.c, exported
// via GetGameAPI but not otherwise prototyped in g_local.h)
//
qboolean ClientConnect (edict_t *ent, char *userinfo);
void ClientBegin (edict_t *ent);
void ClientDisconnect (edict_t *ent);
void ClientThink (edict_t *ent, usercmd_t *cmd);

//
// bot_main.c
//
void Bot_Init (void);			// called from InitGame()
void Bot_Shutdown (void);		// called from ShutdownGame()
void Bot_RunFrame (void);		// called from the top of G_RunFrame()
qboolean Bot_ServerCommand (void);	// handle "sv bot_*"; returns true if consumed
qboolean Bot_IsClient (edict_t *ent);	// true for DLL-driven bots (no net client)
void G_UnicastClient (edict_t *ent, qboolean reliable);	// gi.unicast, skips bots
// true if some OTHER active bot already has 'it' as its current goal_item
qboolean Bot_ItemClaimed (edict_t *it, bot_t *self);
qboolean Bot_Survives (bot_t *b);
qboolean Bot_Dodges (bot_t *b);
// A* link-type mask for this bot: capabilities toggled off exclude their
// link types from pathing (bot_navmask; NAV_MASK_ALL passthrough when off)
int Bot_NavMask (bot_t *b);

extern cvar_t	*bot_count;
extern cvar_t	*bot_forwardspeed;
extern cvar_t	*bot_debug;
extern cvar_t	*bot_quitafter;
extern cvar_t	*bot_rollout;
extern cvar_t	*bot_stucktime;
extern cvar_t	*bot_wallslide;
extern cvar_t	*bot_claim;
extern cvar_t	*bot_decisive;
extern cvar_t	*bot_pathcost;
extern cvar_t	*bot_goalbudget;
extern cvar_t	*bot_budgetcap;
extern cvar_t	*bot_itemfail;
extern cvar_t	*bot_navmask;
extern cvar_t	*bot_reachlog;
extern cvar_t	*bot_goalnode;
extern cvar_t	*bot_commit;	// travel-cost commitment discount: charge the marginal detour
								// past the cheapest reachable item, so a far/hard item only wins
								// when nothing cheaper is nearby (bot idle) or it is itself close.
								// 0 = off, byte-identical to the pre-lever pick. Attacks
								// reachable != completable (teleporter/playbook over-investment).
extern cvar_t	*bot_navlearn;	// runtime nav learning + autosave master switch (default 1).
									// 0 = never add nodes/links or save (play a fixed graph).
									// A graph loaded with the ONAV FROZEN header flag is treated
									// as navlearn==0 regardless, so shipped/baked navs can't
									// over-grow past their tuned sweet spot on a live server.
extern cvar_t	*bot_navvalidate;	// load-time fluke-link pruner (P1; map-general hygiene)
extern cvar_t	*bot_failpersist;	// persist per-item completability across map loads (P4a)
extern cvar_t	*bot_reroutemid;	// penalize a stalled hop mid-attempt, not only at giveup (P4b)
extern cvar_t	*bot_swim;
extern cvar_t	*bot_hazard;	// environmental-hazard awareness (learner refusal, steering
								// probe, death-driven link penalty, load-time node flagging
								// + A* exclusion, in-lava goal filter)
extern cvar_t	*bot_hazlog;	// per-death MOD + path/airborne classification diagnostic
extern cvar_t	*bot_lift;
extern cvar_t	*bot_train;			// the train capability: ride func_trains (bot_move.c)
extern cvar_t	*bot_teleport;		// route through misc_teleporters (seeded TELEPORT links;
									// the teleport itself is automatic on touch -- no controller)
extern cvar_t	*bot_jumppad;		// route through trigger_push jump pads (seeded PUSH links
									// to a ballistic landing; the launch is automatic on touch)
extern cvar_t	*bot_ladder;		// the ladder capability: climb CONTENTS_LADDER surfaces
extern cvar_t	*bot_slimeescape;	// submerged in slime: abandon the goal and climb/swim out
extern cvar_t	*bot_liftcommit;	// commit to riding a rising plat (don't step off mid-ride)
extern cvar_t	*bot_liftlog;
extern cvar_t	*bot_inputlog;
extern cvar_t	*bot_cmdlog;
// resource-need calibration (demos/derived/combat_need/thresholds.json)
extern cvar_t	*bot_ammoneed;		// ammo need ramps with low ammo for the best owned weapon
extern cvar_t	*bot_healthneed;	// health-urgency goal curve by default (not just under bot_survive)
extern cvar_t	*bot_wpnneed;		// unowned-weapon acquisition need weighted by pro kill-rank

//
// bot_move.c -- steering (target point / path following -> usercmd_t)
//
// these set movement intent (b->move_dir / move_yaw / want_jump), not cmd
void Bot_SteerToPoint (bot_t *b, vec3_t target);
void Bot_Wander (bot_t *b);				// explore-mode steering
void Bot_Unstick (bot_t *b);			// dislodge from a wall
qboolean Bot_FollowPath (bot_t *b);		// true while still following
// short-horizon forward-search recovery: rolls a handful of candidate input
// sequences through the real movement code (gi.Pmove) and commits to whichever
// makes the most progress toward the current path waypoint. Used in place of
// Bot_Unstick when simple steering has stalled.
void Bot_RolloutRecover (bot_t *b);
// humanization: micro-steps while holding a spot (waiting on a respawn)
void Bot_Fidget (bot_t *b, vec3_t anchor);
extern cvar_t	*bot_fidget;

// exposed by p_client.c (already file-scope globals there, just not prototyped
// anywhere) -- reused as the trace/passent plumbing for Bot_RolloutRecover's
// own gi.Pmove() calls, so simulated rollouts clip against the world exactly
// like the real per-frame movement does.
extern edict_t	*pm_passent;
trace_t PM_trace (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end);
// projects move_dir onto the facing to produce forward/side/up in cmd
void Bot_ApplyMovement (bot_t *b, usercmd_t *cmd, float facing_yaw);

// traversal contract (plans/nav-oracle.md Phase B): index into b->path of
// the FROM node of the next hop of 'type' in play (traversing now, or
// upcoming within 'engage' range; 'release' is the disengage hysteresis for
// an already-engaged controller), or -1.  The generic query a typed-link
// traversal controller engages from.
int Bot_UpcomingHop (bot_t *b, int type, float engage, float release, qboolean engaged);

// lift riding (bot_lift): WAIT/BOARD/RIDE state machine.  Returns true while
// it owns the frame's movement intent (caller then skips path following,
// stuck recovery, and the goal-budget clock for the frame).
qboolean Bot_LiftThink (bot_t *b);
void Bot_LiftReset (bot_t *b);
qboolean Bot_TrainThink (bot_t *b);
void Bot_TrainReset (bot_t *b);
qboolean Bot_LadderThink (bot_t *b);
void Bot_LadderReset (bot_t *b);
qboolean Bot_SlimeEscapeThink (bot_t *b);	// bot_slimeescape: climb/swim out of slime
// the func_plat whose horizontal footprint contains pos, or NULL
edict_t *Bot_FindPlatAt (vec3_t pos);

// strafe jumping (bot_strafejump): chained bunny hops on qualified runways.
// Returns true while it owns the frame's movement intent (caller skips path
// following and stuck recovery; the goal budget keeps billing normally --
// travel is FASTER than budgeted, unlike a lift wait).
qboolean Bot_StrafeThink (bot_t *b);
void Bot_StrafeReset (bot_t *b);
extern cvar_t	*bot_strafejump;
extern cvar_t	*bot_sjlog;

// generic movement helpers shared with the playback controller
void Bot_SetMoveYaw (bot_t *b, float yaw);
int Bot_LinkType (int from, int to);	// NAV_LINK_* of from->to, or -1

// playbooks (bot_playbook, Phase R4): recorded map-specific maneuvers as
// replayable NAV_LINK_PLAYBOOK links (bot_playback.c).  Bot_PlaybackThink
// returns true while it owns the frame's movement intent AND facing (caller
// skips path following/stuck recovery and freezes the goal-budget clock --
// align waiting must not be billed, like a lift wait).
void Playbook_Load (const char *mapname);	// parse <gamedir>/playbooks/<map>.pbk
void Playbook_Register (void);				// seed nodes + inject PLAYBOOK links
qboolean Bot_PlaybackThink (bot_t *b);
void Bot_PlaybackReset (bot_t *b);
extern cvar_t	*bot_playbook;

// strafe-jump qualification funnel (diagnosis only; bot_sjlog >= 2)
#define SJ_DIAG_QUALIFY		0	// SJ_QualifyRunway calls
#define SJ_DIAG_LINK		1	// stopped by a non-WALK link
#define SJ_DIAG_DZ			2	// stopped by a vertical step
#define SJ_DIAG_TURN		3	// stopped by a sharp bend
#define SJ_DIAG_TRACE		4	// stopped by clearance traces
#define SJ_DIAG_SHORT		5	// stretch ended below SJ_MIN_RUNWAY
#define SJ_DIAG_PRESIM		6	// first-hop pre-sim rejected
#define SJ_DIAG_ENGAGE		7	// engaged
#define SJ_DIAG_MAX			8
extern int sj_diag[SJ_DIAG_MAX];
void Bot_LogSJDiag (void);		// bot_log.c: periodic funnel dump

//
// bot_gaze.c -- humanization: out-of-combat gaze + view-turn dynamics
//
// out-of-combat facing for the frame (handles its own cvar gating; falls back
// to yaw = move_yaw, pitch = 0 when off)
void Bot_GazeThink (bot_t *b, float *facing_yaw, float *facing_pitch);
// exponential-approach angle slew (gain x remaining delta, capped per tick)
float Bot_SlewAngle (float cur, float target, float gain, float cap);
// humanization behaviors apply to this bot (bot_humantest parity gate)
qboolean Bot_Humanized (bot_t *b);
extern cvar_t	*bot_gaze;
extern cvar_t	*bot_turnrate;
extern cvar_t	*bot_humantest;

//
// bot_combat.c -- enemy selection, aim, fire, dodge
//
// Sets aim into *facing_yaw/*facing_pitch and the fire button into cmd; may
// blend a strafe/range component into b->move_dir.  Returns true if engaged.
qboolean Combat_Aim (bot_t *b, usercmd_t *cmd, float *facing_yaw, float *facing_pitch);
float Combat_Strength (edict_t *e);	// effective toughness (health + armor)
// fractional ammo fill (0..1) for the highest-priority owned weapon that
// consumes `ammo_item`, or -1 if the bot owns no weapon that uses it.  Lets the
// goal scorer raise ammo need only when the gun that ammo feeds is running low.
float Combat_AmmoFracForItem (edict_t *ent, gitem_t *ammo_item);
extern cvar_t	*bot_skill;
extern cvar_t	*bot_skilltest;
// weapon-aware combat: per-weapon engagement range/style (demo-calibrated)
extern cvar_t	*bot_wpntactic;		// weapon-appropriate range + engagement style
extern cvar_t	*bot_wpntactictest;	// id-parity A/B (even ids get it, odd control)
extern cvar_t	*bot_wpnlog;		// engagement telemetry (range/weapon/intent)
extern cvar_t	*bot_wpnselect;		// range-aware firing-weapon choice (demo kill-range bands)
extern cvar_t	*bot_wpnselecttest;	// id-parity A/B (even ids get it, odd control)
extern cvar_t	*bot_wpnsellog;		// diagnostic: chosen weapon vs target distance
extern cvar_t	*bot_blastertransit;	// blaster-only: travel to a weapon/armor, fire defensively
extern cvar_t	*bot_blastertransittest;	// id-parity A/B (even ids get it, odd control)
extern cvar_t	*bot_watersight;	// opaque water surface blocks the sightline across it
extern cvar_t	*bot_aimlog;		// per-shot aim-error telemetry (calibration diagnostic)
extern cvar_t	*bot_aimprec;		// scale precision-weapon aim error toward human (0=off)
extern cvar_t	*bot_lead;
extern cvar_t	*bot_leadtest;
extern cvar_t	*bot_flee;
extern cvar_t	*bot_fleetest;
extern cvar_t	*bot_outnumbered;		// retreat from a 2+ enemy crossfire (bot_combat.c)
extern cvar_t	*bot_outnumberedtest;	// id-parity A/B for bot_outnumbered
// enemy last-known-position pursuit: investigate where a lost enemy was
extern cvar_t	*bot_pursuit;		// master switch (0 = off, byte-identical)
extern cvar_t	*bot_pursuittest;	// id-parity A/B (even ids pursue, odd control)
extern cvar_t	*bot_pursuitcost;	// max A* g-cost of the route to the LKP
// whether this bot runs LKP pursuit this frame (cvar + parity + humanization)
qboolean Combat_PursuitOn (bot_t *b);
// true while a blaster-only bot is travelling to a real weapon (bot_blastertransit)
qboolean Combat_BlasterTransitOn (bot_t *b);
extern cvar_t	*bot_aimtest;
extern cvar_t	*bot_aimreact;
extern cvar_t	*bot_aimturn;
extern cvar_t	*bot_aimerr;
extern cvar_t	*bot_aimfire;
extern cvar_t	*bot_aimtexture;
extern cvar_t	*bot_reroute;
extern cvar_t	*bot_losfinal;
extern cvar_t	*bot_survivetest;
extern cvar_t	*bot_survive;
extern cvar_t	*bot_dodge;
extern cvar_t	*bot_dodgetest;
extern cvar_t	*bot_gazelife;
extern cvar_t	*bot_aimflick;
extern cvar_t	*bot_aimsmooth;
extern cvar_t	*bot_fov;
extern cvar_t	*bot_hop;

// bot_fov pain reflex: a bot that takes damage learns where from (hooked from
// player_pain in p_client.c, which vanilla leaves empty)
void Bot_NotePain (edict_t *self, edict_t *attacker);
// bot_hazard death feedback: player_die stashes the killing MOD_* while it is
// still fresh (the global goes stale before the bot's deadflag poll next frame)
void Bot_NoteDeath (edict_t *self, int mod);
// bot_fov hearing: weapon-noise bookkeeping (hooked from PlayerNoise)
void Bot_NoteNoise (edict_t *who);
float Bot_NoiseTime (edict_t *who);

// bot_hearing: a noise taxonomy on top of the weapon-fire timestamp above.
// Each entry records WHERE the noise happened, at the time it happened -- never
// a live handle on the noisemaker.  That distinction is the whole design: the
// bot learns that something occurred at a spot, and can be wrong about where
// the player went next, exactly like a human listening through a wall.
#define NOISE_NONE		0
#define NOISE_WEAPON	1	// unsilenced fire (the legacy Bot_NoteNoise source)
#define NOISE_PICKUP	2	// an item being taken
#define NOISE_PAIN		3	// someone hurt, heard by bystanders
#define NOISE_STEP		4	// footfalls (throttled; quietest)
void Bot_NoteNoiseEx (edict_t *who, int kind, vec3_t origin);
extern cvar_t	*bot_hearing;	// qualifying noise seeds the pursuit LKP
extern cvar_t	*bot_hearlog;	// per-noise diagnostic

// engagement movement styles (bot_combatmove), a BITMASK so each style can be
// A/B'd in isolation.  Only CM_CIRCLE is built: the "stand ground on a height
// advantage" style was pre-gated OUT by the pro corpus, which shows movement
// speed while aimed at an opponent is flat across height buckets (p50 300 high
// / 301 level / 300 low) -- pros do not plant when they hold high ground.
#define CM_NONE		0
#define CM_CIRCLE	1	// commit to an orbit direction instead of re-rolling it
extern cvar_t	*bot_combatmove;		// bitmask of enabled styles
extern cvar_t	*bot_combatmovetest;	// id-parity A/B (even ids get it)
extern cvar_t	*bot_cmlog;				// style-transition diagnostic

//
// bot_goal.c -- item-driven goal selection
//
qboolean Goal_Select (bot_t *b);		// choose b->goal_item; true if found
qboolean Goal_ItemAvailable (edict_t *it);
void Goal_UpdateTrainCosts (void);		// bot_train: price TRAIN links by item availability
qboolean Goal_IsRecovery (edict_t *it);	// health or armor pickup?
edict_t *Goal_NearestItem (bot_t *b, float maxdist);	// for directed exploration
void Goal_Blacklist (edict_t *it, float secs);	// avoid re-targeting briefly
// giveup at an item: escalate avoidance.  navq = the oracle's NAVQ_* verdict
// at giveup time (bot_itemfail 2 fast-tracks route-evaporated items)
void Goal_ItemFailed (edict_t *it, int navq);
void Goal_ReachSweep (const char *when);	// bot_reachlog: item reachability sweep
											// (when = "load" or "quit")
void Goal_ItemSucceeded (edict_t *it);	// collected: reset failure tracking
void Goal_Reset (void);					// clear cooldowns (map change)
void Goal_SeedNavNodes (void);			// seed nav nodes at item spots
// bot_failpersist (P4a): persist per-item giveup counts across map loads so a
// fresh run doesn't re-pay a full-budget giveup per chronically-hard item.
void Goal_LoadFails (const char *mapname);	// restore item_fails from <map>.fail (keys live edicts)
void Goal_SaveFails (const char *mapname);	// write item_fails>0 to <map>.fail (uses load-time keys)

//
// bot_log.c
//
const char *Bot_GameDir (void);
const char *Bot_ItemName (edict_t *ent);	// telemetry label; disambiguates the item_health_* family (all share one "Health" gitem -- key off the edict classname)
void Bot_LogItemEvent (bot_t *b, const char *event, const char *item);
void Bot_LogGiveup (bot_t *b, float gdist, float gvdist, int atnode, int fighting,
	const char *navq);	// navq: oracle verdict on the goal at giveup time
// one reach-sweep record (bot_reachlog; plans/nav-oracle.md Phase C)
void Bot_LogReach (const char *when, const char *item, const char *classname,
	vec3_t org, const char *code, const char *gate, float cost);
void Bot_LogBeginLevel (const char *mapname);	// open a fresh JSONL for this map
void Bot_LogEndLevel (void);					// flush + close the current JSONL
void Bot_LogTick (bot_t *b);					// per-tick state record
void Bot_LogEvent (bot_t *b, const char *event);	// spawn/death/etc.
void Bot_LogHazDeath (bot_t *b, int mod, qboolean penalized);	// bot_hazlog: per-death classification
void Bot_LogFire (edict_t *who);				// weapon discharge (timing invariants)
void Bot_LogEngage (bot_t *b, const char *weapon, float range, int intent);	// bot_wpnlog
// bot_pursuit: chase start (route cost / straight-line distance / what cued it)
// and end (why it ended + how long it ran)
void Bot_LogPursueStart (bot_t *b, float cost, float dist, const char *src);
void Bot_LogPursueEnd (bot_t *b, const char *reason, float dur);
void Bot_LogHear (bot_t *b, int kind, float dist);	// bot_hearlog
void Bot_LogCMove (bot_t *b, int style, float range);	// bot_cmlog
void Bot_LogWpnSel (bot_t *b, const char *chosen, const char *held, float dist);	// bot_wpnsellog
// bot_aimlog: one record per shot fired -- weapon, range, target lateral speed,
// and yaw/pitch error of the committed aim vs the enemy's true bearing.
void Bot_LogAimShot (bot_t *b, const char *weapon, float range, float latspeed,
	float err_yaw, float err_pitch);
void Bot_LogRespawn (const char *event, edict_t *item_ent, float delay);	// respawn scheduling/firing
void Bot_LogInput (edict_t *ent, usercmd_t *ucmd);	// bot_inputlog: human usercmd trace
void Bot_LogMaybeFlush (void);					// periodic flush
// bot_capture: "capture start/stop/status" -- separate, user-named input log
// (+ synced .dm2 demo via g_cmds.c) independent of the per-level telemetry log.
void Bot_CaptureStart (edict_t *ent, const char *name);	// open logs/<name>.jsonl (overwrite)
void Bot_CaptureStop (edict_t *ent);				// close + report frame count
void Bot_CaptureInput (edict_t *ent, usercmd_t *ucmd);	// per-frame write while active
void Bot_CaptureShutdown (void);					// close on level end, no report
qboolean Bot_CaptureActive (void);					// cheap ClientThink gate
const char *Bot_CaptureName (void);				// current capture's name (for demo sync)
// bot_liftlog diagnosis instrumentation (throwaway, see plans/lift-riding.md)
void Bot_LogLiftBegin (void);					// cache func_plats + emit platinfo
void Bot_LogLiftTick (bot_t *b);				// per-tick record while near a plat
void Bot_LogPenalize (bot_t *b, int from, int to);	// link penalization event
// bot_sjlog: strafe-jump controller events (engage / hop / done / abort_*)
void Bot_LogSJ (bot_t *b, const char *phase, int hops, float peak);

#endif // OZBOT_BOT_H
