/*
oz_ctf.c -- capture the flag for ozbot on q2red.

The brain was written for free-for-all deathmatch; on q2red the server can
run a rerelease CTF (MuffMode, MMREX, id's), and every bot used to roam for
items past both flags.  This layer gives each bot a flag objective ahead of
the item economy, through the ordinary GOAL machinery (path, steering, lifts,
stuck recovery):

  1. carrying the enemy flag     -> go home and touch our flag (capture), or,
                                    if ours is not home, wait on its stand
  2. our flag lies dropped        -> go and touch it (return)
  3. an enemy carries our flag    -> hunt the carrier
  4. the enemy flag is home or    -> go and take it: one bot per team
     lying dropped                   attacks a home flag, and it fetches a
                                     weapon first unless the flag is close
  5. otherwise                    -> items, as in deathmatch

Everything is read from what the rerelease publishes in sv_entity_t:

  - flags are items with classname item_flag_team1 (red, team 1) and
    item_flag_team2 (blue, team 2); MuffMode renames them item_flag_team_red
    and item_flag_team_blue;
  - the flag at home is the entity the map spawned, and it goes hidden while
    someone carries it; a dropped flag is a second entity of the same
    classname.  Items do not publish their spawnflags, so DROPPED_ITEM cannot
    tell them apart: the home flag is remembered as the first one seen on the
    map (only home flags exist when a map starts);
  - a carrier has the flag in its inventory.

Where the map has no flags this is inert.  Nothing here is compiled into the
q2repro DLL.
*/

#include "../src/g_local.h"
#include "../src/bot.h"
#include "../src/bot_nav.h"

#define CTF_TEAMS			3		// index by team: 1 red, 2 blue
#define CTF_TAKE_NEAR		1200.0f	// an enemy flag this close is worth taking unarmed
#define CTF_HUNT_REAIM		384.0f	// re-route a hunt when the carrier moves this far
#define CTF_HUNT_SECS		3.0f	// ...or after this long on one route

typedef struct
{
	edict_t	*home;			// the flag at its stand (hidden while carried)
	edict_t	*dropped;		// lying dropped, NULL if none
	edict_t	*carrier;		// the client carrying it, NULL if none
	qboolean	at_home;	// home flag visible and touchable
} ctf_flag_t;

static ctf_flag_t	ctf_flags[CTF_TEAMS];	// by the flag's team
static qboolean		ctf_active;
static gitem_t		*ctf_item[CTF_TEAMS];
static int			ctf_home_num[CTF_TEAMS];	// edict number of each home flag

int OzHost_Team (edict_t *ent);

static int Ctf_ItemTeam (gitem_t *item)
{
	if (!item || !item->classname)
		return 0;
	if (!strcmp (item->classname, "item_flag_team1")
		|| !strcmp (item->classname, "item_flag_team_red"))
		return 1;
	if (!strcmp (item->classname, "item_flag_team2")
		|| !strcmp (item->classname, "item_flag_team_blue"))
		return 2;
	return 0;
}

qboolean Ctf_IsFlag (edict_t *it)
{
	return it && Ctf_ItemTeam (it->item) != 0;
}

void Ctf_MapBegin (void)
{
	memset (ctf_home_num, 0, sizeof(ctf_home_num));
	memset (ctf_flags, 0, sizeof(ctf_flags));
	ctf_active = false;
}

/*
=================
Ctf_Frame

Read the three flag states once per frame, after the world is mirrored.
=================
*/
void Ctf_Frame (void)
{
	int		i, t;

	memset (ctf_flags, 0, sizeof(ctf_flags));
	memset (ctf_item, 0, sizeof(ctf_item));
	ctf_active = false;

	for (i = (int)game.maxclients + 1; i < globals.num_edicts; i++)
	{
		edict_t	*e = g_edicts + i;

		if (!e->inuse || !(t = Ctf_ItemTeam (e->item)))
			continue;
		ctf_item[t] = e->item;
		if (!ctf_home_num[t])
			ctf_home_num[t] = i;
		if (i == ctf_home_num[t])
		{
			ctf_flags[t].home = e;
			ctf_flags[t].at_home = Goal_ItemAvailable (e);
		}
		else if (Goal_ItemAvailable (e))
			ctf_flags[t].dropped = e;
	}
	// a home flag that went away (the mod reset it): learn it again
	for (t = 1; t < CTF_TEAMS; t++)
		if (ctf_home_num[t] && !ctf_flags[t].home)
			ctf_home_num[t] = 0;
	if (!ctf_flags[1].home || !ctf_flags[2].home)
		return;
	ctf_active = true;

	for (i = 1; i <= (int)game.maxclients; i++)
	{
		edict_t	*c = g_edicts + i;

		if (!c->inuse || !c->client || c->deadflag || c->health <= 0)
			continue;
		for (t = 1; t < CTF_TEAMS; t++)
			if (ctf_item[t] && c->client->pers.inventory[ITEM_INDEX (ctf_item[t])] > 0)
				ctf_flags[t].carrier = c;
	}
}

qboolean Ctf_Active (void)
{
	return ctf_active;
}

static int Ctf_Enemy (int team)
{
	return team == 1 ? 2 : team == 2 ? 1 : 0;
}

/* The bot carries the enemy flag. */
static qboolean Ctf_Carrying (bot_t *b, int team)
{
	return ctf_flags[Ctf_Enemy (team)].carrier == b->ent;
}

/* o carries a flag of `team`'s: the target a defender wants most. */
qboolean Ctf_CarriesFlagOf (edict_t *o, int team)
{
	return ctf_active && team > 0 && team < CTF_TEAMS && o
		&& ctf_flags[team].carrier == o;
}

int Ctf_TeamOf (edict_t *ent)
{
	return OzHost_Team (ent);
}

/* Armed beyond the blaster: worth sending into the enemy base. */
static qboolean Ctf_Armed (edict_t *ent)
{
	int	i;

	for (i = 1; i < game.num_items; i++)
	{
		gitem_t	*it = &itemlist[i];
		if (!(it->flags & IT_WEAPON) || !it->pickup_name
			|| !strcmp (it->pickup_name, "Blaster") || (it->flags & IT_AMMO))
			continue;
		if (ent->client->pers.inventory[i] > 0)
			return true;
	}
	return false;
}

/*
=================
Ctf_Want

The objective this bot should be on now (CTF_NONE: play the item economy).
*ent is the flag to touch or the carrier to hunt; *timing is set for a wait
on our empty stand (the goal machinery holds there until the flag returns).
=================
*/
int Ctf_Want (bot_t *b, edict_t **ent, qboolean *timing)
{
	int			team, enemy;
	ctf_flag_t	*mine, *theirs;
	vec3_t		d;

	*ent = NULL;
	*timing = false;
	if (!ctf_active || !b->ent || !b->ent->client || b->ent->deadflag)
		return CTF_NONE;
	team = OzHost_Team (b->ent);
	if (!(enemy = Ctf_Enemy (team)))
		return CTF_NONE;
	mine = &ctf_flags[team];
	theirs = &ctf_flags[enemy];

	if (Ctf_Carrying (b, team))
	{
		*ent = mine->home;
		*timing = !mine->at_home;
		return mine->at_home ? CTF_CAPTURE : CTF_HOLD;
	}
	if (mine->dropped)
	{
		*ent = mine->dropped;
		return CTF_RETURN;
	}
	if (mine->carrier)
	{
		*ent = mine->carrier;
		return CTF_HUNT;
	}
	if (theirs->dropped)
	{
		*ent = theirs->dropped;
		return CTF_TAKE;
	}
	if (theirs->at_home && !Bot_CtfClaimed (b, CTF_TAKE, theirs->home))
	{
		VectorSubtract (theirs->home->s.origin, b->ent->s.origin, d);
		if (Ctf_Armed (b->ent) || VectorLength (d) < CTF_TAKE_NEAR)
		{
			*ent = theirs->home;
			return CTF_TAKE;
		}
	}
	return CTF_NONE;
}

/*
=================
Ctf_LogOutcome

Telemetry (bot_telemetry 1): what a retired objective achieved, read from
the flag states.  ctf_took / ctf_captured / ctf_returned.
=================
*/
static void Ctf_LogOutcome (bot_t *b)
{
	int		team = OzHost_Team (b->ent);
	int		enemy = Ctf_Enemy (team);

	if (!enemy || b->ent->deadflag || b->ent->health <= 0)
		return;
	if (b->ctf_kind == CTF_TAKE && Ctf_Carrying (b, team))
		Bot_LogEvent (b, "ctf_took");
	else if (b->ctf_kind == CTF_CAPTURE && !Ctf_Carrying (b, team) && ctf_flags[team].at_home)
		Bot_LogEvent (b, "ctf_captured");
	else if (b->ctf_kind == CTF_RETURN && !ctf_flags[team].dropped && ctf_flags[team].at_home)
	{
		vec3_t	d;
		VectorSubtract (b->ctf_point, b->ent->s.origin, d);
		if (VectorLength (d) < 128)		// we touched it (not a timeout or a teammate)
			Bot_LogEvent (b, "ctf_returned");
	}
}

/*
=================
Ctf_LegStale

True when the bot's current GOAL leg should be dropped for a new pick:
its objective changed or was met, or one that outranks an item route (our
flag in play, or the enemy flag in our hands) appeared.  Taking the enemy
flag does not interrupt an item route: the bot finishes the pickup first.
=================
*/
qboolean Ctf_LegStale (bot_t *b)
{
	edict_t		*ent;
	qboolean	timing;
	int			want;
	vec3_t		d;

	if (!ctf_active)
		return b->ctf_kind != CTF_NONE;
	want = Ctf_Want (b, &ent, &timing);

	if (b->ctf_kind == CTF_NONE)
	{
		if (want == b->ctf_fail_kind && ent == b->ctf_fail_ent && level.time < b->ctf_fail_until)
			return false;	// no route to it just now: the bot is on items meanwhile
		if (want == CTF_NONE || want == CTF_TAKE)
			return want == CTF_TAKE && !b->goal_item && !b->pursuing;	// a roam leg
		return true;
	}
	if (want != b->ctf_kind || ent != b->ctf_ent)
	{
		Ctf_LogOutcome (b);
		return true;
	}
	if (b->ctf_approach && b->path_idx >= b->path_len)
		return true;	// at the edge of the graph: push on (Ctf_SteerTarget)
	if (want == CTF_HUNT)
	{
		VectorSubtract (ent->s.origin, b->ctf_point, d);
		if (VectorLength (d) > CTF_HUNT_REAIM || level.time - b->ctf_time > CTF_HUNT_SECS)
			return true;
	}
	return false;
}

/*
=================
Ctf_Pick

At a replan, put the bot on its objective if the graph can route there from
`start`: sets goal_item (the flag) and goal_timing, records the objective and
returns its node in *goal.  An objective with no route is put on a short
cooldown for this bot and false is returned, so the replan falls through to
the item economy instead of retrying the dead route every second.
=================
*/
#define CTF_FAIL_SECS	8.0f
#define CTF_APPROACH_GAIN	192.0f	// an approach must end at least this much closer

/*
The node reachable from `start` that lies nearest `target`, if it is
meaningfully nearer than `start` itself (else -1).  A plain flood over the
links; Ctf_Pick then routes to it with the bot's own mask.
*/
static int Ctf_ApproachNode (bot_t *b, int start, vec3_t target)
{
	static int	queue[NAV_MAX_NODES];
	static byte	seen[NAV_MAX_NODES];
	int			head = 0, tail = 0, best = -1, len, i;
	float		bestd, d0;
	vec3_t		d;
	int			path[BOT_MAX_PATH];

	if (start < 0 || start >= nav.num_nodes)
		return -1;
	memset (seen, 0, (size_t)nav.num_nodes);
	VectorSubtract (nav.nodes[start].origin, target, d);
	d0 = bestd = VectorLength (d);
	queue[tail++] = start;
	seen[start] = 1;
	while (head < tail)
	{
		nav_node_t	*n = &nav.nodes[queue[head++]];
		for (i = 0; i < n->num_links; i++)
		{
			int		to = n->links[i].to;
			float	dd;
			if (to < 0 || to >= nav.num_nodes || seen[to])
				continue;
			seen[to] = 1;
			queue[tail++] = to;
			VectorSubtract (nav.nodes[to].origin, target, d);
			dd = VectorLength (d);
			if (dd < bestd)
			{
				bestd = dd;
				best = to;
			}
		}
	}
	if (best < 0 || bestd > d0 - CTF_APPROACH_GAIN)
		return -1;
	len = Nav_FindPathMasked (start, best, Bot_NavMask (b), path, BOT_MAX_PATH);
	return len > 1 ? best : -1;
}

qboolean Ctf_Pick (bot_t *b, int start, int *goal)
{
	edict_t		*ent;
	qboolean	timing;
	int			want = Ctf_Want (b, &ent, &timing);
	int			node, len, path[BOT_MAX_PATH];

	b->ctf_kind = CTF_NONE;
	b->ctf_ent = NULL;
	if (want == CTF_NONE || !ent)
		return false;
	if (want == b->ctf_fail_kind && ent == b->ctf_fail_ent && level.time < b->ctf_fail_until)
		return false;

	node = Nav_NearestGoalNode (ent->s.origin);
	len = (node < 0 || start < 0) ? 0
		: (node == start) ? 1
		: Nav_FindPathMasked (start, node, Bot_NavMask (b), path, BOT_MAX_PATH);
	b->ctf_approach = false;
	if (len <= 0)
	{
		// no route: get as close as the graph goes, then push on from there
		// (Ctf_SteerTarget).  Our base and theirs are often separate pieces
		// of a graph that has not yet seen anyone cross between them.
		b->ctf_fail_kind = want;
		b->ctf_fail_ent = ent;
		b->ctf_fail_until = level.time + CTF_FAIL_SECS;
		node = Ctf_ApproachNode (b, start, ent->s.origin);
		if (node < 0)
			return false;
		b->ctf_approach = true;
	}

	b->ctf_kind = want;
	b->ctf_ent = ent;
	b->ctf_time = level.time;
	VectorCopy (ent->s.origin, b->ctf_point);
	// a hunt routes to where the carrier is now and owns no item; nor does
	// an approach, which ends at the edge of the graph
	b->goal_item = (want == CTF_HUNT || b->ctf_approach) ? NULL : ent;
	b->goal_timing = b->ctf_approach ? false : timing;
	*goal = node;
	return true;
}

/*
After a stale leg: how long to stay off the planner.  At the end of an
approach the bot pushes on toward the flag (Ctf_SteerTarget) for a while
rather than taking the first item route on offer.
*/
#define CTF_PUSH_SECS	4.0f

float Ctf_ReplanDelay (bot_t *b)
{
	return (b->ctf_approach && b->path_idx >= b->path_len) ? CTF_PUSH_SECS : 0;
}

/*
=================
Ctf_SteerTarget

Between legs: a flag objective the graph could not route to, or one in sight
within maxdist, for the bot to walk straight at.  On a learning
graph this is what joins the bases up; on a frozen one it covers the gap.
=================
*/
edict_t *Ctf_SteerTarget (bot_t *b, float maxdist)
{
	edict_t		*ent;
	qboolean	timing;
	vec3_t		d;
	int			want = Ctf_Want (b, &ent, &timing);

	if (want == CTF_NONE || want == CTF_HUNT || !ent)
		return NULL;
	// out of the graph's reach just now: head straight for it from wherever
	// the approach left us (stuck recovery takes over at a wall)
	if (ent == b->ctf_fail_ent && level.time < b->ctf_fail_until)
		return ent;
	// else only one in sight and near
	VectorSubtract (ent->s.origin, b->ent->s.origin, d);
	if (VectorLength (d) > maxdist || !visible (b->ent, ent))
		return NULL;
	return ent;
}

const char *Ctf_KindName (int kind)
{
	switch (kind)
	{
	case CTF_TAKE:		return "ctf_take";
	case CTF_CAPTURE:	return "ctf_capture";
	case CTF_HOLD:		return "ctf_hold";
	case CTF_RETURN:	return "ctf_return";
	case CTF_HUNT:		return "ctf_hunt";
	}
	return "ctf_none";
}
