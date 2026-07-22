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

// bot_capture: separate, user-named input-log file driven by the in-game
// "capture start/stop" command -- independent of the per-level telemetry
// log_fp above, so a human can carve many granular takes in one session.
static FILE		*capture_fp;
static int		capture_frames;
static char		capture_name[64];

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

	// run header: lets analyzers know the game tick rate instead of assuming
	// 10Hz (the ozbot-re 40Hz port varies it).  No wall-clock fields -- the
	// same-seed bit-exact md5 gate hashes this line too.
	fprintf (log_fp, "{\"type\":\"run\",\"map\":\"%s\",\"tick_rate\":%d,\"maxclients\":%d}\n",
		(mapname && mapname[0]) ? mapname : "unknown",
		(int)(1.0f / FRAMETIME + 0.5f), (int)game.maxclients);

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

	// a "map" change or ShutdownGame routes here -- make sure an in-flight
	// named capture is closed too, so it isn't left truncated.
	Bot_CaptureShutdown ();
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
		"\"yaw\":%.2f,\"pitch\":%.2f,\"onground\":%s,"
		"\"health\":%d,\"armor\":%d,\"weapon\":\"%s\",\"dead\":%s,"
		"\"mode\":%d,\"cur_node\":%d,\"goal_node\":%d,\"path_len\":%d,\"nav_nodes\":%d,"
		"\"enemy\":%d,\"score\":%d}\n",
		level.time, b->id, b->id,
		ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
		ent->velocity[0], ent->velocity[1], ent->velocity[2],
		ent->client->ps.viewangles[YAW],
		ent->client->ps.viewangles[PITCH],
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
Bot_LogHazDeath

bot_hazlog: one record per death classifying it for the environmental-death
audit -- the MOD, whether the bot was airborne / in liquid, its velocity
(a fast downward vz = fell/knocked in), path state, and whether the
bot_hazard death penalty could fire (a route existed to erode).  Lets an
analysis split the residual hazard deaths into ground-walk-in (should be
caught by the steer probe), airborne fall/knockback (currently missed), and
already-in-liquid.  Gated on the cvar so off-state telemetry is byte-exact.
=================
*/
void Bot_LogHazDeath (bot_t *b, int mod, qboolean penalized)
{
	edict_t	*ent;

	if (!log_fp || !bot_hazlog || bot_hazlog->value == 0 || !b || !b->ent)
		return;

	ent = b->ent;
	fprintf (log_fp,
		"{\"type\":\"event\",\"event\":\"hazclass\",\"t\":%.2f,\"bot\":%d,"
		"\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"vz\":%.1f,"
		"\"mod\":%d,\"onground\":%s,\"waterlevel\":%d,\"watertype\":%d,"
		"\"mode\":%d,\"enemy\":%d,\"path_idx\":%d,\"path_len\":%d,"
		"\"penalized\":%s}\n",
		level.time, b->id,
		ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], ent->velocity[2],
		mod, ent->groundentity ? "true" : "false",
		ent->waterlevel, ent->watertype,
		b->mode, b->enemy ? (int)(b->enemy - g_edicts) : -1,
		b->path_idx, b->path_len,
		penalized ? "true" : "false");
}

/*
=================
Bot_LogFire

One record per weapon discharge (hooked from PlayerNoise PNOISE_WEAPON, which
every firing weapon reaches).  Built for tools/verify_timing.py: fire-rate
invariants are the acceptance gate for the variable-FPS (40Hz) conversion --
a miss in the FRAMESYNC weapon gating shows up here as a 4x rate change.
=================
*/
void Bot_LogFire (edict_t *who)
{
	if (!log_fp || !who->client)
		return;

	fprintf (log_fp,
		"{\"type\":\"event\",\"event\":\"fire\",\"t\":%.2f,\"bot\":%d,\"weapon\":\"%s\"}\n",
		level.time, (int)(who - g_edicts) - 1,
		who->client->pers.weapon ? who->client->pers.weapon->classname : "unknown");
}

/*
=================
Bot_LogEngage

bot_wpnlog: one record per 10Hz keyframe the bot is engaged, capturing the held
weapon, the range to the target, and the engagement intent (ENG_*).  Gated on
the cvar so the off-state telemetry is byte-identical.  tools/humanness.py
wpnrange reads these against the demo weapon_profiles.json to check that the
bot's per-weapon engagement range separates (rail far, SSG close) toward human.
=================
*/
void Bot_LogEngage (bot_t *b, const char *weapon, float range, int intent)
{
	if (!log_fp || !bot_wpnlog || bot_wpnlog->value == 0 || !b)
		return;
	fprintf (log_fp,
		"{\"type\":\"engage\",\"t\":%.2f,\"bot\":%d,\"weapon\":\"%s\","
		"\"range\":%.0f,\"intent\":%d}\n",
		level.time, b->id, weapon, range, intent);
}

// Either the master lever or the parity harness is live.  Both arms of a
// bot_pursuittest A/B must log or the treatment arm is undiagnosable; with both
// cvars at 0 nothing is written, so the off-state stays byte-identical.
static qboolean PursueLogOn (void)
{
	return (bot_pursuit && bot_pursuit->value != 0)
		|| (bot_pursuittest && bot_pursuittest->value != 0);
}

/*
=================
Bot_LogPursueStart / Bot_LogPursueEnd

bot_pursuit telemetry.  `cost` is the A* g-cost of the committed chase route
(what bot_pursuitcost bounds) and `dist` the straight-line distance to the spot,
so the pair sizes how much travel a chase actually diverts from the item
economy; `src` is what cued it.  The end record's reason/dur distribution is the
diagnostic that says whether chases are paying: mostly "reacquired" means the
lever is finding fights, mostly "timeout"/"arrived" means it is buying walks.
Gated on the cvar so off-state telemetry is byte-identical.
=================
*/
void Bot_LogPursueStart (bot_t *b, float cost, float dist, const char *src)
{
	if (!log_fp || !b || !PursueLogOn ())
		return;
	fprintf (log_fp,
		"{\"type\":\"pursue_start\",\"t\":%.2f,\"bot\":%d,\"cost\":%.0f,"
		"\"dist\":%.0f,\"src\":\"%s\"}\n",
		level.time, b->id, cost, dist, src);
}

void Bot_LogPursueEnd (bot_t *b, const char *reason, float dur)
{
	if (!log_fp || !b || !PursueLogOn ())
		return;
	fprintf (log_fp,
		"{\"type\":\"pursue_end\",\"t\":%.2f,\"bot\":%d,\"reason\":\"%s\","
		"\"dur\":%.2f}\n",
		level.time, b->id, reason, dur);
}

/*
=================
Bot_LogHear

bot_hearlog: one record each time a heard noise is promoted to a pursuit cue.
Kind + distance is what says whether hearing is buying anything real or just
re-reporting gunfights the bot could already see.
=================
*/
void Bot_LogHear (bot_t *b, int kind, float dist)
{
	if (!log_fp || !b || !bot_hearlog || bot_hearlog->value == 0)
		return;
	fprintf (log_fp,
		"{\"type\":\"hear\",\"t\":%.2f,\"bot\":%d,\"kind\":%d,\"dist\":%.0f}\n",
		level.time, b->id, kind, dist);
}

/*
=================
Bot_LogCMove

bot_cmlog: one record per engagement-movement style transition, so an A/B can
confirm the style is actually engaging (and at what ranges) rather than being
gated out by its own eligibility test.
=================
*/
void Bot_LogCMove (bot_t *b, int style, float range)
{
	if (!log_fp || !b || !bot_cmlog || bot_cmlog->value == 0)
		return;
	fprintf (log_fp,
		"{\"type\":\"cmove\",\"t\":%.2f,\"bot\":%d,\"style\":%d,\"range\":%.0f}\n",
		level.time, b->id, style, range);
}

/*
=================
Bot_LogWpnSel

bot_wpnsellog: one record each time range-aware selection (bot_wpnselect) chooses
a firing weapon, capturing the chosen weapon, the weapon currently held, and the
distance to the target.  Gated on the cvar so the off-state telemetry is
byte-identical.  Lets an A/B confirm the lever actually shifts weapon choice by
range (e.g. railgun -> super shotgun as a foe closes).
=================
*/
void Bot_LogWpnSel (bot_t *b, const char *chosen, const char *held, float dist)
{
	if (!log_fp || !bot_wpnsellog || bot_wpnsellog->value == 0 || !b)
		return;
	fprintf (log_fp,
		"{\"type\":\"wpnsel\",\"t\":%.2f,\"bot\":%d,\"chosen\":\"%s\","
		"\"held\":\"%s\",\"dist\":%.0f}\n",
		level.time, b->id, chosen, held, dist);
}

void Bot_LogAimShot (bot_t *b, const char *weapon, float range, float latspeed,
	float err_yaw, float err_pitch)
{
	if (!log_fp || !bot_aimlog || bot_aimlog->value == 0 || !b)
		return;
	fprintf (log_fp,
		"{\"type\":\"aimshot\",\"t\":%.2f,\"bot\":%d,\"weapon\":\"%s\","
		"\"range\":%.0f,\"lat\":%.0f,\"eyaw\":%.2f,\"epitch\":%.2f}\n",
		level.time, b->id, weapon, range, latspeed, err_yaw, err_pitch);
}

/*
=================
Bot_ItemName

Display/telemetry label for an item entity.  The four item_health_* entities all
share ONE "Health" gitem (SP_item_health_mega spawns FindItem("Health")), so
every health pickup logged as "Health" -- Megahealth was indistinguishable from
ordinary health, hiding mega collection in analysis.  The gitem carries no
distinguishing name; the EDICT classname does.  Disambiguate the health family by
edict classname; everything else keeps its gitem pickup_name.  Used by every
item-naming log path (pickup / item_lost / goal_item / respawn / reach).
=================
*/
const char *Bot_ItemName (edict_t *ent)
{
	if (!ent || !ent->item)
		return "";
	if (ent->classname)
	{
		if (!strcmp (ent->classname, "item_health_mega"))  return "Mega Health";
		if (!strcmp (ent->classname, "item_health_large")) return "Large Health";
		if (!strcmp (ent->classname, "item_health_small")) return "Small Health";
	}
	return ent->item->pickup_name ? ent->item->pickup_name
		: (ent->classname ? ent->classname : "");
}

/*
=================
Bot_LogItemEvent

Item respawn scheduling ("respawn_scheduled", with the requested delay) and
firing ("item_respawned").  tools/verify_timing.py pairs them per entity: the
elapsed time must equal the delay at every sv_fps -- this validates the whole
seconds-based nextthink scheduler under variable FPS.
=================
*/
void Bot_LogRespawn (const char *event, edict_t *item_ent, float delay)
{
	if (!log_fp || !item_ent->item)
		return;

	fprintf (log_fp,
		"{\"type\":\"event\",\"event\":\"%s\",\"t\":%.2f,\"ent\":%d,\"item\":\"%s\",\"delay\":%.1f}\n",
		event, level.time, (int)(item_ent - g_edicts),
		Bot_ItemName (item_ent), delay);
}

/*
=================
Write_InputRecord

Shared formatter for one frame of a REAL player's usercmd_t -- the exact
inputs a .dm2 demo doesn't carry (forward/side/up move, jump = up>0, attack, and
the per-frame view yaw/pitch sweep), plus the state the player is acting from
(origin/velocity/ground).  Used by both Bot_LogInput (global telemetry) and
Bot_CaptureInput (named capture file) so the two paths never diverge in format.
=================
*/
static void Write_InputRecord (FILE *fp, edict_t *ent, usercmd_t *ucmd)
{
	int		slot;
	float	vx, vy, vz, spd;

	slot = (int)(ent - g_edicts) - 1;
	vx = ent->velocity[0];
	vy = ent->velocity[1];
	vz = ent->velocity[2];
	spd = (float)sqrt (vx * vx + vy * vy);

	fprintf (fp,
		"{\"type\":\"input\",\"t\":%.2f,\"slot\":%d,\"msec\":%d,"
		"\"fwd\":%d,\"side\":%d,\"up\":%d,\"atk\":%d,"
		"\"yaw\":%.2f,\"pitch\":%.2f,"
		"\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,"
		"\"vx\":%.1f,\"vy\":%.1f,\"vz\":%.1f,\"spd\":%.1f,"
		"\"onground\":%s,\"water\":%d}\n",
		level.time, slot, (int)ucmd->msec,
		(int)ucmd->forwardmove, (int)ucmd->sidemove, (int)ucmd->upmove,
		(ucmd->buttons & BUTTON_ATTACK) ? 1 : 0,
		SHORT2ANGLE (ucmd->angles[YAW]), SHORT2ANGLE (ucmd->angles[PITCH]),
		ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
		vx, vy, vz, spd,
		ent->groundentity ? "true" : "false",
		(int)ent->waterlevel);
}

/*
=================
Bot_LogInput

bot_inputlog: one record per frame of a REAL player's usercmd_t, via
Write_InputRecord.  Feeds tools/input_view.py; recorded via
ozbot/record_inputs.bat.  Caller gates on bot_inputlog and !Bot_IsClient so
bots are excluded.
=================
*/
void Bot_LogInput (edict_t *ent, usercmd_t *ucmd)
{
	if (!log_fp || !ent || !ent->client || !ucmd)
		return;

	Write_InputRecord (log_fp, ent, ucmd);
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
analyzer can tell *why* item runs fail.  'navq' is the oracle's verdict on
the goal at giveup time (plans/nav-oracle.md Phase C): "ok" = a route still
existed and execution failed; "no_path" = the route evaporated (penalization
pruned it mid-attempt).
=================
*/
void Bot_LogGiveup (bot_t *b, float gdist, float gvdist, int atnode, int fighting,
	const char *navq)
{
	edict_t	*ent;

	if (!log_fp || !b || !b->ent)
		return;

	ent = b->ent;
	fprintf (log_fp,
		"{\"type\":\"event\",\"event\":\"giveup\",\"t\":%.2f,\"bot\":%d,"
		"\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,"
		"\"gdist\":%.0f,\"gvdist\":%.0f,\"atnode\":%d,\"fighting\":%d,"
		"\"pidx\":%d,\"plen\":%d,\"gbest\":%.0f,\"navq\":\"%s\"}\n",
		level.time, b->id,
		ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
		gdist, gvdist, atnode, fighting,
		b->path_idx, b->path_len, b->goal_best,
		navq ? navq : "");
}

/*
=================
Bot_LogReach

One record per item from the map-load reachability sweep (bot_reachlog;
plans/nav-oracle.md Phase C): can the graph route this item from a spawn,
and which capability unlocks it if not plainly walkable.
=================
*/
void Bot_LogReach (const char *when, const char *item, const char *classname,
	vec3_t org, const char *code, const char *gate, float cost)
{
	if (!log_fp)
		return;

	fprintf (log_fp,
		"{\"type\":\"reach\",\"when\":\"%s\",\"item\":\"%s\",\"classname\":\"%s\","
		"\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,"
		"\"code\":\"%s\",\"gate\":\"%s\",\"cost\":%.0f}\n",
		when, item ? item : "", classname ? classname : "",
		org[0], org[1], org[2],
		code, gate ? gate : "", cost);
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
			Bot_ItemName (b->goal_item),
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
Bot_LogSJ

bot_sjlog: strafe-jump controller events -- "engage", per-takeoff "hop",
"done", "abort_*" -- with the engage's hop count and peak 2D speed.
=================
*/
void Bot_LogSJ (bot_t *b, const char *phase, int hops, float peak)
{
	edict_t	*ent;
	float	spd;

	if (!log_fp || !bot_sjlog || bot_sjlog->value == 0)
		return;
	if (!b || !b->ent)
		return;

	ent = b->ent;
	spd = (float)sqrt (ent->velocity[0] * ent->velocity[0]
					 + ent->velocity[1] * ent->velocity[1]);

	fprintf (log_fp,
		"{\"type\":\"sj\",\"phase\":\"%s\",\"t\":%.2f,\"bot\":%d,"
		"\"hops\":%d,\"peak\":%.1f,\"spd\":%.1f,"
		"\"x\":%.1f,\"y\":%.1f,\"z\":%.1f}\n",
		phase, level.time, b->id,
		hops, peak, spd,
		ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/*
=================
Bot_LogSJDiag

bot_sjlog >= 2: periodic dump of the strafe-jump qualification funnel, so the
runway thresholds can be tuned against what actually stops candidates.
=================
*/
void Bot_LogSJDiag (void)
{
	static float next;

	if (!log_fp || !bot_sjlog || bot_sjlog->value < 2)
		return;
	if (level.time < next)
		return;
	next = level.time + 10.0f;

	fprintf (log_fp,
		"{\"type\":\"sjdiag\",\"t\":%.2f,\"qualify\":%d,\"link\":%d,"
		"\"dz\":%d,\"turn\":%d,\"trace\":%d,\"short\":%d,"
		"\"presim\":%d,\"engage\":%d}\n",
		level.time,
		sj_diag[SJ_DIAG_QUALIFY], sj_diag[SJ_DIAG_LINK],
		sj_diag[SJ_DIAG_DZ], sj_diag[SJ_DIAG_TURN],
		sj_diag[SJ_DIAG_TRACE], sj_diag[SJ_DIAG_SHORT],
		sj_diag[SJ_DIAG_PRESIM], sj_diag[SJ_DIAG_ENGAGE]);
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

/*
=================
Bot_CaptureStart

"capture start <name>": opens a fresh, user-named JSONL input log at
<gamedir>/logs/<name>.jsonl, independent of the per-level telemetry log_fp.
Ends any capture already in progress first.  name is sanitized to
[A-Za-z0-9_-] (anything else becomes '_'); an empty/NULL name falls back to
"capture_<HHMMSS>".  Overwrites on collision.  Writes the same "run" header
as Bot_LogBeginLevel so tools/input_view.py and tools/make_playbook.py pick
up the tick rate without a --tickrate override.
=================
*/
void Bot_CaptureStart (edict_t *ent, const char *name)
{
	char		dir[MAX_OSPATH];
	char		path[MAX_OSPATH];
	char		stamp[32];
	time_t		now;
	struct tm	*lt;
	int			i, len;

	if (capture_fp)
		Bot_CaptureStop (ent);

	capture_name[0] = '\0';
	if (name && name[0])
	{
		len = (int)strlen (name);
		if (len > (int)sizeof(capture_name) - 1)
			len = (int)sizeof(capture_name) - 1;
		for (i = 0; i < len; i++)
		{
			char c = name[i];
			if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
				(c >= '0' && c <= '9') || c == '_' || c == '-')
				capture_name[i] = c;
			else
				capture_name[i] = '_';
		}
		capture_name[len] = '\0';
	}

	if (!capture_name[0])
	{
		now = time (NULL);
		lt = localtime (&now);
		if (lt)
			strftime (stamp, sizeof(stamp), "%H%M%S", lt);
		else
			Com_sprintf (stamp, sizeof(stamp), "0");
		Com_sprintf (capture_name, sizeof(capture_name), "capture_%s", stamp);
	}

	Com_sprintf (dir, sizeof(dir), "%s/logs", Bot_GameDir());
	OZ_MKDIR (dir);	// ignore "already exists"

	Com_sprintf (path, sizeof(path), "%s/%s.jsonl", dir, capture_name);

	capture_fp = fopen (path, "w");
	if (!capture_fp)
	{
		gi.cprintf (ent, PRINT_HIGH, "capture: could not open %s\n", path);
		return;
	}

	fprintf (capture_fp, "{\"type\":\"run\",\"map\":\"%s\",\"tick_rate\":%d,\"maxclients\":%d}\n",
		(level.mapname[0]) ? level.mapname : "unknown",
		(int)(1.0f / FRAMETIME + 0.5f), (int)game.maxclients);

	capture_frames = 0;
	gi.cprintf (ent, PRINT_HIGH, "capture: recording to logs/%s.jsonl\n", capture_name);
}

/*
=================
Bot_CaptureStop

"capture stop": closes the current capture file (if any) and reports how
many frames were written.
=================
*/
void Bot_CaptureStop (edict_t *ent)
{
	if (!capture_fp)
	{
		gi.cprintf (ent, PRINT_HIGH, "capture: not capturing\n");
		return;
	}

	fclose (capture_fp);
	capture_fp = NULL;

	gi.cprintf (ent, PRINT_HIGH, "capture: %d frames -> logs/%s.jsonl\n",
		capture_frames, capture_name);
}

/*
=================
Bot_CaptureInput

Per-frame write to the active named capture file, via the same
Write_InputRecord formatter as the global telemetry log.  Flushed every
frame -- this is a single-player, low-rate capture (40Hz), so the cost is
cheap and it makes a hard exit crash-safe (no lost take).
=================
*/
void Bot_CaptureInput (edict_t *ent, usercmd_t *ucmd)
{
	if (!capture_fp || !ent || !ent->client || !ucmd)
		return;

	Write_InputRecord (capture_fp, ent, ucmd);
	capture_frames++;
	fflush (capture_fp);
}

/*
=================
Bot_CaptureActive
=================
*/
qboolean Bot_CaptureActive (void)
{
	return capture_fp != NULL;
}

/*
=================
Bot_CaptureName
=================
*/
const char *Bot_CaptureName (void)
{
	return capture_name;
}

/*
=================
Bot_CaptureShutdown

Closes the capture file if one is open (level end / ShutdownGame), without
the "N frames" report Bot_CaptureStop gives an interactive caller.
=================
*/
void Bot_CaptureShutdown (void)
{
	if (capture_fp)
	{
		fclose (capture_fp);
		capture_fp = NULL;
	}
}
