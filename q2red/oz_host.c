/*
ozbot - q2red host adapter

oz_host.c -- runs the unchanged ozbot brain (src/bot_*.c) inside the q2red
dedicated server, which drives it through q2red's bot brain contract
(q2red_bot_brain.h, a pinned copy of q2red's src/include/q2red_bot_brain.h).

On q2repro the bot lives inside a 3.19 game DLL and reads that DLL's edicts
directly.  On q2red the game module is a Quake II Rerelease mod (id's,
MuffMode, MMREX) that this code never sees: the server owns the bots as real
clients and hands the brain a read-only view of the world.  So this file
rebuilds, once per server frame, the legacy world the brain was written
against --

  - g_edicts[] / game.clients[]: a mirror of every entity, filled from the
    server's entity snapshots (entity_state, the shared edict fields, and the
    sv_entity_t block every rerelease module publishes for engine bots);
  - itemlist[]: the 3.19 items by pickup name, each mapped to the running
    mod's own item id (Bot_GetItemID), so inventories translate both ways;
  - target / movedir / speed / dmg, which the rerelease game does not
    publish, read from the map's entity string;
  - gi: the 3.19 import table, over the server's traces and the game
    module's own Pmove;

-- and turns the brain's outputs back into the rerelease's: each bot's
usercmd (3.19 shorts -> rerelease floats, upmove -> jump/crouch buttons) and
its weapon choice (client->newweapon -> the game's Bot_SetWeapon).

The 3.19 game code hooks the brain used (player_pain, PlayerNoise, ...) do not
exist here; the same signals are recovered from what the server can observe
(health drops, the published attack button, footstep events, pickup sounds).

Build: ./build_q2red.sh -> dist/ozbot_x64.so, loaded with `sv_bot_brain ozbot`.
Nothing here is compiled into the q2repro DLL.
*/

#include "../src/g_local.h"
#include "../src/bot.h"
#include "q2red_bot_brain.h"

#include <stdarg.h>
#include <stdlib.h>

/*
==============================================================================

STATE

==============================================================================
*/

static const q2red_bot_host_t	*host;

static void OZ_strncpyz (char *dst, const char *src, int size)
{
	if (size < 1)
		return;
	strncpy (dst, src ? src : "", (size_t)size - 1);
	dst[size - 1] = 0;
}

// what the brain sees as the game
game_locals_t	game;
level_locals_t	level;
game_import_t	gi;
game_export_t	globals;
edict_t			*g_edicts;
cvar_t			*deathmatch;
cvar_t			*dedicated;
cvar_t			*maxclients;
cvar_t			*sv_gravity;
edict_t			*pm_passent;

static gclient_t	*oz_clients;
static int			oz_num_edicts;		// mirrored this frame (<= MAX_EDICTS)

// per-entity data the snapshots do not carry
typedef struct
{
	int		rr_item;			// the mod's item id, 0 = not an item
	float	prev_z;				// movers: last frame's height (plat direction)
	int		prev_health;		// clients: last frame's health (pain/death)
	qboolean	was_dead;
	qboolean	have_spawn;		// entity-string data matched this map
	int		team;				// sv_entity_t team: 0 none, else the team
} oz_extra_t;

static oz_extra_t	oz_extra[MAX_EDICTS];

// per-bot outputs, by entity number
static usercmd_t	oz_cmd[MAX_EDICTS];
static qboolean		oz_cmd_ready[MAX_EDICTS];

static qboolean	oz_level_active;
static qboolean	oz_name_prefixed;		// the mod prefixes bot names itself
static cvar_t	*bot_telemetry_cv;		// see Bot_LogBeginLevel (#ifdef OZBOT_Q2RED)
static cvar_t	*bot_minplayers_cv;		// fill mode, see OZ_FillPopulation
static cvar_t	*bot_minclients_cv;		// the rerelease's fill cvar (-1 off, 0 no bots)
static cvar_t	*bot_numbots_cv;		// the rerelease's fixed count (<= 0 off)

/*
==============================================================================

CVARS

The host owns mirrors of its cvars and refreshes them on the way into the
brain.  The brain reads 3.19 cvar_t fields, so each host mirror is paired with
a cvar_t that is refreshed from it at the same points.

==============================================================================
*/

#define OZ_MAX_CVARS	320

typedef struct
{
	q2red_bot_cvar_t	*h;
	cvar_t				cv;
} oz_cvar_t;

static oz_cvar_t	oz_cvars[OZ_MAX_CVARS];
static int			oz_num_cvars;

static void OZ_SyncCvar (oz_cvar_t *c)
{
	c->cv.string = (char *)c->h->string;
	if (c->cv.value != c->h->value)
		c->cv.modified = true;
	c->cv.value = c->h->value;
}

static void OZ_SyncCvars (void)
{
	int	i;

	for (i = 0; i < oz_num_cvars; i++)
		OZ_SyncCvar (&oz_cvars[i]);
}

static cvar_t *OZ_FindCvar (const char *name)
{
	int	i;

	for (i = 0; i < oz_num_cvars; i++)
		if (!Q_stricmp ((char *)oz_cvars[i].h->name, (char *)name))
			return &oz_cvars[i].cv;
	return NULL;
}

static cvar_t *GI_cvar (char *name, char *value, int flags)
{
	cvar_t				*cv;
	q2red_bot_cvar_t	*h;
	oz_cvar_t			*c;

	if ((cv = OZ_FindCvar (name)) != NULL)
		return cv;
	h = host->cvar (name, value, flags);
	if (!h || oz_num_cvars >= OZ_MAX_CVARS)
	{
		static cvar_t dummy;
		dummy.name = name;
		dummy.string = value;
		dummy.value = (float)atof (value);
		return &dummy;
	}
	c = &oz_cvars[oz_num_cvars++];
	c->h = h;
	c->cv.name = (char *)h->name;
	c->cv.flags = flags;
	OZ_SyncCvar (c);
	c->cv.modified = false;
	return &c->cv;
}

static void OZ_SyncCvarNamed (const char *name)
{
	int	i;

	for (i = 0; i < oz_num_cvars; i++)
		if (!Q_stricmp ((char *)oz_cvars[i].h->name, (char *)name))
			OZ_SyncCvar (&oz_cvars[i]);
}

static cvar_t *GI_cvar_set (char *name, char *value)
{
	host->cvar_set (name, value);
	OZ_SyncCvarNamed (name);
	return OZ_FindCvar (name);
}

static cvar_t *GI_cvar_forceset (char *name, char *value)
{
	host->cvar_forceset (name, value);
	OZ_SyncCvarNamed (name);
	return OZ_FindCvar (name);
}

/*
==============================================================================

PRINTS, COMMANDS

==============================================================================
*/

static void GI_dprintf (char *fmt, ...)
{
	char	text[2048];
	va_list	ap;

	va_start (ap, fmt);
	vsnprintf (text, sizeof(text), fmt, ap);
	va_end (ap);
	host->dprint (text);
}

static void GI_bprintf (int level_, char *fmt, ...)
{
	char	text[2048];
	va_list	ap;

	va_start (ap, fmt);
	vsnprintf (text, sizeof(text), fmt, ap);
	va_end (ap);
	host->bprint (level_, text);
}

static void GI_cprintf (edict_t *ent, int level_, char *fmt, ...)
{
	char	text[2048];
	va_list	ap;

	va_start (ap, fmt);
	vsnprintf (text, sizeof(text), fmt, ap);
	va_end (ap);
	if (ent)
		host->cprint ((int)(ent - g_edicts), level_, text);
	else
		host->print (text);
}

static void GI_centerprintf (edict_t *ent, char *fmt, ...)
{
	(void)ent; (void)fmt;
}

void Com_Printf (char *fmt, ...)
{
	char	text[2048];
	va_list	ap;

	va_start (ap, fmt);
	vsnprintf (text, sizeof(text), fmt, ap);
	va_end (ap);
	host->print (text);
}

static void GI_error (char *fmt, ...)
{
	char	text[2048];
	va_list	ap;

	va_start (ap, fmt);
	vsnprintf (text, sizeof(text), fmt, ap);
	va_end (ap);
	host->print ("ozbot: error: ");
	host->print (text);
	host->print ("\n");
}

// The brain's console commands are registered as top-level server commands
// (q2red has no game-side `sv` for them), so argv is shifted by one while
// one runs: Bot_ServerCommand reads its command name from argv(1) as `sv
// bot_add` would give it.
static int	oz_argv_shift;

static int GI_argc (void)
{
	return host->argc () + oz_argv_shift;
}

static char *GI_argv (int n)
{
	if (oz_argv_shift)
	{
		if (n == 0)
			return "sv";
		return (char *)host->argv (n - oz_argv_shift);
	}
	return (char *)host->argv (n);
}

static char *GI_args (void)
{
	return (char *)host->args ();
}

static void GI_AddCommandString (char *text)
{
	host->add_command_string (text);
}

// network messages: the brain only uses them for debug trails; it never has
// a client of its own to unicast to
static void GI_noop_int (int c) { (void)c; }
static void GI_noop_float (float f) { (void)f; }
static void GI_noop_str (char *s) { (void)s; }
static void GI_noop_vec (vec3_t v) { (void)v; }
static void GI_multicast (vec3_t origin, multicast_t to) { (void)origin; (void)to; }
static void GI_unicast (edict_t *ent, qboolean reliable) { (void)ent; (void)reliable; }
static void GI_configstring (int num, char *s) { (void)num; (void)s; }
static int GI_index (char *name) { (void)name; return 0; }
static void GI_setmodel (edict_t *ent, char *name) { (void)ent; (void)name; }
static void GI_linkentity (edict_t *ent) { (void)ent; }
static void GI_sound (edict_t *ent, int channel, int soundindex, float volume, float attenuation, float timeofs)
{
	(void)ent; (void)channel; (void)soundindex; (void)volume; (void)attenuation; (void)timeofs;
}
static void GI_positioned_sound (vec3_t origin, edict_t *ent, int channel, int soundindex, float volume, float attenuation, float timeofs)
{
	(void)origin; (void)ent; (void)channel; (void)soundindex; (void)volume; (void)attenuation; (void)timeofs;
}
static void GI_SetAreaPortalState (int portalnum, qboolean open) { (void)portalnum; (void)open; }
static qboolean GI_AreasConnected (int a, int b) { (void)a; (void)b; return true; }
static int GI_BoxEdicts (vec3_t mins, vec3_t maxs, edict_t **list, int maxcount, int areatype)
{
	(void)mins; (void)maxs; (void)list; (void)maxcount; (void)areatype;
	return 0;
}
static void *GI_TagMalloc (int size, int tag) { (void)tag; return calloc (1, (size_t)size); }
static void GI_TagFree (void *block) { free (block); }
static void GI_FreeTags (int tag) { (void)tag; }
static void GI_DebugGraph (float value, int color) { (void)value; (void)color; }

/*
==============================================================================

WORLD QUERIES

==============================================================================
*/

// rerelease contents bits with no 3.19 meaning; players are CONTENTS_PLAYER
// there, not CONTENTS_MONSTER, so a 3.19 mask that hits monsters must hit
// players too, and a player hit reads back as a monster hit
#define RR_CONTENTS_PLAYER		(1u << 30)
#define RR_CONTENTS_PROJECTILE	(1u << 31)

static int OZ_MaskToHost (int mask)
{
	unsigned m = (unsigned)mask;

	if (m & CONTENTS_MONSTER)
		m |= RR_CONTENTS_PLAYER;
	return (int)m;
}

static int OZ_ContentsFromHost (int contents)
{
	unsigned c = (unsigned)contents;

	if (c & RR_CONTENTS_PLAYER)
		c |= CONTENTS_MONSTER;
	return (int)(c & ~(RR_CONTENTS_PLAYER | RR_CONTENTS_PROJECTILE));
}

static edict_t *OZ_Edict (int num)
{
	if (num < 0 || num >= MAX_EDICTS)
		return NULL;
	return &g_edicts[num];
}

#define OZ_SURFACE_RING	64
static csurface_t	oz_surfaces[OZ_SURFACE_RING];
static int			oz_surface_next;

static trace_t GI_trace (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, edict_t *passent, int contentmask)
{
	q2red_bot_trace_t	ht;
	trace_t				tr;
	csurface_t			*surf;

	host->trace (start, mins, maxs, end, passent ? (int)(passent - g_edicts) : -1,
		OZ_MaskToHost (contentmask), &ht);

	memset (&tr, 0, sizeof(tr));
	tr.allsolid = ht.allsolid ? true : false;
	tr.startsolid = ht.startsolid ? true : false;
	tr.fraction = ht.fraction;
	VectorCopy (ht.endpos, tr.endpos);
	VectorCopy (ht.plane_normal, tr.plane.normal);
	tr.plane.dist = ht.plane_dist;
	tr.contents = OZ_ContentsFromHost (ht.contents);

	surf = &oz_surfaces[oz_surface_next++ % OZ_SURFACE_RING];
	memset (surf, 0, sizeof(*surf));
	surf->flags = ht.surface_flags;
	tr.surface = surf;

	// an entity beyond the mirror (rare: >1024 edicts) reads as the world
	tr.ent = (ht.ent >= 0) ? OZ_Edict (ht.ent) : NULL;
	if (ht.ent >= MAX_EDICTS)
		tr.ent = g_edicts;
	if (!tr.ent && tr.fraction < 1.0f)
		tr.ent = g_edicts;
	return tr;
}

static int GI_pointcontents (vec3_t point)
{
	return OZ_ContentsFromHost (host->pointcontents (point));
}

static qboolean GI_inPVS (vec3_t p1, vec3_t p2)
{
	return host->in_pvs (p1, p2) ? true : false;
}

static qboolean GI_inPHS (vec3_t p1, vec3_t p2)
{
	return host->in_phs (p1, p2) ? true : false;
}

// 3.19 and rerelease pm_type orders differ
static int OZ_PmTypeToHost (int t)
{
	switch (t)
	{
	case PM_SPECTATOR:	return 3;
	case PM_DEAD:		return 4;
	case PM_GIB:		return 5;
	case PM_FREEZE:		return 6;
	default:			return 0;
	}
}

static int OZ_PmTypeFromHost (int t)
{
	switch (t)
	{
	case 2:	case 3:	return PM_SPECTATOR;	// noclip, spectator
	case 4:	return PM_DEAD;
	case 5:	return PM_GIB;
	case 6:	return PM_FREEZE;
	default: return PM_NORMAL;				// normal, grapple
	}
}

static void OZ_CmdToHost (const usercmd_t *in, q2red_bot_usercmd_t *out)
{
	int	i;

	memset (out, 0, sizeof(*out));
	out->msec = in->msec;
	out->buttons = in->buttons & (BUTTON_ATTACK | BUTTON_USE | BUTTON_ANY);
	if (in->upmove > 0)
		out->buttons |= Q2RED_BOT_BUTTON_JUMP;
	else if (in->upmove < 0)
		out->buttons |= Q2RED_BOT_BUTTON_CROUCH;
	for (i = 0; i < 3; i++)
		out->angles[i] = SHORT2ANGLE(in->angles[i]);
	out->forwardmove = in->forwardmove;
	out->sidemove = in->sidemove;
}

/*
The brain's movement sims hand the 3.19 Pmove its fixed-point state; the
game's own Pmove runs on floats.  Converting each call keeps the brain's
one-eighth-unit quantisation, which is what it was tuned against.  (3.19
shorts cap positions at +-4096 units; on a larger rerelease map the sims'
predictions are off, the real movement is not.)
*/
static void GI_Pmove (pmove_t *pm)
{
	q2red_bot_pmove_t	hp;
	int					i;

	memset (&hp, 0, sizeof(hp));
	hp.pm_type = OZ_PmTypeToHost (pm->s.pm_type);
	for (i = 0; i < 3; i++)
	{
		hp.origin[i] = pm->s.origin[i] * 0.125f;
		hp.velocity[i] = pm->s.velocity[i] * 0.125f;
		hp.delta_angles[i] = SHORT2ANGLE(pm->s.delta_angles[i]);
	}
	hp.pm_flags = pm->s.pm_flags;
	hp.pm_time = pm->s.pm_time * 8;
	hp.gravity = pm->s.gravity;
	hp.viewheight = 22;
	OZ_CmdToHost (&pm->cmd, &hp.cmd);
	hp.passent = pm_passent ? (int)(pm_passent - g_edicts) : -1;
	hp.snapinitial = pm->snapinitial;

	host->pmove (&hp);

	pm->s.pm_type = OZ_PmTypeFromHost (hp.pm_type);
	for (i = 0; i < 3; i++)
	{
		pm->s.origin[i] = (short)(hp.origin[i] * 8.0f);
		pm->s.velocity[i] = (short)(hp.velocity[i] * 8.0f);
		pm->s.delta_angles[i] = (short)ANGLE2SHORT(hp.delta_angles[i]);
	}
	pm->s.pm_flags = (byte)(hp.pm_flags & 0xff);
	pm->s.pm_time = (byte)((hp.pm_time + 7) / 8 > 255 ? 255 : (hp.pm_time + 7) / 8);
	pm->s.gravity = (short)hp.gravity;
	VectorCopy (hp.viewangles, pm->viewangles);
	pm->viewheight = (float)hp.viewheight;
	VectorCopy (hp.mins, pm->mins);
	VectorCopy (hp.maxs, pm->maxs);
	pm->groundentity = (hp.groundentity >= 0) ? OZ_Edict (hp.groundentity) : NULL;
	if (hp.groundentity >= MAX_EDICTS)
		pm->groundentity = g_edicts;
	pm->watertype = OZ_ContentsFromHost (hp.watertype);
	pm->waterlevel = hp.waterlevel;
	pm->numtouch = 0;
}

/*
==============================================================================

GAME FUNCTIONS THE BRAIN CALLS

Copies of the 3.19 originals (g_utils.c, g_ai.c, g_items.c, p_client.c),
over the mirror.

==============================================================================
*/

void DoRespawn (edict_t *ent)
{
	(void)ent;		// identity only: marks a mirrored item waiting to respawn
}

trace_t PM_trace (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end)
{
	if (pm_passent && pm_passent->health > 0)
		return gi.trace (start, mins, maxs, end, pm_passent, MASK_PLAYERSOLID);
	return gi.trace (start, mins, maxs, end, pm_passent, MASK_DEADSOLID);
}

edict_t *G_Find (edict_t *from, int fieldofs, char *match)
{
	char	*s;

	if (!from)
		from = g_edicts;
	else
		from++;

	for ( ; from < &g_edicts[globals.num_edicts] ; from++)
	{
		if (!from->inuse)
			continue;
		s = *(char **) ((byte *)from + fieldofs);
		if (!s)
			continue;
		if (!Q_stricmp (s, match))
			return from;
	}
	return NULL;
}

#define MAXCHOICES	8

edict_t *G_PickTarget (char *targetname)
{
	edict_t	*ent = NULL;
	int		num_choices = 0;
	edict_t	*choice[MAXCHOICES];

	if (!targetname)
		return NULL;
	while (1)
	{
		ent = G_Find (ent, FOFS(targetname), targetname);
		if (!ent)
			break;
		choice[num_choices++] = ent;
		if (num_choices == MAXCHOICES)
			break;
	}
	if (!num_choices)
		return NULL;
	return choice[rand() % num_choices];
}

float vectoyaw (vec3_t vec)
{
	float	yaw;

	if (vec[PITCH] == 0)
	{
		yaw = 0;
		if (vec[YAW] > 0)
			yaw = 90;
		else if (vec[YAW] < 0)
			yaw = -90;
	}
	else
	{
		yaw = (int) (atan2(vec[YAW], vec[PITCH]) * 180 / M_PI);
		if (yaw < 0)
			yaw += 360;
	}
	return yaw;
}

void vectoangles (vec3_t value1, vec3_t angles)
{
	float	forward;
	float	yaw, pitch;

	if (value1[1] == 0 && value1[0] == 0)
	{
		yaw = 0;
		if (value1[2] > 0)
			pitch = 90;
		else
			pitch = 270;
	}
	else
	{
		if (value1[0])
			yaw = (int) (atan2(value1[1], value1[0]) * 180 / M_PI);
		else if (value1[1] > 0)
			yaw = 90;
		else
			yaw = -90;
		if (yaw < 0)
			yaw += 360;

		forward = sqrt (value1[0]*value1[0] + value1[1]*value1[1]);
		pitch = (int) (atan2(value1[2], forward) * 180 / M_PI);
		if (pitch < 0)
			pitch += 360;
	}
	angles[PITCH] = -pitch;
	angles[YAW] = yaw;
	angles[ROLL] = 0;
}

qboolean visible (edict_t *self, edict_t *other)
{
	vec3_t	spot1;
	vec3_t	spot2;
	trace_t	trace;

	VectorCopy (self->s.origin, spot1);
	spot1[2] += self->viewheight;
	VectorCopy (other->s.origin, spot2);
	spot2[2] += other->viewheight;
	trace = gi.trace (spot1, vec3_origin, vec3_origin, spot2, self, MASK_OPAQUE);
	return trace.fraction == 1.0 ? true : false;
}

/*
==============================================================================

ITEMS

The 3.19 item table, by pickup name, as far as the brain reads it.  Each
entry is mapped to the running mod's own item id at map start.  The four
health classnames share the one 3.19 "Health" item, as they do in 3.19.

==============================================================================
*/

#define ITW	IT_WEAPON
#define ITA	IT_AMMO
#define ITR	IT_ARMOR
#define ITP	IT_POWERUP
#define ITK	IT_KEY

gitem_t	itemlist[] =
{
	{ NULL },	// leave index 0 alone
#define OZ_ITEM(cls, name, qty, ammo, flags) \
	{ cls, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL, name, 0, qty, ammo, flags },
	OZ_ITEM ("item_armor_body",			"Body Armor",		0,	NULL,		ITR)
	OZ_ITEM ("item_armor_combat",		"Combat Armor",		0,	NULL,		ITR)
	OZ_ITEM ("item_armor_jacket",		"Jacket Armor",		0,	NULL,		ITR)
	OZ_ITEM ("item_armor_shard",		"Armor Shard",		0,	NULL,		ITR)
	OZ_ITEM ("item_power_screen",		"Power Screen",		60,	NULL,		ITR)
	OZ_ITEM ("item_power_shield",		"Power Shield",		60,	NULL,		ITR)
	OZ_ITEM ("weapon_blaster",			"Blaster",			0,	NULL,		ITW)
	OZ_ITEM ("weapon_shotgun",			"Shotgun",			1,	"Shells",	ITW)
	OZ_ITEM ("weapon_supershotgun",		"Super Shotgun",	2,	"Shells",	ITW)
	OZ_ITEM ("weapon_machinegun",		"Machinegun",		1,	"Bullets",	ITW)
	OZ_ITEM ("weapon_chaingun",			"Chaingun",			1,	"Bullets",	ITW)
	OZ_ITEM ("ammo_grenades",			"Grenades",			5,	"grenades",	ITA | ITW)
	OZ_ITEM ("weapon_grenadelauncher",	"Grenade Launcher",	1,	"Grenades",	ITW)
	OZ_ITEM ("weapon_rocketlauncher",	"Rocket Launcher",	1,	"Rockets",	ITW)
	OZ_ITEM ("weapon_hyperblaster",		"HyperBlaster",		1,	"Cells",	ITW)
	OZ_ITEM ("weapon_railgun",			"Railgun",			1,	"Slugs",	ITW)
	OZ_ITEM ("weapon_bfg",				"BFG10K",			50,	"Cells",	ITW)
	OZ_ITEM ("ammo_shells",				"Shells",			10,	NULL,		ITA)
	OZ_ITEM ("ammo_bullets",			"Bullets",			50,	NULL,		ITA)
	OZ_ITEM ("ammo_cells",				"Cells",			50,	NULL,		ITA)
	OZ_ITEM ("ammo_rockets",			"Rockets",			5,	NULL,		ITA)
	OZ_ITEM ("ammo_slugs",				"Slugs",			10,	NULL,		ITA)
	OZ_ITEM ("item_quad",				"Quad Damage",		60,	NULL,		ITP)
	OZ_ITEM ("item_invulnerability",	"Invulnerability",	300, NULL,		ITP)
	OZ_ITEM ("item_silencer",			"Silencer",			60,	NULL,		ITP)
	OZ_ITEM ("item_breather",			"Rebreather",		60,	NULL,		ITP)
	OZ_ITEM ("item_enviro",				"Environment Suit",	60,	NULL,		ITP)
	OZ_ITEM ("item_ancient_head",		"Ancient Head",		60,	NULL,		ITP)
	OZ_ITEM ("item_adrenaline",			"Adrenaline",		60,	NULL,		ITP)
	OZ_ITEM ("item_bandolier",			"Bandolier",		60,	NULL,		ITP)
	OZ_ITEM ("item_pack",				"Ammo Pack",		180, NULL,		ITP)
	OZ_ITEM ("key_data_cd",				"Data CD",			0,	NULL,		ITK)
	OZ_ITEM ("key_power_cube",			"Power Cube",		0,	NULL,		ITK)
	OZ_ITEM ("key_pyramid",				"Pyramid Key",		0,	NULL,		ITK)
	OZ_ITEM ("key_data_spinner",		"Data Spinner",		0,	NULL,		ITK)
	OZ_ITEM ("key_pass",				"Security Pass",	0,	NULL,		ITK)
	OZ_ITEM ("key_blue_key",			"Blue Key",			0,	NULL,		ITK)
	OZ_ITEM ("key_red_key",				"Red Key",			0,	NULL,		ITK)
	OZ_ITEM ("key_commander_head",		"Commander's Head",	0,	NULL,		ITK)
	OZ_ITEM ("key_airstrike_target",	"Airstrike Marker",	0,	NULL,		ITK)
	// CTF flags: no 3.19 item, but the objectives oz_ctf.c plays for.  id's
	// and MMREX's classnames, then MuffMode's (only one pair resolves to an
	// item id on a given mod; the other stays unmapped)
	OZ_ITEM ("item_flag_team1",			"Red Flag",			0,	NULL,		0)
	OZ_ITEM ("item_flag_team2",			"Blue Flag",		0,	NULL,		0)
	OZ_ITEM ("item_flag_team_red",		"Red Flag",			0,	NULL,		0)
	OZ_ITEM ("item_flag_team_blue",		"Blue Flag",		0,	NULL,		0)
	OZ_ITEM (NULL,						"Health",			0,	NULL,		0)
#undef OZ_ITEM
	{ NULL }
};

#define OZ_NUM_ITEMS	((int)(sizeof(itemlist) / sizeof(itemlist[0])) - 1)

static int	oz_item_rr[OZ_NUM_ITEMS];			// legacy index -> the mod's id
static int	oz_rr_item[Q2RED_BOT_MAX_ITEMS];	// the mod's id -> legacy index
static int	oz_health_item;

static const char *oz_health_classes[] = {
	"item_health", "item_health_small", "item_health_large", "item_health_mega", NULL
};

static int OZ_LegacyItemForClass (const char *classname)
{
	int	i;

	if (!classname || !classname[0])
		return 0;
	for (i = 0; oz_health_classes[i]; i++)
		if (!Q_stricmp ((char *)classname, (char *)oz_health_classes[i]))
			return oz_health_item;
	for (i = 1; i < OZ_NUM_ITEMS; i++)
		if (itemlist[i].classname && !Q_stricmp ((char *)classname, itemlist[i].classname))
			return i;
	return 0;
}

static void OZ_MapItems (void)
{
	int	i, id;

	memset (oz_item_rr, 0, sizeof(oz_item_rr));
	memset (oz_rr_item, 0, sizeof(oz_rr_item));
	oz_health_item = 0;
	for (i = 1; i < OZ_NUM_ITEMS; i++)
		if (!itemlist[i].classname && itemlist[i].pickup_name
			&& !strcmp (itemlist[i].pickup_name, "Health"))
			oz_health_item = i;
	game.num_items = OZ_NUM_ITEMS;

	for (i = 1; i < OZ_NUM_ITEMS; i++)
	{
		if (!itemlist[i].classname)
			continue;
		id = host->item_id (itemlist[i].classname);
		if (id <= 0 || id >= Q2RED_BOT_MAX_ITEMS)
			continue;
		oz_item_rr[i] = id;
		oz_rr_item[id] = i;
	}
	// the health variants all read back as the one Health item
	for (i = 0; oz_health_classes[i]; i++)
	{
		id = host->item_id (oz_health_classes[i]);
		if (id > 0 && id < Q2RED_BOT_MAX_ITEMS)
			oz_rr_item[id] = oz_health_item;
	}
}

gitem_t *FindItem (char *pickup_name)
{
	int	i;

	for (i = 1; i < OZ_NUM_ITEMS; i++)
		if (itemlist[i].pickup_name && !Q_stricmp (itemlist[i].pickup_name, pickup_name))
			return &itemlist[i];
	return NULL;
}

int ArmorIndex (edict_t *ent)
{
	int	i;

	if (!ent->client)
		return 0;
	for (i = 1; i < OZ_NUM_ITEMS; i++)
	{
		if (!(itemlist[i].flags & IT_ARMOR) || !itemlist[i].classname)
			continue;
		if (strcmp (itemlist[i].classname, "item_armor_jacket")
			&& strcmp (itemlist[i].classname, "item_armor_combat")
			&& strcmp (itemlist[i].classname, "item_armor_body"))
			continue;
		if (ent->client->pers.inventory[i] > 0)
			return i;
	}
	return 0;
}

/*
==============================================================================

ENTITY STRING

The rerelease publishes classname / targetname / spawnflags for every entity
but not the keys the brain's capability seeding reads (target, angle,
speed, dmg).  Those come from the map's entity lump: brush entities match by
their inline model ("*N"), point entities by classname and origin.

==============================================================================
*/

#define OZ_MAX_SPAWNS	2048

typedef struct
{
	char	classname[64];
	char	model[16];
	char	target[64];
	char	targetname[64];
	vec3_t	origin;
	qboolean	has_origin;
	float	angle;
	vec3_t	angles;
	qboolean	has_angles;
	float	speed;
	int		dmg;
	float	wait;
	qboolean	used;
} oz_spawn_t;

static oz_spawn_t	oz_spawns[OZ_MAX_SPAWNS];
static int			oz_num_spawns;

// string pool for mirrored char * fields (lives for the map)
#define OZ_POOL_SIZE	(256 * 1024)
static char	oz_pool[OZ_POOL_SIZE];
static int	oz_pool_used;

static char *OZ_Intern (const char *s)
{
	int		len;
	int		at;
	char	*p;

	if (!s || !s[0])
		return NULL;
	// short linear lookup: a map has a few hundred distinct strings
	for (at = 0; at < oz_pool_used; at += (int)strlen (oz_pool + at) + 1)
		if (!strcmp (oz_pool + at, s))
			return oz_pool + at;
	len = (int)strlen (s) + 1;
	if (oz_pool_used + len > OZ_POOL_SIZE)
		return NULL;
	p = oz_pool + oz_pool_used;
	memcpy (p, s, (size_t)len);
	oz_pool_used += len;
	return p;
}

static void OZ_ParseEntityString (void)
{
	const char	*data = host->entity_string ();
	char		*token;
	char		key[256];
	oz_spawn_t	*sp;

	oz_num_spawns = 0;
	if (!data)
		return;

	while (1)
	{
		token = COM_Parse ((char **)&data);
		if (!data || !token[0])
			break;
		if (token[0] != '{')
			break;
		sp = (oz_num_spawns < OZ_MAX_SPAWNS) ? &oz_spawns[oz_num_spawns] : NULL;
		if (sp)
			memset (sp, 0, sizeof(*sp));
		while (1)
		{
			token = COM_Parse ((char **)&data);
			if (!data || token[0] == '}')
				break;
			OZ_strncpyz (key, token, sizeof(key));
			token = COM_Parse ((char **)&data);
			if (!data)
				break;
			if (!sp)
				continue;
			if (!Q_stricmp (key, "classname"))
				OZ_strncpyz (sp->classname, token, sizeof(sp->classname));
			else if (!Q_stricmp (key, "model"))
				OZ_strncpyz (sp->model, token, sizeof(sp->model));
			else if (!Q_stricmp (key, "target"))
				OZ_strncpyz (sp->target, token, sizeof(sp->target));
			else if (!Q_stricmp (key, "targetname"))
				OZ_strncpyz (sp->targetname, token, sizeof(sp->targetname));
			else if (!Q_stricmp (key, "origin"))
				sp->has_origin = sscanf (token, "%f %f %f", &sp->origin[0], &sp->origin[1], &sp->origin[2]) == 3;
			else if (!Q_stricmp (key, "angle"))
				sp->angle = (float)atof (token);
			else if (!Q_stricmp (key, "angles"))
				sp->has_angles = sscanf (token, "%f %f %f", &sp->angles[0], &sp->angles[1], &sp->angles[2]) == 3;
			else if (!Q_stricmp (key, "speed"))
				sp->speed = (float)atof (token);
			else if (!Q_stricmp (key, "dmg"))
				sp->dmg = atoi (token);
			else if (!Q_stricmp (key, "wait"))
				sp->wait = (float)atof (token);
		}
		if (sp)
			oz_num_spawns++;
	}
}

// G_SetMovedir, from the spawn's angle(s)
static void OZ_SpawnMovedir (const oz_spawn_t *sp, vec3_t movedir)
{
	static vec3_t	VEC_UP = {0, -1, 0};
	static vec3_t	VEC_DOWN = {0, -2, 0};
	vec3_t			angles;

	VectorClear (angles);
	if (sp->has_angles)
		VectorCopy (sp->angles, angles);
	else
		angles[YAW] = sp->angle;

	if (VectorCompare (angles, VEC_UP))
		VectorSet (movedir, 0, 0, 1);
	else if (VectorCompare (angles, VEC_DOWN))
		VectorSet (movedir, 0, 0, -1);
	else
		AngleVectors (angles, movedir, NULL, NULL);
}

static oz_spawn_t *OZ_MatchSpawn (edict_t *e, int modelindex)
{
	const char	*model;
	oz_spawn_t	*best = NULL;
	float		best_d = 2.0f;
	int			i;

	if (!e->classname)
		return NULL;

	model = (modelindex > 0) ? host->configstring (62 + modelindex) : "";	// CS_MODELS
	if (model && model[0] == '*')
	{
		for (i = 0; i < oz_num_spawns; i++)
			if (!oz_spawns[i].used && !strcmp (oz_spawns[i].model, model)
				&& !Q_stricmp (oz_spawns[i].classname, e->classname))
				return &oz_spawns[i];
		return NULL;
	}

	for (i = 0; i < oz_num_spawns; i++)
	{
		vec3_t	d;
		float	dist;

		if (oz_spawns[i].used || !oz_spawns[i].has_origin
			|| Q_stricmp (oz_spawns[i].classname, e->classname))
			continue;
		VectorSubtract (oz_spawns[i].origin, e->s.origin, d);
		dist = VectorLength (d);
		if (dist < best_d)
		{
			best_d = dist;
			best = &oz_spawns[i];
		}
	}
	return best;
}

// spawn-function defaults the brain's readers assume (g_trigger.c, g_func.c)
static void OZ_ApplySpawn (edict_t *e, const oz_spawn_t *sp)
{
	e->target = OZ_Intern (sp->target);
	if (!e->targetname)
		e->targetname = OZ_Intern (sp->targetname);
	e->speed = sp->speed;
	e->dmg = sp->dmg;
	e->wait = sp->wait;
	e->angle = sp->angle;
	OZ_SpawnMovedir (sp, e->movedir);

	if (!Q_stricmp (e->classname, "trigger_push"))
	{
		if (!e->speed)
			e->speed = 1000;
	}
	else if (!Q_stricmp (e->classname, "trigger_hurt"))
	{
		if (!e->dmg)
			e->dmg = 5;
	}
	else if (!Q_stricmp (e->classname, "func_plat"))
	{
		if (!e->speed)
			e->speed = 20;
		e->speed *= 0.1f;
		e->moveinfo.speed = e->speed;
	}
	else if (!Q_stricmp (e->classname, "func_train"))
	{
		if (!e->speed)
			e->speed = 100;
		e->moveinfo.speed = e->speed;
	}
}

/*
==============================================================================

THE MIRROR

==============================================================================
*/

static void OZ_MirrorClient (edict_t *e, const q2red_bot_entity_t *s)
{
	gclient_t	*cl = e->client;
	int			i, li;

	cl->ps.pmove.pm_type = OZ_PmTypeFromHost (s->pm_type);
	for (i = 0; i < 3; i++)
	{
		cl->ps.pmove.origin[i] = (short)(s->pm_origin[i] * 8.0f);
		cl->ps.pmove.velocity[i] = (short)(s->pm_velocity[i] * 8.0f);
		cl->ps.pmove.delta_angles[i] = (short)ANGLE2SHORT(s->delta_angles[i]);
	}
	cl->ps.pmove.pm_flags = (byte)(s->pm_flags & 0xff);
	cl->ps.pmove.pm_time = (byte)(s->pm_time / 8 > 255 ? 255 : s->pm_time / 8);
	cl->ps.pmove.gravity = (short)s->gravity;
	VectorCopy (s->viewangles, cl->ps.viewangles);
	cl->ps.viewoffset[2] = (float)s->viewheight;

	memset (cl->pers.inventory, 0, sizeof(cl->pers.inventory));
	if (s->inventory)
	{
		for (li = 1; li < OZ_NUM_ITEMS; li++)
			if (oz_item_rr[li])
				cl->pers.inventory[li] = s->inventory[oz_item_rr[li]];
	}
	cl->pers.weapon = (s->weapon > 0 && s->weapon < Q2RED_BOT_MAX_ITEMS && oz_rr_item[s->weapon])
		? &itemlist[oz_rr_item[s->weapon]] : NULL;
	OZ_strncpyz (cl->pers.netname, (char *)s->netname, sizeof(cl->pers.netname));
	cl->pers.connected = s->inuse ? true : false;
	cl->pers.health = s->health;
	cl->pers.max_health = s->max_health;
	cl->resp.spectator = (s->sv_flags & (1ull << 29)) ? true : false;	// SVFL_IS_SPECTATOR
	cl->pers.spectator = cl->resp.spectator;
	cl->resp.score = s->stats ? s->stats[14] : 0;	// STAT_FRAGS
	cl->ping = 0;
}

static void OZ_MirrorEntity (int num, qboolean full)
{
	q2red_bot_entity_t	s;
	edict_t				*e = &g_edicts[num];
	oz_extra_t			*x = &oz_extra[num];
	char				*cls;

	if (!host->get_entity (num, &s) || !s.inuse)
	{
		e->inuse = false;
		return;
	}

	e->inuse = true;
	e->s.number = num;
	VectorCopy (s.origin, e->s.origin);
	VectorCopy (s.old_origin, e->s.old_origin);
	VectorCopy (s.angles, e->s.angles);
	e->s.modelindex = s.modelindex;
	e->s.effects = (unsigned)s.effects;
	e->s.renderfx = s.renderfx;
	e->s.sound = s.sound;
	e->s.event = s.event;
	e->svflags = (int)(s.svflags & 0xff);
	e->solid = (solid_t)s.solid;
	e->clipmask = OZ_ContentsFromHost ((int)s.clipmask);
	VectorCopy (s.mins, e->mins);
	VectorCopy (s.maxs, e->maxs);
	VectorCopy (s.absmin, e->absmin);
	VectorCopy (s.absmax, e->absmax);
	VectorSubtract (e->absmax, e->absmin, e->size);
	e->owner = (s.owner >= 0) ? OZ_Edict (s.owner) : NULL;
	e->enemy = (s.enemy >= 0) ? OZ_Edict (s.enemy) : NULL;
	e->groundentity = (s.ground_entity >= 0) ? OZ_Edict (s.ground_entity) : NULL;
	if (s.ground_entity >= MAX_EDICTS)
		e->groundentity = g_edicts;
	e->spawnflags = (int)s.spawnflags;
	x->team = s.team;

	cls = OZ_Intern (s.classname);
	if (cls != e->classname || full)
	{
		e->classname = cls;
		e->targetname = OZ_Intern (s.targetname);
		x->have_spawn = false;
	}
	if (!num)
		e->classname = "worldspawn";

	e->health = s.health;
	e->max_health = s.max_health;
	e->viewheight = s.viewheight;
	e->waterlevel = s.waterlevel;
	VectorCopy (s.velocity, e->velocity);

	// items: the 3.19 item, and the pending-respawn think the brain reads
	e->item = NULL;
	if (s.sv_flags & (1ull << 6))	// SVFL_IS_ITEM
	{
		int li = OZ_LegacyItemForClass (s.classname);
		if (!li && s.item_id > 0 && s.item_id < Q2RED_BOT_MAX_ITEMS)
			li = oz_rr_item[s.item_id];
		e->item = li ? &itemlist[li] : NULL;
		x->rr_item = s.item_id;
		if (s.respawntime > 0 && s.respawntime != 0x7fffffff)
		{
			e->think = DoRespawn;
			e->nextthink = level.time + s.respawntime * 0.001f;
		}
		else
		{
			e->think = NULL;
			e->nextthink = 0;
		}
	}

	// movers: plat state (3.19 STATE_TOP 0, BOTTOM 1, UP 2, DOWN 3)
	if (e->solid == SOLID_BSP && num)
	{
		if (s.sv_flags & (1ull << 21))			// SVFL_MOVESTATE_TOP
			e->moveinfo.state = 0;
		else if (s.sv_flags & (1ull << 22))		// SVFL_MOVESTATE_BOTTOM
			e->moveinfo.state = 1;
		else if (s.sv_flags & (1ull << 23))		// SVFL_MOVESTATE_MOVING
			e->moveinfo.state = (s.origin[2] >= x->prev_z) ? 2 : 3;
		x->prev_z = s.origin[2];
		VectorCopy (s.start_origin, e->moveinfo.start_origin);
		VectorCopy (s.end_origin, e->moveinfo.end_origin);
		VectorCopy (s.start_origin, e->pos1);
		VectorCopy (s.end_origin, e->pos2);
	}

	if (!x->have_spawn && e->classname)
	{
		oz_spawn_t *sp = OZ_MatchSpawn (e, s.modelindex);
		if (sp)
		{
			sp->used = true;
			OZ_ApplySpawn (e, sp);
		}
		x->have_spawn = true;
	}

	if (s.is_client && num >= 1 && num <= game.maxclients)
	{
		e->client = &oz_clients[num - 1];
		// a client the game has not published yet (just admitted, this
		// frame) is a fresh spawn, not a corpse
		if (!s.sv_init)
		{
			e->health = 100;
			e->max_health = 100;
		}
		e->deadflag = (s.sv_init && s.health <= 0) ? DEAD_DEAD : DEAD_NO;
		if (e->health < 0)
			e->health = 0;
		e->watertype = 0;
		if (s.waterlevel > 0)
		{
			vec3_t p;
			VectorCopy (s.origin, p);
			p[2] += s.mins[2] + 1;
			e->watertype = GI_pointcontents (p) & MASK_WATER;
		}
		OZ_MirrorClient (e, &s);
	}
	else
	{
		e->client = NULL;
		e->deadflag = (s.svflags & SVF_MONSTER && s.health <= 0) ? DEAD_DEAD : DEAD_NO;
	}
}

static void OZ_MirrorWorld (qboolean full)
{
	int	n, i;

	n = host->num_edicts ();
	if (n > MAX_EDICTS)
		n = MAX_EDICTS;
	for (i = 0; i < n; i++)
		OZ_MirrorEntity (i, full);
	for ( ; i < oz_num_edicts; i++)
		g_edicts[i].inuse = false;
	oz_num_edicts = n;
	globals.num_edicts = n;
}

/*
==============================================================================

CLIENTS

==============================================================================
*/

// Bot_Add's half (bot_main.c, #ifdef OZBOT_Q2RED): the server admits the bot
// and the game module picks its slot.  NULL if the server refused.
edict_t *OzHost_AddBot (char *userinfo)
{
	int	num;

	num = host->add_bot (userinfo);
	if (num < 1 || num > game.maxclients)
		return NULL;
	OZ_MirrorEntity (num, true);
	return &g_edicts[num];
}

/*
Teammates: both clients carry the same nonzero team.  The rerelease publishes
0 for no team (free-for-all), a team index in team modes, and puts every
player on one team in coop-style modes (Horde included).
*/
qboolean OzHost_IsTeammate (edict_t *a, edict_t *b)
{
	int	ta, tb;

	if (!a || !b || a == b || !a->client || !b->client)
		return false;
	ta = oz_extra[a - g_edicts].team;
	tb = oz_extra[b - g_edicts].team;
	return ta != 0 && ta == tb;
}

int OzHost_Team (edict_t *ent)
{
	return (ent && ent >= g_edicts && ent - g_edicts < MAX_EDICTS)
		? oz_extra[ent - g_edicts].team : 0;
}

qboolean OzHost_IsBot (edict_t *ent)
{
	return host->is_bot ((int)(ent - g_edicts)) ? true : false;
}

void ClientDisconnect (edict_t *ent)
{
	int num = (int)(ent - g_edicts);

	if (host->is_bot (num))
		host->kick_bot (num);
}

// The bot's Bot_Add path does not run these on q2red (the server already ran
// the game's own); they exist for the link.
qboolean ClientConnect (edict_t *ent, char *userinfo)
{
	(void)ent; (void)userinfo;
	return false;
}

void ClientBegin (edict_t *ent)
{
	(void)ent;
}

// The brain's command for this frame; the host collects it in think.
void ClientThink (edict_t *ent, usercmd_t *cmd)
{
	int num = (int)(ent - g_edicts);

	if (num < 1 || num >= MAX_EDICTS)
		return;
	oz_cmd[num] = *cmd;
	oz_cmd_ready[num] = true;
}

/*
==============================================================================

PERCEPTION

The 3.19 game code called the brain from player_pain, player_die and
PlayerNoise.  The same facts, as the server sees them.

==============================================================================
*/

#define RR_EV_FOOTSTEP			2
#define RR_EV_OTHER_FOOTSTEP	8
#define RR_EV_LADDER_STEP		9
#define RR_CHAN_ITEM			3

static void OZ_Perceive (void)
{
	int			i;
	edict_t		*e;
	oz_extra_t	*x;

	for (i = 1; i <= game.maxclients && i < oz_num_edicts; i++)
	{
		e = &g_edicts[i];
		x = &oz_extra[i];
		if (!e->inuse || !e->client)
		{
			x->prev_health = 0;
			x->was_dead = false;
			continue;
		}

		// gunfire: the attack button the game publishes for every client
		{
			q2red_bot_entity_t s;
			if (host->get_entity (i, &s) && (s.buttons & BUTTON_ATTACK)
				&& !e->deadflag && e->client->pers.weapon)
				Bot_NoteNoise (e);
		}

		// footsteps
		if (e->s.event == RR_EV_FOOTSTEP || e->s.event == RR_EV_OTHER_FOOTSTEP
			|| e->s.event == RR_EV_LADDER_STEP)
			Bot_NoteNoiseEx (e, NOISE_STEP, e->s.origin);

		// pain and death
		if (!e->deadflag && x->prev_health > 0 && e->health < x->prev_health)
		{
			Bot_NoteNoiseEx (e, NOISE_PAIN, e->s.origin);
			// the attacker: a visible client that fired in the last moment,
			// nearest first (the rerelease publishes when, not who)
			{
				int		k;
				edict_t	*best = NULL;
				float	best_d = 1e9f;
				for (k = 1; k <= game.maxclients && k < oz_num_edicts; k++)
				{
					edict_t	*a = &g_edicts[k];
					vec3_t	d;
					float	dist;
					if (a == e || !a->inuse || !a->client || a->deadflag)
						continue;
					if (OzHost_IsTeammate (a, e))
						continue;
					if (Bot_NoiseTime (a) < level.time - 0.3f)
						continue;
					if (!visible (e, a))
						continue;
					VectorSubtract (a->s.origin, e->s.origin, d);
					dist = VectorLength (d);
					if (dist < best_d)
					{
						best_d = dist;
						best = a;
					}
				}
				if (best)
					Bot_NotePain (e, best);
			}
		}
		if (e->deadflag && !x->was_dead)
		{
			int		mod = 0;
			int		c = GI_pointcontents (e->s.origin);
			if (c & CONTENTS_LAVA)
				mod = MOD_LAVA;
			else if (c & CONTENTS_SLIME)
				mod = MOD_SLIME;
			else if ((c & CONTENTS_WATER) && e->waterlevel >= 3)
				mod = MOD_WATER;
			Bot_NoteDeath (e, mod);
		}
		x->was_dead = e->deadflag ? true : false;
		x->prev_health = e->health;
	}
}

/*
==============================================================================

BRAIN CALLBACKS

==============================================================================
*/

static void OZ_SetTiming (void)
{
	int rate = (int)host->tick_rate ();

	if (rate < 10)
		rate = 10;
	game.framerate = rate;
	game.frametime = 1.0 / rate;
	game.framediv = rate / 10;
	if (game.framediv < 1)
		game.framediv = 1;
}

static void OZ_SetLevelTime (void)
{
	level.time = (float)(host->level_time_ms () * 0.001);
}

static void OZ_Command (void)
{
	OZ_SyncCvars ();
	oz_argv_shift = 1;
	Bot_ServerCommand ();
	oz_argv_shift = 0;
}

static int OZ_Init (void)
{
	memset (&gi, 0, sizeof(gi));
	gi.bprintf = GI_bprintf;
	gi.dprintf = GI_dprintf;
	gi.cprintf = GI_cprintf;
	gi.centerprintf = GI_centerprintf;
	gi.sound = GI_sound;
	gi.positioned_sound = GI_positioned_sound;
	gi.configstring = GI_configstring;
	gi.error = GI_error;
	gi.modelindex = GI_index;
	gi.soundindex = GI_index;
	gi.imageindex = GI_index;
	gi.setmodel = GI_setmodel;
	gi.trace = GI_trace;
	gi.pointcontents = GI_pointcontents;
	gi.inPVS = GI_inPVS;
	gi.inPHS = GI_inPHS;
	gi.SetAreaPortalState = GI_SetAreaPortalState;
	gi.AreasConnected = GI_AreasConnected;
	gi.linkentity = GI_linkentity;
	gi.unlinkentity = GI_linkentity;
	gi.BoxEdicts = GI_BoxEdicts;
	gi.Pmove = GI_Pmove;
	gi.multicast = GI_multicast;
	gi.unicast = GI_unicast;
	gi.WriteChar = GI_noop_int;
	gi.WriteByte = GI_noop_int;
	gi.WriteShort = GI_noop_int;
	gi.WriteLong = GI_noop_int;
	gi.WriteFloat = GI_noop_float;
	gi.WriteString = GI_noop_str;
	gi.WritePosition = GI_noop_vec;
	gi.WriteDir = GI_noop_vec;
	gi.WriteAngle = GI_noop_float;
	gi.TagMalloc = GI_TagMalloc;
	gi.TagFree = GI_TagFree;
	gi.FreeTags = GI_FreeTags;
	gi.cvar = GI_cvar;
	gi.cvar_set = GI_cvar_set;
	gi.cvar_forceset = GI_cvar_forceset;
	gi.argc = GI_argc;
	gi.argv = GI_argv;
	gi.args = GI_args;
	gi.AddCommandString = GI_AddCommandString;
	gi.DebugGraph = GI_DebugGraph;

	g_edicts = calloc (MAX_EDICTS, sizeof(edict_t));
	if (!g_edicts)
		return 0;
	game.maxclients = host->max_clients ();
	if (game.maxclients < 1 || game.maxclients > MAX_CLIENTS)
		game.maxclients = MAX_CLIENTS;
	oz_clients = calloc ((size_t)game.maxclients, sizeof(gclient_t));
	if (!oz_clients)
		return 0;
	game.clients = oz_clients;
	game.maxentities = MAX_EDICTS;
	globals.edicts = g_edicts;
	globals.edict_size = sizeof(edict_t);
	globals.max_edicts = MAX_EDICTS;
	OZ_SetTiming ();

	deathmatch = gi.cvar ("deathmatch", "0", 0);
	dedicated = gi.cvar ("dedicated", "0", 0);
	maxclients = gi.cvar ("maxclients", "4", 0);
	sv_gravity = gi.cvar ("sv_gravity", "800", 0);
	bot_telemetry_cv = gi.cvar ("bot_telemetry", "0", 0);
	bot_minplayers_cv = gi.cvar ("bot_minplayers", "0", 0);
	bot_minclients_cv = gi.cvar ("bot_minClients", "-1", 0);
	bot_numbots_cv = gi.cvar ("bot_numBots", "-1", 0);

	// MuffMode and MMREX prefix bot names themselves (bot_name_prefix, "B|");
	// ozbot's own prefix would double it
	{
		cvar_t *mod_prefix = gi.cvar ("bot_name_prefix", "", 0);
		oz_name_prefixed = (mod_prefix && mod_prefix->string && mod_prefix->string[0]) ? true : false;
	}

	Bot_Init ();
	if (oz_name_prefixed)
		gi.cvar_forceset ("bot_nameprefix", "");

	host->add_command ("bot_add", OZ_Command);
	host->add_command ("bot_remove", OZ_Command);
	host->add_command ("bot_clear", OZ_Command);
	host->add_command ("nav_query", OZ_Command);
	return 1;
}

static void OZ_Shutdown (void)
{
	OZ_SyncCvars ();
	Bot_Shutdown ();
	free (g_edicts);
	g_edicts = NULL;
	free (oz_clients);
	oz_clients = NULL;
	oz_num_cvars = 0;
}

static void OZ_MapBegin (const char *mapname)
{
	OZ_SyncCvars ();
	OZ_SetTiming ();
	memset (&level, 0, sizeof(level));
	OZ_strncpyz (level.mapname, (char *)mapname, sizeof(level.mapname));
	OZ_SetLevelTime ();
	oz_pool_used = 0;
	memset (oz_extra, 0, sizeof(oz_extra));
	memset (oz_cmd_ready, 0, sizeof(oz_cmd_ready));
	// every char * the last map's mirror held pointed into the pool
	{
		int i;
		for (i = 0; i < MAX_EDICTS; i++)
		{
			edict_t	*e = &g_edicts[i];
			e->classname = e->targetname = e->target = NULL;
			e->inuse = false;
		}
	}
	OZ_MapItems ();
	OZ_ParseEntityString ();
	Ctf_MapBegin ();
	Arena_MapBegin ();
	OZ_MirrorWorld (true);
	oz_level_active = true;
}

static void OZ_MapEnd (void)
{
	oz_level_active = false;
}

/*
Fill mode drives bot_count, which the brain's population manager then meets one
bot a frame.  Bots are real server clients here, so without it a server filled
with bots would turn humans away.  Spectating humans count: they hold a slot.
The rerelease's own cvars come first, so a mod's per-gametype configs work as
written; ozbot's bot_minplayers is the fallback when they are off:
  bot_numBots N    (> 0) exactly N bots
  bot_minClients N (>= 0) keep humans + bots at N; 0 = no bots (Kex: "No Bots
                   Allowed"), -1 (default) = off
  bot_minplayers N (> 0, q2red only) the same fill, when bot_minClients is -1
With all three off, bot_count is left to the operator or the mod (RA2's
ra2_bots adds its own).
*/
static void OZ_FillPopulation (void)
{
	int		i, humans = 0, want, target;
	cvar_t	*count;

	if (bot_numbots_cv && bot_numbots_cv->value > 0)
		target = -(int)bot_numbots_cv->value;		// < 0: a fixed count
	else if (bot_minclients_cv && bot_minclients_cv->value >= 0)
		target = (int)bot_minclients_cv->value;
	else if (bot_minplayers_cv && bot_minplayers_cv->value > 0)
		target = (int)bot_minplayers_cv->value;
	else
		return;
	for (i = 1; i <= game.maxclients && i < oz_num_edicts; i++)
		if (g_edicts[i].inuse && g_edicts[i].client && !host->is_bot (i))
			humans++;
	want = (target < 0) ? -target : target - humans;
	if (want > game.maxclients - humans - 1)
		want = game.maxclients - humans - 1;	// always leave a slot to join into
	if (want < 0)
		want = 0;
	count = OZ_FindCvar ("bot_count");
	if (count && (int)count->value != want)
		gi.cvar_forceset ("bot_count", va ("%d", want));
}

static void OZ_FrameBegin (void)
{
	int		i;

	if (!oz_level_active)
		return;
	// the host refreshed its mirrors on the way in; carry them into the
	// 3.19 cvar_t copies, so an operator's `set bot_minplayers 2` (or any
	// bot_* cvar) takes effect this frame, not at the next map
	OZ_SyncCvars ();
	level.framenum++;
	OZ_SetLevelTime ();
	OZ_MirrorWorld (false);
	OZ_Perceive ();
	Ctf_Frame ();
	Arena_Frame ();

	memset (oz_cmd_ready, 0, sizeof(oz_cmd_ready));
	for (i = 0; i < game.maxclients; i++)
		oz_clients[i].newweapon = NULL;

	OZ_FillPopulation ();
	Bot_RunFrame ();

	// the brain switches weapons by setting client->newweapon
	for (i = 0; i < game.maxclients; i++)
	{
		gitem_t	*w = oz_clients[i].newweapon;
		int		li;

		if (!w)
			continue;
		li = (int)(w - itemlist);
		if (li > 0 && li < OZ_NUM_ITEMS && oz_item_rr[li])
			host->set_weapon (i + 1, oz_item_rr[li], 0);
		oz_clients[i].newweapon = NULL;
	}
}

static void OZ_Think (int32_t entnum, q2red_bot_usercmd_t *cmd)
{
	if (entnum < 1 || entnum >= MAX_EDICTS || !oz_cmd_ready[entnum])
		return;		// not one of ours this frame: the host sends an idle command
	OZ_CmdToHost (&oz_cmd[entnum], cmd);
}

static void OZ_ClientBegin (int32_t entnum, int32_t is_bot)
{
	(void)is_bot;
	if (oz_level_active && entnum > 0 && entnum < MAX_EDICTS)
		OZ_MirrorEntity (entnum, true);
}

static void OZ_ClientDisconnect (int32_t entnum, int32_t is_bot)
{
	int n;

	(void)is_bot;
	if (entnum > 0 && entnum < MAX_EDICTS)
	{
		oz_cmd_ready[entnum] = false;
		// a bot of ours that the server dropped (a mod's or operator's
		// `kick <slot>`) lowers the population, or the manager would put a
		// new bot straight back; in fill mode OZ_FillPopulation decides anyway
		if (Bot_ForgetSlot (&g_edicts[entnum]))
		{
			OZ_SyncCvars ();
			n = (int)OZ_FindCvar ("bot_count")->value - 1;
			gi.cvar_set ("bot_count", va ("%d", n < 0 ? 0 : n));
		}
	}
}

// the operator's addbot / kickbot move the population target, which the
// brain's manager then meets one bot a frame
static void OZ_CmdAddBot (const char *name)
{
	(void)name;
	OZ_SyncCvars ();
	gi.cvar_set ("bot_count", va ("%d", (int)OZ_FindCvar ("bot_count")->value + 1));
}

static void OZ_CmdKickBot (void)
{
	int n;

	OZ_SyncCvars ();
	n = (int)OZ_FindCvar ("bot_count")->value - 1;
	gi.cvar_set ("bot_count", va ("%d", n < 0 ? 0 : n));
}

static void OZ_Sound (int32_t entnum, const float origin[3], int32_t channel,
	int32_t soundindex, float volume, float attenuation)
{
	(void)soundindex; (void)volume; (void)attenuation;
	if (!oz_level_active || entnum < 1 || entnum > game.maxclients || entnum >= MAX_EDICTS)
		return;
	if ((channel & 7) == RR_CHAN_ITEM && g_edicts[entnum].client)
		Bot_NoteNoiseEx (&g_edicts[entnum], NOISE_PICKUP, (float *)origin);
}

qboolean OzHost_TelemetryOn (void)
{
	return bot_telemetry_cv && bot_telemetry_cv->value != 0;
}

// q2red A64: the server asks us to play a simulated player (sv_sim_puppets)
static void OZ_Adopt (int32_t entnum, int32_t on)
{
	if (entnum < 1 || entnum >= MAX_EDICTS)
		return;
	if (on)
		Bot_Adopt (&g_edicts[entnum]);
	else
	{
		Bot_Release (&g_edicts[entnum]);
		oz_cmd_ready[entnum] = false;
	}
}

static const q2red_bot_brain_t oz_brain = {
	Q2RED_BOT_BRAIN_API_VERSION,
	sizeof(q2red_bot_brain_t),
	"ozbot",
	"ozbot-re (q2red host)",

	OZ_Init,
	OZ_Shutdown,
	OZ_MapBegin,
	OZ_MapEnd,
	OZ_ClientBegin,
	OZ_ClientDisconnect,
	OZ_FrameBegin,
	OZ_Think,
	NULL,
	OZ_CmdAddBot,
	OZ_CmdKickBot,
	OZ_Sound,
	NULL,
	NULL,
	OZ_Adopt,
};

__attribute__((visibility("default")))
const q2red_bot_brain_t *Q2RED_GetBotBrainAPI (uint32_t api_version, const q2red_bot_host_t *h)
{
	if (api_version != Q2RED_BOT_BRAIN_API_VERSION || !h
		|| h->api_version != Q2RED_BOT_BRAIN_API_VERSION
		|| h->size < sizeof(q2red_bot_host_t))
		return NULL;
	host = h;
	return &oz_brain;
}
