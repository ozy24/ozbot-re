/*
ozbot - self-learning q2dm1 bot

bot_nav.c -- learned waypoint graph: build, save/load, nearest-node queries,
and A* pathfinding.
*/

#include "g_local.h"
#include "bot.h"
#include "bot_nav.h"

#ifdef _WIN32
#include <direct.h>
#define OZ_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define OZ_MKDIR(p) mkdir(p, 0755)
#endif

nav_graph_t		nav;
static qboolean	nav_dirty;

// transient per-link failure counters (not persisted): a link the bots keep
// failing to traverse gets penalized, then removed, so A* routes around it
static byte		link_fails[NAV_MAX_NODES][NAV_MAX_LINKS];

// player bounding box, used for walkability traces
static const vec3_t pl_mins = {-16, -16, -24};
static const vec3_t pl_maxs = { 16,  16,  32};

static float Nav_Dist (vec3_t a, vec3_t b)
{
	vec3_t	d;
	VectorSubtract (a, b, d);
	return VectorLength (d);
}

/*
=================
Nav_LinkCost
=================
*/
static float Nav_LinkCost (vec3_t a, vec3_t b, int type)
{
	float	d = Nav_Dist(a, b);
	switch (type)
	{
	case NAV_LINK_FALL:		return d * 1.2f;
	case NAV_LINK_JUMP:		return d * 1.5f + 32.0f;
	case NAV_LINK_TELEPORT:	return 64.0f;	// cheap, fixed
	case NAV_LINK_WATER:	return d * 1.6f;
	case NAV_LINK_PLAT:		return d + 400.0f;	// wait allowance: boarding may
												// mean waiting out a plat cycle
	default:				return d;
	}
}

/*
=================
Nav_CanWalk

True if a player-sized box can move from 'from' to 'to' without hitting solid.
A coarse reachability test (ignores floor continuity), used for nearest-node
visibility and path smoothing -- never for creating links.
=================
*/
qboolean Nav_CanWalk (vec3_t from, vec3_t to, edict_t *ignore)
{
	trace_t	tr;
	vec3_t	s, e;

	VectorCopy (from, s);
	VectorCopy (to, e);
	s[2] += 18;	// lift to step height so we clear small lips
	e[2] += 18;

	tr = gi.trace (s, (float *)pl_mins, (float *)pl_maxs, e, ignore, MASK_PLAYERSOLID);
	return (tr.fraction == 1.0f && !tr.startsolid && !tr.allsolid);
}

/*
=================
Nav_AddNode

Returns an existing node within NAV_NODE_DENSITY of origin, else creates one.
Returns -1 if the graph is full.
=================
*/
static int Nav_AddNode (vec3_t origin, byte flags)
{
	int		i, best = -1;
	float	bestd = NAV_NODE_DENSITY * NAV_NODE_DENSITY;
	nav_node_t *n;

	for (i = 0; i < nav.num_nodes; i++)
	{
		vec3_t	d;
		float	dsq;
		VectorSubtract (origin, nav.nodes[i].origin, d);
		if (d[2] > 48 || d[2] < -48)
			continue;	// different vertical level
		dsq = d[0]*d[0] + d[1]*d[1] + d[2]*d[2];
		if (dsq < bestd)
		{
			bestd = dsq;
			best = i;
		}
	}
	if (best >= 0)
		return best;

	if (nav.num_nodes >= NAV_MAX_NODES)
		return -1;

	n = &nav.nodes[nav.num_nodes];
	VectorCopy (origin, n->origin);
	n->flags = flags;
	n->num_links = 0;
	nav_dirty = true;
	return nav.num_nodes++;
}

/*
=================
Nav_AddLink

Adds a directed link from -> to (no duplicates).
=================
*/
static void Nav_AddLink (int from, int to, int type)
{
	nav_node_t	*n;
	int			i;

	if (from < 0 || to < 0 || from == to)
		return;

	n = &nav.nodes[from];
	for (i = 0; i < n->num_links; i++)
		if (n->links[i].to == to)
			return;	// already linked

	if (n->num_links >= NAV_MAX_LINKS)
		return;

	n->links[n->num_links].to = to;
	n->links[n->num_links].type = (byte)type;
	n->links[n->num_links].cost = Nav_LinkCost (n->origin, nav.nodes[to].origin, type);
	n->num_links++;
	nav_dirty = true;
}

/*
=================
Nav_LearnStep

Called for a bot each frame.  If the bot is on standable ground, snap it to a
node (creating one if needed) and, if it came from a previous node, record the
traversed link (in both directions for walkable moves).  Returns the current
node index, or prev_node if no node was registered this step.
=================
*/
int Nav_LearnStep (edict_t *ent, int prev_node, int link_type)
{
	int		cur;
	byte	flags = 0;

	// only place nodes on solid ground (or while swimming) and while alive
	if (ent->deadflag)
		return prev_node;
	if (!ent->groundentity && ent->waterlevel < 2)
		return prev_node;	// airborne; wait until we land

	if (ent->waterlevel >= 2)
		flags |= NAV_FLAG_WATER;

	cur = Nav_AddNode (ent->s.origin, flags);
	if (cur < 0)
		return prev_node;	// graph full

	if (prev_node >= 0 && prev_node != cur)
	{
		// only link nodes that are plausibly adjacent; a large jump in position
		// between frames means a teleport or discontinuity, not a walkable edge.
		if (Nav_Dist (nav.nodes[prev_node].origin, nav.nodes[cur].origin) <= 200.0f)
		{
			Nav_AddLink (prev_node, cur, link_type);
			// walkable moves are reversible; falls/jumps are recorded one-way
			// (the reverse, if reachable, will be learned when actually walked).
			if (link_type == NAV_LINK_WALK || link_type == NAV_LINK_WATER)
				Nav_AddLink (cur, prev_node, link_type);
		}
	}

	return cur;
}

/*
=================
Nav_TagPlatLinks

Retag learned lift-column links as NAV_LINK_PLAT so path costs price the
ride's wait and the lift controller knows where to engage.  A column link is
a near-vertical walk link (the signature of being carried while "grounded" on
a rising plat) whose BOTH endpoints sit inside a real func_plat's horizontal
footprint -- the entity check matters: pure geometry also matches water-jump
ledge exits (measured on q2dm1, Phase 0 of plans/lift-riding.md).

Gated on bot_lift so the capability-off control graph is untouched.
=================
*/
void Nav_TagPlatLinks (void)
{
	int		i, j, tagged = 0;

	if (bot_lift->value == 0)
		return;

	for (i = 0; i < nav.num_nodes; i++)
	{
		nav_node_t *n = &nav.nodes[i];
		for (j = 0; j < n->num_links; j++)
		{
			nav_link_t	*l = &n->links[j];
			nav_node_t	*m;
			vec3_t		d;
			edict_t		*p;

			if (l->type != NAV_LINK_WALK)
				continue;
			if (l->to < 0 || l->to >= nav.num_nodes)
				continue;
			m = &nav.nodes[l->to];
			VectorSubtract (m->origin, n->origin, d);
			if (d[2] < 48)
				continue;	// not a climb
			if (d[0]*d[0] + d[1]*d[1] > 30*30)
				continue;	// not near-vertical
			p = Bot_FindPlatAt (n->origin);
			if (!p || Bot_FindPlatAt (m->origin) != p)
				continue;	// endpoints not on one real plat

			l->type = NAV_LINK_PLAT;
			l->cost = Nav_LinkCost (n->origin, m->origin, NAV_LINK_PLAT);
			tagged++;
			nav_dirty = true;
		}
	}

	if (tagged)
		gi.dprintf ("ozbot: tagged %d plat link(s)\n", tagged);
}

/*
=================
Nav_SeedNode

Ensure a node exists at 'origin' (e.g. an item spot) and connect it to nearby
existing nodes reachable by a clear walk.  Bootstraps coverage of areas bots
haven't wandered through yet, so the goal layer can route to those items.
Returns the node index, or -1.
=================
*/
// append a node at exactly 'origin' without merging into a nearby node
static int Nav_AddNodeForce (vec3_t origin, byte flags)
{
	nav_node_t *n;
	if (nav.num_nodes >= NAV_MAX_NODES)
		return -1;
	n = &nav.nodes[nav.num_nodes];
	VectorCopy (origin, n->origin);
	n->flags = flags;
	n->num_links = 0;
	nav_dirty = true;
	return nav.num_nodes++;
}

int Nav_SeedNode (vec3_t origin)
{
	int		idx, i, links = 0, nearest;

	// Put a node *exactly* at the item so a routed path ends on the pickup
	// ("arrived" == "touched").  Reuse an existing node only if it sits right on
	// the item AND has a clear final hop to it -- otherwise the bot reaches the
	// node but a wall blocks the last few units to the item (the dominant
	// touch-failure we measured).
	nearest = Nav_NearestNode (origin);
	if (nearest >= 0
		&& Nav_Dist (nav.nodes[nearest].origin, origin) <= 48.0f
		&& Nav_CanWalk (nav.nodes[nearest].origin, origin, NULL))
		idx = nearest;
	else
		idx = Nav_AddNodeForce (origin, 0);
	if (idx < 0)
		return -1;

	for (i = 0; i < nav.num_nodes; i++)
	{
		vec3_t	d;
		float	dist;
		if (i == idx)
			continue;
		VectorSubtract (nav.nodes[i].origin, origin, d);
		if (d[2] > 64 || d[2] < -64)
			continue;
		dist = VectorLength (d);
		if (dist < 1 || dist > 256)
			continue;
		if (!Nav_CanWalk (origin, nav.nodes[i].origin, NULL))
			continue;
		Nav_AddLink (idx, i, NAV_LINK_WALK);
		Nav_AddLink (i, idx, NAV_LINK_WALK);
		links++;
	}
	return idx;
}

/*
=================
Nav_PenalizeLink

Called when a bot repeatedly fails to traverse from 'from' to 'to'.  Doubles
the link cost (so A* prefers alternatives) and, after enough failures, removes
the link entirely so the graph self-corrects untraversable edges.
=================
*/
void Nav_PenalizeLink (int from, int to)
{
	nav_node_t	*n;
	int			i, j;

	if (from < 0 || from >= nav.num_nodes)
		return;
	n = &nav.nodes[from];
	for (i = 0; i < n->num_links; i++)
	{
		if (n->links[i].to != to)
			continue;

		// plat links are executed by the lift controller (bot_move.c), which
		// suppresses stuck detection during a ride -- a bot that stalls near a
		// shaft anyway must not erode the learned column
		if (bot_lift->value != 0 && n->links[i].type == NAV_LINK_PLAT)
			return;

		if (++link_fails[from][i] >= 3)
		{
			// drop the link
			for (j = i + 1; j < n->num_links; j++)
			{
				n->links[j - 1] = n->links[j];
				link_fails[from][j - 1] = link_fails[from][j];
			}
			n->num_links--;
			link_fails[from][n->num_links] = 0;
			nav_dirty = true;
		}
		else
		{
			n->links[i].cost *= 2.0f;
			nav_dirty = true;
		}
		return;
	}
}

/*
=================
Nav_NearestNode
=================
*/
int Nav_NearestNode (vec3_t origin)
{
	int		i, best = -1;
	float	bestd = 1e18f;

	for (i = 0; i < nav.num_nodes; i++)
	{
		vec3_t	d;
		float	dsq;
		VectorSubtract (origin, nav.nodes[i].origin, d);
		d[2] *= 2.0f;	// penalize vertical separation
		dsq = d[0]*d[0] + d[1]*d[1] + d[2]*d[2];
		if (dsq < bestd)
		{
			bestd = dsq;
			best = i;
		}
	}
	return best;
}

/*
=================
Nav_NearestVisibleNode

Nearest node reachable by a clear player-sized trace.  Falls back to the
plain nearest node if none is visible.
=================
*/
int Nav_NearestVisibleNode (vec3_t origin, edict_t *ignore)
{
	int		i, best = -1;
	float	bestd = 1e18f;

	for (i = 0; i < nav.num_nodes; i++)
	{
		vec3_t	d;
		float	dsq;
		VectorSubtract (origin, nav.nodes[i].origin, d);
		dsq = d[0]*d[0] + d[1]*d[1] + d[2]*d[2];
		if (dsq >= bestd)
			continue;
		if (!Nav_CanWalk (origin, nav.nodes[i].origin, ignore))
			continue;
		bestd = dsq;
		best = i;
	}
	if (best < 0)
		best = Nav_NearestNode (origin);
	return best;
}

/*
=================
Nav_FindPath

A* from start to goal over node centers.  Writes the node sequence (including
both endpoints) into out[] and returns its length, or 0 if unreachable.
=================
*/
static float	nav_path_cost;	// g-cost of the last successful Nav_FindPath

float Nav_LastPathCost (void)
{
	return nav_path_cost;
}

int Nav_FindPath (int start, int goal, int *out, int max)
{
	static float	gscore[NAV_MAX_NODES];
	static float	fscore[NAV_MAX_NODES];
	static int		came[NAV_MAX_NODES];
	static byte		open[NAV_MAX_NODES];
	static byte		closed[NAV_MAX_NODES];
	int		i, count;
	int		tmp[NAV_MAX_NODES];

	nav_path_cost = 1e18f;
	if (start < 0 || goal < 0 || start >= nav.num_nodes || goal >= nav.num_nodes)
		return 0;
	if (start == goal)
	{
		if (max > 0) out[0] = start;
		nav_path_cost = 0;
		return (max > 0) ? 1 : 0;
	}

	for (i = 0; i < nav.num_nodes; i++)
	{
		gscore[i] = 1e18f;
		fscore[i] = 1e18f;
		came[i] = -1;
		open[i] = 0;
		closed[i] = 0;
	}

	gscore[start] = 0;
	fscore[start] = Nav_Dist (nav.nodes[start].origin, nav.nodes[goal].origin);
	open[start] = 1;

	for (;;)
	{
		int		cur = -1;
		float	bestf = 1e18f;
		nav_node_t *n;

		// pick open node with lowest fscore (linear scan; replans are rare)
		for (i = 0; i < nav.num_nodes; i++)
			if (open[i] && fscore[i] < bestf)
			{
				bestf = fscore[i];
				cur = i;
			}

		if (cur < 0)
			return 0;	// open set empty -> unreachable

		if (cur == goal)
			break;

		open[cur] = 0;
		closed[cur] = 1;
		n = &nav.nodes[cur];

		for (i = 0; i < n->num_links; i++)
		{
			int		nb = n->links[i].to;
			float	tentative;
			if (closed[nb])
				continue;
			tentative = gscore[cur] + n->links[i].cost;
			if (tentative < gscore[nb])
			{
				came[nb] = cur;
				gscore[nb] = tentative;
				fscore[nb] = tentative + Nav_Dist (nav.nodes[nb].origin, nav.nodes[goal].origin);
				open[nb] = 1;
			}
		}
	}

	// reconstruct (goal -> start), then reverse into out
	count = 0;
	for (i = goal; i >= 0 && count < NAV_MAX_NODES; i = came[i])
		tmp[count++] = i;

	if (count > max)
		return 0;	// caller's buffer too small

	for (i = 0; i < count; i++)
		out[i] = tmp[count - 1 - i];
	nav_path_cost = gscore[goal];
	return count;
}

//==========================================================================
// persistence
//==========================================================================

static void Nav_Path (const char *mapname, char *out, int outsize)
{
	Com_sprintf (out, outsize, "%s/nav/%s.nav",
		Bot_GameDir(), (mapname && mapname[0]) ? mapname : "unknown");
}

/*
=================
Nav_Load
=================
*/
static qboolean Nav_Load (const char *mapname)
{
	char	dir[MAX_OSPATH];
	char	path[MAX_OSPATH];
	FILE	*f;
	int		magic = 0, version = 0, count = 0, i;

	Com_sprintf (dir, sizeof(dir), "%s/nav", Bot_GameDir());
	OZ_MKDIR (dir);

	Nav_Path (mapname, path, sizeof(path));
	f = fopen (path, "rb");
	if (!f)
		return false;

	fread (&magic, sizeof(magic), 1, f);
	fread (&version, sizeof(version), 1, f);
	fread (&count, sizeof(count), 1, f);
	if (magic != NAV_MAGIC || version != NAV_VERSION || count < 0 || count > NAV_MAX_NODES)
	{
		gi.dprintf ("ozbot: nav file %s is incompatible; ignoring\n", path);
		fclose (f);
		return false;
	}

	nav.num_nodes = count;
	for (i = 0; i < count; i++)
	{
		nav_node_t *n = &nav.nodes[i];
		fread (n->origin, sizeof(n->origin), 1, f);
		fread (&n->flags, sizeof(n->flags), 1, f);
		fread (&n->num_links, sizeof(n->num_links), 1, f);
		if (n->num_links < 0 || n->num_links > NAV_MAX_LINKS)
			n->num_links = 0;
		fread (n->links, sizeof(nav_link_t), n->num_links, f);
	}
	fclose (f);

	gi.dprintf ("ozbot: loaded nav %s (%d nodes)\n", path, count);
	return true;
}

/*
=================
Nav_Save
=================
*/
static void Nav_Save (const char *mapname)
{
	char	dir[MAX_OSPATH];
	char	path[MAX_OSPATH];
	FILE	*f;
	int		magic = NAV_MAGIC, version = NAV_VERSION, i;

	if (!nav.num_nodes)
		return;

	Com_sprintf (dir, sizeof(dir), "%s/nav", Bot_GameDir());
	OZ_MKDIR (dir);

	Nav_Path (mapname, path, sizeof(path));
	f = fopen (path, "wb");
	if (!f)
	{
		gi.dprintf ("ozbot: could not write nav %s\n", path);
		return;
	}

	fwrite (&magic, sizeof(magic), 1, f);
	fwrite (&version, sizeof(version), 1, f);
	fwrite (&nav.num_nodes, sizeof(nav.num_nodes), 1, f);
	for (i = 0; i < nav.num_nodes; i++)
	{
		nav_node_t *n = &nav.nodes[i];
		fwrite (n->origin, sizeof(n->origin), 1, f);
		fwrite (&n->flags, sizeof(n->flags), 1, f);
		fwrite (&n->num_links, sizeof(n->num_links), 1, f);
		fwrite (n->links, sizeof(nav_link_t), n->num_links, f);
	}
	fclose (f);

	gi.dprintf ("ozbot: saved nav %s (%d nodes)\n", path, nav.num_nodes);
}

/*
=================
Nav_MaybeSave

Autosave at most every 30s while the graph is changing, so an unclean exit
still persists what was learned.
=================
*/
void Nav_MaybeSave (const char *mapname)
{
	static float next_save;

	if (!nav_dirty)
		return;
	if (level.time < next_save)
		return;
	next_save = level.time + 30.0f;
	Nav_Save (mapname);
	nav_dirty = false;
}

/*
=================
Nav_Init
=================
*/
void Nav_Init (const char *mapname)
{
	memset (&nav, 0, sizeof(nav));
	memset (link_fails, 0, sizeof(link_fails));
	nav_dirty = false;
	Nav_Load (mapname);
}

/*
=================
Nav_Shutdown
=================
*/
void Nav_Shutdown (const char *mapname)
{
	if (nav_dirty)
		Nav_Save (mapname);
	nav_dirty = false;
}
