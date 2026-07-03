/*
ozbot - self-learning q2dm1 bot

bot_log.c -- JSONL telemetry.

Writes one JSON object per line to <gamedir>/logs/<map>_<timestamp>.jsonl:
  - per-tick state records (~10 Hz, one per bot per frame)
  - event records (spawn, death, remove)

Paths are resolved relative to the server's working directory with the active
game (mod) directory as a prefix, matching how the rest of the game writes
files (see g_svcmds.c SVCmd_WriteIP_f).
*/

#include "g_local.h"
#include "bot.h"
#include "bot_nav.h"

#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define OZ_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define OZ_MKDIR(p) mkdir(p, 0755)
#endif

static FILE		*log_fp;
static float	log_last_flush;

/*
=================
Bot_GameDir
=================
*/
const char *Bot_GameDir (void)
{
	cvar_t *gamecvar = gi.cvar ("game", "", 0);
	if (gamecvar && gamecvar->string && gamecvar->string[0])
		return gamecvar->string;
	return GAMEVERSION;	// "baseq2"
}

/*
=================
Bot_LogBeginLevel

Opens a fresh telemetry file for the given map.  Closes any previous file.
=================
*/
void Bot_LogBeginLevel (const char *mapname)
{
	char		dir[MAX_OSPATH];
	char		path[MAX_OSPATH];
	char		stamp[32];
	time_t		now;
	struct tm	*lt;

	Bot_LogEndLevel ();

	now = time (NULL);
	lt = localtime (&now);
	if (lt)
		strftime (stamp, sizeof(stamp), "%Y%m%d_%H%M%S", lt);
	else
		Com_sprintf (stamp, sizeof(stamp), "0");

	Com_sprintf (dir, sizeof(dir), "%s/logs", Bot_GameDir());
	OZ_MKDIR (dir);	// ignore "already exists"

	Com_sprintf (path, sizeof(path), "%s/%s_%s.jsonl",
		dir, (mapname && mapname[0]) ? mapname : "unknown", stamp);

	log_fp = fopen (path, "w");
	if (!log_fp)
	{
		gi.dprintf ("ozbot: could not open log file %s\n", path);
		return;
	}

	log_last_flush = level.time;
	gi.dprintf ("ozbot: logging telemetry to %s\n", path);
}

/*
=================
Bot_LogEndLevel
=================
*/
void Bot_LogEndLevel (void)
{
	if (log_fp)
	{
		fclose (log_fp);
		log_fp = NULL;
	}
}

/*
=================
Bot_LogTick

One per-bot state record.
=================
*/
void Bot_LogTick (bot_t *b)
{
	edict_t	*ent;
	int		armor_index, armor;
	const char *weapon;

	if (!log_fp || !b || !b->ent || !b->ent->client)
		return;

	ent = b->ent;

	armor_index = ArmorIndex (ent);
	armor = armor_index ? ent->client->pers.inventory[armor_index] : 0;

	// the default Blaster has a NULL pickup_name; label it rather than "none"
	if (!ent->client->pers.weapon)
		weapon = "none";
	else if (ent->client->pers.weapon->pickup_name)
		weapon = ent->client->pers.weapon->pickup_name;
	else
		weapon = "Blaster";

	fprintf (log_fp,
		"{\"type\":\"tick\",\"t\":%.2f,\"bot\":%d,\"name\":\"OzBot%d\","
		"\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,"
		"\"vx\":%.1f,\"vy\":%.1f,\"vz\":%.1f,"
		"\"yaw\":%.1f,\"onground\":%s,"
		"\"health\":%d,\"armor\":%d,\"weapon\":\"%s\",\"dead\":%s,"
		"\"mode\":%d,\"cur_node\":%d,\"goal_node\":%d,\"path_len\":%d,\"nav_nodes\":%d,"
		"\"enemy\":%d,\"score\":%d}\n",
		level.time, b->id, b->id,
		ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
		ent->velocity[0], ent->velocity[1], ent->velocity[2],
		ent->client->ps.viewangles[YAW],
		ent->groundentity ? "true" : "false",
		ent->health, armor, weapon,
		ent->deadflag ? "true" : "false",
		b->mode, b->cur_node, b->goal_node, b->path_len, nav.num_nodes,
		b->enemy ? (int)(b->enemy - g_edicts) : -1,
		ent->client->resp.score);
}

/*
=================
Bot_LogEvent
=================
*/
void Bot_LogEvent (bot_t *b, const char *event)
{
	edict_t	*ent;

	if (!log_fp || !b || !b->ent)
		return;

	ent = b->ent;

	fprintf (log_fp,
		"{\"type\":\"event\",\"event\":\"%s\",\"t\":%.2f,\"bot\":%d,"
		"\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"health\":%d}\n",
		event, level.time, b->id,
		ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
		ent->health);
}

/*
=================
Bot_LogItemEvent

Like Bot_LogEvent but with an "item" field (pickup / goal_item / item_lost).
=================
*/
void Bot_LogItemEvent (bot_t *b, const char *event, const char *item)
{
	edict_t	*ent;

	if (!log_fp || !b || !b->ent)
		return;

	ent = b->ent;

	fprintf (log_fp,
		"{\"type\":\"event\",\"event\":\"%s\",\"t\":%.2f,\"bot\":%d,\"item\":\"%s\","
		"\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"health\":%d}\n",
		event, level.time, b->id, item ? item : "",
		ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
		ent->health);
}

/*
=================
Bot_LogGiveup

A giveup with diagnostic context: how far (horizontal/vertical) from the goal,
whether we'd reached the goal node, and whether we were fighting -- so the
analyzer can tell *why* item runs fail.
=================
*/
void Bot_LogGiveup (bot_t *b, float gdist, float gvdist, int atnode, int fighting)
{
	edict_t	*ent;

	if (!log_fp || !b || !b->ent)
		return;

	ent = b->ent;
	fprintf (log_fp,
		"{\"type\":\"event\",\"event\":\"giveup\",\"t\":%.2f,\"bot\":%d,"
		"\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,"
		"\"gdist\":%.0f,\"gvdist\":%.0f,\"atnode\":%d,\"fighting\":%d,"
		"\"pidx\":%d,\"plen\":%d,\"gbest\":%.0f}\n",
		level.time, b->id,
		ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
		gdist, gvdist, atnode, fighting,
		b->path_idx, b->path_len, b->goal_best);
}

//==========================================================================
// bot_liftlog -- Phase-0 diagnosis instrumentation for the lift-riding plan
// (plans/lift-riding.md).  Gated on the bot_liftlog cvar; throwaway.
//==========================================================================

#define LIFTLOG_MAX_PLATS	8
#define LIFTLOG_RANGE		400.0f	// log bots within this 2D range of a plat

static edict_t	*liftlog_plats[LIFTLOG_MAX_PLATS];
static int		liftlog_num_plats;

/*
=================
Bot_LogLiftBegin

Cache the map's func_plat entities and emit one platinfo record per plat, so
the analyzer knows each plat's footprint, travel range, and rest state.
Called once per map after entities have spawned.
=================
*/
void Bot_LogLiftBegin (void)
{
	int i;

	liftlog_num_plats = 0;
	if (!bot_liftlog || bot_liftlog->value == 0)
		return;

	for (i = (int)game.maxclients + 1; i < globals.num_edicts; i++)
	{
		edict_t *e = g_edicts + i;
		if (!e->inuse || !e->classname || strcmp (e->classname, "func_plat") != 0)
			continue;
		if (liftlog_num_plats >= LIFTLOG_MAX_PLATS)
			break;
		liftlog_plats[liftlog_num_plats] = e;
		if (log_fp)
			fprintf (log_fp,
				"{\"type\":\"platinfo\",\"plat\":%d,\"ent\":%d,"
				"\"absmin\":[%.0f,%.0f,%.0f],\"absmax\":[%.0f,%.0f,%.0f],"
				"\"top\":%.0f,\"bottom\":%.0f,"
				"\"state\":%d,\"targeted\":%d,\"speed\":%.0f}\n",
				liftlog_num_plats, (int)(e - g_edicts),
				e->absmin[0], e->absmin[1], e->absmin[2],
				e->absmax[0], e->absmax[1], e->absmax[2],
				e->pos1[2], e->pos2[2],
				e->moveinfo.state, e->targetname ? 1 : 0,
				e->moveinfo.speed);
		liftlog_num_plats++;
	}
	gi.dprintf ("ozbot: liftlog tracking %d func_plat(s)\n", liftlog_num_plats);
}

/*
=================
Bot_LogLiftTick

One record per bot per frame while within LIFTLOG_RANGE (2D) of a tracked
plat: where the bot is, what it stands on, the plat's state, and the nav
context (mode/goal/next waypoint/stall clock) -- enough to attribute an
approach failure to a specific subsystem.
=================
*/
void Bot_LogLiftTick (bot_t *b)
{
	edict_t	*ent;
	int		i;

	if (!log_fp || !bot_liftlog || bot_liftlog->value == 0 || !liftlog_num_plats)
		return;
	if (!b || !b->ent || !b->ent->client || b->ent->deadflag)
		return;

	ent = b->ent;
	for (i = 0; i < liftlog_num_plats; i++)
	{
		edict_t	*p = liftlog_plats[i];
		float	cx = (p->absmin[0] + p->absmax[0]) * 0.5f;
		float	cy = (p->absmin[1] + p->absmax[1]) * 0.5f;
		float	dx = ent->s.origin[0] - cx;
		float	dy = ent->s.origin[1] - cy;
		int		next_node = (b->path_idx < b->path_len) ? b->path[b->path_idx] : -1;

		if (dx*dx + dy*dy > LIFTLOG_RANGE * LIFTLOG_RANGE)
			continue;

		fprintf (log_fp,
			"{\"type\":\"liftlog\",\"t\":%.2f,\"bot\":%d,\"plat\":%d,"
			"\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,"
			"\"pstate\":%d,\"ptopz\":%.1f,"
			"\"ground\":\"%s\",\"onplat\":%d,"
			"\"mode\":%d,\"item\":\"%s\",\"pidx\":%d,\"plen\":%d,"
			"\"nextnode\":%d,\"nextz\":%.0f,"
			"\"stall\":%.1f,\"lstate\":%d}\n",
			level.time, b->id, i,
			ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
			p->moveinfo.state, p->absmax[2],
			ent->groundentity ? (ent->groundentity->classname ? ent->groundentity->classname : "?") : "",
			(ent->groundentity == p) ? 1 : 0,
			b->mode,
			(b->goal_item && b->goal_item->item && b->goal_item->item->pickup_name)
				? b->goal_item->item->pickup_name : "",
			b->path_idx, b->path_len,
			next_node,
			(next_node >= 0 && next_node < nav.num_nodes) ? nav.nodes[next_node].origin[2] : 0.0f,
			level.time - b->progress_time,
			b->lift_state);
	}
}

/*
=================
Bot_LogPenalize

Records a Nav_PenalizeLink call (the stuck-replan path punishing a link), so
the miner can see exactly when the learned lift column gets penalized away.
=================
*/
void Bot_LogPenalize (bot_t *b, int from, int to)
{
	if (!log_fp || !bot_liftlog || bot_liftlog->value == 0)
		return;
	if (!b || !b->ent)
		return;
	if (from < 0 || to < 0 || from >= nav.num_nodes || to >= nav.num_nodes)
		return;

	fprintf (log_fp,
		"{\"type\":\"event\",\"event\":\"penalize\",\"t\":%.2f,\"bot\":%d,"
		"\"from\":%d,\"to\":%d,\"fz\":%.0f,\"tz\":%.0f,"
		"\"x\":%.1f,\"y\":%.1f,\"z\":%.1f}\n",
		level.time, b->id,
		from, to, nav.nodes[from].origin[2], nav.nodes[to].origin[2],
		b->ent->s.origin[0], b->ent->s.origin[1], b->ent->s.origin[2]);
}

/*
=================
Bot_LogMaybeFlush

Flush at most ~1 Hz to bound data loss without per-line I/O cost.
=================
*/
void Bot_LogMaybeFlush (void)
{
	if (!log_fp)
		return;
	if (level.time - log_last_flush >= 1.0)
	{
		fflush (log_fp);
		log_last_flush = level.time;
	}
}
