/*
ozbot - self-learning q2dm1 bot

bot_nav.h -- waypoint navigation graph.

The graph is *learned*: as bots explore, we drop nodes on reachable ground and
link consecutive nodes the bot actually traversed.  Links are therefore
traversable by construction, which makes path-following reliable without a
precomputed navmesh.  The graph is saved per-map and reloaded on later runs.

NOTE: include "g_local.h" and "bot.h" before this header.
*/

#ifndef OZBOT_BOT_NAV_H
#define OZBOT_BOT_NAV_H

#define NAV_MAGIC			(('V'<<24)|('A'<<16)|('N'<<8)|'O')	// "ONAV"
#define NAV_VERSION			1

#define NAV_MAX_NODES		2048
#define NAV_MAX_LINKS		12		// per node (was 8; playbook links need room on
									// saturated corridor nodes -- the .nav format
									// stores num_links per node, so files written
									// at either cap load fine)
#define NAV_NODE_DENSITY	96.0f	// min spacing between distinct nodes
#define NAV_ARRIVE_RADIUS	48.0f	// horizontal "reached this node" radius
#define NAV_ARRIVE_Z		48.0f	// vertical tolerance for arrival

// link types
#define NAV_LINK_WALK		0
#define NAV_LINK_FALL		1		// stepped/dropped down to dest
#define NAV_LINK_JUMP		2		// left ground via jump to reach dest
#define NAV_LINK_TELEPORT	3
#define NAV_LINK_WATER		4
#define NAV_LINK_PLAT		5		// carried up by a func_plat (one-way; bot_lift)
#define NAV_LINK_PLAYBOOK	6		// recorded-input maneuver (one-way; bot_playbook).
									// Injected from <gamedir>/playbooks/<map>.pbk at map
									// load, never saved to the .nav (the executor data
									// lives in the playbook file, not the graph)

// capability masks for filtered pathfinding (bot_navmask): bit (1 << type)
// lets A* expand links of that type.  Masks never touch the graph itself --
// interpretation happens at path time, so one shared graph serves any
// capability configuration (a lift-off bot routes around PLAT links instead
// of attempting rides it can't execute).  The WATER bit is special: the
// learner stamps water-ness on NODES (NAV_FLAG_WATER), not links, so
// clearing the WATER bit also excludes links into water-flagged nodes.
#define NAV_MASK(t)			(1 << (t))
#define NAV_MASK_ALL		0xff

// Nav_QueryPath return codes -- the reachability oracle (plans/nav-oracle.md
// Phase C), the re-release PathReturnCode idea over the learned graph.
// A learned graph can only prove reachability, never UNreachability:
// NO_PATH means "no route known (yet)", not "impossible".
#define NAVQ_OK				0	// routable with the given mask
#define NAVQ_GATED			1	// routable only with capabilities outside the mask
#define NAVQ_NO_PATH		2	// no route known even with every capability
#define NAVQ_NO_GOAL_NODE	3	// graph has no node near the goal position
#define NAVQ_NO_START_NODE	4	// graph has no node near the start position

// node flags
#define NAV_FLAG_WATER		1

typedef struct
{
	int		to;			// destination node index
	byte	type;		// NAV_LINK_*
	float	cost;		// precomputed traversal cost
} nav_link_t;

typedef struct
{
	vec3_t		origin;
	byte		flags;
	int			num_links;
	nav_link_t	links[NAV_MAX_LINKS];
} nav_node_t;

typedef struct
{
	int			num_nodes;
	nav_node_t	nodes[NAV_MAX_NODES];
} nav_graph_t;

extern nav_graph_t	nav;

//
// lifecycle
//
void Nav_Init (const char *mapname);	// clear + try to load <map>.nav
void Nav_Shutdown (const char *mapname);	// save if dirty
void Nav_MaybeSave (const char *mapname);	// periodic autosave if dirty

//
// queries
//
int  Nav_SeedNode (vec3_t origin);					// ensure+connect a node here
int  Nav_NearestNode (vec3_t origin);				// nearest node, or -1
int  Nav_NearestGoalNode (vec3_t origin);			// nearest A*-targetable node
													// (bot_goalnode: skips in-degree-0 orphans)
int  Nav_NearestVisibleNode (vec3_t origin, edict_t *ignore);	// nearest with a clear walk, or -1
int  Nav_FindPath (int start, int goal, int *out, int max);	// A*; returns node count
int  Nav_FindPathMasked (int start, int goal, int mask, int *out, int max);	// A* over NAV_MASK-allowed link types
// the oracle: can 'to' be routed to from 'from' under 'mask'?  Returns a
// NAVQ_* code; on OK/GATED writes the route's g-cost to *cost, on GATED
// writes the mask of link types that unlock the route to *gate (either
// out-param may be NULL)
int  Nav_QueryPath (vec3_t from, vec3_t to, int mask, float *cost, int *gate);
const char *Nav_QueryName (int code);				// NAVQ_* -> short string for logs
void Nav_MaskNames (int mask, char *out, int outsize);	// link-type mask -> "water+plat"
float Nav_LastPathCost (void);						// g-cost of the last successful A*
void Nav_PenalizeLink (int from, int to);			// learn an untraversable edge
qboolean Nav_CanWalk (vec3_t from, vec3_t to, edict_t *ignore);	// clear player-sized path?

//
// learning (called per bot per frame)
//
int  Nav_LearnStep (edict_t *ent, int prev_node, int link_type);

// explicit link injection (bot_playback.c playbook registration); same
// dedup/cost rules as learned links
void Nav_AddLinkType (int from, int to, int type);

// retag learned lift-column links (vertical walk links inside a real
// func_plat's footprint) as NAV_LINK_PLAT.  Needs spawned entities, so it is
// called from the per-map setup in Bot_RunFrame, not from Nav_Init.  Gated on
// bot_lift so the capability-off graph stays byte-identical to the baseline.
void Nav_TagPlatLinks (void);

#endif // OZBOT_BOT_NAV_H
