/*
ozbot - self-learning q2dm1 bot

bot_main.c -- bot lifecycle and per-frame driver.

A bot is an ordinary client-slot edict driven entirely from inside the game
DLL (the ACEBot approach): we run the normal ClientConnect -> ClientBegin path
to spawn it, then synthesize a usercmd_t each frame and call ClientThink().
No engine changes and no engine fake-client API are involved.  Bots are fully
visible entities, so we never set SVF_NOCLIENT on them.

Phase 1 adds navigation: each bot learns a waypoint graph as it explores, then
uses A* over that graph to travel point-to-point between goal nodes.
*/

#include "g_local.h"
#include "bot.h"
#include "bot_nav.h"

#include <time.h>
#include <process.h>	// _getpid (per-process rng entropy)

cvar_t	*bot_count;
cvar_t	*bot_forwardspeed;
cvar_t	*bot_debug;
cvar_t	*bot_seed;
cvar_t	*bot_quitafter;
cvar_t	*bot_cmdlog;
cvar_t	*bot_rollout;
cvar_t	*bot_stucktime;
cvar_t	*bot_wallslide;
cvar_t	*bot_claim;
cvar_t	*bot_decisive;
cvar_t	*bot_reroute;
cvar_t	*bot_losfinal;
cvar_t	*bot_survivetest;
cvar_t	*bot_pathcost;
cvar_t	*bot_goalbudget;
cvar_t	*bot_budgetcap;
cvar_t	*bot_itemfail;
cvar_t	*bot_navmask;
cvar_t	*bot_reachlog;
cvar_t	*bot_goalnode;
cvar_t	*bot_commit;	// travel-cost commitment discount (see bot.h)
cvar_t	*bot_navlearn;	// runtime nav learning + autosave master switch (see bot.h)
cvar_t	*bot_navvalidate;
cvar_t	*bot_failpersist;
cvar_t	*bot_reroutemid;
cvar_t	*bot_swim;
cvar_t	*bot_hazard;	// environmental-hazard awareness (lava/slime/hurt volumes)
cvar_t	*bot_hazlog;	// per-death diagnostic: MOD + path/airborne state (default 0)
cvar_t	*bot_lift;
cvar_t	*bot_train;			// ride func_trains (horizontal shuttles) -- see bot_move.c
cvar_t	*bot_teleport;		// route through misc_teleporters (seeded TELEPORT links)
cvar_t	*bot_jumppad;		// route through trigger_push jump pads (seeded PUSH links)
cvar_t	*bot_ladder;		// climb CONTENTS_LADDER surfaces -- see bot_move.c
cvar_t	*bot_slimeescape;	// submerged in slime: climb/swim out (see bot_move.c)
cvar_t	*bot_liftcommit;	// once on a rising plat, commit to the ride (don't step off)
cvar_t	*bot_liftlog;
cvar_t	*bot_strafejump;
cvar_t	*bot_sjlog;
cvar_t	*bot_inputlog;
cvar_t	*bot_ammoneed;
cvar_t	*bot_healthneed;
cvar_t	*bot_wpnneed;

// registry indexed by client slot (index i <-> g_edicts[i+1])
static bot_t	bots[MAX_CLIENTS];
static int		bot_next_id;			// monotonically increasing for names
static char		bot_logged_map[MAX_QPATH];	// map the current log/nav is for

// when each client slot last made weapon noise (bot_fov hearing; kept out of
// gclient_t deliberately -- no vanilla struct edits)
static float	bot_noise_time[MAX_CLIENTS];
// bot_hearing: the generalized registry beside the legacy weapon-fire slot
static float	bot_noise_at[MAX_CLIENTS];		// when the last noise happened
static int		bot_noise_kind[MAX_CLIENTS];	// NOISE_*
static vec3_t	bot_noise_pos[MAX_CLIENTS];		// where it happened, AT THAT TIME
static float	bot_noise_step[MAX_CLIENTS];	// footstep throttle

// bot_slotlog: diagnostic (default off) -- dumps the full client-slot table
// (inuse/connected/netname + which slots the DLL owns as bots) so client-slot
// ownership across a `gamemap` is visible in the console.  Written to nail the
// map-change puppeting bug (a bot grabbing the listen host's slot 0); kept as
// permanent infra for any future slot-ownership question.
static cvar_t	*bot_slotlog;

#define BOT_GRAPH_READY		24		// nodes needed before goal-seeking starts
#define BOT_GOAL_TIMEOUT	12.0f	// abandon a goal not reached within this

// bot_pursuit constants.  Mined from the pro demo corpus by
// tools/dm2_combat.py pursue (sight loss = an opponent leaving the recorder's
// packet-entity set), then baked here -- there is no runtime dependency on the
// JSON, exactly like the bot_ammoneed/bot_wpnneed thresholds.
#define BOT_PURSUE_FRESH	1.5f	// an older last-known position is stale
#define BOT_PURSUE_EXTRAP	0.3f	// seconds of velocity extrapolation past the
									// sighting (minimizes the pro corpus' median
									// reappearance offset)
#define BOT_PURSUE_EXTRAP_MAX	200.0f	// ...clamped to this much lead
#define BOT_PURSUE_MAXSECS	3.5f	// hard wall-clock cap on any one chase (p75
									// of the gap before a pro re-sights someone
									// they were chasing; past it they've lost them)
#define BOT_PURSUE_MINSTR	70.0f	// never chase below this strength
#define BOT_PURSUE_NEARGOAL	400.0f	// an item this close outranks a chase

// bot_hearing.  Radii are per noise kind (a railgun carries; a footfall does
// not), and audibility additionally requires a PHS check -- sound in Quake 2
// propagates through the potentially-hearable set, so a listener two sealed
// rooms away hears nothing no matter how close the straight-line distance.
#define BOT_NOISE_FRESH			0.5f	// only act on a noise this recent
#define BOT_NOISE_STEP_THROTTLE	0.5f	// per-source footstep rate limit
#define BOT_NOISE_LOUD_HOLD		0.5f	// a footfall won't overwrite a louder cue
#define BOT_NOISE_R_WEAPON		700.0f
#define BOT_NOISE_R_PICKUP		512.0f
#define BOT_NOISE_R_PAIN		512.0f
#define BOT_NOISE_R_STEP		256.0f
#define BOT_REROUTEMID_STALL	3.5f	// bot_reroutemid: pure-nav no-advance window
										// that triggers a mid-attempt hop penalty

// bot_goalbudget: timeout scaled to the committed route's A* cost instead of
// the flat BOT_GOAL_TIMEOUT -- short hops recycle faster, honest long routes
// get funded.  base + cost/speed crosses the flat 12s at cost ~600u.
// The cap comes from the bot_budgetcap cvar (default 15): pickups p95 is
// ~11s, so budget past that mostly funds giveups, not successes.
// (constants live in bot.h so Goal_Select's bot_goalnode 2 fundability
// filter prices candidates with the same formula)

/*
=================
Bot_TickGain

40Hz adaptation: convert an exponential-approach gain ("this fraction of the
remaining delta per 10Hz tick") to the actual tick rate, preserving the
per-second convergence it was tuned for.  Exact no-op at 10Hz.
=================
*/
float Bot_TickGain (float gain10)
{
	if (game.framediv == 1)
		return gain10;
	return 1.0f - (float)pow ((double)(1.0f - gain10), (double)BOT_TICK_RATIO);
}

/*
=================
Bot_Init
=================
*/
void Bot_Init (void)
{
	bot_count        = gi.cvar ("bot_count", "0", 0);
	bot_forwardspeed = gi.cvar ("bot_forwardspeed", "400", 0);
	bot_debug        = gi.cvar ("bot_debug", "0", 0);
	bot_skill        = gi.cvar ("bot_skill", "0.6", 0);
	bot_seed         = gi.cvar ("bot_seed", "0", 0);
	bot_quitafter    = gi.cvar ("bot_quitafter", "0", 0);	// >0: quit after N game seconds (times fastsim runs)
	bot_rollout      = gi.cvar ("bot_rollout", "1", 0);
	bot_stucktime    = gi.cvar ("bot_stucktime", "1.0", 0);	// seconds of <24u travel before stuck-recovery fires
	bot_wallslide    = gi.cvar ("bot_wallslide", "0", 0);	// deflect move intent along walls instead of pinning
	bot_claim        = gi.cvar ("bot_claim", "1", 0);
	bot_decisive     = gi.cvar ("bot_decisive", "1", 0);	// prompt goal re-picks + sticky explore steering
															// (kills the standing A<->B re-decide loop; Phase 20:
															// pickups +45%, ITEM +6pts, 5/5 seeds)
	bot_pathcost     = gi.cvar ("bot_pathcost", "1", 0);	// score items by A* route cost, not straight-line distance
	bot_goalbudget   = gi.cvar ("bot_goalbudget", "1", 0);	// goal timeout scaled to route cost, not flat 12s
	bot_budgetcap    = gi.cvar ("bot_budgetcap", "15", 0);	// max seconds to fund any one goal route
	bot_itemfail     = gi.cvar ("bot_itemfail", "1", 0);	// escalating shared blacklist for items bots keep failing
															// (2 = also fast-track items whose route evaporated, per
															// the giveup-time oracle verdict; plans/nav-oracle.md)
	bot_navmask      = gi.cvar ("bot_navmask", "0", 0);		// A* skips link types whose capability cvar is off
															// (plans/nav-oracle.md Phase A)
	bot_reachlog     = gi.cvar ("bot_reachlog", "1", 0);	// map-load item reachability sweep (oracle diagnostics)
	bot_goalnode     = gi.cvar ("bot_goalnode", "0", 0);	// resolve item goal nodes to CONNECTED nodes (skip
															// in-degree-0 orphans that shadow real coverage)
	bot_commit       = gi.cvar ("bot_commit", "0.8", 0);	// travel-cost commitment discount (default-ON): penalize
															// the detour past the cheapest reachable item so far/hard
															// items only win when idle or themselves close. Net +pickups
															// (strong in deathmatch contention; 0 = pre-lever, bit-exact)
	bot_navlearn     = gi.cvar ("bot_navlearn", "1", 0);	// runtime nav learning + autosave master switch;
															// FROZEN-flagged graphs ignore it and never grow/save
	bot_navvalidate  = gi.cvar ("bot_navvalidate", "0", 0);	// P1: at load, drop learned fluke WALK links (steep,
															// one-way -- lucky falls/combat shoves A* re-sells)
	bot_failpersist  = gi.cvar ("bot_failpersist", "0", 0);	// P4a: persist per-item giveup counts across map loads
															// (sidecar <map>.fail; keyed by classname + origin)
	bot_reroutemid   = gi.cvar ("bot_reroutemid", "1", 0);	// P4b (default ON): penalize a stalled hop mid-attempt
															// (pure-nav, !enemy) instead of only after the full
															// goal budget.  A/B: giveup rate -4.4%, pathfail -23%,
															// ITEM flat, no nav erosion; win concentrated on maps
															// with unreliable routes (q2dm2), neutral on matured
															// navs (q2dm1/8).  Set 0 to recover pre-P4b behavior.
	// resource-need calibration, mined from the pro demo corpus
	// (tools/dm2_combat.py need -> demos/derived/combat_need/thresholds.json,
	// 5859 demos).  A/B'd 16x2 seed-bases; each is bit-exact when set to 0.
	bot_ammoneed     = gi.cvar ("bot_ammoneed", "1", 0);	// ammo need ramps as fill for the best owned weapon drops
															// (default ON: pickups +4-7%, ITEM +1pt, combat neutral;
															// the bot had NO ammo awareness before this)
	bot_healthneed   = gi.cvar ("bot_healthneed", "0", 0);	// health-urgency goal curve by default (pros top up at ~74hp)
															// (default OFF: inconclusive across seeds -- one +frags,
															// one -20 frags/+32 giveups; same asymmetric health-seek
															// cost that keeps bot_survive off.  Kept as infra.)
	bot_wpnneed      = gi.cvar ("bot_wpnneed", "1", 0);		// unowned-weapon need weighted by pro kill-rank (promotes CG,
															// demotes hyperblaster).  Default ON: pickups +10%, ITEM
															// +2-3pts, frags neutral (biggest single lever)
	bot_swim         = gi.cvar ("bot_swim", "1", 0);		// 3D steering in water (vertical swim + water-jump exits)
	// Environmental-hazard awareness.  The stack was hazard-blind end to end
	// and the lava maps punished it: q2dm3/q2dm6 ran ~700 environmental
	// suicides per 16x90s DM (deaths 712 vs frags 19), and even SOLO bots died
	// 114-131x.  Mechanism (diagnosed from death telemetry + code): lava sets
	// waterlevel like water, so maturation wrote pool-interior nodes into the
	// graphs (23% of q2dm6's) as routable "water"; A* priced the crossings
	// cheap; route-following trusts learned links ("known-traversable") so the
	// explore-only StepIsSafe probe never fired; and dying triggers NONE of
	// the alive-only link penalties, so the death routes never eroded -- flat
	// death rate for the whole run, plus a DM bait loop (weapons dropped by
	// lava victims sink into the pool and become goals).  Five layers, one
	// gate: (1) learner refuses nodes/links while in lava; (2) steering
	// probes the floor ahead in ALL modes and stands down before a burn (the
	// stall then feeds the normal reroutemid/stuck penalties); (3) an
	// environmental death (lava/slime/hurt-brush/drown/fall) penalizes the hop
	// being traversed, both directions, and itemfails the goal -- death
	// finally erodes the graph; (4) at load, nodes inside lava or active
	// trigger_hurt volumes get NAV_FLAG_HAZARD and A* refuses to route INTO
	// them (legacy poisoned graphs heal without regrowing); (5) items lying IN
	// lava are never goals (kills the bait loop).  SLIME is different: a
	// survivable wade (10-30 hp/s vs lava's 30-90) and q2dm7's channels are
	// legitimate crossings (a blanket exclusion cost q2dm7 solo 74%->42%),
	// so slime nodes stay learnable/routable but NAV_FLAG_SLIME prices them
	// 4x and roam avoids them; slime deaths still penalize via (3).
	// Off-state byte-exact.
	bot_hazard       = gi.cvar ("bot_hazard", "1", 0);		// don't walk into lava; unlearn what does
	bot_hazlog       = gi.cvar ("bot_hazlog", "0", 0);		// per-death MOD + path/airborne classification diagnostic
	bot_lift         = gi.cvar ("bot_lift", "1", 0);		// the lift capability: plat links, wait/board/ride
															// controller, 3D column arrival, level-aware homing
	bot_ladder       = gi.cvar ("bot_ladder", "1", 0);		// the ladder capability: face + climb
															// CONTENTS_LADDER surfaces toward a higher/lower goal
	bot_slimeescape  = gi.cvar ("bot_slimeescape", "0", 0);	// submerged in slime: abandon the item goal and
															// climb the nearest ladder / swim to the nearest dry
															// node (q2dm7 slime deaths were 57% of all deaths)
	bot_train        = gi.cvar ("bot_train", "1", 0);		// the train capability: ride func_trains across
															// gaps they bridge (seeded TRAIN links + controller +
															// availability-priced links, Goal_UpdateTrainCosts).
															// Default ON for map-completeness: reliably rides and
															// unlocks q2dm2's otherwise-no_path RG/BFG/Body Armor
															// platform.  NOTE the benchmark's raw pickup count dips
															// on q2dm2 (a ride is an expensive round trip) and frags
															// are ~flat -- the symmetric sim gives every bot equal
															// access so the unlock shows no RELATIVE edge, but a bot
															// that can reach the whole map is more capable in real
															// play.  Bit-identical on train-less maps (q2dm1). See
															// ozbot-re-q2dm2-* memory
	bot_teleport     = gi.cvar ("bot_teleport", "1", 0);	// route through misc_teleporters: seed one-way
															// TELEPORT links pad->destination (Nav_SeedTeleportLinks).
															// The teleport is automatic on touch, so no controller --
															// just the routing edge the >200u learn guard refuses to
															// learn.  Off = graph untouched (byte-identical).
	bot_jumppad      = gi.cvar ("bot_jumppad", "1", 0);	// route through trigger_push jump pads: seed one-way
															// PUSH links pad->ballistic-landing (Nav_SeedPushLinks).
															// The launch is automatic on touch, so no controller --
															// just the routing edge the learn guard refuses to learn.
															// Off = no push links (byte-identical on pad-less maps).
	// once the bot is standing in a RISING plat's footprint, commit to riding it
	// to the top (hold center) instead of the WAIT logic backing it out -- fixes
	// "the bot steps off the lift before it goes up".  Off = pre-fix behavior.
	bot_liftcommit   = gi.cvar ("bot_liftcommit", "1", 0);	// stay on a rising plat; don't step off mid-ride
	bot_liftlog      = gi.cvar ("bot_liftlog", "0", 0);		// diagnosis telemetry near func_plats (plans/lift-riding.md)
	bot_strafejump   = gi.cvar ("bot_strafejump", "1", 0);	// chained strafe-jump travel on clear runways (Phase 19:
															// +6% pickups, giveups -11%, frags flat, 8-seed A/B)
	bot_sjlog        = gi.cvar ("bot_sjlog", "0", 0);		// strafe-jump controller event telemetry
	bot_inputlog     = gi.cvar ("bot_inputlog", "0", 0);	// 1 = log real players' per-frame usercmd (jump analysis; see ozbot-input-logger memory)
	bot_playbook     = gi.cvar ("bot_playbook", "1", 0);	// recorded maneuvers as nav links (Phase R4;
															// inert unless playbooks/<map>.pbk exists)
	bot_cmdlog       = gi.cvar ("bot_cmdlog", "0", 0);		// log BOT usercmds in the input-log schema
															// (playbook pipeline validation)
	bot_gaze         = gi.cvar ("bot_gaze", "1", 0);		// humanization: path look-ahead, glances, live pitch
	bot_turnrate     = gi.cvar ("bot_turnrate", "1", 0);	// humanization: slew-limited view turns
	bot_humantest    = gi.cvar ("bot_humantest", "0", 0);	// head-to-head: even ids humanized, odd stock
	bot_skilltest    = gi.cvar ("bot_skilltest", "0", 0);
	bot_lead         = gi.cvar ("bot_lead", "1", 0);		// lead moving targets by projectile flight time
	bot_leadtest     = gi.cvar ("bot_leadtest", "0", 0);	// head-to-head: even bot ids lead, odd don't
	bot_flee         = gi.cvar ("bot_flee", "1", 0);		// retreat + fetch health/armor when outmatched
	bot_fleetest     = gi.cvar ("bot_fleetest", "0", 0);	// head-to-head: even bot ids flee, odd don't
	bot_outnumbered     = gi.cvar ("bot_outnumbered", "0", 0);	// break off + retreat from a LOSING 2+ enemy crossfire
															// (strength-gated).  Default OFF: parity A/B is marginal/
															// mixed (q2dm1/6 net-positive, q2dm8 regresses) -- promising
															// but unconfirmed; needs multi-seed + q2dm8 diagnosis
	bot_outnumberedtest = gi.cvar ("bot_outnumberedtest", "0", 0);	// id-parity A/B: even ids use it, odd control
	// enemy last-known-position pursuit.  Default ON: parity A/B is +2.7/+4.8pt
	// kill share on q2dm1 and +0.1/+2.6pt on q2dm8 (two seed-bases x16 seeds,
	// mean +2.55pt) with the death share DOWN in all four -- chasing bots trade
	// better, they don't just trade more.  Symmetric economy check is flat on
	// pickups with giveups -22%.  The cost/time/strength bounds are what keep
	// this from becoming an over-investment channel: one sight loss buys at most
	// one chase, priced in A* g-cost, and only while we're strong enough to want
	// the fight.  0 = byte-identical to the pre-lever bot.
	bot_pursuit      = gi.cvar ("bot_pursuit", "1", 0);		// investigate a lost enemy's last known position
	bot_pursuittest  = gi.cvar ("bot_pursuittest", "0", 0);	// id-parity A/B: even ids pursue, odd control
	// noise -> pursuit cue.  Default OFF pending its own parity A/B; inert
	// without bot_pursuit by construction (it only writes fields that layer reads).
	// engagement movement styles (bitmask).  Default OFF pending the per-style
	// parity A/B.  Only CM_CIRCLE (1) is built -- the stand-ground style was
	// pre-gated out by the pro corpus (aimed movement speed is flat across
	// height advantage, so pros do not plant when they hold high ground).
	// corner-cut steering: blend the movement target toward the node after next
	// on shallow plain-ground bends.  Value IS the max blend weight (0 = off).
	bot_lookahead    = gi.cvar ("bot_lookahead", "0", 0);	// 0.4 is the tested weight
	bot_combatmove     = gi.cvar ("bot_combatmove", "0", 0);		// 1 = committed circle-strafe
	bot_combatmovetest = gi.cvar ("bot_combatmovetest", "0", 0);	// id-parity A/B: even ids get it
	bot_cmlog          = gi.cvar ("bot_cmlog", "0", 0);				// style-transition diagnostic
	bot_hearing      = gi.cvar ("bot_hearing", "0", 0);		// hear fire/pickups/pain/footsteps
	bot_hearlog      = gi.cvar ("bot_hearlog", "0", 0);		// per-noise diagnostic
	bot_pursuitcost  = gi.cvar ("bot_pursuitcost", "670", 0);	// max A* g-cost of the chase route
															// (pro corpus p75 of the path a human spends while
															// still closing on a last-known position)
	bot_aimtest      = gi.cvar ("bot_aimtest", "0", 0);		// head-to-head: even ids apply the bot_aim* multipliers
	bot_aimreact     = gi.cvar ("bot_aimreact", "1", 0);	//   reaction-delay multiplier
	bot_aimturn      = gi.cvar ("bot_aimturn", "1", 0);		//   turn-rate multiplier
	bot_aimerr       = gi.cvar ("bot_aimerr", "1", 0);		//   aim-error multiplier
	bot_aimfire      = gi.cvar ("bot_aimfire", "1", 0);		//   fire-threshold multiplier
	bot_aimtexture   = gi.cvar ("bot_aimtexture", "1", 0);	// humanization: wandering aim error + reversal overshoot
	bot_reroute      = gi.cvar ("bot_reroute", "1", 0);		// penalize the stalled hop on a pure-nav giveup (reroute next time)
	bot_losfinal     = gi.cvar ("bot_losfinal", "1", 0);		// final-approach LOS gate (no homing through walls)
	bot_survive      = gi.cvar ("bot_survive", "0", 0);		// survival instinct: seek health + flee when low (asymmetric-negative)
	bot_survivetest  = gi.cvar ("bot_survivetest", "0", 0);	// id-parity A/B: even bots survive, odd control
	bot_dodge        = gi.cvar ("bot_dodge", "0", 0);		// directed rocket dodge (re-test at 40Hz; default OFF pending A/B)
	bot_dodgetest    = gi.cvar ("bot_dodgetest", "0", 0);	// id-parity A/B: even ids dodge, odd control
	bot_gazelife     = gi.cvar ("bot_gazelife", "1", 0);		// glance around between fire windows (humanization)
	bot_aimflick     = gi.cvar ("bot_aimflick", "1", 0);		// flick-speed cap multiplier (1 = stock 20-60 deg/tick)
	bot_aimsmooth    = gi.cvar ("bot_aimsmooth", "1", 0);	// 40Hz view glide toward the 10Hz aim (anti-judder)
	bot_fov          = gi.cvar ("bot_fov", "1", 0);			// humanization: ~120 deg vision cone + pain reflex
	bot_hop          = gi.cvar ("bot_hop", "1", 0);			// humanization: combat jump/strafe rhythm from demo stats
	bot_fidget       = gi.cvar ("bot_fidget", "1", 0);		// humanization: idle fidget, wall turn-away, travel hops
	// weapon-aware combat: engagement range/style per weapon, demo-calibrated
	// (tools/dm2_combat.py tactics -> demos/derived/combat_tactics/weapon_profiles.json).
	// Default ON: breaks the weapon-blind circling into weapon-appropriate motion
	// (SSG/chaingun press, railgun holds far, GL hangs back).  pickups +3-5% /
	// giveups down / ITEM +1-2pt (2 seed-bases x16); lethality neutral (parity,
	// bias-netted -- combat is zero-sum); off-state bit-exact when 0.
	bot_wpntactic    = gi.cvar ("bot_wpntactic", "1", 0);	// rail holds far, SSG/chaingun brawl close, GL hangs back
	bot_wpntactictest= gi.cvar ("bot_wpntactictest", "0", 0);	// id-parity A/B: even ids get it, odd control
	bot_wpnlog       = gi.cvar ("bot_wpnlog", "0", 0);		// per-engagement telemetry (weapon/range/intent)
	// bot_wpntactic moves the BODY to each weapon's demo range; the firing weapon
	// itself is picked by a fixed rail-first priority (Combat_SelectWeapon), so a
	// bot with a railgun rails a foe in its face.  bot_wpnselect picked instead the
	// owned+ammo weapon whose demo kill-range band best fit the target distance
	// (rail far, SSG/RL/chaingun close).  TESTED AND REJECTED (default OFF, kept as
	// documented infra like bot_dodge/bot_survive): 4 seed-bases x16 id-parity --
	// aggressive re-selection cost the treatment bots -7.7pt kill share (even kills
	// 583->422) from weapon-switch downtime + surrendering the railgun's hitscan
	// edge; a hysteresis variant recovered to frag-neutral only by suppressing the
	// switching (near-inert).  No lethality win either way: the pro weapon-by-range
	// map is optimal for HUMANS (who miss with rail up close), not for a bot with
	// near-perfect hitscan -- the same "combat policy doesn't transfer" wall as the
	// movement/dodge rejections.  bot_wpnneed (which guns to ACQUIRE) was the win;
	// which to FIRE is not.  off-state bit-exact.  bot_wpnselecttest = id-parity.
	bot_wpnselect    = gi.cvar ("bot_wpnselect", "0", 0);	// range-aware firing-weapon choice (demo kill-range bands) -- tested-negative infra
	bot_wpnselecttest= gi.cvar ("bot_wpnselecttest", "0", 0);	// id-parity A/B: even ids get it, odd control
	bot_wpnsellog    = gi.cvar ("bot_wpnsellog", "0", 0);	// diagnostic: chosen weapon vs target distance
	// A blaster-only bot (fresh spawn, or shot dry) should be racing to a real
	// weapon/armor, not planting to duel with the pea-shooter.  bot_blastertransit
	// keeps such a bot at full nav weight toward its goal and drops the
	// range-closing pull, so it travels to a weapon while its aim tracks and fires
	// the blaster defensively (movement is relative to facing -> it back-pedals to
	// the goal while shooting).  Only affects bots with NOTHING but the blaster
	// (Combat_BlasterOnly), so a bot with a real weapon is byte-identical when off.
	bot_blastertransit= gi.cvar ("bot_blastertransit", "1", 0);	// blaster-only: travel to a weapon, fire defensively
	bot_blastertransittest= gi.cvar ("bot_blastertransittest", "0", 0);	// id-parity A/B: even ids get it, odd control
	// The water surface is opaque -- the engine's visible() trace passes through
	// liquid, letting a submerged bot railgun someone on the steps (and vice
	// versa).  bot_watersight blocks any sightline whose two eye points straddle
	// the surface (one underwater, one not).  Same-medium fights are byte-identical.
	bot_watersight   = gi.cvar ("bot_watersight", "1", 0);	// no seeing/shooting across the opaque water surface
	bot_aimlog       = gi.cvar ("bot_aimlog", "0", 0);		// per-SHOT aim-error telemetry (weapon/range/target
															// lateral speed/yaw+pitch error vs true bearing) --
															// calibration diagnostic vs demos/derived/combat_aim
	bot_aimprec      = gi.cvar ("bot_aimprec", "1", 0);		// (default ON) scale railgun/blaster/hyperblaster aim
															// error toward human fallibility with a range + target-
															// lateral-speed term.  Strength scalar (0=off, 1=the
															// calibrated "between" target: railgun fire |yaw err|
															// p50 1.4->2.7 deg, ~60% of the pro corpus -- visibly
															// fallible on moving/distant shots, still sharper than
															// an average pro).  Symmetric frag-neutral (K/D even,
															// lethality -10%), ITEM flat/+.  Set 0 to recover the
															// pre-fix uncanny accuracy.
	bot_slotlog      = gi.cvar ("bot_slotlog", "0", 0);		// diagnostic: client-slot ownership trace (gamemap puppet-bug class)

	// Seed the game's RNG.  The vanilla game never calls srand(), so every
	// process starts from the same default sequence -- which makes parallel
	// headless sims (see tools/run_parallel.py) run in identical lockstep
	// (same DM spawn points, same combat rolls).  Seed per process so the
	// instances diverge into independent samples.  A positive bot_seed gives a
	// reproducible run; 0 (default) derives an independent seed from pid+time.
	{
		unsigned seed = (bot_seed->value > 0)
			? (unsigned)bot_seed->value
			: (unsigned)(time(NULL) ^ ((unsigned)_getpid() << 16));
		srand (seed);
		gi.dprintf ("ozbot: rng seed %u\n", seed);
	}

	memset (bots, 0, sizeof(bots));
	bot_next_id = 0;
	bot_logged_map[0] = 0;

	gi.dprintf ("ozbot: bot subsystem initialized\n");
}

/*
=================
Bot_Shutdown
=================
*/
void Bot_Shutdown (void)
{
	if (bot_logged_map[0])
	{
		Goal_ReachSweep ("quit");	// the run-matured graph's reachability truth
		Goal_SaveFails (bot_logged_map);	// P4a: persist hard-item knowledge on the way out
		Nav_Shutdown (bot_logged_map);
	}
	Bot_LogEndLevel ();
}

/*
=================
Bot_IsClient

True for ozbot-driven client slots.  These edicts have no real network
connection (ClientConnect is called from the DLL), so gi.unicast to them
spams "PF_Unicast to a free/zombie client" in q2pro.
=================
*/
qboolean Bot_IsClient (edict_t *ent)
{
	int	i;

	if (!ent || !ent->client || !ent->inuse)
		return false;
	i = ent - g_edicts - 1;
	if (i < 0 || i >= game.maxclients)
		return false;
	return bots[i].inuse && bots[i].ent == ent;
}

/*
=================
G_UnicastClient

gi.unicast wrapper that silently skips bot slots.
=================
*/
void G_UnicastClient (edict_t *ent, qboolean reliable)
{
	if (!ent || !ent->inuse || !ent->client)
		return;
	if (Bot_IsClient (ent))
		return;
	gi.unicast (ent, reliable);
}

/*
=================
Bot_ResetNavState
=================
*/
static void Bot_ResetNavState (bot_t *b)
{
	b->mode      = BOT_MODE_EXPLORE;
	b->cur_node  = -1;
	b->prev_node = -1;
	b->goal_node = -1;
	b->goal_item = NULL;
	b->steer_item = NULL;
	b->goal_timing = false;
	b->goal_cost = 0;
	b->flee      = false;
	b->path_len  = 0;
	b->path_idx  = 0;
	b->replan_time  = level.time + 1.0;
	b->progress_time = level.time;
	b->pursuing     = false;	// bot_pursuit: a respawn ends any chase outright
	b->pursue_until = 0;
	b->pursue_began = 0;
	b->lkp_noise    = false;
	b->cm_style     = CM_NONE;	// bot_combatmove: no stale style across a respawn
	b->cm_until     = 0;
	b->lkp_ent      = NULL;
	b->lkp_time     = 0;
	VectorClear (b->lkp_pos);
	VectorClear (b->lkp_vel);
	Bot_LiftReset (b);
	Bot_TrainReset (b);
	Bot_LadderReset (b);
	Bot_StrafeReset (b);
	Bot_PlaybackReset (b);
	b->glance_until = 0;			// no stale glance across a respawn
	// deterministic (NO random() here): this runs with humanization off too,
	// and an extra rand() per respawn would shift the whole stream vs stock,
	// breaking same-seed comparisons against historical baselines
	b->next_glance_time = level.time + 1.5f;
	// a respawn is a fresh start: forget the fight we died in.  Keeping
	// b->enemy/threat across death let a bot_fov bot re-acquire its killer
	// cone-free and with no reaction delay the instant it respawned.  The
	// b->enemy clear is gated so a cvars-off build stays behavior- and
	// RNG-identical to stock (threat/aim_err are humanization-only state --
	// stock never reads them, so clearing those is always safe)
	if (bot_fov->value != 0)
		b->enemy = NULL;
	b->threat_ent = NULL;
	b->threat_time = 0;
	b->aim_err[YAW] = b->aim_err[PITCH] = 0;
	b->aim_sweep_sign = 0;
	if (b->ent)
		VectorCopy (b->ent->s.origin, b->last_pos);
}

/*
=================
Bot_Survives

Whether this bot runs the survival instinct (health-need urgency + low-hp
caution flee).  bot_survivetest gives an id-parity head-to-head (even ids
survive, odd are the control) so the effect can be measured in an ASYMMETRIC
sim -- survivability washes out when every bot has it, but a survive-vs-control
kill/death ratio shift is real.  Read paired with a bot_survivetest 0 control
run to net out the harness's baseline odd-bias.
=================
*/
qboolean Bot_Survives (bot_t *b)
{
	if (bot_survivetest && bot_survivetest->value != 0)
		return (b->id & 1) == 0;
	return bot_survive && bot_survive->value != 0;
}

/*
=================
Bot_Dodges

Whether this bot runs the directed rocket dodge.  bot_dodgetest gives the same
id-parity head-to-head as Bot_Survives (even ids dodge, odd control) so the
effect is measurable in the symmetric sim: read the survive/control kill+death
ratio shift paired against a bot_dodgetest 0 control run.
=================
*/
qboolean Bot_Dodges (bot_t *b)
{
	if (bot_dodgetest && bot_dodgetest->value != 0)
		return (b->id & 1) == 0;
	return bot_dodge && bot_dodge->value != 0;
}

/*
=================
Bot_ItemClaimed

Used by Goal_Select to avoid piling multiple bots onto the same item -- without
this, the shared value*need/distance scoring converges bots onto the same
best-looking target (especially right after a synchronized spawn), and only
one wins; the rest register as item_lost, wasting a goal-attempt's worth of
travel on a foregone conclusion.
=================
*/
qboolean Bot_ItemClaimed (edict_t *it, bot_t *self)
{
	int	i;
	for (i = 0; i < game.maxclients; i++)
	{
		bot_t *b = &bots[i];
		if (!b->inuse || b == self)
			continue;
		if (b->goal_item == it)
			return true;
	}
	return false;
}

/*
=================
Bot_NavMask

Link types this bot's A* may expand (bot_navmask; plans/nav-oracle.md).  A
capability toggled off excludes its link types from pathing, so one shared
graph serves any capability config: a lift-off bot routes AROUND plat links
instead of committing budget to a ride it can't execute (and a lift-matured
graph no longer needs swapping out for lift-off A/B runs).  Returns
NAV_MASK_ALL when the cvar is off (legacy behavior: every link is
path-eligible).  Takes the bot so a future per-bot capability-parity harness
(bot_skilltest-style) gets the right signature for free.
=================
*/
int Bot_NavMask (bot_t *b)
{
	int	mask = NAV_MASK_ALL;

	if (bot_navmask->value == 0)
		return mask;
	if (bot_swim->value == 0)
		mask &= ~NAV_MASK (NAV_LINK_WATER);
	if (bot_lift->value == 0)
		mask &= ~NAV_MASK (NAV_LINK_PLAT);
	if (bot_train->value == 0)
		mask &= ~NAV_MASK (NAV_LINK_TRAIN);
	return mask;
}

/*
=================
Bot_NotePain

Hooked from player_pain (p_client.c, empty in vanilla): a bot that takes
damage learns who hurt it.  Combat_FindEnemy treats a recent attacker as
acquirable regardless of the bot_fov view cone -- getting shot turns you
around (with the normal aim turn dynamics, not a snap).
=================
*/
void Bot_NotePain (edict_t *self, edict_t *attacker)
{
	int		i;
	bot_t	*b;

	if (!self || !self->client || !attacker || !attacker->client
		|| attacker == self)
		return;
	i = self - g_edicts - 1;
	if (i < 0 || i >= game.maxclients)
		return;
	b = &bots[i];
	if (!b->inuse || b->ent != self)
		return;
	b->threat_ent = attacker;
	b->threat_time = level.time;
}

/*
=================
Bot_NoteDeath

Hooked from player_die (p_client.c): stash the killing MOD_* while it is
still fresh.  The bot layer notices death by polling deadflag at the top of
the NEXT G_RunFrame, by which time the global meansOfDeath may have been
overwritten by later damage in the same frame -- so capture it at die time.
Feeds the bot_hazard environmental-death nav feedback; no-op for non-bots.
=================
*/
void Bot_NoteDeath (edict_t *self, int mod)
{
	int		i;
	bot_t	*b;

	if (!self || !self->client)
		return;
	i = self - g_edicts - 1;
	if (i < 0 || i >= game.maxclients)
		return;
	b = &bots[i];
	if (!b->inuse || b->ent != self)
		return;
	b->death_mod = mod & ~MOD_FRIENDLY_FIRE;
}

/*
=================
Bot_NoteNoise / Bot_NoiseTime

Hooked from PlayerNoise (p_weapon.c) before its deathmatch early-out: any
client's unsilenced weapon fire is "heard".  Combat_FindEnemy uses it so a
bot_fov bot can acquire a visible shooter outside its view cone -- Q2 weapons
are loud, and a human absolutely turns toward gunfire behind them.
=================
*/
void Bot_NoteNoise (edict_t *who)
{
	// weapon fire keeps its own dedicated slot so the Combat_FindEnemy
	// cone-bypass consumer is byte-identical to the pre-bot_hearing bot
	Bot_NoteNoiseEx (who, NOISE_WEAPON, who ? who->s.origin : NULL);
}

/*
=================
Bot_NoteNoiseEx

Record a noise: what kind, and the origin AT THE MOMENT IT HAPPENED.  Storing
the position rather than the entity is deliberate -- a listener may later walk
to where the sound came from and find nobody there, which is the correct
outcome.  Nothing here reads bot_hearing: the registry is written unconditionally
(it is inert unless something consumes it) so the write path stays branch-free
and the legacy weapon slot behaves exactly as before.
=================
*/
void Bot_NoteNoiseEx (edict_t *who, int kind, vec3_t origin)
{
	int	i;

	if (!who || !who->client || !origin)
		return;
	i = who - g_edicts - 1;
	if (i < 0 || i >= MAX_CLIENTS)
		return;

	if (kind == NOISE_WEAPON)
		bot_noise_time[i] = level.time;		// legacy slot: weapon fire only

	// footsteps are constant, so they are both throttled and outranked -- a
	// footfall must never overwrite the gunshot that just told us more
	if (kind == NOISE_STEP)
	{
		if (level.time - bot_noise_step[i] < BOT_NOISE_STEP_THROTTLE)
			return;
		bot_noise_step[i] = level.time;
		if (bot_noise_kind[i] != NOISE_STEP
			&& level.time - bot_noise_at[i] < BOT_NOISE_LOUD_HOLD)
			return;
	}

	bot_noise_at[i]   = level.time;
	bot_noise_kind[i] = kind;
	VectorCopy (origin, bot_noise_pos[i]);
}

float Bot_NoiseTime (edict_t *who)
{
	int	i;

	if (!who || !who->client)
		return -9999;
	i = who - g_edicts - 1;
	if (i < 0 || i >= MAX_CLIENTS)
		return -9999;
	return bot_noise_time[i];
}

/*
=================
Bot_CountActive
=================
*/
static int Bot_CountActive (void)
{
	int i, n = 0;
	for (i = 0; i < game.maxclients; i++)
		if (bots[i].inuse)
			n++;
	return n;
}

/*
=================
Bot_DumpSlots  (bot_slotlog diagnostic)
=================
*/
static void Bot_DumpSlots (const char *tag)
{
	int			i, botidx;
	edict_t		*ent;
	gclient_t	*cl;

	if (!bot_slotlog || bot_slotlog->value == 0)
		return;

	gi.dprintf ("=== SLOTS [%s] map=%s time=%.2f ===\n", tag, level.mapname, level.time);
	for (i = 0; i < game.maxclients; i++)
	{
		ent = g_edicts + 1 + i;
		cl  = &game.clients[i];

		botidx = -1;
		if (bots[i].inuse)
			botidx = bots[i].id;

		if (!cl->pers.connected && !ent->inuse && botidx < 0)
			continue;	// wholly empty slot, skip the noise

		gi.dprintf ("  [%d] inuse=%d conn=%d name='%s' bots[].inuse=%d id=%d\n",
			i, ent->inuse ? 1 : 0, cl->pers.connected ? 1 : 0,
			cl->pers.netname, bots[i].inuse ? 1 : 0, botidx);
	}
}

/*
=================
Bot_SlotHeldByHuman

A client slot can belong to a real, connected human whose player edict is not
yet `inuse` -- most importantly the listen-server host across a `gamemap`.  The
engine does NOT re-run ClientConnect on a level change ("Changing levels will
NOT cause this to be called again", p_client.c), so the host's real userinfo is
never re-applied, yet the host stays connected and keeps its client slot.  If
Bot_Add grabbed that slot in the frames before the host's ClientBegin it would
overwrite game.clients[i].pers with bot data (netname "OzBot<id>"); because the
host's userinfo is never re-sent, the netname-release guard in Bot_RunFrame
could then never detect the collision and the bot think-loop would drive
(puppet) the human.  Bots always carry an "OzBot" netname, so a *connected*
slot whose name is NOT ours is a human we must never claim.  Dedicated fastsim
servers have no connected humans at map start, so this is inert there (the
same-seed measurement gate is unaffected).
=================
*/
static qboolean Bot_SlotHeldByHuman (int i)
{
	gclient_t	*cl = &game.clients[i];

	if (!cl->pers.connected)
		return false;
	return strncmp (cl->pers.netname, "OzBot", 5) != 0;
}

/*
=================
Bot_Add
=================
*/
static qboolean Bot_Add (void)
{
	edict_t	*ent;
	int		i;
	char	userinfo[MAX_INFO_STRING];
	char	name[32];
	bot_t	*b;

	if (!deathmatch->value)
	{
		gi.dprintf ("ozbot: bots require deathmatch 1\n");
		return false;
	}

	for (i = 0; i < game.maxclients; i++)
	{
		// On a listen server the local host permanently owns client slot 0.
		// Reserve it: a bot must never claim slot 0 even in the frames after a
		// map load before the host's ClientBegin marks the edict inuse -- that
		// window is exactly when a bot would overwrite the host's pers and
		// puppet them across a `gamemap` (confirmed by the bot_slotlog trace).
		// Dedicated servers have no local host (dedicated 1), so slot 0 is a
		// normal bot slot there and the fastsim measurement rig is unaffected.
		if (i == 0 && !dedicated->value)
			continue;

		ent = g_edicts + 1 + i;
		// Also skip any *other* connected human (a remote player whose edict
		// has not begun yet -- see Bot_SlotHeldByHuman).
		if (!ent->inuse && !bots[i].inuse && !Bot_SlotHeldByHuman (i))
			break;
	}
	if (i == game.maxclients)
		return false;	// no free slots

	b = &bots[i];
	memset (b, 0, sizeof(*b));
	b->ent = ent;
	b->id  = bot_next_id++;
	// bot_skilltest: genuine head-to-head A/B for bot_skill's effect (never
	// properly measured -- an open question since Phase 4). Splits bots by id
	// parity within one match rather than trusting symmetric self-play, which
	// can't tell "better" from "just more combat activity" (see bot_claim's
	// weapon-priority test in ozbot-demo-combat-calibration for why that
	// distinction mattered there).
	b->skill_ovr = (bot_skilltest->value != 0)
		? (((b->id % 2) == 0) ? 0.9f : 0.1f)
		: -1.0f;

	Com_sprintf (name, sizeof(name), "OzBot%d", b->id);
	userinfo[0] = 0;
	Info_SetValueForKey (userinfo, "name", name);
	Info_SetValueForKey (userinfo, "skin", "male/grunt");
	Info_SetValueForKey (userinfo, "hand", "2");
	Info_SetValueForKey (userinfo, "fov", "90");

	if (!ClientConnect (ent, userinfo))
	{
		gi.dprintf ("ozbot: ClientConnect refused bot\n");
		memset (b, 0, sizeof(*b));
		return false;
	}

	ClientBegin (ent);

	b->inuse = true;
	b->was_dead = false;
	b->next_wander_time = level.time;
	Bot_ResetNavState (b);

	Bot_LogEvent (b, "spawn");
	gi.bprintf (PRINT_HIGH, "ozbot: %s added\n", name);
	return true;
}

/*
=================
Bot_RemoveSlot
=================
*/
static void Bot_RemoveSlot (int i)
{
	bot_t	*b = &bots[i];
	edict_t	*ent;

	if (!b->inuse)
		return;

	ent = b->ent;
	Bot_LogEvent (b, "remove");

	if (ent && ent->inuse)
		ClientDisconnect (ent);

	memset (b, 0, sizeof(*b));
}

/*
=================
Bot_RemoveOne
=================
*/
static qboolean Bot_RemoveOne (void)
{
	int i;
	for (i = game.maxclients - 1; i >= 0; i--)
	{
		if (bots[i].inuse)
		{
			Bot_RemoveSlot (i);
			return true;
		}
	}
	return false;
}

/*
=================
Bot_ClearAll
=================
*/
static void Bot_ClearAll (void)
{
	int i;
	for (i = 0; i < MAX_CLIENTS; i++)
		memset (&bots[i], 0, sizeof(bots[i]));
}

//==========================================================================
// navigation behavior
//==========================================================================

/*
=================
Bot_GoExplore

Drop into wander mode for a short while (grows the graph between goals).
=================
*/
static void Bot_GoExplore (bot_t *b)
{
	// briefly avoid re-fixating on whatever item we were chasing (also spreads
	// bots across items); harmless if we just picked it up (it's respawning)
	if (b->goal_item)
		Goal_Blacklist (b->goal_item, BOT_ITEM_COOLDOWN);

	b->mode      = BOT_MODE_EXPLORE;
	b->goal_node = -1;
	b->goal_item = NULL;
	b->goal_timing = false;
	b->goal_cost = 0;
	b->path_len  = 0;
	b->path_idx  = 0;
	b->replan_time   = level.time + 1.0 + random() * 2.0;
	b->progress_time = level.time;
	b->reroute_fired = false;	// fresh leg: mid-attempt reroute may fire again
	b->reroute_last_idx = 0;
	b->reroute_idx_time = level.time;
	b->steer_item    = NULL;	// fresh explore leg: no stale steering target
	// bot_pursuit: every goal teardown ends any chase riding on that goal leg.
	// The sighting is dropped with it so a chase can never be resumed from a
	// stale last-known position after the bot has moved on to something else.
	b->pursuing     = false;
	b->pursue_until = 0;
	b->pursue_began = 0;
	b->lkp_ent      = NULL;
	b->lkp_time     = 0;
	Bot_LiftReset (b);
	Bot_TrainReset (b);
	Bot_LadderReset (b);
	Bot_StrafeReset (b);
	Bot_PlaybackReset (b);
}

/*
=================
Bot_DecisiveReplan

bot_decisive: don't stand around after a goal ends -- re-pick promptly.  The
10s item blacklist applied by Bot_GoExplore is what actually prevents
re-choosing the abandoned item; the 1-3s wander pause GoExplore schedules on
top of it read as indecision (measured: 24% of goal transitions were >=1s
standing re-decides, median 2.3s, with the view swinging between candidates).
Called AFTER Bot_GoExplore so the off-arm RNG stream is untouched -- the
random() in GoExplore still runs, its result is just overwritten.
=================
*/
static void Bot_DecisiveReplan (bot_t *b, float delay)
{
	if (bot_decisive->value != 0)
		b->replan_time = level.time + delay;
}

/*
=================
Bot_PursueEnd

bot_pursuit: tear down an active chase and hand the frame back to the item
economy.  Deliberately does NOT blacklist anything -- no item was involved, so
the goal-failure ladder must stay out of this.  Invalidating lkp_time is what
enforces "one sight loss buys one chase": without it a bot that timed out at a
last-known position would immediately re-commit to the same stale spot.
=================
*/
static void Bot_PursueEnd (bot_t *b, const char *reason)
{
	if (!b->pursuing)
		return;
	Bot_LogPursueEnd (b, reason, level.time - b->pursue_began);
	b->lkp_ent  = NULL;
	b->lkp_time = 0;			// consumed: this sighting cannot fund another chase
	Bot_GoExplore (b);			// clears pursuing/pursue_until with the goal leg
	Bot_DecisiveReplan (b, 0.2f);
}

/*
=================
Bot_NoiseRadius
=================
*/
static float Bot_NoiseRadius (int kind)
{
	switch (kind)
	{
	case NOISE_WEAPON:	return BOT_NOISE_R_WEAPON;
	case NOISE_PICKUP:	return BOT_NOISE_R_PICKUP;
	case NOISE_PAIN:	return BOT_NOISE_R_PAIN;
	case NOISE_STEP:	return BOT_NOISE_R_STEP;
	}
	return 0;
}

/*
=================
Bot_HearThink

bot_hearing: turn a recent audible noise into a pursuit cue.  Runs only when
the bot has nothing better to go on (no visible enemy, no chase already in
flight, no fresh sighting) -- hearing supplements sight, it never overrides it.

The cue is written as an ordinary last-known position with ZERO velocity: we
know a sound happened at a spot, and nothing whatsoever about where the
noisemaker went next.  Every pursuit gate (cost, strength, freshness, item
priority) then applies unchanged, so hearing cannot buy a chase that sight
could not.  Inert without bot_pursuit by construction -- it only writes fields
that the pursuit layer reads.
=================
*/
static void Bot_HearThink (bot_t *b)
{
	edict_t	*self = b->ent;
	vec3_t	eyes, d;
	int		i;
	float	best = 0, dist;
	int		bestn = -1;

	if (!bot_hearing || bot_hearing->value == 0)
		return;
	if (!Combat_PursuitOn (b))
		return;					// the pursuit layer is what consumes this
	if (b->enemy || b->pursuing || b->flee)
		return;
	if (b->lkp_time && level.time - b->lkp_time <= BOT_PURSUE_FRESH)
		return;					// a real sighting outranks anything we heard

	VectorCopy (self->s.origin, eyes);
	eyes[2] += self->viewheight;

	for (i = 0; i < game.maxclients; i++)
	{
		edict_t	*who = &g_edicts[i + 1];
		float	radius;

		if (bot_noise_kind[i] == NOISE_NONE)
			continue;
		if (level.time - bot_noise_at[i] > BOT_NOISE_FRESH)
			continue;
		if (who == self || !who->inuse || !who->client)
			continue;
		if (who->deadflag || who->health <= 0 || who->client->resp.spectator)
			continue;

		radius = Bot_NoiseRadius (bot_noise_kind[i]);
		VectorSubtract (bot_noise_pos[i], eyes, d);
		dist = VectorLength (d);
		if (dist > radius)
			continue;
		if (!gi.inPHS (eyes, bot_noise_pos[i]))
			continue;			// sealed off: the sound never reached us

		// nearest audible noise wins
		if (bestn < 0 || dist < best)
		{
			best = dist;
			bestn = i;
		}
	}

	if (bestn < 0)
		return;

	b->lkp_ent = &g_edicts[bestn + 1];
	VectorCopy (bot_noise_pos[bestn], b->lkp_pos);
	VectorClear (b->lkp_vel);	// a sound has no heading
	b->lkp_time  = level.time;
	b->lkp_noise = true;
	if (bot_hearlog && bot_hearlog->value != 0)
		Bot_LogHear (b, bot_noise_kind[bestn], best);
}

/*
=================
Bot_PursueTry

bot_pursuit: decide whether to spend travel investigating where an enemy was
last seen, and commit the chase through the ordinary GOAL machinery (path,
steering, stuck recovery and reroute all then apply unchanged -- the only
difference from an item route is goal_item == NULL and the pursue_until clock).

Every gate here is a bound on over-investment.  Returns true if a chase started.
=================
*/
static qboolean Bot_PursueTry (bot_t *b)
{
	edict_t	*ent = b->ent;
	edict_t	*foe = b->lkp_ent;
	vec3_t	tgt, lead, d;
	float	len, cost;
	int		start, goal, len_path;

	if (!Combat_PursuitOn (b))
		return false;
	if (b->pursuing || b->enemy || b->flee)
		return false;			// already chasing / can see them / running away
	if (!b->lkp_time || level.time - b->lkp_time > BOT_PURSUE_FRESH)
		return false;			// no sighting, or too stale to be worth walking to
	if (!foe || !foe->inuse || !foe->client || foe->deadflag || foe->health <= 0
		|| foe->client->resp.spectator)
		return false;			// they died or left: nothing to find
	if (Combat_Strength (ent) < BOT_PURSUE_MINSTR)
		return false;			// too weak to want the fight we'd be walking into
	if (Combat_BlasterTransitOn (b))
		return false;			// blaster-only: fetching a real weapon outranks a chase
	if (b->goal_timing)
		return false;			// camped on a respawn timing: don't break the wait

	// an item we're already nearly on top of beats a chase -- finishing the
	// cheap thing first is the same logic bot_commit applies to goal selection
	if (b->goal_item && Goal_ItemAvailable (b->goal_item))
	{
		VectorSubtract (b->goal_item->s.origin, ent->s.origin, d);
		if (VectorLength (d) < BOT_PURSUE_NEARGOAL)
			return false;
	}

	// aim at where they were going, not where they were: a short velocity
	// extrapolation, clamped so a fast mover can't fling the target point into
	// somewhere they never went
	VectorScale (b->lkp_vel, BOT_PURSUE_EXTRAP, lead);
	len = VectorLength (lead);
	if (len > BOT_PURSUE_EXTRAP_MAX)
		VectorScale (lead, BOT_PURSUE_EXTRAP_MAX / len, lead);
	VectorAdd (b->lkp_pos, lead, tgt);

	goal = Nav_NearestGoalNode (tgt);
	if (goal < 0)
		goal = Nav_NearestGoalNode (b->lkp_pos);	// extrapolated spot is off-graph
	if (goal < 0)
		return false;

	start = Nav_NearestNode (ent->s.origin);
	if (start < 0 || goal == start)
		return false;			// already there: nothing to walk to

	len_path = Nav_FindPathMasked (start, goal, Bot_NavMask (b), b->path, BOT_MAX_PATH);
	if (len_path <= 1)
		return false;			// no route
	cost = Nav_LastPathCost ();
	if (bot_pursuitcost->value > 0 && cost > bot_pursuitcost->value)
		return false;			// too expensive: the item economy is worth more

	// commit exactly like the ordinary goal-commit block below
	b->path_len = len_path;
	b->path_idx = 0;
	b->goal_node = goal;
	b->goal_item = NULL;		// a chase owns no item: never blacklists on exit
	b->mode = BOT_MODE_GOAL;
	b->replan_time   = level.time + 0.5f;
	b->progress_time = level.time;
	b->goal_time     = level.time;
	b->goal_cost     = cost;
	b->goal_best     = 99999;
	b->reroute_fired = false;
	b->reroute_last_idx = 0;
	b->reroute_idx_time = level.time;
	Bot_LiftReset (b);
	Bot_TrainReset (b);
	Bot_LadderReset (b);
	Bot_StrafeReset (b);
	Bot_PlaybackReset (b);

	b->pursuing = true;
	b->pursue_began = level.time;
	// the wall clock scales with the route (a long chase is allowed longer) but
	// is capped hard -- a chase that outlives the cap has lost them
	b->pursue_until = level.time + 2.0f + cost / BOT_GOAL_BUDGET_SPEED;
	if (b->pursue_until > level.time + BOT_PURSUE_MAXSECS)
		b->pursue_until = level.time + BOT_PURSUE_MAXSECS;

	VectorSubtract (tgt, ent->s.origin, d);
	Bot_LogPursueStart (b, cost, VectorLength (d), b->lkp_noise ? "noise" : "sight");
	return true;
}

/*
=================
Bot_PickGoal

Pick a distant node to travel to (farthest of a few random samples).
=================
*/
static int Bot_PickGoal (int start)
{
	int		idx;

	if (nav.num_nodes < 2)
		return -1;

	// uniform-random node so the reach metric reflects general navigation
	// (not only longest-distance trips)
	idx = (int)(random() * nav.num_nodes);
	if (idx < 0)
		idx = 0;
	if (idx >= nav.num_nodes)
		idx = nav.num_nodes - 1;
	if (idx == start)
		idx = (idx + 1) % nav.num_nodes;
	// bot_hazard: never roam INTO a lava/hurt/slime node (q2dm6 solo was 95%
	// roam arrivals -- and dozens of roam targets sat inside the pools; a
	// slime bath for no goal is pure health tax).  Deterministic stride
	// re-probe: no extra random() draws, so the RNG stream stays aligned
	// with the off state by construction.
	if (bot_hazard->value != 0)
	{
		int	guard;
		for (guard = 0; guard < 16
			&& (nav.nodes[idx].flags & (NAV_FLAG_HAZARD | NAV_FLAG_SLIME)); guard++)
			idx = (idx + 1) % nav.num_nodes;
	}
	return idx;
}

/*
=================
Bot_UpdateStuck

Tracks progress; returns true if the bot hasn't moved meaningfully for a while.
=================
*/
static qboolean Bot_UpdateStuck (bot_t *b)
{
	vec3_t	d;
	float	moved;

	VectorSubtract (b->ent->s.origin, b->last_pos, d);
	moved = VectorLength (d);
	if (moved > 24)
	{
		b->progress_time = level.time;
		VectorCopy (b->ent->s.origin, b->last_pos);
	}
	{
		float thresh = bot_stucktime->value;
		if (thresh <= 0)
			thresh = 1.0f;		// guard: 0 would fire every frame
		return (level.time - b->progress_time) > thresh;
	}
}

/*
=================
Bot_Navigate

Per-frame: learn the graph, then either follow a path to a goal or wander
(exploring) until a goal can be chosen.
=================
*/
static void Bot_Navigate (bot_t *b)
{
	edict_t		*ent = b->ent;
	qboolean	ready = (nav.num_nodes >= BOT_GRAPH_READY);
	qboolean	stuck;
	int			link;

	b->want_jump = false;	// steering sets this per-frame as needed

	// ---- learn the graph from where the bot actually is ----
	link = NAV_LINK_WALK;
	if (b->prev_node >= 0 && b->prev_node < nav.num_nodes)
	{
		float dz = ent->s.origin[2] - nav.nodes[b->prev_node].origin[2];
		if (dz < -40)
			link = NAV_LINK_FALL;					// dropped down
		else if (b->did_jump && dz > 24)
			link = NAV_LINK_JUMP;					// jumped up onto something
		// carried up while standing on a func_plat: a plat link, one-way
		// (takes priority over jump -- the plat did the lifting)
		if (bot_lift->value != 0 && dz > 24
			&& ent->groundentity && ent->groundentity->classname
			&& strcmp (ent->groundentity->classname, "func_plat") == 0)
			link = NAV_LINK_PLAT;
	}
	b->prev_node = Nav_LearnStep (ent, b->prev_node, link);
	b->cur_node  = b->prev_node;
	if (ent->groundentity)
		b->did_jump = false;	// landed; jump (if any) has been accounted for

	stuck = Bot_UpdateStuck (b);

	// ---- goal-seeking ----
	// flee decay: combat only clears the flag while an enemy is visible, so
	// once we've shaken the pursuer, drop it as soon as toughness is back
	if (b->flee && !b->enemy && Combat_Strength (ent) > 80)
		b->flee = false;

	// bot_hearing feeds the same last-known-position slot the sight path writes,
	// on the 10Hz decision cadence (a per-tick scan would be 4x the work for no
	// extra information -- noises persist for BOT_NOISE_FRESH).
	if (FRAMESYNC)
		Bot_HearThink (b);

	// ---- bot_pursuit ----
	// An active chase ends the moment it stops paying; otherwise consider
	// starting one.  Both paths fall through: a torn-down chase leaves the bot
	// in EXPLORE and lands in the wander/goal-selection section below, and a
	// freshly committed chase is followed by the GOAL block this same frame.
	if (b->pursuing)
	{
		if (b->enemy)
			Bot_PursueEnd (b, "reacquired");	// found them: combat owns it now
		else if (b->flee)
			Bot_PursueEnd (b, "flee");
		else if (!b->lkp_ent || !b->lkp_ent->inuse || b->lkp_ent->deadflag
			|| b->lkp_ent->health <= 0)
			Bot_PursueEnd (b, "target_died");
		else if (level.time > b->pursue_until)
			Bot_PursueEnd (b, "timeout");
	}
	else
		Bot_PursueTry (b);

	if (b->mode == BOT_MODE_GOAL && b->goal_node >= 0 && b->goal_node < nav.num_nodes)
	{
		// note: a fleeing bot deliberately does NOT abandon its current goal --
		// that was tried and the abandon/blacklist/re-pick churn cost ~7 ITEM%
		// points while contributing nothing measurable to survival (the escape
		// comes from the retreat movement; flee only re-weights NEW goal picks
		// toward health/armor via Item_Score)

		// track closest approach to the goal node (diagnostics)
		{
			vec3_t	gd;
			float	ghd;
			VectorSubtract (nav.nodes[b->goal_node].origin, ent->s.origin, gd);
			gd[2] = 0;
			ghd = VectorLength (gd);
			if (ghd < b->goal_best)
				b->goal_best = ghd;
		}

		// resolve item state: grabbed/taken, or still waiting on a respawn
		if (b->goal_item)
		{
			qboolean avail = Goal_ItemAvailable (b->goal_item);

			if (b->goal_timing)
			{
				// pre-positioning for a respawn: stop "timing" once it's live
				if (avail)
					b->goal_timing = false;
				// otherwise keep navigating to the spot and wait (below)
			}
			else if (!avail)
			{
				// a live item we were chasing vanished: we grabbed it (if we're
				// on it) or someone else took it
				vec3_t	dv;
				float	dd;
				const char *nm = Bot_ItemName (b->goal_item);
				VectorSubtract (b->goal_item->s.origin, ent->s.origin, dv);
				dd = VectorLength (dv);
				Bot_LogItemEvent (b, (dd < 96) ? "pickup" : "item_lost", nm);
				Goal_ItemSucceeded (b->goal_item);	// either way, someone collected it
				Bot_GoExplore (b);
				Bot_DecisiveReplan (b, 0.2f);	// success: next objective, now
				Bot_Wander (b);
				return;
			}
		}

		// mid-attempt reroute (bot_reroutemid): the full goal budget can run 15s
		// before a giveup penalizes the stalled hop.  If path_idx stops advancing
		// for a few seconds while we're NOT fighting and NOT inside a lift/playbook
		// maneuver (those legitimately stand still), the current hop is likely an
		// "A* sold a route the bot can't execute" edge -- penalize it and repath
		// NOW, once per attempt, so we don't burn the whole budget re-selling it.
		if (bot_reroutemid && bot_reroutemid->value != 0 && !b->reroute_fired
			&& !b->enemy && b->lift_state == LIFT_NONE && b->pb_state == PB_NONE
			&& b->path_idx > 0 && b->path_idx < b->path_len)
		{
			if (b->path_idx != b->reroute_last_idx)
			{
				b->reroute_last_idx = b->path_idx;
				b->reroute_idx_time = level.time;
			}
			else if (level.time - b->reroute_idx_time > BOT_REROUTEMID_STALL)
			{
				Nav_PenalizeLink (b->path[b->path_idx - 1], b->path[b->path_idx]);
				Bot_LogPenalize (b, b->path[b->path_idx - 1], b->path[b->path_idx]);
				b->reroute_fired = true;
				b->path_len = 0;	// force a fresh A* around the penalized hop
				b->path_idx = 0;
			}
		}

		// give up on a goal we can't reach so we don't loop on it forever.
		// A chase is exempt: pursue_until above is its (shorter) clock, and
		// routing a pursuit through the item-giveup ladder would log phantom
		// giveups and penalize links over a goal that was never an item.
		if (!b->pursuing)
		{
			float budget = BOT_GOAL_TIMEOUT;
			if (bot_goalbudget->value != 0 && b->goal_cost > 0)
			{
				float cap = (bot_budgetcap->value > 0) ? bot_budgetcap->value : BOT_GOAL_BUDGET_MAX;
				budget = BOT_GOAL_BUDGET_BASE + b->goal_cost / BOT_GOAL_BUDGET_SPEED;
				if (budget > cap)
					budget = cap;
			}
			if (level.time - b->goal_time > budget)
			{
			vec3_t	tgt, d;
			int		atnode = (b->path_idx >= b->path_len) ? 1 : 0;
			int		navq = NAVQ_OK;
			if (b->goal_item)
				VectorCopy (b->goal_item->s.origin, tgt);
			else if (b->goal_node >= 0 && b->goal_node < nav.num_nodes)
				VectorCopy (nav.nodes[b->goal_node].origin, tgt);
			else
				VectorCopy (ent->s.origin, tgt);
			VectorSubtract (tgt, ent->s.origin, d);
			// oracle verdict at giveup time (plans/nav-oracle.md Phase C):
			// "ok" = a route still existed and execution failed; "no_path" =
			// the route evaporated (penalization pruned it mid-attempt)
			if (b->goal_item)
				navq = Nav_QueryPath (ent->s.origin, b->goal_item->s.origin,
					NAV_MASK_ALL, NULL, NULL);
			Bot_LogGiveup (b, (float)sqrt(d[0]*d[0] + d[1]*d[1]), d[2],
				atnode, b->enemy ? 1 : 0,
				b->goal_item ? Nav_QueryName (navq) : "");
			// execution-failure reroute (bot_reroute): we stalled mid-route
			// (not at the goal node) while a route still existed (navq ok) and
			// were not in a fight -- the "graph claims a route the bot can't
			// execute" case.  Penalize the hop we could not get past so the NEXT
			// attempt reroutes around it instead of re-selling the same
			// unexecutable link.  Gated on !enemy so combat-interrupted giveups
			// (20% of them) never erode a good link.
			if (bot_reroute && bot_reroute->value != 0 && !atnode
				&& navq == NAVQ_OK && !b->enemy
				&& b->path_idx > 0 && b->path_idx < b->path_len)
			{
				Nav_PenalizeLink (b->path[b->path_idx - 1], b->path[b->path_idx]);
				Bot_LogPenalize (b, b->path[b->path_idx - 1], b->path[b->path_idx]);
			}
			if (b->goal_item)
				Goal_ItemFailed (b->goal_item, navq);
			Bot_GoExplore (b);
			Bot_DecisiveReplan (b, 0.6f);	// failure: brief settle, then move on
			Bot_Wander (b);
			return;
			}
		}

		if (b->path_len <= 0 || b->path_idx >= b->path_len)
		{
			int start = Nav_NearestNode (ent->s.origin);
			b->path_len = Nav_FindPathMasked (start, b->goal_node, Bot_NavMask (b), b->path, BOT_MAX_PATH);
			b->path_idx = 0;
			if (b->path_len <= 0)
			{
				Bot_LogEvent (b, "pathfail");
				Bot_GoExplore (b);
				Bot_DecisiveReplan (b, 0.6f);
				Bot_Wander (b);
				return;
			}
		}

		// lift riding: when a plat hop is in play the controller owns the
		// movement intent.  Waiting counts as progress (no stuck recovery)
		// and isn't billed to the goal budget -- deliberate stillness is the
		// whole capability (plans/lift-riding.md).
		if (bot_lift->value != 0)
		{
			if (Bot_LiftThink (b))
			{
				b->progress_time = level.time;
				b->goal_time += FRAMETIME;
				b->pursue_until += FRAMETIME;	// chase clock freezes too
				if (b->sj_state != SJ_NONE)
					Bot_StrafeReset (b);	// the lift owns the frame now
				return;
			}
		}
		else if (b->lift_state != LIFT_NONE)
			Bot_LiftReset (b);	// cvar turned off mid-attempt: clear stale state

		// func_train riding: same deal as the lift for a horizontal shuttle hop
		if (bot_train->value != 0)
		{
			if (Bot_TrainThink (b))
			{
				b->progress_time = level.time;
				b->goal_time += FRAMETIME;
				b->pursue_until += FRAMETIME;	// chase clock freezes too
				if (b->sj_state != SJ_NONE)
					Bot_StrafeReset (b);	// the train owns the frame now
				return;
			}
		}
		else if (b->train_state != TRAIN_NONE)
			Bot_TrainReset (b);	// cvar turned off mid-attempt: clear stale state

		// playbook maneuvers (bot_playbook): when a recorded-move hop is next
		// on the path the controller owns movement AND facing.  Budget clock
		// freezes like a lift (the align-up wait must not be billed).
		// MUST run before the ladder controller: playbook exits are often
		// elevated (e.g. q2dm3 mh_ladder), so LadderThink would otherwise
		// steal every frame near the mouth and ALIGN never reaches engage.
		if (Bot_PlaybackThink (b))
		{
			b->progress_time = level.time;
			b->goal_time += FRAMETIME;
			b->pursue_until += FRAMETIME;	// chase clock freezes too
			if (b->sj_state != SJ_NONE)
				Bot_StrafeReset (b);	// the replay owns the frame now
			if (b->on_ladder)
				Bot_LadderReset (b);	// recorded climb owns upmove via pb_cmd
			b->want_jump = false;		// path-fidget JUMP must not bounce ALIGN
			return;
		}

		// ladder climbing: on a ladder with a higher/lower goal, the controller
		// owns movement (face + climb).  Frozen budget/stuck like the lift.
		if (bot_ladder->value != 0)
		{
			if (Bot_LadderThink (b))
			{
				b->progress_time = level.time;
				b->goal_time += FRAMETIME;
				b->pursue_until += FRAMETIME;	// chase clock freezes too
				if (b->sj_state != SJ_NONE)
					Bot_StrafeReset (b);	// the ladder owns the frame now
				return;
			}
		}
		else if (b->on_ladder)
			Bot_LadderReset (b);	// cvar turned off mid-climb: clear stale state

		// slime escape (bot_slimeescape): submerged in slime with no ladder in
		// reach -- abandon the item goal and swim to the nearest dry node.  Runs
		// AFTER the ladder controller so an adjacent ladder wins (it climbs out
		// directly).  Sets move_dir/move_yaw; Bot_ApplyMovement (in Bot_Think)
		// turns the upward intent into swim upmove.  Counts as progress so the
		// active climb-out isn't mistaken for a stall.
		if (Bot_SlimeEscapeThink (b))
		{
			b->progress_time = level.time;
			return;
		}

		// strafe jumping: on a qualified runway the controller owns movement
		// AND facing.  Unlike the lift, the goal budget keeps billing (we are
		// travelling faster than budgeted) and stuck detection stays live
		// (40-55u/tick never trips it; the controller's own aborts fire first).
		if (Bot_StrafeThink (b))
			return;

		if (!Bot_FollowPath (b))
		{
			// arrived at the goal node
			if (b->goal_item && (Goal_ItemAvailable (b->goal_item) || b->goal_timing))
			{
				// home in on the item (or hold on the spot while we wait for it
				// to respawn) until we touch it or time out
				qboolean waiting = b->goal_timing && !Goal_ItemAvailable (b->goal_item);
				Bot_SteerToPoint (b, b->goal_item->s.origin);
				if (waiting)
					Bot_Fidget (b, b->goal_item->s.origin);	// humanization: no statue waits
				return;
			}
			// chase reached the last-known position without finding them
			if (b->pursuing)
			{
				Bot_PursueEnd (b, "arrived");
				Bot_Wander (b);
				return;
			}
			// roam node reached
			Bot_LogEvent (b, "reach");
			Bot_GoExplore (b);
			Bot_DecisiveReplan (b, 0.2f);	// arrival: next objective, now
			Bot_Wander (b);
			return;
		}

		if (stuck)
		{
			if (bot_rollout->value != 0)
				Bot_RolloutRecover (b);
			else
				Bot_Unstick (b);
			if (level.time >= b->replan_time)
			{
				int start;
				// penalize the segment we keep failing to traverse so the replan
				// routes around it (the graph learns untraversable links)
				if (b->path_idx > 0 && b->path_idx < b->path_len)
				{
					Nav_PenalizeLink (b->path[b->path_idx - 1], b->path[b->path_idx]);
					Bot_LogPenalize (b, b->path[b->path_idx - 1], b->path[b->path_idx]);
				}
				start = Nav_NearestNode (ent->s.origin);
				b->path_len = Nav_FindPathMasked (start, b->goal_node, Bot_NavMask (b), b->path, BOT_MAX_PATH);
				b->path_idx = 0;
				b->replan_time = level.time + 1.5;
				if (b->path_len <= 0)
				{
					Bot_LogEvent (b, "pathfail");
					Bot_GoExplore (b);
					Bot_DecisiveReplan (b, 0.6f);
					Bot_Wander (b);
				}
			}
			return;
		}

		// final approach: home straight in only when the item is close AND we
		// have a clear shot to it; otherwise keep following the learned path
		// (which now ends at the item) so we don't wall-bump around corners
		if (b->goal_item && (Goal_ItemAvailable (b->goal_item) || b->goal_timing))
		{
			vec3_t	dv;
			VectorSubtract (b->goal_item->s.origin, ent->s.origin, dv);
			// (bot_lift) only home when the item is on our level: the 2D-only
			// check trapped bots directly UNDER elevated items (Phase 0: 6/8
			// GL giveups orbited beneath it, path progress frozen, while the
			// route to the lift ran the other way)
			if (bot_lift->value == 0 || fabs (dv[2]) < 64)
			{
				dv[2] = 0;
				if (VectorLength (dv) < 200)
				{
					// clear-shot gate (bot_losfinal): a same-level item within
					// 200u is NOT always reachable in a straight line -- q2dm1's
					// HyperBlaster sits behind a wall from the CG platform, and
					// homing 2D at it drove the bot into the wall forever (the
					// nav path, the ramp around, was ignored).  Trace to the item;
					// if something solid is between us, DON'T home -- fall through
					// to the path follower (move_dir already points along it).
					qboolean clear = true;
					if (bot_losfinal && bot_losfinal->value != 0)
					{
						// trace at knee height between two raised points so a wall
						// (full-height) blocks it but a floor lip / step near the
						// item does not (that would spuriously refuse a reachable
						// grab and cost pickups)
						vec3_t	from, to;
						trace_t	tr;
						VectorCopy (ent->s.origin, from);       from[2] += 18;
						VectorCopy (b->goal_item->s.origin, to); to[2]   += 18;
						tr = gi.trace (from, vec3_origin, vec3_origin, to, ent, MASK_SOLID);
						clear = (tr.fraction >= 0.98f || tr.ent == b->goal_item);
					}
					if (clear)
						Bot_SteerToPoint (b, b->goal_item->s.origin);
				}
			}
		}
		return;
	}

	// ---- explore: head toward the nearest wanted item if one is in range
	//      (collects it by contact and connects its spot into the graph), else
	//      wander.  Fall back to wander when stuck so we don't grind a wall.
	{
		// grab a *nearby* item opportunistically, but otherwise wander widely so
		// the graph keeps growing into new areas (broad coverage -> connectivity
		// -> more items become routable goals)
		edict_t *it = stuck ? NULL : Goal_NearestItem (b, 350);
		if (it)
			Bot_SteerToPoint (b, it->s.origin);
		else
			Bot_Wander (b);
	}

	// when the graph can actually route us to a good item, switch to GOAL
	if (ready && level.time >= b->replan_time)
	{
		int start = Nav_NearestNode (ent->s.origin);
		int goal  = -1;

		if (Goal_Select (b) && b->goal_item)
			goal = Nav_NearestGoalNode (b->goal_item->s.origin);	// must match
											// Goal_Select's resolution exactly

		if (goal < 0)
			goal = Bot_PickGoal (start);	// nothing worth grabbing -> roam

		if (goal >= 0 && (goal != start || b->goal_item))
		{
			int len = Nav_FindPathMasked (start, goal, Bot_NavMask (b), b->path, BOT_MAX_PATH);
			if (len > 1 || (b->goal_item && len >= 1))
			{
				b->path_len = len;
				b->path_idx = 0;
				b->goal_node = goal;
				b->mode = BOT_MODE_GOAL;
				b->replan_time   = level.time + 0.5;
				b->progress_time = level.time;
				b->goal_time     = level.time;
				b->goal_cost     = Nav_LastPathCost ();
				b->goal_best     = 99999;
				b->reroute_fired = false;	// new attempt: allow one mid-reroute
				b->reroute_last_idx = 0;
				b->reroute_idx_time = level.time;
				Bot_LiftReset (b);
				Bot_TrainReset (b);
				Bot_LadderReset (b);
				Bot_StrafeReset (b);	// fresh path: any runway is stale
				Bot_PlaybackReset (b);
				if (b->goal_item)
					Bot_LogItemEvent (b, "goal_item", Bot_ItemName (b->goal_item));
				else
					Bot_LogEvent (b, "goal");
			}
		}
		if (b->mode != BOT_MODE_GOAL)
		{
			b->replan_time = level.time + 1.0;
			// bot_decisive: Goal_Select may have set goal_item on an
			// evaluation whose commit was rejected -- while we sit in
			// EXPLORE that phantom pick still "claims" the item against
			// other bots (Bot_ItemClaimed), enabling anti-correlated
			// pair swapping.  An uncommitted pick claims nothing.
			if (bot_decisive->value != 0)
				b->goal_item = NULL;
		}
	}
}

/*
=================
Bot_Think

Builds one frame's usercmd_t for a bot.
=================
*/
static void Bot_Think (bot_t *b, usercmd_t *cmd)
{
	edict_t	*ent = b->ent;
	float	facing_yaw, facing_pitch = 0;

	memset (cmd, 0, sizeof(*cmd));
	cmd->msec = (byte)(FRAMETIME * 1000);

	if (ent->deadflag)
	{
		// Respawn waits for a fresh attack *press*: ClientBeginServerFrame checks
		// latched_buttons, which is edge-triggered (buttons & ~oldbuttons).  Holding
		// BUTTON_ATTACK every frame never re-latches, so a bot that died while firing
		// would never respawn -- toggle it so each cycle produces a rising edge.
		if (((int)(level.time * 10.0f)) & 1)
			cmd->buttons = BUTTON_ATTACK;	// (0 on alternate frames -> edge)
		return;
	}

	// 1) navigation decides where we want to move (sets b->move_dir / move_yaw)
	Bot_Navigate (b);

	// 2) combat aims/fires and may bias the move direction (strafe/range); when
	//    not engaged the gaze layer picks the facing (stock behavior -- face
	//    the travel direction, pitch 0 -- when its cvars are off).
	//    A playbook replay owns the facing outright (the recorded view IS the
	//    maneuver -- e.g. a strafe-jump's yaw sweep) and suppresses combat for
	//    its few seconds: firing mid-trick would corrupt the recorded inputs.
	if (bot_playbook->value != 0
		&& (b->pb_state == PB_REPLAY || b->pb_state == PB_ALIGN))
	{
		facing_yaw = b->pb_yaw;
		facing_pitch = b->pb_pitch;
	}
	else if (!Combat_Aim (b, cmd, &facing_yaw, &facing_pitch))
	{
		// strafe jumping: the controller's yaw IS the maneuver (the wishdir
		// angle the speed gain depends on) -- the gaze slew/glances would
		// corrupt it.  The sweep it produces is the human-looking motion.
		if (bot_strafejump->value != 0 && b->sj_state == SJ_ACTIVE)
		{
			facing_yaw = b->move_yaw;
			facing_pitch = 0;
		}
		else
			Bot_GazeThink (b, &facing_yaw, &facing_pitch);
	}

	cmd->angles[YAW]   = (short)(ANGLE2SHORT(facing_yaw)   - ent->client->ps.pmove.delta_angles[YAW]);
	cmd->angles[PITCH] = (short)(ANGLE2SHORT(facing_pitch) - ent->client->ps.pmove.delta_angles[PITCH]);
	cmd->angles[ROLL]  = 0;

	// 3) move toward the goal relative to wherever we're facing
	Bot_ApplyMovement (b, cmd, facing_yaw);
}

/*
=================
Bot_DebugDraw

When bot_debug is set, render each bot's current A* path as debug trails (and a
line to its current enemy).  Throttled; visible to any connected spectator.
=================
*/
static void Bot_DebugDraw (void)
{
	static float	next;
	int				i, k;

	if (!bot_debug || !bot_debug->value)
		return;
	if (level.time < next)
		return;
	next = level.time + 0.5f;

	for (i = 0; i < game.maxclients; i++)
	{
		bot_t *b = &bots[i];
		if (!b->inuse || !b->ent || !b->ent->inuse)
			continue;

		for (k = b->path_idx; k + 1 < b->path_len; k++)
		{
			int a = b->path[k], c = b->path[k + 1];
			if (a < 0 || c < 0 || a >= nav.num_nodes || c >= nav.num_nodes)
				continue;
			gi.WriteByte (svc_temp_entity);
			gi.WriteByte (TE_DEBUGTRAIL);
			gi.WritePosition (nav.nodes[a].origin);
			gi.WritePosition (nav.nodes[c].origin);
			gi.multicast (nav.nodes[a].origin, MULTICAST_ALL);
		}

		if (b->enemy && b->enemy->inuse)
		{
			gi.WriteByte (svc_temp_entity);
			gi.WriteByte (TE_BFG_LASER);
			gi.WritePosition (b->ent->s.origin);
			gi.WritePosition (b->enemy->s.origin);
			gi.multicast (b->ent->s.origin, MULTICAST_ALL);
		}
	}
}

/*
=================
Bot_RunFrame

Called from the top of G_RunFrame(), before the entity loop, so each bot's
ClientThink runs before ClientBeginServerFrame processes its buttons.
=================
*/
void Bot_RunFrame (void)
{
	int			i, active;
	usercmd_t	cmd;
	bot_t		*b;
	edict_t		*ent;

	if (!deathmatch->value)
		return;

	// bot_quitafter: quit the server after N *game* seconds.  This is how
	// fastsim runs are timed (run_parallel.py --fastsim): the patched engine is
	// CPU-bound with no per-tick sleep, so a server that never quits pins a
	// core at 100% forever -- the quit MUST come from game time, not wall
	// time.  ShutdownGame saves the nav graph on the way out.
	if (bot_quitafter->value > 0 && level.time >= bot_quitafter->value)
	{
		static qboolean quitting;
		if (!quitting)
		{
			quitting = true;
			gi.dprintf ("ozbot: bot_quitafter %g reached, quitting\n",
				bot_quitafter->value);
			gi.AddCommandString ("quit\n");
		}
		return;
	}

	// new map?  the engine wiped g_edicts; save the old graph, reset, and
	// start fresh logging + nav for the new map.
	if (Q_stricmp (bot_logged_map, level.mapname) != 0)
	{
		Bot_DumpSlots ("new-map-entry (pre-clear)");	// what survived the level change
		if (bot_logged_map[0])
			Nav_Shutdown (bot_logged_map);
		if (bot_logged_map[0])
			Goal_SaveFails (bot_logged_map);	// P4a: persist old map's hard-item knowledge
												// (before Goal_Reset; uses load-time key snapshot)
		Bot_ClearAll ();
		Goal_Reset ();
		memset (bot_noise_time, 0, sizeof(bot_noise_time));
		memset (bot_noise_at, 0, sizeof(bot_noise_at));
		memset (bot_noise_kind, 0, sizeof(bot_noise_kind));
		memset (bot_noise_pos, 0, sizeof(bot_noise_pos));
		memset (bot_noise_step, 0, sizeof(bot_noise_step));	// level.time restarts;
									// stale times would hold the hearing gate open
		Bot_LogBeginLevel (level.mapname);
		Nav_Init (level.mapname);
		Goal_SeedNavNodes ();		// ensure item spots are covered + routable
		Goal_LoadFails (level.mapname);	// P4a: restore hard-item blacklist for this map
		Nav_TagPlatLinks ();		// bot_lift: retag learned lift columns
		if (bot_navvalidate->value != 0)	// P1: prune fluke walk links AFTER plat
			Nav_ValidateLinks ();			// columns are protected (PLAT-typed)
		Nav_SeedTrainLinks ();			// bot_train: bridge func_train gaps (AFTER validate
										// so its seeded approach links aren't pruned as flukes)
		Nav_SeedTeleportLinks ();		// bot_teleport: route through misc_teleporters
										// (one-way pad->dest links; the learn guard can't learn them)
		Nav_SeedPushLinks ();			// bot_jumppad: route through trigger_push jump pads
										// (one-way pad->ballistic-landing links; automatic on touch)
		Playbook_Load (level.mapname);	// bot_playbook: recorded maneuvers...
		Playbook_Register ();			// ...surfaced as NAV_LINK_PLAYBOOK links
		Nav_FlagHazardNodes ();		// bot_hazard: mark lava/slime/hurt-volume nodes
									// (BEFORE the reach sweep, so its verdicts see
									// the hazard exclusion the bots will route under)
		Goal_ReachSweep ("load");	// bot_reachlog: the persisted graph's reachability truth
		Bot_LogLiftBegin ();		// bot_liftlog: cache plats for diagnosis telemetry
		Com_sprintf (bot_logged_map, sizeof(bot_logged_map), "%s", level.mapname);
	}

	active = Bot_CountActive ();
	if (active < (int)bot_count->value)
		Bot_Add ();
	else if (active > (int)bot_count->value)
		Bot_RemoveOne ();

	// bot_train: re-price train links by current item availability (~2 Hz -- item
	// respawn is on a 30s timer, so this is plenty responsive and cheap).  Reset
	// the throttle when level.time jumps back (a map change).
	if (bot_train->value != 0)
	{
		static float next_traincost = 0.0f, last_ttime = -1.0f;
		if (level.time < last_ttime)
			next_traincost = 0.0f;
		last_ttime = level.time;
		if (level.time >= next_traincost)
		{
			next_traincost = level.time + 0.5f;
			Goal_UpdateTrainCosts ();
		}
	}

	for (i = 0; i < game.maxclients; i++)
	{
		b = &bots[i];
		if (!b->inuse)
			continue;

		ent = b->ent;
		if (!ent->inuse || !ent->client)
		{
			memset (b, 0, sizeof(*b));
			continue;
		}

		// A real player can end up in this engine client slot -- most often the
		// listen-server host, who connects a frame or two after the map loads,
		// into a slot a bot already grabbed.  Bots aren't in the engine's client
		// pool, so the engine doesn't know the slot is "taken" and can hand it to
		// a human.  Detect that by the netname no longer being ours and release
		// the slot, so we never drive (fight) a real player's edict.  Bot_Add
		// then refills the population in a genuinely free slot.
		{
			char	nm[32];
			Com_sprintf (nm, sizeof(nm), "OzBot%d", b->id);
			if (strcmp (ent->client->pers.netname, nm) != 0)
			{
				memset (b, 0, sizeof(*b));
				continue;
			}
		}

		// death/spawn transitions
		if (ent->deadflag && !b->was_dead)
		{
			b->was_dead = true;
			// bot_hazard death feedback: an environmental death is the
			// strongest "this hop is not traversable" signal there is -- and
			// the only one the alive-only penalty paths (reroutemid/giveup/
			// stuck) can never see, because lava kills in ~1.5s while the
			// cheapest of them needs a 3.5s stall.  Penalize the hop being
			// traversed like reroutemid does, but twice (a death outweighs a
			// stall) and both directions (walk links are bidirectional; the
			// pit kills both ways), and itemfail the goal so the whole
			// population cools on it.  Combat kills (MOD_ROCKET etc.) never
			// reach here -- the MOD filter keeps good links unharmed.  State
			// is still intact: Bot_ResetNavState only runs at respawn.
			if (bot_hazard->value != 0
				&& (b->death_mod == MOD_LAVA || b->death_mod == MOD_SLIME
					|| b->death_mod == MOD_WATER || b->death_mod == MOD_FALLING
					|| b->death_mod == MOD_TRIGGER_HURT)
				&& b->path_idx > 0 && b->path_idx < b->path_len)
			{
				int	from = b->path[b->path_idx - 1];
				int	to   = b->path[b->path_idx];
				Nav_PenalizeLink (from, to);
				Nav_PenalizeLink (from, to);
				Nav_PenalizeLink (to, from);
				Nav_PenalizeLink (to, from);
				Bot_LogPenalize (b, from, to);
				// blacklist the goal item unless a fight plausibly shoved us
				// in (mirrors the !enemy gate on giveup-time itemfails)
				if (b->goal_item && !b->enemy)
					Goal_ItemFailed (b->goal_item, NAVQ_OK);
				Bot_LogEvent (b, "hazdeath");
			}
			// bot_hazlog: classify EVERY death (MOD, airborne, path state,
			// whether a route existed to penalize) so the residual
			// environmental deaths the penalty misses -- airborne/knocked-in,
			// or path already run off the end -- can be sized and targeted.
			if (bot_hazlog->value != 0)
			{
				qboolean penalized = (bot_hazard->value != 0
					&& (b->death_mod == MOD_LAVA || b->death_mod == MOD_SLIME
						|| b->death_mod == MOD_WATER || b->death_mod == MOD_FALLING
						|| b->death_mod == MOD_TRIGGER_HURT)
					&& b->path_idx > 0 && b->path_idx < b->path_len);
				Bot_LogHazDeath (b, b->death_mod, penalized);
			}
			b->death_mod = 0;
			Bot_LogEvent (b, "death");
		}
		else if (!ent->deadflag && b->was_dead)
		{
			b->was_dead = false;
			Bot_ResetNavState (b);		// don't link across the respawn jump
			Bot_LogEvent (b, "spawn");
		}

		Bot_Think (b, &cmd);
		// bot_cmdlog: log the bot's own usercmd stream in the bot_inputlog
		// schema -- tools/make_playbook.py can bake a playbook from a bot's
		// traversal (the pipeline validation source; human captures come via
		// bot_inputlog/record_inputs.bat)
		if (bot_cmdlog->value != 0)
			Bot_LogInput (ent, &cmd);
		ClientThink (ent, &cmd);

		// strafe jumping (bot_strafejump): physics parity with real clients.
		// q2pro gives networked clients the "strafejump hack" (no PMF_TIME_LAND
		// jump lockout on landing; sv_strafejump_hack, default 1) but bots run
		// Pmove with the engine's default params, so every hop landing would
		// force a 100ms ground-friction frame (~60% speed loss) and kill the
		// chain.  The game owns ps.pmove between frames: clearing the flag here
		// reproduces strafehack semantics exactly (it is only read at the START
		// of the next Pmove), works on any stock engine, and is gated to bots
		// actively mid-chain so everything else stays byte-identical.
		if (bot_strafejump->value != 0 && b->sj_state == SJ_ACTIVE
			&& (ent->client->ps.pmove.pm_flags & PMF_TIME_LAND)
			&& !(ent->client->ps.pmove.pm_flags & (PMF_TIME_TELEPORT | PMF_TIME_WATERJUMP)))
		{
			ent->client->ps.pmove.pm_flags &= ~PMF_TIME_LAND;
			ent->client->ps.pmove.pm_time = 0;
		}

		Bot_LogTick (b);
		Bot_LogLiftTick (b);
	}

	Bot_DebugDraw ();
	Bot_LogSJDiag ();
	Nav_MaybeSave (bot_logged_map);
	Bot_LogMaybeFlush ();

	// bot_slotlog: throttled steady-state slot dump (~2 Hz) so the human's
	// slot ownership after a gamemap is visible as it settles.  Reset the
	// throttle when level.time jumps backwards (a map change restarts it).
	if (bot_slotlog && bot_slotlog->value != 0)
	{
		static float last_time = -1.0f, next_slotdump;
		if (level.time < last_time)
			next_slotdump = 0.0f;
		last_time = level.time;
		if (level.time >= next_slotdump)
		{
			next_slotdump = level.time + 0.5f;
			Bot_DumpSlots ("tick");
		}
	}
}

/*
=================
Bot_ServerCommand
=================
*/
qboolean Bot_ServerCommand (void)
{
	char	*cmd = gi.argv (1);
	int		n;

	if (Q_stricmp (cmd, "bot_add") == 0)
	{
		n = (gi.argc() > 2) ? atoi (gi.argv(2)) : 1;
		if (n < 1) n = 1;
		gi.cvar_set ("bot_count", va("%d", (int)bot_count->value + n));
		return true;
	}
	else if (Q_stricmp (cmd, "bot_remove") == 0)
	{
		n = (gi.argc() > 2) ? atoi (gi.argv(2)) : 1;
		if (n < 1) n = 1;
		n = (int)bot_count->value - n;
		if (n < 0) n = 0;
		gi.cvar_set ("bot_count", va("%d", n));
		return true;
	}
	else if (Q_stricmp (cmd, "bot_clear") == 0)
	{
		gi.cvar_set ("bot_count", "0");
		return true;
	}
	else if (Q_stricmp (cmd, "nav_query") == 0)
	{
		// sv nav_query <item substring> -- interactive oracle: for every item
		// matching the classname/pickup-name substring, print reachability
		// from a deathmatch spawn, both with every capability and with the
		// basic (no water/plat) mask so gated items name their unlock
		// (plans/nav-oracle.md Phase C)
		edict_t	*spawn, *it;
		char	*want = (gi.argc() > 2) ? gi.argv (2) : "";
		int		i, shown = 0;
		int		base = NAV_MASK_ALL & ~(NAV_MASK (NAV_LINK_WATER) | NAV_MASK (NAV_LINK_PLAT));

		spawn = G_Find (NULL, FOFS (classname), "info_player_deathmatch");
		if (!spawn)
			spawn = G_Find (NULL, FOFS (classname), "info_player_start");
		if (!spawn)
		{
			gi.cprintf (NULL, PRINT_HIGH, "nav_query: no spawn point to query from\n");
			return true;
		}

		for (i = (int)game.maxclients + 1; i < globals.num_edicts; i++)
		{
			int			code, gate = 0;
			float		cost = 0;
			char		gates[40];
			const char	*nm;

			it = g_edicts + i;
			if (!it->inuse || !it->item)
				continue;
			if (it->spawnflags & DROPPED_ITEM)
				continue;
			nm = it->item->pickup_name ? it->item->pickup_name : it->classname;
			if (want[0] && !strstr (nm, want)
				&& !(it->classname && strstr (it->classname, want)))
				continue;

			code = Nav_QueryPath (spawn->s.origin, it->s.origin, base, &cost, &gate);
			Nav_MaskNames (gate, gates, sizeof(gates));
			if (code == NAVQ_GATED)
				gi.cprintf (NULL, PRINT_HIGH, "%s @ (%.0f %.0f %.0f): gated by %s (cost %.0f)\n",
					nm, it->s.origin[0], it->s.origin[1], it->s.origin[2], gates, cost);
			else
				gi.cprintf (NULL, PRINT_HIGH, "%s @ (%.0f %.0f %.0f): %s%s\n",
					nm, it->s.origin[0], it->s.origin[1], it->s.origin[2],
					Nav_QueryName (code),
					(code == NAVQ_OK) ? va(" (cost %.0f)", cost) : "");
			shown++;
		}
		if (!shown)
			gi.cprintf (NULL, PRINT_HIGH, "nav_query: no item matches '%s'\n", want);
		return true;
	}

	return false;
}
