/*
ozbot - self-learning q2dm1 bot

bot.h -- shared declarations for the bot subsystem.

NOTE: g_local.h has no include guard, so this header must NOT include it.
Every .c file that includes bot.h must include "g_local.h" first.
*/

#ifndef OZBOT_BOT_H
#define OZBOT_BOT_H

#define BOT_MAX_PATH	128

// bot behavior mode
#define BOT_MODE_EXPLORE	0	// wander, growing the nav graph
#define BOT_MODE_GOAL		1	// follow an A* path to a goal node

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
	qboolean	goal_timing;	// pre-positioning for an item about to respawn
	int			path[BOT_MAX_PATH];
	int			path_len;
	int			path_idx;		// index of the next node to reach
	float		replan_time;	// next time we may replan
	float		goal_time;		// level.time the current goal was set
	float		goal_cost;		// A* g-cost of the committed route (0 = unknown)
	float		goal_best;		// closest we've come to the goal node this attempt
	int			pending_link;	// link type to record on next learn step

	// stuck / progress tracking
	vec3_t		last_pos;
	float		progress_time;	// last time we made progress
	qboolean	was_onground;
	qboolean	did_jump;		// issued a jump; used to learn jump links

	// combat
	edict_t		*enemy;			// current target (NULL = none)
	vec3_t		aim;			// current aim angles (tracks toward target)
	float		reaction_until;	// earliest time we may fire after acquiring
	float		dodge_until;	// when to re-pick a strafe direction
	int			dodge_dir;		// -1 / +1 strafe

	// transition tracking for event logging
	qboolean	was_dead;
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
// true if some OTHER active bot already has 'it' as its current goal_item
qboolean Bot_ItemClaimed (edict_t *it, bot_t *self);

extern cvar_t	*bot_count;
extern cvar_t	*bot_forwardspeed;
extern cvar_t	*bot_debug;
extern cvar_t	*bot_rollout;
extern cvar_t	*bot_claim;
extern cvar_t	*bot_pathcost;
extern cvar_t	*bot_goalbudget;

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

// exposed by p_client.c (already file-scope globals there, just not prototyped
// anywhere) -- reused as the trace/passent plumbing for Bot_RolloutRecover's
// own gi.Pmove() calls, so simulated rollouts clip against the world exactly
// like the real per-frame movement does.
extern edict_t	*pm_passent;
trace_t PM_trace (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end);
// projects move_dir onto the facing to produce forward/side/up in cmd
void Bot_ApplyMovement (bot_t *b, usercmd_t *cmd, float facing_yaw);

//
// bot_combat.c -- enemy selection, aim, fire, dodge
//
// Sets aim into *facing_yaw/*facing_pitch and the fire button into cmd; may
// blend a strafe/range component into b->move_dir.  Returns true if engaged.
qboolean Combat_Aim (bot_t *b, usercmd_t *cmd, float *facing_yaw, float *facing_pitch);
extern cvar_t	*bot_skill;
extern cvar_t	*bot_skilltest;
extern cvar_t	*bot_lead;
extern cvar_t	*bot_leadtest;

//
// bot_goal.c -- item-driven goal selection
//
qboolean Goal_Select (bot_t *b);		// choose b->goal_item; true if found
qboolean Goal_ItemAvailable (edict_t *it);
edict_t *Goal_NearestItem (bot_t *b, float maxdist);	// for directed exploration
void Goal_Blacklist (edict_t *it, float secs);	// avoid re-targeting briefly
void Goal_Reset (void);					// clear cooldowns (map change)
void Goal_SeedNavNodes (void);			// seed nav nodes at item spots

//
// bot_log.c
//
const char *Bot_GameDir (void);
void Bot_LogItemEvent (bot_t *b, const char *event, const char *item);
void Bot_LogGiveup (bot_t *b, float gdist, float gvdist, int atnode, int fighting);
void Bot_LogBeginLevel (const char *mapname);	// open a fresh JSONL for this map
void Bot_LogEndLevel (void);					// flush + close the current JSONL
void Bot_LogTick (bot_t *b);					// per-tick state record
void Bot_LogEvent (bot_t *b, const char *event);	// spawn/death/etc.
void Bot_LogMaybeFlush (void);					// periodic flush

#endif // OZBOT_BOT_H
