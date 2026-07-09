/*
ozbot - self-learning q2dm1 bot

bot_goal.c -- item-driven goal selection.

Items are discovered dynamically by scanning entities with ->item set, so this
works on any map and tracks real availability/respawn state from the entity
itself (no hardcoded coordinate table needed).  Each candidate is scored by
value * need / distance; the best becomes the bot's goal.
*/

#include "g_local.h"
#include "bot.h"
#include "bot_nav.h"

// distance falloff shared by naive (straight-line) and path-cost scoring
#define GOAL_DIST_FALLOFF	512.0f

// per-item cooldown so bots that fail/abandon an item don't immediately
// re-fixate on it (and so multiple bots spread across items)
static float	item_cooldown[MAX_EDICTS];

// consecutive giveups at each item (any bot).  Items bots keep failing to
// reach get an escalating shared blacklist (bot_itemfail): the graph may
// claim a route the bots can't actually execute (vertically-gated spots),
// and each such attempt burns the full goal budget.
static byte		item_fails[MAX_EDICTS];

void Goal_Reset (void)
{
	memset (item_cooldown, 0, sizeof(item_cooldown));
	memset (item_fails, 0, sizeof(item_fails));
}

/*
=================
Failure-knowledge persistence (bot_failpersist, P4a)

item_fails is keyed by edict index, which is spawn-order-stable within a map
but NOT portable across sessions.  The sidecar keys each failing item by
(classname + rounded origin) -- stable across runs of the same map -- so a
fresh run can re-apply the escalated blacklist immediately instead of burning
one full goal budget per hard item to rediscover it.

Save timing: Goal_SaveFails must run BEFORE Goal_Reset on a map change, and the
engine has already spawned the NEXT map's edicts by then, so we cannot iterate
live edicts for the OLD map's keys.  Instead we snapshot each item's key at LOAD
time (Goal_LoadFails, when the current map's edicts are valid) into fail_key_*
parallel to item_fails, and save from that snapshot.
=================
*/
#define OZFL_MAGIC		(('L'<<24)|('F'<<16)|('Z'<<8)|'O')	// "OZFL"
#define OZFL_VERSION	1

typedef struct
{
	char	classname[32];
	vec3_t	origin;
} fail_key_t;

static fail_key_t	fail_key[MAX_EDICTS];	// snapshot captured in Goal_LoadFails
static qboolean		fail_key_set[MAX_EDICTS];

static void Fail_Path (const char *mapname, char *out, int outsize)
{
	Com_sprintf (out, outsize, "%s/nav/%s.fail",
		Bot_GameDir(), (mapname && mapname[0]) ? mapname : "unknown");
}

// snapshot the (classname, origin) key of every live item edict, indexed the
// same way as item_fails (edict index), so Goal_SaveFails has stable keys even
// after the map's edicts are gone.
static void Goal_CaptureItemKeys (void)
{
	edict_t	*it;
	int		i;

	memset (fail_key_set, 0, sizeof(fail_key_set));
	for (i = (int)game.maxclients + 1; i < globals.num_edicts; i++)
	{
		it = g_edicts + i;
		if (!it->inuse || !it->item || i >= MAX_EDICTS)
			continue;
		if (it->spawnflags & DROPPED_ITEM)
			continue;
		Com_sprintf (fail_key[i].classname, sizeof(fail_key[i].classname),
			"%s", it->classname ? it->classname : "");
		VectorCopy (it->s.origin, fail_key[i].origin);
		fail_key_set[i] = true;
	}
}

void Goal_LoadFails (const char *mapname)
{
	char	path[MAX_OSPATH];
	FILE	*f;
	int		magic = 0, version = 0, count = 0, i, applied = 0;

	if (bot_failpersist->value == 0)
		return;

	// snapshot keys first: needed for a later save regardless of whether a
	// sidecar exists yet.
	Goal_CaptureItemKeys ();

	Fail_Path (mapname, path, sizeof(path));
	f = fopen (path, "rb");
	if (!f)
		return;

	fread (&magic, sizeof(magic), 1, f);
	fread (&version, sizeof(version), 1, f);
	fread (&count, sizeof(count), 1, f);
	if (magic != OZFL_MAGIC || version != OZFL_VERSION || count < 0 || count > MAX_EDICTS)
	{
		gi.dprintf ("ozbot: fail file %s incompatible; ignoring\n", path);
		fclose (f);
		return;
	}

	for (i = 0; i < count; i++)
	{
		fail_key_t	k;
		int			fails = 0, j;

		if (fread (k.classname, sizeof(k.classname), 1, f) != 1)
			break;
		if (fread (k.origin, sizeof(k.origin), 1, f) != 1)
			break;
		if (fread (&fails, sizeof(fails), 1, f) != 1)
			break;
		if (fails <= 0)
			continue;
		if (fails > 4)
			fails = 4;
		k.classname[sizeof(k.classname) - 1] = 0;

		// match against a live item edict by classname + nearby origin
		for (j = (int)game.maxclients + 1; j < globals.num_edicts && j < MAX_EDICTS; j++)
		{
			edict_t	*it = g_edicts + j;
			vec3_t	d;
			if (!fail_key_set[j])
				continue;
			if (Q_stricmp (fail_key[j].classname, k.classname) != 0)
				continue;
			VectorSubtract (fail_key[j].origin, k.origin, d);
			if (VectorLength (d) > 16.0f)
				continue;
			// re-apply the escalated avoidance so bots skip this known-hard item
			// from t=0 instead of paying a full-budget giveup to rediscover it.
			item_fails[j] = (byte)fails;
			Goal_Blacklist (it, BOT_ITEM_COOLDOWN * (float)(1 << fails));
			applied++;
			break;
		}
	}
	fclose (f);
	if (applied)
		gi.dprintf ("ozbot: failpersist restored %d hard-item blacklist(s) from %s\n",
			applied, path);
}

void Goal_SaveFails (const char *mapname)
{
	char	path[MAX_OSPATH];
	FILE	*f;
	int		magic = OZFL_MAGIC, version = OZFL_VERSION, count = 0, i;

	if (bot_failpersist->value == 0)
		return;

	// count items worth persisting (any recorded giveups)
	for (i = 0; i < MAX_EDICTS; i++)
		if (fail_key_set[i] && item_fails[i] > 0)
			count++;
	if (!count)
		return;

	Fail_Path (mapname, path, sizeof(path));
	f = fopen (path, "wb");
	if (!f)
	{
		gi.dprintf ("ozbot: could not write fail file %s\n", path);
		return;
	}
	fwrite (&magic, sizeof(magic), 1, f);
	fwrite (&version, sizeof(version), 1, f);
	fwrite (&count, sizeof(count), 1, f);
	for (i = 0; i < MAX_EDICTS; i++)
	{
		int fails;
		if (!fail_key_set[i] || item_fails[i] <= 0)
			continue;
		fails = item_fails[i];
		fwrite (fail_key[i].classname, sizeof(fail_key[i].classname), 1, f);
		fwrite (fail_key[i].origin, sizeof(fail_key[i].origin), 1, f);
		fwrite (&fails, sizeof(fails), 1, f);
	}
	fclose (f);
	gi.dprintf ("ozbot: failpersist saved %d hard item(s) to %s\n", count, path);
}

/*
=================
Goal_SeedNavNodes

Seed a connected nav node at every item spot so the goal layer can route to
items even in areas bots haven't organically explored.  Call once per map
after the nav graph is loaded.
=================
*/
void Goal_SeedNavNodes (void)
{
	edict_t	*it;
	int		i, seeded = 0;

	for (i = (int)game.maxclients + 1; i < globals.num_edicts; i++)
	{
		it = g_edicts + i;
		if (!it->inuse || !it->item)
			continue;
		if (Nav_SeedNode (it->s.origin) >= 0)
			seeded++;
	}
	gi.dprintf ("ozbot: seeded nav nodes for %d items\n", seeded);
}

/*
=================
Goal_ReachSweep

Map-load reachability sweep (bot_reachlog; plans/nav-oracle.md Phase C):
classify every item spot with the oracle from a deathmatch spawn, against
the "no special capabilities" mask, so gated items name what unlocks them --
the diagnosis that took sessions of telemetry mining for the Railgun (water)
and Grenade Launcher (plat).  Read-only diagnostics: one JSONL record per
item plus a console summary.  Learned-graph caveat: "ok" proves a route
exists IN-GRAPH, not that bots can execute it; "no_path" means "not learned
yet", never "impossible".

Runs twice per level: when="load" (the persisted graph's truth -- measures
STALENESS: seeded item islands the learner reconnects within seconds of
play) and when="quit" (the run-matured graph's truth -- the honest
steady-state readout, since worker-gamedir maturation is discarded).
=================
*/
void Goal_ReachSweep (const char *when)
{
	edict_t	*spawn, *it;
	int		i, total = 0, ok = 0, gated = 0, nopath = 0, nonode = 0;
	int		base = NAV_MASK_ALL & ~(NAV_MASK (NAV_LINK_WATER) | NAV_MASK (NAV_LINK_PLAT));

	if (bot_reachlog->value == 0)
		return;

	spawn = G_Find (NULL, FOFS (classname), "info_player_deathmatch");
	if (!spawn)
		spawn = G_Find (NULL, FOFS (classname), "info_player_start");
	if (!spawn)
		return;

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
			continue;	// weapons dropped at death spots aren't map item spots

		code = Nav_QueryPath (spawn->s.origin, it->s.origin, base, &cost, &gate);
		Nav_MaskNames (gate, gates, sizeof(gates));
		nm = Bot_ItemName (it);

		total++;
		if (code == NAVQ_OK)
			ok++;
		else if (code == NAVQ_GATED)
		{
			gated++;
			gi.dprintf ("ozbot: reach[%s]: %s gated by %s (cost %.0f)\n", when, nm, gates, cost);
		}
		else if (code == NAVQ_NO_PATH)
		{
			nopath++;
			gi.dprintf ("ozbot: reach[%s]: %s NO PATH\n", when, nm);
		}
		else
			nonode++;

		Bot_LogReach (when, nm, it->classname, it->s.origin,
			Nav_QueryName (code), (code == NAVQ_GATED) ? gates : "", cost);
	}

	gi.dprintf ("ozbot: reach sweep[%s]: %d items: %d ok, %d gated, %d no-path, %d uncovered\n",
		when, total, ok, gated, nopath, nonode);
}

void Goal_Blacklist (edict_t *it, float secs)
{
	int n;
	if (!it)
		return;
	n = it - g_edicts;
	// max-keeping: the short spread-cooldown Bot_GoExplore applies on every
	// goal exit must not shorten an escalated failure blacklist
	if (n >= 0 && n < MAX_EDICTS && level.time + secs > item_cooldown[n])
		item_cooldown[n] = level.time + secs;
}

/*
=================
Goal_ItemFailed / Goal_ItemSucceeded

Track consecutive giveups per item.  A giveup means a bot committed its full
goal budget and still couldn't touch the item, so make everyone avoid it for
an escalating while (20/40/80/160s); any successful collection resets it.
=================
*/
void Goal_ItemFailed (edict_t *it, int navq)
{
	int n;
	if (!it || bot_itemfail->value == 0)
		return;
	n = it - g_edicts;
	if (n < 0 || n >= MAX_EDICTS)
		return;
	if (item_fails[n] < 4)
		item_fails[n]++;
	// bot_itemfail 2: a giveup whose route is GONE from the graph (the oracle
	// says no_path / no_goal_node at giveup time) jumps straight to the full
	// escalation -- explore has to relearn the area before another attempt
	// can succeed, so don't burn three more goal budgets proving that.
	// (NO_START_NODE is the bot's own position being uncovered, not the
	// item's fault, so it keeps the normal ladder.)
	if (bot_itemfail->value >= 2
		&& (navq == NAVQ_NO_PATH || navq == NAVQ_NO_GOAL_NODE))
		item_fails[n] = 4;
	Goal_Blacklist (it, BOT_ITEM_COOLDOWN * (float)(1 << item_fails[n]));
}

void Goal_ItemSucceeded (edict_t *it)
{
	int n;
	if (!it)
		return;
	n = it - g_edicts;
	if (n >= 0 && n < MAX_EDICTS)
	{
		item_fails[n] = 0;
		item_cooldown[n] = 0;	// proven collectable; drop any failure blacklist
	}
}

static qboolean Goal_OnCooldown (edict_t *it)
{
	int n = it - g_edicts;
	return (n >= 0 && n < MAX_EDICTS && item_cooldown[n] > level.time);
}

/*
=================
Goal_ItemAvailable

True if the item is currently sitting in the world ready to be picked up
(in deathmatch, picked-up items go SOLID_NOT + SVF_NOCLIENT until they respawn).
=================
*/
qboolean Goal_ItemAvailable (edict_t *it)
{
	return it && it->inuse && it->item
		&& it->solid != SOLID_NOT
		&& !(it->svflags & SVF_NOCLIENT);
}

static qboolean has (const char *s, const char *sub)
{
	return (s && sub && strstr (s, sub)) ? true : false;
}

/*
=================
Goal_IsRecovery

True for pickups that restore toughness (health or armor) -- what a fleeing
bot should be running for.
=================
*/
qboolean Goal_IsRecovery (edict_t *it)
{
	gitem_t	*item = it ? it->item : NULL;
	if (!item)
		return false;
	if (item->flags & IT_ARMOR)
		return true;
	return has (item->classname, "health")
		|| has (item->pickup_name, "Health") || has (item->pickup_name, "health");
}

// base value at/above which an item is a "control" item: worth crossing the
// map for and worth timing its respawn.  Value-driven so it adapts to whatever
// items a map actually has (q2dm1 has no Red Armor/Mega/Quad -- here this picks
// out Combat Armor and the top weapons).
#define ITEM_CONTROL_VALUE	50.0f

// resource-need curve constants, calibrated from the pro demo corpus
// (tools/dm2_combat.py need -> demos/derived/combat_need/thresholds.json).
// bot_ammoneed: ammo need ramps as the fill for the gun it feeds drops.  Full
// (frac>=1, at/above the human refill level) -> MIN, which sits BELOW the legacy
// flat 0.5 so a topped-up bot stops chasing ammo; empty -> MAX.  Base value for
// ammo is only 12, so even MAX keeps ammo a local top-up, never a cross-map quest.
#define AMMO_NEED_MIN		0.25f
#define AMMO_NEED_MAX		1.50f
#define AMMO_NEED_UNUSED	0.20f	// ammo for a weapon the bot doesn't own
// bot_healthneed: urgency gain on the health-need curve (pros top up at ~74hp,
// so a curve already elevating need there matches them); 1.0 near full .. 1+GAIN
// near death.  Was an inline literal gated behind bot_survive only.
#define HEALTH_URGENCY_GAIN	2.0f

/*
=================
Item_BaseValue

Intrinsic worth of an item type, independent of the bot's current need.
=================
*/
static float Item_BaseValue (gitem_t *item)
{
	const char *nm = item->pickup_name ? item->pickup_name : "";
	const char *cn = item->classname ? item->classname : "";
	int		flags = item->flags;

	if (flags & IT_WEAPON)
		return (has(nm,"Railgun") || has(nm,"Rocket")) ? 60 :
		       (has(nm,"Hyper") || has(nm,"Chaingun") || has(nm,"Super Shotgun")) ? 45 : 35;
	if (flags & IT_ARMOR)
		return has(nm,"Body") ? 90 : has(nm,"Combat") ? 55 : has(nm,"Jacket") ? 28 : 8;
	if (flags & IT_POWERUP)
		return has(nm,"Quad") ? 120 : has(nm,"Invulnerability") ? 95 : 35;
	if (flags & IT_AMMO)
		return 12;
	if (has(cn,"health") || has(nm,"Health") || has(nm,"health"))
		return (has(cn,"mega") || has(nm,"Mega")) ? 100 : has(nm,"Large") ? 25 : 15;
	return 10;
}

/*
=================
Weapon_KillRankWeight

bot_wpnneed: acquisition need for an UNOWNED weapon, weighted by how much pros
actually kill with it instead of a flat 1.0.  Corrects the base-value tiers that
lumped the chaingun (a top-3 pro weapon) in with the barely-used hyperblaster.
From tools/dm2_combat.py need over 5859 demos (weapon_at_kill_pct): rocket 31 /
rail 28 / chaingun 28 dominate (top-3 = 87%); super shotgun 7; everything else <3%.
=================
*/
static float Weapon_KillRankWeight (gitem_t *item)
{
	const char *nm = item->pickup_name ? item->pickup_name : "";
	if (has(nm,"Railgun") || has(nm,"Rocket") || has(nm,"Chaingun"))
		return 1.25f;		// the guns pros win with -- race for these first
	if (has(nm,"Super Shotgun"))
		return 0.85f;
	return 0.55f;			// hyperblaster / GL / MG / shotgun / blaster: rarely used
}

/*
=================
Item_Score

value * need / distance-falloff.  Returns 0 for things the bot doesn't want.
=================
*/
static float Item_Score (bot_t *b, edict_t *it, float *out_dist)
{
	edict_t	*bot = b->ent;
	gitem_t	*item = it->item;
	const char *nm = item->pickup_name ? item->pickup_name : "";
	const char *cn = item->classname ? item->classname : "";
	int		flags = item->flags;
	float	value = Item_BaseValue (item), need = 0.4f, dist, falloff;
	vec3_t	d;

	VectorSubtract (it->s.origin, bot->s.origin, d);
	dist = VectorLength (d);
	*out_dist = dist;

	if (flags & IT_WEAPON)
	{
		if (bot->client->pers.inventory[ITEM_INDEX(item)] > 0)
			need = 0.25f;						// already own it
		else if (bot_wpnneed->value != 0)
			need = Weapon_KillRankWeight (item);	// race for the guns pros win with
		else
			need = 1.0f;
	}
	else if (flags & IT_ARMOR)
	{
		int ai = ArmorIndex (bot);
		int cur = ai ? bot->client->pers.inventory[ai] : 0;
		float frac = cur / 200.0f;
		if (frac > 1) frac = 1;
		need = 1.0f - frac * 0.5f;	// 0.5 .. 1.0
	}
	else if (flags & IT_POWERUP)
		need = 1.0f;
	else if (flags & IT_AMMO)
	{
		if (bot_ammoneed->value != 0)
		{
			// need spikes as the fill for the gun this ammo feeds drops
			float frac = Combat_AmmoFracForItem (bot, item);
			if (frac < 0.0f)
				need = AMMO_NEED_UNUSED;		// no owned weapon uses this ammo
			else
				need = AMMO_NEED_MIN + (AMMO_NEED_MAX - AMMO_NEED_MIN) * (1.0f - frac);
		}
		else
			need = 0.5f;						// legacy flat (off-state stays bit-exact)
	}
	else if (has(cn,"health") || has(nm,"Health") || has(nm,"health"))
	{
		if (has(cn,"mega") || has(nm,"Mega"))
			need = (bot->health < 250) ? 1.0f : 0.5f;
		else if (bot->health >= bot->max_health)
				need = 0.0f;
			else if (bot_healthneed->value != 0 || Bot_Survives (b))
			{
				// urgency: a hurt bot wants health more the lower it is, so it
				// breaks off to heal instead of dying mid-fight.  Pros top up at
				// a median of ~74hp (demo corpus), so this curve should already
				// be pulling by then.  1.0 near full .. 1+GAIN near death.
				// (bot_healthneed makes just this goal-scoring urgency the default,
				// WITHOUT bot_survive's flee coupling / asymmetric-negative frags.)
				float frac = bot->health / (float)(bot->max_health > 0 ? bot->max_health : 100);
				if (frac > 1.0f) frac = 1.0f;
				need = 1.0f + (1.0f - frac) * HEALTH_URGENCY_GAIN;
			}
			else
				need = 1.0f;
	}

	if (need <= 0)
		return 0;

	// a fleeing bot wants toughness back above everything else
	if (b->flee && Goal_IsRecovery (it))
		value *= 2.5f;

	// keep goals local enough to actually reach (chasing far items just exposes
	// nav-to-item precision limits and inflates timeouts)
	falloff = GOAL_DIST_FALLOFF;
	return value * need / (1.0f + dist / falloff);
}

// how far ahead of a respawn we'll start moving to "time" a control item
#define ITEM_PREEMPT_SECS	4.0f

/*
=================
Item_HighValue

Control items worth pre-positioning for as they respawn (value-driven; on
q2dm1 this is Combat Armor and the top weapons).
=================
*/
static qboolean Item_HighValue (edict_t *it)
{
	return Item_BaseValue (it->item) >= ITEM_CONTROL_VALUE;
}

/*
=================
Item_RespawnEta

Seconds until a picked-up item respawns, or a large number if it isn't coming
back.  0 if already available.
=================
*/
static float Item_RespawnEta (edict_t *it)
{
	if (Goal_ItemAvailable (it))
		return 0;
	if (!(it->flags & FL_RESPAWN) || it->nextthink <= 0)
		return 99999.0f;
	return it->nextthink - level.time;
}

/*
=================
Goal_Select

Picks the highest-scoring item into b->goal_item.  Considers items that are
available now, plus high-value control items about to respawn (so the bot can
time them) -- in which case b->goal_timing is set and the bot waits on the spot.
Returns true if a worthwhile goal was found.
=================
*/
#define GOAL_MAX_CAND	256
#define GOAL_MAX_TRIES	8	// reachability A* checks per decision (bounded cost)

qboolean Goal_Select (bot_t *b)
{
	edict_t	*cand[GOAL_MAX_CAND];
	float	score[GOAL_MAX_CAND];
	float	sldist[GOAL_MAX_CAND];
	byte	timing[GOAL_MAX_CAND];
	byte	rejected[GOAL_MAX_CAND];
	int		ncand = 0;
	int		start, i, tries;
	int		pathbuf[BOT_MAX_PATH];

	b->goal_item = NULL;
	b->goal_timing = false;

	// ---- gather candidate items (available now, or high-value about to spawn) ----
	for (i = (int)game.maxclients + 1; i < globals.num_edicts && ncand < GOAL_MAX_CAND; i++)
	{
		edict_t		*it = g_edicts + i;
		int			node;
		vec3_t		d;
		float		s, eta, dist;
		qboolean	avail, tm = false;

		if (!it->inuse || !it->item)
			continue;
		if (Goal_OnCooldown (it))
			continue;
		if (bot_claim->value != 0 && Bot_ItemClaimed (it, b))
			continue;	// another bot is already headed there

		avail = Goal_ItemAvailable (it);
		if (!avail)
		{
			if (!Item_HighValue (it))
				continue;
			eta = Item_RespawnEta (it);
			if (eta <= 0 || eta > ITEM_PREEMPT_SECS)
				continue;
			tm = true;
		}

		// nav graph must cover the item spot (bot_goalnode: with a routable
		// node -- an orphan exactly at the item must not shadow real coverage)
		node = Nav_NearestGoalNode (it->s.origin);
		if (node < 0)
			continue;
		VectorSubtract (nav.nodes[node].origin, it->s.origin, d);
		if (VectorLength (d) > 192)
			continue;

		s = Item_Score (b, it, &dist);
		if (s <= 0)
			continue;
		if (tm)
			s *= 0.9f;

		cand[ncand] = it;
		score[ncand] = s;
		sldist[ncand] = dist;
		timing[ncand] = tm;
		rejected[ncand] = 0;
		ncand++;
	}

	if (!ncand)
		return false;

	// ---- pick the best-scoring item we can actually path to ----
	// bot_pathcost 0: take the first reachable candidate in naive score order.
	// bot_pathcost 1: spend the same bounded set of A* checks, but re-rank the
	// reachable candidates by what the route *really* costs (A* g-cost, which
	// carries the jump/fall/water link multipliers), so an item behind a lift
	// or long detour stops out-scoring an easier one that's farther as the
	// crow flies.
	start = Nav_NearestNode (b->ent->s.origin);
	{
		int		pickcand = -1;
		float	pickscore = 0;

		for (tries = 0; tries < GOAL_MAX_TRIES; tries++)
		{
			int		best = -1;
			float	bestscore = 0;
			int		node, len;

			for (i = 0; i < ncand; i++)
				if (!rejected[i] && score[i] > bestscore)
				{
					bestscore = score[i];
					best = i;
				}
			if (best < 0)
				break;
			rejected[best] = 1;	// consumed (reachable or not)

			node = Nav_NearestGoalNode (cand[best]->s.origin);
			len = Nav_FindPathMasked (start, node, Bot_NavMask (b), pathbuf, BOT_MAX_PATH);
			if (len <= 0)
				continue;		// unreachable from here; try the next best

			// bot_goalnode 2: also skip routes the goal budget can't
			// plausibly fund.  Connected-node resolution (mode 1) makes
			// distant vertically-gated items routable, but a route costing
			// several budget-caps of travel is a giveup foretold (measured:
			// +40% giveups for flat pickups when these commit).  1.5x slack
			// because strafejump travel beats the budget's cost model.
			if (bot_goalnode->value >= 2 && bot_goalbudget->value != 0)
			{
				float cap = (bot_budgetcap->value > 0) ? bot_budgetcap->value
													   : BOT_GOAL_BUDGET_MAX;
				if (BOT_GOAL_BUDGET_BASE + Nav_LastPathCost () / BOT_GOAL_BUDGET_SPEED
					> cap * 1.5f)
					continue;	// not fundable: try the next best
			}

			if (bot_pathcost->value == 0)
			{
				b->goal_item = cand[best];
				b->goal_timing = timing[best];
				return true;
			}

			// recover value*need from the naive score, re-divide by path cost
			{
				float	s = score[best] * (1.0f + sldist[best] / GOAL_DIST_FALLOFF)
							/ (1.0f + Nav_LastPathCost () / GOAL_DIST_FALLOFF);
				if (s > pickscore)
				{
					pickscore = s;
					pickcand = best;
				}
			}
		}

		if (pickcand >= 0)
		{
			b->goal_item = cand[pickcand];
			b->goal_timing = timing[pickcand];
			return true;
		}
	}

	return false;	// nothing reachable
}

/*
=================
Goal_NearestItem

Nearest currently-available item the bot wants (Item_Score > 0), within maxdist,
ignoring nav coverage and cooldown.  Used for directed exploration so bots walk
toward items (collecting them by contact and connecting their spots into the
graph) even before a routed path exists.

bot_decisive: this runs EVERY FRAME between goals, and the raw argmin made a
bot standing between two near-equal items swing its heading A<->B for the whole
inter-goal gap (measured: 24% of goal transitions were >=1s standing re-decides,
median 2.3s) -- and happily pulled toward the very item the bot just gave up on
(cooldown was ignored).  Decisive mode adds the blacklist check and a sticky
target: keep the current steer target unless it dies or something meaningfully
(25%) closer shows up.
=================
*/
edict_t *Goal_NearestItem (bot_t *b, float maxdist)
{
	edict_t	*it, *best = NULL;
	float	bestd = maxdist, dist;
	qboolean decisive = (bot_decisive->value != 0);
	int		i;

	for (i = (int)game.maxclients + 1; i < globals.num_edicts; i++)
	{
		it = g_edicts + i;
		if (!Goal_ItemAvailable (it))
			continue;
		if (decisive && Goal_OnCooldown (it))
			continue;			// never steer at what we just abandoned
		if (Item_Score (b, it, &dist) <= 0)
			continue;
		if (dist < bestd)
		{
			bestd = dist;
			best = it;
		}
	}

	if (decisive)
	{
		// sticky steer target: hold the previous pick unless it's gone stale
		// (collected, cooled down, unwanted, out of range) or the new winner
		// is at least 25% closer
		if (b->steer_item && b->steer_item != best)
		{
			float	curd;

			if (Goal_ItemAvailable (b->steer_item)
				&& !Goal_OnCooldown (b->steer_item)
				&& Item_Score (b, b->steer_item, &curd) > 0
				&& curd < maxdist
				&& !(best && bestd < curd * 0.75f))
				return b->steer_item;
		}
		b->steer_item = best;
	}
	return best;
}
