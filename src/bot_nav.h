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
#define NAV_MAX_LINKS		8		// per node
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
int  Nav_NearestVisibleNode (vec3_t origin, edict_t *ignore);	// nearest with a clear walk, or -1
int  Nav_FindPath (int start, int goal, int *out, int max);	// A*; returns node count
float Nav_LastPathCost (void);						// g-cost of the last successful A*
void Nav_PenalizeLink (int from, int to);			// learn an untraversable edge
qboolean Nav_CanWalk (vec3_t from, vec3_t to, edict_t *ignore);	// clear player-sized path?

//
// learning (called per bot per frame)
//
int  Nav_LearnStep (edict_t *ent, int prev_node, int link_type);

// retag learned lift-column links (vertical walk links inside a real
// func_plat's footprint) as NAV_LINK_PLAT.  Needs spawned entities, so it is
// called from the per-map setup in Bot_RunFrame, not from Nav_Init.  Gated on
// bot_lift so the capability-off graph stays byte-identical to the baseline.
void Nav_TagPlatLinks (void);

#endif // OZBOT_BOT_NAV_H
