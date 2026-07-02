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
cvar_t	*bot_rollout;
cvar_t	*bot_claim;
cvar_t	*bot_pathcost;
cvar_t	*bot_goalbudget;

// registry indexed by client slot (index i <-> g_edicts[i+1])
static bot_t	bots[MAX_CLIENTS];
static int		bot_next_id;			// monotonically increasing for names
static char		bot_logged_map[MAX_QPATH];	// map the current log/nav is for

#define BOT_GRAPH_READY		24		// nodes needed before goal-seeking starts
#define BOT_GOAL_TIMEOUT	12.0f	// abandon a goal not reached within this
#define BOT_ITEM_COOLDOWN	10.0f	// how long to avoid a failed item

// bot_goalbudget: timeout scaled to the committed route's A* cost instead of
// the flat BOT_GOAL_TIMEOUT -- short hops recycle faster, honest long routes
// get funded.  base + cost/speed crosses the flat 12s at cost ~600u.
#define BOT_GOAL_BUDGET_BASE	6.0f	// seconds of slack regardless of route
#define BOT_GOAL_BUDGET_SPEED	100.0f	// effective travel speed (cost units/sec)
#define BOT_GOAL_BUDGET_MAX		20.0f	// never fund a route longer than this

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
	bot_rollout      = gi.cvar ("bot_rollout", "1", 0);
	bot_claim        = gi.cvar ("bot_claim", "1", 0);
	bot_pathcost     = gi.cvar ("bot_pathcost", "1", 0);	// score items by A* route cost, not straight-line distance
	bot_goalbudget   = gi.cvar ("bot_goalbudget", "1", 0);	// goal timeout scaled to route cost, not flat 12s
	bot_skilltest    = gi.cvar ("bot_skilltest", "0", 0);
	bot_lead         = gi.cvar ("bot_lead", "1", 0);		// lead moving targets by projectile flight time
	bot_leadtest     = gi.cvar ("bot_leadtest", "0", 0);	// head-to-head: even bot ids lead, odd don't

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
		Nav_Shutdown (bot_logged_map);
	Bot_LogEndLevel ();
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
	b->goal_timing = false;
	b->goal_cost = 0;
	b->path_len  = 0;
	b->path_idx  = 0;
	b->replan_time  = level.time + 1.0;
	b->progress_time = level.time;
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
	return (level.time - b->progress_time) > 1.0;
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
	}
	b->prev_node = Nav_LearnStep (ent, b->prev_node, link);
	b->cur_node  = b->prev_node;
	if (ent->groundentity)
		b->did_jump = false;	// landed; jump (if any) has been accounted for

	stuck = Bot_UpdateStuck (b);

	// ---- goal-seeking ----
	if (b->mode == BOT_MODE_GOAL && b->goal_node >= 0 && b->goal_node < nav.num_nodes)
	{
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
				Bot_GoExplore (b);
				Bot_Wander (b);
				return;
			}
		}

		// give up on a goal we can't reach so we don't loop on it forever
		{
			float budget = BOT_GOAL_TIMEOUT;
			if (bot_goalbudget->value != 0 && b->goal_cost > 0)
			{
				budget = BOT_GOAL_BUDGET_BASE + b->goal_cost / BOT_GOAL_BUDGET_SPEED;
				if (budget > BOT_GOAL_BUDGET_MAX)
					budget = BOT_GOAL_BUDGET_MAX;
			}
			if (level.time - b->goal_time > budget)
			{
			vec3_t	tgt, d;
			int		atnode = (b->path_idx >= b->path_len) ? 1 : 0;
			if (b->goal_item)
				VectorCopy (b->goal_item->s.origin, tgt);
			else if (b->goal_node >= 0 && b->goal_node < nav.num_nodes)
				VectorCopy (nav.nodes[b->goal_node].origin, tgt);
			else
				VectorCopy (ent->s.origin, tgt);
			VectorSubtract (tgt, ent->s.origin, d);
			Bot_LogGiveup (b, (float)sqrt(d[0]*d[0] + d[1]*d[1]), d[2],
				atnode, b->enemy ? 1 : 0);
			Bot_GoExplore (b);
			Bot_Wander (b);
			return;
			}
		}

		if (b->path_len <= 0 || b->path_idx >= b->path_len)
		{
			int start = Nav_NearestNode (ent->s.origin);
			b->path_len = Nav_FindPath (start, b->goal_node, b->path, BOT_MAX_PATH);
			b->path_idx = 0;
			if (b->path_len <= 0)
			{
				Bot_LogEvent (b, "pathfail");
				Bot_GoExplore (b);
				Bot_Wander (b);
				return;
			}
		}

		if (!Bot_FollowPath (b))
		{
			// arrived at the goal node
			if (b->goal_item && (Goal_ItemAvailable (b->goal_item) || b->goal_timing))
			{
				// home in on the item (or hold on the spot while we wait for it
				// to respawn) until we touch it or time out
				Bot_SteerToPoint (b, b->goal_item->s.origin);
				return;
			}
			// roam node reached
			Bot_LogEvent (b, "reach");
			Bot_GoExplore (b);
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
					Nav_PenalizeLink (b->path[b->path_idx - 1], b->path[b->path_idx]);
				start = Nav_NearestNode (ent->s.origin);
				b->path_len = Nav_FindPath (start, b->goal_node, b->path, BOT_MAX_PATH);
				b->path_idx = 0;
				b->replan_time = level.time + 1.5;
				if (b->path_len <= 0)
				{
					Bot_LogEvent (b, "pathfail");
					Bot_GoExplore (b);
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
			dv[2] = 0;
			if (VectorLength (dv) < 200)
				Bot_SteerToPoint (b, b->goal_item->s.origin);
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
			goal = Nav_NearestNode (b->goal_item->s.origin);

		if (goal < 0)
			goal = Bot_PickGoal (start);	// nothing worth grabbing -> roam

		if (goal >= 0 && (goal != start || b->goal_item))
		{
			int len = Nav_FindPath (start, goal, b->path, BOT_MAX_PATH);
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
				if (b->goal_item)
					Bot_LogItemEvent (b, "goal_item", b->goal_item->item->pickup_name);
				else
					Bot_LogEvent (b, "goal");
			}
		}
		if (b->mode != BOT_MODE_GOAL)
			b->replan_time = level.time + 1.0;
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
	//    not engaged we face our travel direction
	if (!Combat_Aim (b, cmd, &facing_yaw, &facing_pitch))
	{
		facing_yaw = b->move_yaw;
		facing_pitch = 0;
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

	// new map?  the engine wiped g_edicts; save the old graph, reset, and
	// start fresh logging + nav for the new map.
	if (Q_stricmp (bot_logged_map, level.mapname) != 0)
	{
		if (bot_logged_map[0])
			Nav_Shutdown (bot_logged_map);
		Bot_ClearAll ();
		Goal_Reset ();
		Bot_LogBeginLevel (level.mapname);
		Nav_Init (level.mapname);
		Goal_SeedNavNodes ();		// ensure item spots are covered + routable
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
		ClientThink (ent, &cmd);

		Bot_LogTick (b);
	}

	Bot_DebugDraw ();
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

	return false;
}
