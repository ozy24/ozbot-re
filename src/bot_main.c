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
cvar_t	*bot_pathcost;
cvar_t	*bot_goalbudget;
cvar_t	*bot_budgetcap;
cvar_t	*bot_itemfail;
cvar_t	*bot_navmask;
cvar_t	*bot_reachlog;
cvar_t	*bot_goalnode;
cvar_t	*bot_swim;
cvar_t	*bot_lift;
cvar_t	*bot_liftlog;
cvar_t	*bot_strafejump;
cvar_t	*bot_sjlog;
cvar_t	*bot_inputlog;

// registry indexed by client slot (index i <-> g_edicts[i+1])
static bot_t	bots[MAX_CLIENTS];
static int		bot_next_id;			// monotonically increasing for names
static char		bot_logged_map[MAX_QPATH];	// map the current log/nav is for

// when each client slot last made weapon noise (bot_fov hearing; kept out of
// gclient_t deliberately -- no vanilla struct edits)
static float	bot_noise_time[MAX_CLIENTS];

#define BOT_GRAPH_READY		24		// nodes needed before goal-seeking starts
#define BOT_GOAL_TIMEOUT	12.0f	// abandon a goal not reached within this

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
	bot_swim         = gi.cvar ("bot_swim", "1", 0);		// 3D steering in water (vertical swim + water-jump exits)
	bot_lift         = gi.cvar ("bot_lift", "1", 0);		// the lift capability: plat links, wait/board/ride
															// controller, 3D column arrival, level-aware homing
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
	bot_aimtest      = gi.cvar ("bot_aimtest", "0", 0);		// head-to-head: even ids apply the bot_aim* multipliers
	bot_aimreact     = gi.cvar ("bot_aimreact", "1", 0);	//   reaction-delay multiplier
	bot_aimturn      = gi.cvar ("bot_aimturn", "1", 0);		//   turn-rate multiplier
	bot_aimerr       = gi.cvar ("bot_aimerr", "1", 0);		//   aim-error multiplier
	bot_aimfire      = gi.cvar ("bot_aimfire", "1", 0);		//   fire-threshold multiplier
	bot_aimtexture   = gi.cvar ("bot_aimtexture", "1", 0);	// humanization: wandering aim error + reversal overshoot
	bot_aimsmooth    = gi.cvar ("bot_aimsmooth", "1", 0);	// 40Hz view glide toward the 10Hz aim (anti-judder)
	bot_fov          = gi.cvar ("bot_fov", "1", 0);			// humanization: ~120 deg vision cone + pain reflex
	bot_hop          = gi.cvar ("bot_hop", "1", 0);			// humanization: combat jump/strafe rhythm from demo stats
	bot_fidget       = gi.cvar ("bot_fidget", "1", 0);		// humanization: idle fidget, wall turn-away, travel hops

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
	Bot_LiftReset (b);
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
Bot_NoteNoise / Bot_NoiseTime

Hooked from PlayerNoise (p_weapon.c) before its deathmatch early-out: any
client's unsilenced weapon fire is "heard".  Combat_FindEnemy uses it so a
bot_fov bot can acquire a visible shooter outside its view cone -- Q2 weapons
are loud, and a human absolutely turns toward gunfire behind them.
=================
*/
void Bot_NoteNoise (edict_t *who)
{
	int	i;

	if (!who || !who->client)
		return;
	i = who - g_edicts - 1;
	if (i < 0 || i >= MAX_CLIENTS)
		return;
	bot_noise_time[i] = level.time;
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
		ent = g_edicts + 1 + i;
		if (!ent->inuse && !bots[i].inuse)
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
	b->steer_item    = NULL;	// fresh explore leg: no stale steering target
	Bot_LiftReset (b);
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
				const char *nm = b->goal_item->item ? b->goal_item->item->pickup_name : "";
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

		// give up on a goal we can't reach so we don't loop on it forever
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
				if (b->sj_state != SJ_NONE)
					Bot_StrafeReset (b);	// the lift owns the frame now
				return;
			}
		}
		else if (b->lift_state != LIFT_NONE)
			Bot_LiftReset (b);	// cvar turned off mid-attempt: clear stale state

		// playbook maneuvers (bot_playbook): when a recorded-move hop is next
		// on the path the controller owns movement AND facing.  Budget clock
		// freezes like a lift (the align-up wait must not be billed).
		if (Bot_PlaybackThink (b))
		{
			b->progress_time = level.time;
			b->goal_time += FRAMETIME;
			if (b->sj_state != SJ_NONE)
				Bot_StrafeReset (b);	// the replay owns the frame now
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
					Bot_SteerToPoint (b, b->goal_item->s.origin);
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
				Bot_LiftReset (b);
				Bot_StrafeReset (b);	// fresh path: any runway is stale
				Bot_PlaybackReset (b);
				if (b->goal_item)
					Bot_LogItemEvent (b, "goal_item", b->goal_item->item->pickup_name);
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
		if (bot_logged_map[0])
			Nav_Shutdown (bot_logged_map);
		Bot_ClearAll ();
		Goal_Reset ();
		memset (bot_noise_time, 0, sizeof(bot_noise_time));	// level.time restarts;
									// stale times would hold the hearing gate open
		Bot_LogBeginLevel (level.mapname);
		Nav_Init (level.mapname);
		Goal_SeedNavNodes ();		// ensure item spots are covered + routable
		Nav_TagPlatLinks ();		// bot_lift: retag learned lift columns
		Playbook_Load (level.mapname);	// bot_playbook: recorded maneuvers...
		Playbook_Register ();			// ...surfaced as NAV_LINK_PLAYBOOK links
		Goal_ReachSweep ("load");	// bot_reachlog: the persisted graph's reachability truth
		Bot_LogLiftBegin ();		// bot_liftlog: cache plats for diagnosis telemetry
		Com_sprintf (bot_logged_map, sizeof(bot_logged_map), "%s", level.mapname);
	}

	active = Bot_CountActive ();
	if (active < (int)bot_count->value)
		Bot_Add ();
	else if (active > (int)bot_count->value)
		Bot_RemoveOne ();

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
