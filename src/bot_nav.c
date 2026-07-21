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

// bot_navlearn / ONAV FROZEN flag: when the loaded graph is frozen (a baked
// gen_nav.exe output) the runtime learner never adds to it and it is never
// re-saved, so live play can't grow it past its tuned sweet spot.  nav_flags
// carries the loaded header flags through to the next save (v1 files -> 0).
static qboolean	nav_frozen;
static int		nav_flags;

// transient per-link failure counters (not persisted): a link the bots keep
// failing to traverse gets penalized, then removed, so A* routes around it
static byte		link_fails[NAV_MAX_NODES][NAV_MAX_LINKS];

// in-link count per node, maintained incrementally (AddLink / PenalizeLink
// removal / load).  A node with in-degree 0 can never be an A* *target* --
// Nav_NearestGoalNode (bot_goalnode) skips such orphans so they can't shadow
// real coverage next to an item (the Phase C reach-sweep discovery).
static short	nav_indegree[NAV_MAX_NODES];

// player bounding box, used for walkability traces
static const vec3_t pl_mins = {-16, -16, -24};
static const vec3_t pl_maxs = { 16,  16,  32};

// bot_hazard: active trigger_hurt volumes cached per map (pointcontents can
// never see SOLID_TRIGGER entities, so hurt brushes need their own test).
// Filled by Nav_FlagHazardNodes in the per-map setup.
#define NAV_MAX_HURT_VOLUMES	32
static vec3_t	hurt_mins[NAV_MAX_HURT_VOLUMES];
static vec3_t	hurt_maxs[NAV_MAX_HURT_VOLUMES];
static int		num_hurt_volumes;

/*
=================
Nav_PointHazardFlags

Hazard classification for a node position (the bot's origin, so ~24u above
the floor): NAV_FLAG_HAZARD for lava or a cached trigger_hurt volume (A*
forbids), NAV_FLAG_SLIME for slime (A* prices 4x), 0 for safe.  Probes the
point itself and 20u below it -- a node learned while standing on a knee-deep
liquid floor has its origin in open air but its feet in the burn.
=================
*/
static byte Nav_PointHazardFlags (vec3_t origin)
{
	vec3_t	p;
	int		i, c;

	c = gi.pointcontents (origin);
	VectorCopy (origin, p);
	p[2] -= 20;
	c |= gi.pointcontents (p);
	if (c & CONTENTS_LAVA)
		return NAV_FLAG_HAZARD;
	for (i = 0; i < num_hurt_volumes; i++)
	{
		if (origin[0] >= hurt_mins[i][0] - 8 && origin[0] <= hurt_maxs[i][0] + 8
			&& origin[1] >= hurt_mins[i][1] - 8 && origin[1] <= hurt_maxs[i][1] + 8
			&& origin[2] >= hurt_mins[i][2] - 8 && origin[2] <= hurt_maxs[i][2] + 32)
			return NAV_FLAG_HAZARD;
	}
	if (c & CONTENTS_SLIME)
		return NAV_FLAG_SLIME;
	return 0;
}

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
	case NAV_LINK_PUSH:		return 96.0f;	// jump pad: fast, near-free to ride (a
											// short launch-and-land), but a hair above
											// TELEPORT -- the arc costs a moment of
											// airtime and the landing snap is fuzzier
											// than a teleport's exact drop-point
	case NAV_LINK_WATER:	return d * 1.6f;
	case NAV_LINK_PLAT:		return d + 400.0f;	// wait allowance: boarding may
												// mean waiting out a plat cycle
	case NAV_LINK_PLAYBOOK:	return 64.0f + d * 0.6f;	// recorded runs move at strafe-jump
												// pace (~1.5x run speed) plus an
												// align-up allowance; pricing below
												// walk distance is what routes A*
												// through the maneuver (teleport
												// links already break strict
												// distance admissibility)
	case NAV_LINK_TRAIN:	return d + 1500.0f;	// a shuttle ride is a big time sink
												// (wait for it + ride a full leg): price
												// it well above a plat so A* only detours
												// here for a goal worth the round trip,
												// not to re-check a respawning platform
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
	nav_indegree[to]++;
	nav_dirty = true;
}

/*
=================
Nav_AddLinkType

Explicit link injection for the playbook loader (bot_playback.c).
=================
*/
void Nav_AddLinkType (int from, int to, int type)
{
	Nav_AddLink (from, to, type);
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

	// bot_navlearn / FROZEN: a baked or explicitly-frozen graph is played as-is
	// -- never grow or mutate it (protects the tuned node-count sweet spot on a
	// long-running live server).  Seeding (items/train/teleport/plat) and
	// playbook injection still run at load; only the per-frame LEARNER stops.
	if (nav_frozen)
		return prev_node;
	if (bot_navlearn && bot_navlearn->value == 0)
		return prev_node;

	// only place nodes on solid ground (or while swimming) and while alive
	if (ent->deadflag)
		return prev_node;
	if (!ent->groundentity && ent->waterlevel < 2)
		return prev_node;	// airborne; wait until we land

	// bot_hazard: never learn nodes/links while in LAVA.  The engine sets
	// waterlevel for ANY liquid, so a bot surviving a burn for a few frames
	// used to write the pool into the graph as routable "water" -- the
	// q2dm3/q2dm6 mass-suicide mechanism (q2dm3's shipped graph: 78 "water"
	// nodes = 64 lava + 14 slime + 0 actual water).  Return -1, not
	// prev_node: keeping the chain alive would stitch a rim->rim WALK link
	// across the pit the moment the bot claws out the far side.  SLIME stays
	// learnable (flagged, priced 4x by A*) -- q2dm7's channels are
	// legitimate, survivable crossings the graph must keep.
	if (bot_hazard->value != 0 && ent->waterlevel >= 1
		&& (ent->watertype & CONTENTS_LAVA))
		return -1;

	if (ent->waterlevel >= 2)
		flags |= NAV_FLAG_WATER;
	if (bot_hazard->value != 0 && ent->waterlevel >= 1
		&& (ent->watertype & CONTENTS_SLIME))
		flags |= NAV_FLAG_SLIME;

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
Nav_SeedTrainLinks

bot_train: surface each func_train's ride as NAV_LINK_TRAIN links so A* can
route a bot across the gap the train bridges.  Unlike a func_plat -- which
rests flush at the floor, so bots walk onto it and the ride is LEARNED then
retagged (Nav_TagPlatLinks) -- a func_train shuttles horizontally with its
brush elevated / across a gap, so bots can never board it organically and no
link is ever learned.  We seed it from geometry instead.

The QUAKED spec: a path_corner's origin is the train's MIN point at that
corner, so the standable top-center of the train parked at that corner is
corner + (size.x/2, size.y/2, size.z).  Seed a node there (Nav_SeedNode wires
it to the adjacent static ground -- the boarding ledge at one end, the far
platform at the other) and link consecutive corners two-way (the shuttle
carries either direction).  Never saved (Nav_Save filters TRAIN), regenerated
each load.  Gated on bot_train so the capability-off graph is untouched.
=================
*/
void Nav_SeedTrainLinks (void)
{
	edict_t	*tr;
	int		seeded = 0;

	if (!bot_train || bot_train->value == 0)
		return;

	for (tr = NULL; (tr = G_Find (tr, FOFS(classname), "func_train")) != NULL; )
	{
		vec3_t	size;
		edict_t	*c, *first;
		int		guard, prev_node = -1, first_node = -1;
		float	deck_off = 0;

		if (!tr->target)
			continue;

		// bot_train handles HORIZONTAL shuttles (a moving bridge across a gap).
		// A func_train that travels mostly VERTICALLY is an elevator -- boarding
		// and riding one is awkward for this controller, and there is normally a
		// ladder/plat alternative (q2dm2's quad-pool lift, which bots got stuck
		// milling under), so skip it and let bots take those instead.
		{
			edict_t	*c1, *c2;
			c1 = G_PickTarget (tr->target);
			c2 = c1 ? G_PickTarget (c1->target) : NULL;
			if (c1 && c2)
			{
				vec3_t	cd;
				VectorSubtract (c2->s.origin, c1->s.origin, cd);
				if (fabs (cd[2]) > sqrt (cd[0]*cd[0] + cd[1]*cd[1]))
					continue;	// vertical elevator, not a shuttle
			}
		}

		VectorSubtract (tr->maxs, tr->mins, size);

		// The train's standable deck is NOT its bbox top: the brush can carry a
		// wall/pillar that makes size.z far taller than the surface a bot walks
		// on (q2dm2's RG/BFG train is 178 tall over a ~44-high deck).  Trace down
		// onto the spawn bbox to find the real deck, as an offset above the min
		// point (rigid, so it holds at every corner).  Add a stand offset so the
		// seeded node sits at bot-origin height, not on the surface.
		{
			// sample a grid across the bbox and take the LOWEST surface hit: the
			// walkable deck sits low, any wall/pillar rides above it, so the min
			// avoids being fooled by a central pillar (q2dm2's RG/BFG train).
			static const float fx[5] = { 0.0f, -0.35f, 0.35f, -0.35f, 0.35f };
			static const float fy[5] = { 0.0f, -0.35f, -0.35f, 0.35f, 0.35f };
			float	cx = (tr->absmin[0] + tr->absmax[0]) * 0.5f;
			float	cy = (tr->absmin[1] + tr->absmax[1]) * 0.5f;
			float	best = 1e9f;
			int		s;
			for (s = 0; s < 5; s++)
			{
				vec3_t	ts, te;
				trace_t	dn;
				ts[0] = te[0] = cx + fx[s] * size[0];
				ts[1] = te[1] = cy + fy[s] * size[1];
				ts[2] = tr->absmax[2] + 16;
				te[2] = tr->absmin[2] - 16;
				dn = gi.trace (ts, vec3_origin, vec3_origin, te, NULL, MASK_SOLID);
				if (dn.fraction < 1.0f && dn.endpos[2] < best)
					best = dn.endpos[2];
			}
			if (best < 1e8f)
				deck_off = best - tr->absmin[2] + 24.0f;
			else
				deck_off = size[2];
		}

		first = G_PickTarget (tr->target);
		for (c = first, guard = 0; c && guard < 16; guard++)
		{
			vec3_t	top;
			int		node;

			top[0] = c->s.origin[0] + size[0] * 0.5f;
			top[1] = c->s.origin[1] + size[1] * 0.5f;
			top[2] = c->s.origin[2] + deck_off;
			node = Nav_SeedNode (top);
			if (node >= 0)
			{
				int	m;

				// The brush-top endpoint floats in mid-air whenever the train
				// has shuttled to its OTHER corner, so Nav_SeedNode's clear-walk
				// connector (a floor trace) can't wire this end to the adjacent
				// ground.  Force a short walk link to nearby existing nodes: the
				// train is the floor when parked here, and these are the
				// step-on/step-off ledges (board side at the near corner, far
				// platform -- the gated items -- at the far corner).
				for (m = 0; m < nav.num_nodes; m++)
				{
					vec3_t	dd;
					if (m == node)
						continue;
					VectorSubtract (nav.nodes[m].origin, top, dd);
					if (dd[2] > 56 || dd[2] < -56)
						continue;
					if (dd[0]*dd[0] + dd[1]*dd[1] > 120.0f*120.0f)
						continue;
					Nav_AddLink (node, m, NAV_LINK_WALK);
					Nav_AddLink (m, node, NAV_LINK_WALK);
				}

				if (first_node < 0)
					first_node = node;
				if (prev_node >= 0 && prev_node != node)
				{
					Nav_AddLinkType (prev_node, node, NAV_LINK_TRAIN);
					Nav_AddLinkType (node, prev_node, NAV_LINK_TRAIN);
					seeded++;
				}
				prev_node = node;
			}

			if (!c->target)
				break;
			c = G_PickTarget (c->target);
			if (c == first)			// path loops closed
			{
				if (prev_node >= 0 && first_node >= 0 && prev_node != first_node)
				{
					Nav_AddLinkType (prev_node, first_node, NAV_LINK_TRAIN);
					Nav_AddLinkType (first_node, prev_node, NAV_LINK_TRAIN);
					seeded++;
				}
				break;
			}
		}
	}

	if (seeded)
		gi.dprintf ("ozbot: seeded %d func_train link(s)\n", seeded);
}

/*
=================
Nav_SeedTeleportLinks

bot_teleport: surface each misc_teleporter as a one-way NAV_LINK_TELEPORT so A*
can route a bot through it.  A teleporter is AUTOMATIC -- the bot just walks onto
the pad and teleporter_touch relocates it -- so unlike lifts/trains there is no
ride controller, only the routing edge.  The learner never captures it: stepping
on the pad jumps the bot >200u to the destination, and Nav_LearnStep's
discontinuity guard correctly refuses to link that (a teleported "traversal" is
not a walkable edge).  Seed from geometry instead -- source = the misc_teleporter
pad origin, destination = its targeted misc_teleporter_dest (teleporter_touch
drops the player at dest origin +10z).  Never saved (Nav_Save strips TELEPORT),
regenerated each load.  Gated on bot_teleport so the capability-off graph is
untouched.

NOTE: this seeds BOTH teleporter forms.  Point-entity misc_teleporter (+
misc_teleporter_dest) is the id/base disc bots stand ON (source = pad origin).
Brush trigger_teleport is the volume most custom DM maps use -- it now has a
working spawn function (SP_trigger_teleport, g_misc.c, reusing teleporter_touch),
so bots walk THROUGH it (source = the volume's floor center) and it resolves its
destination the same way teleporter_touch does (target -> targetname match).
=================
*/
static int Nav_SeedOneTeleport (edict_t *tp, vec3_t src)
{
	edict_t	*dest;
	vec3_t	dst;
	int		snode, dnode;

	if (!tp->target)
		return 0;
	dest = G_Find (NULL, FOFS(targetname), tp->target);
	if (!dest)
		return 0;	// dangling teleporter (no destination); leave unrouted

	VectorCopy (dest->s.origin, dst);
	dst[2] += 10.0f;		// matches teleporter_touch's arrival offset

	snode = Nav_SeedNode (src);
	dnode = Nav_SeedNode (dst);
	if (snode >= 0 && dnode >= 0 && snode != dnode)
	{
		Nav_AddLinkType (snode, dnode, NAV_LINK_TELEPORT);	// one-way
		return 1;
	}
	return 0;
}

void Nav_SeedTeleportLinks (void)
{
	edict_t	*tp;
	int		seeded = 0;

	if (!bot_teleport || bot_teleport->value == 0)
		return;

	// misc_teleporter: a point-entity disc; bots stand ON it, so the pad origin
	// is the source.  Nav_SeedNode reuses the node bots learned walking onto it.
	for (tp = NULL; (tp = G_Find (tp, FOFS(classname), "misc_teleporter")) != NULL; )
	{
		vec3_t	src;
		VectorCopy (tp->s.origin, src);
		seeded += Nav_SeedOneTeleport (tp, src);
	}

	// trigger_teleport: a BRUSH volume most custom DM maps use.  Bots walk
	// THROUGH it, so the source is the volume's horizontal center dropped to its
	// floor (absmin z); Nav_SeedNode wires it to the walked-through ground nodes.
	for (tp = NULL; (tp = G_Find (tp, FOFS(classname), "trigger_teleport")) != NULL; )
	{
		vec3_t	src;
		src[0] = (tp->absmin[0] + tp->absmax[0]) * 0.5f;
		src[1] = (tp->absmin[1] + tp->absmax[1]) * 0.5f;
		src[2] = tp->absmin[2] + 24.0f;		// stand height above the trigger floor
		seeded += Nav_SeedOneTeleport (tp, src);
	}

	if (seeded)
		gi.dprintf ("ozbot: seeded %d teleporter link(s)\n", seeded);
}

/*
=================
Nav_SeedPushLinks

bot_jumppad: surface each trigger_push (jump pad / wind tunnel) as a one-way
NAV_LINK_PUSH edge so A* can deliberately route a bot through it.  Like a
teleporter the push is AUTOMATIC -- trigger_push_touch overwrites the toucher's
velocity with movedir*speed*10 -- so there is no ride controller, only the
routing edge; and like a teleporter the learner never captures it (the pad flings
the bot far enough that Nav_LearnStep's discontinuity guard rejects the "walk").

The destination is not an entity we can look up (unlike a teleporter's dest), so
we PREDICT it by integrating the ballistic arc the launch imparts: start at the
pad floor, apply the exact launch velocity, step the parabola under sv_gravity,
and trace the player box between successive points until it lands on a walkable
floor.  Source = the trigger brush floor center (same as the brush teleport);
landing = that first floor impact.  Both are snapped via Nav_SeedNode (creating +
wiring a node if the spot is uncovered).  One-way (a pad launches, it does not
pull back).  Never saved (Nav_Save strips PUSH), regenerated each load.  Gated on
bot_jumppad so the capability-off graph is byte-identical.

LIMITATION: trigger_push_touch OVERWRITES velocity (it does not add to the bot's
run momentum), so a purely vertical pad launches straight up and the arc lands
back on the pad -- source and landing snap to the same node and no link is seeded
(correctly: a vertical pad to a ledge directly overhead is not modeled here).
Angled pads (a horizontal movedir component) route fine.  The airborne leg also
has no controller, so a mid-arc snag on geometry the trace missed is possible --
watch for it in play.
=================
*/
static int Nav_SeedOnePush (edict_t *tp, vec3_t src)
{
	vec3_t	vel, pos, next, landing;
	float	grav, dt;
	int		i, maxsteps, snode, dnode;
	qboolean found = false;

	// launch velocity EXACTLY as trigger_push_touch applies it on touch
	VectorScale (tp->movedir, tp->speed * 10.0f, vel);
	if (vel[0] == 0.0f && vel[1] == 0.0f && vel[2] == 0.0f)
		return 0;	// no movedir/angle set -> the pad imparts no push (inert); nothing to route

	grav = sv_gravity ? sv_gravity->value : 800.0f;
	dt = 0.05f;
	maxsteps = 200;		// <=10s of flight -- generous; a real pad arc is 1-3s

	VectorCopy (src, pos);
	pos[2] += 1.0f;		// lift a hair off the pad floor so step 0 isn't startsolid

	for (i = 0; i < maxsteps; i++)
	{
		trace_t	tr;

		next[0] = pos[0] + vel[0] * dt;
		next[1] = pos[1] + vel[1] * dt;
		next[2] = pos[2] + vel[2] * dt;

		tr = gi.trace (pos, (float *)pl_mins, (float *)pl_maxs, next, tp, MASK_PLAYERSOLID);

		if (tr.startsolid || tr.allsolid)
		{
			// began inside solid (e.g. still clipping the pad floor on the first
			// ascending steps): don't treat as a landing, just advance freely.
			VectorCopy (next, pos);
			vel[2] -= grav * dt;
			continue;
		}

		if (tr.fraction < 1.0f)
		{
			if (tr.plane.normal[2] > 0.7f && vel[2] <= 0.0f)
			{
				// descending onto a walkable floor -> this is the landing spot
				VectorCopy (tr.endpos, landing);
				found = true;
				break;
			}
			// hit a wall or ceiling: slide along it (kill the velocity component
			// into the surface, like pmove would) and keep integrating so an arc
			// that grazes a wall on the way up still finds its floor.
			{
				float	d = DotProduct (vel, tr.plane.normal);
				VectorMA (vel, -d, tr.plane.normal, vel);
				VectorCopy (tr.endpos, pos);
				VectorMA (pos, 0.5f, tr.plane.normal, pos);	// nudge off the surface
			}
		}
		else
		{
			VectorCopy (next, pos);
		}

		vel[2] -= grav * dt;
	}

	if (!found)
		return 0;	// never resolved a clean landing (arc left the world / capped out)

	snode = Nav_SeedNode (src);
	dnode = Nav_SeedNode (landing);
	if (snode >= 0 && dnode >= 0 && snode != dnode)
	{
		Nav_AddLinkType (snode, dnode, NAV_LINK_PUSH);	// one-way
		return 1;
	}
	return 0;
}

void Nav_SeedPushLinks (void)
{
	edict_t	*tp;
	int		found = 0, seeded = 0;

	if (!bot_jumppad || bot_jumppad->value == 0)
		return;

	// trigger_push is always a BRUSH volume: bots walk THROUGH it and touch fires
	// the launch, so the source is the volume's horizontal center dropped to its
	// floor (the same derivation as the brush trigger_teleport source).
	for (tp = NULL; (tp = G_Find (tp, FOFS(classname), "trigger_push")) != NULL; )
	{
		vec3_t	src;
		found++;
		src[0] = (tp->absmin[0] + tp->absmax[0]) * 0.5f;
		src[1] = (tp->absmin[1] + tp->absmax[1]) * 0.5f;
		src[2] = tp->absmin[2] + 24.0f;		// stand height above the trigger floor
		seeded += Nav_SeedOnePush (tp, src);
	}

	if (found)
		gi.dprintf ("ozbot: seeded %d push link(s) from %d jump pad(s)\n", seeded, found);
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
	float	near_dist;

	// Put a node *exactly* at the item so a routed path ends on the pickup
	// ("arrived" == "touched").  Reuse an existing node only if it sits right on
	// the item AND has a clear final hop to it -- otherwise the bot reaches the
	// node but a wall blocks the last few units to the item (the dominant
	// touch-failure we measured).
	//
	// Coincident reuse (dist <= 8) skips CanWalk: a zero-length player-box
	// trace at an item pedestal / recorded stand point often returns startsolid
	// even when a human just stood there.  Forcing a duplicate orphaned the
	// q2dm3 Megahealth playbook exit (PLAYBOOK landed on a new node with no
	// edge to the real item node → MH stayed no_path).
	nearest = Nav_NearestNode (origin);
	near_dist = (nearest >= 0) ? Nav_Dist (nav.nodes[nearest].origin, origin) : 1e18f;
	if (nearest >= 0 && near_dist <= 48.0f
		&& (near_dist <= 8.0f
			|| Nav_CanWalk (nav.nodes[nearest].origin, origin, NULL)))
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
			if (to >= 0 && to < nav.num_nodes && nav_indegree[to] > 0)
				nav_indegree[to]--;
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
Nav_NearestGoalNode

Nearest node that can serve as an A* *target*: one some link leads INTO
(in-degree > 0).  Exact-at-item seeded nodes that never connected otherwise
shadow real coverage nearby: Nav_NearestNode returns the orphan, A* to it
fails, and Goal_Select wrongly rejects the item as unreachable -- the Phase C
reach sweep measured ~20 elevated q2dm1 items lost this way
(plans/completed/nav-oracle.md, Discovery 1).  Falls back to the plain
nearest node when no linked node exists (or with bot_goalnode off: legacy
behavior, orphan and all).
=================
*/
int Nav_NearestGoalNode (vec3_t origin)
{
	int		i, best = -1;
	float	bestd = 1e18f;

	if (bot_goalnode->value == 0)
		return Nav_NearestNode (origin);

	for (i = 0; i < nav.num_nodes; i++)
	{
		vec3_t	d;
		float	dsq;
		if (nav_indegree[i] <= 0)
			continue;	// nothing routes INTO it: useless as a goal
		VectorSubtract (origin, nav.nodes[i].origin, d);
		d[2] *= 2.0f;	// penalize vertical separation (as Nav_NearestNode)
		dsq = d[0]*d[0] + d[1]*d[1] + d[2]*d[2];
		if (dsq < bestd)
		{
			bestd = dsq;
			best = i;
		}
	}
	if (best < 0)
		return Nav_NearestNode (origin);
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
Nav_FindPath / Nav_FindPathMasked

A* from start to goal over node centers.  Writes the node sequence (including
both endpoints) into out[] and returns its length, or 0 if unreachable.

The masked form only expands links whose type bit is set in 'mask' (see
NAV_MASK in bot_nav.h), so pathing can honor a bot's capability config --
e.g. exclude PLAT links for a lift-off bot -- without touching the graph.
=================
*/
static float	nav_path_cost;	// g-cost of the last successful Nav_FindPath

float Nav_LastPathCost (void)
{
	return nav_path_cost;
}

int Nav_FindPath (int start, int goal, int *out, int max)
{
	return Nav_FindPathMasked (start, goal, NAV_MASK_ALL, out, max);
}

int Nav_FindPathMasked (int start, int goal, int mask, int *out, int max)
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
			if (!(mask & NAV_MASK (n->links[i].type)))
				continue;	// link type outside the caller's capabilities
			// the learner records swim traversals as WALK links between
			// water-flagged NODES (NAV_LINK_WATER is never stamped on links),
			// so the water capability masks by destination node: a swimless
			// bot must not be routed through deep water
			if (!(mask & NAV_MASK (NAV_LINK_WATER)) && (nav.nodes[nb].flags & NAV_FLAG_WATER))
				continue;
			// bot_hazard: never route INTO a lava/hurt node (same
			// destination-node pattern as the water mask).  Such nodes exist
			// only in graphs learned before the learner-refusal; flagged at
			// load by Nav_FlagHazardNodes.  Links OUT of a hazard node stay
			// expandable, so a bot knocked into a pool can still route out.
			if (bot_hazard->value != 0 && (nav.nodes[nb].flags & NAV_FLAG_HAZARD))
				continue;
			if (closed[nb])
				continue;
			tentative = gscore[cur] + n->links[i].cost;
			// bot_hazard: slime is a survivable wade, not a wall -- price a
			// hop INTO a slime node 4x so A* detours when a dry route exists
			// and pays the toll when none does (q2dm7's channel crossings)
			if (bot_hazard->value != 0 && (nav.nodes[nb].flags & NAV_FLAG_SLIME))
				tentative += n->links[i].cost * 3.0f;
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

/*
=================
Nav_QueryPath

The reachability oracle (plans/nav-oracle.md Phase C): classify whether 'to'
is routable from 'from' under a capability mask, without committing a bot or
a budget to finding out.  Read-only -- no graph mutation, no RNG.

GATED means the full-capability graph routes it but 'mask' doesn't; *gate
names the minimal unlocking link types (each missing type tried singly; if
only a combination unlocks it, *gate is every missing type).  Node-proximity
failures use the same 192u radius Goal_Select requires of item coverage.
=================
*/
#define NAVQ_NEAR	192.0f

int Nav_QueryPath (vec3_t from, vec3_t to, int mask, float *cost, int *gate)
{
	static int	buf[NAV_MAX_NODES];
	int			start, goal, missing, t;

	if (cost) *cost = 0;
	if (gate) *gate = 0;

	start = Nav_NearestNode (from);
	if (start < 0 || Nav_Dist (nav.nodes[start].origin, from) > NAVQ_NEAR)
		return NAVQ_NO_START_NODE;
	goal = Nav_NearestGoalNode (to);	// same resolution Goal_Select uses,
										// so verdicts predict bot behavior
	if (goal < 0 || Nav_Dist (nav.nodes[goal].origin, to) > NAVQ_NEAR)
		return NAVQ_NO_GOAL_NODE;

	if (Nav_FindPathMasked (start, goal, mask, buf, NAV_MAX_NODES) > 0)
	{
		if (cost) *cost = nav_path_cost;
		return NAVQ_OK;
	}
	if (mask == NAV_MASK_ALL
		|| Nav_FindPathMasked (start, goal, NAV_MASK_ALL, buf, NAV_MAX_NODES) <= 0)
		return NAVQ_NO_PATH;

	if (cost)
		*cost = nav_path_cost;	// what the full-capability route costs
	missing = NAV_MASK_ALL & ~mask;
	if (gate)
	{
		for (t = 0; t <= NAV_LINK_PLAT; t++)
			if ((missing & NAV_MASK (t))
				&& Nav_FindPathMasked (start, goal, mask | NAV_MASK (t), buf, NAV_MAX_NODES) > 0)
				*gate |= NAV_MASK (t);
		// TRAIN (bot_train) is a capability gate like PLAT/WATER but sits above
		// PLAYBOOK in the enum, so test it explicitly (the loop stops at PLAT to
		// skip the non-capability PLAYBOOK type)
		if ((missing & NAV_MASK (NAV_LINK_TRAIN))
			&& Nav_FindPathMasked (start, goal, mask | NAV_MASK (NAV_LINK_TRAIN), buf, NAV_MAX_NODES) > 0)
			*gate |= NAV_MASK (NAV_LINK_TRAIN);
		if (!*gate)
			*gate = missing;	// only a combination of capabilities unlocks it
	}
	return NAVQ_GATED;
}

const char *Nav_QueryName (int code)
{
	switch (code)
	{
	case NAVQ_OK:				return "ok";
	case NAVQ_GATED:			return "gated";
	case NAVQ_NO_PATH:			return "no_path";
	case NAVQ_NO_GOAL_NODE:		return "no_goal_node";
	case NAVQ_NO_START_NODE:	return "no_start_node";
	}
	return "unknown";
}

/*
=================
Nav_MaskNames

Link-type mask -> "water+plat" style string for logs/console.
=================
*/
void Nav_MaskNames (int mask, char *out, int outsize)
{
	static const char *names[] = { "walk", "fall", "jump", "teleport", "water", "plat" };
	int		t;

	out[0] = 0;
	for (t = 0; t <= NAV_LINK_PLAT; t++)
	{
		if (!(mask & NAV_MASK (t)))
			continue;
		if ((int)(strlen (out) + strlen (names[t]) + 2) > outsize)
			break;	// callers pass >=40 bytes; every full mask fits
		if (out[0])
			strcat (out, "+");
		strcat (out, names[t]);
	}
	if (!out[0])
		Com_sprintf (out, outsize, "none");
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
	if (magic != NAV_MAGIC || (version != 1 && version != 2)
		|| count < 0 || count > NAV_MAX_NODES)
	{
		gi.dprintf ("ozbot: nav file %s is incompatible; ignoring\n", path);
		fclose (f);
		return false;
	}

	// v2 carries a header flags word (FROZEN etc.) right after the count; v1
	// has none.  The node records that follow are byte-identical either way.
	nav_flags = 0;
	if (version >= 2)
		fread (&nav_flags, sizeof(nav_flags), 1, f);
	nav_frozen = (nav_flags & NAVHDR_FROZEN) != 0;

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

	// rebuild the in-degree cache from the loaded links
	memset (nav_indegree, 0, sizeof(nav_indegree));
	for (i = 0; i < count; i++)
	{
		int j;
		for (j = 0; j < nav.nodes[i].num_links; j++)
		{
			int to = nav.nodes[i].links[j].to;
			if (to >= 0 && to < count)
				nav_indegree[to]++;
		}
	}

	gi.dprintf ("ozbot: loaded nav %s (%d nodes%s)\n", path, count,
		nav_frozen ? ", frozen" : "");
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

	// bot_navlearn / FROZEN: never persist a frozen or learn-disabled graph
	// (belt-and-suspenders -- the learner already can't dirty it).
	if (nav_frozen || (bot_navlearn && bot_navlearn->value == 0))
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
	fwrite (&nav_flags, sizeof(nav_flags), 1, f);	// v2 header flags word
	for (i = 0; i < nav.num_nodes; i++)
	{
		nav_node_t *n = &nav.nodes[i];
		int			j, keep = 0;

		// PLAYBOOK, TRAIN and TELEPORT links are derived at map load (playbook
		// file / func_train + misc_teleporter geometry), not learned -- saving
		// them would leave dangling maneuvers if the map/playbook changes (and
		// breaks the capability-off byte-identical graph)
		for (j = 0; j < n->num_links; j++)
			if (!NAV_LINK_DERIVED (n->links[j].type))
				keep++;

		fwrite (n->origin, sizeof(n->origin), 1, f);
		fwrite (&n->flags, sizeof(n->flags), 1, f);
		fwrite (&keep, sizeof(keep), 1, f);
		for (j = 0; j < n->num_links; j++)
			if (!NAV_LINK_DERIVED (n->links[j].type))
				fwrite (&n->links[j], sizeof(nav_link_t), 1, f);
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
Nav_HasWalkLink

True if node 'from' holds a WALK link to 'to' -- the bidirectionality test the
fluke validator uses (legit walkable terrain is learned both ways).
=================
*/
static qboolean Nav_HasWalkLink (int from, int to)
{
	nav_node_t	*n;
	int			i;

	if (from < 0 || from >= nav.num_nodes)
		return false;
	n = &nav.nodes[from];
	for (i = 0; i < n->num_links; i++)
		if (n->links[i].to == to && n->links[i].type == NAV_LINK_WALK)
			return true;
	return false;
}

/*
=================
Nav_ValidateLinks

Load-time graph hygiene (bot_navvalidate): the graph is learned from ANY
successful traversal, so a lucky fall or a combat shove down a ledge can be
recorded as a WALK link that A* then sells as a route the bot cannot re-walk.
The reliable fluke signature is GEOMETRY, not a walkability trace -- a straight
box-trace from the high node to the low node falls through OPEN AIR (fraction
1.0) and keeps the fluke, so Nav_CanWalk is the wrong test here.  Instead drop
a link iff it is a WALK link with a large drop (|dz| > NAV_FLUKE_DZ) that is
NOT bidirectional: genuine walkable terrain (ramps, stairs, floors) is learned
in BOTH directions (Nav_LearnStep links WALK moves reversibly), while an
irreversible steep "walk" only ever happened once, downhill, by accident.
FALL / JUMP / WATER / PLAT / PLAYBOOK / TELEPORT links are never touched -- they
encode real one-way capabilities.

Runs from Nav_Init right after Nav_Load, where the CURRENT map's collision is
loaded -- NOT from Nav_Save (Nav_Shutdown of the OLD map fires after the NEW
map's world is loaded, so tracing there would hit the wrong collision; the
geometry test here needs no trace anyway).  Sets nav_dirty so the cleaned graph
persists on the next autosave.  Reuses Nav_PenalizeLink's compaction + indegree
bookkeeping verbatim.
=================
*/
#define NAV_FLUKE_DZ	64.0f	// a steeper one-way "walk" is a fall/shove fluke

void Nav_ValidateLinks (void)
{
	int	i, dropped = 0;

	for (i = 0; i < nav.num_nodes; i++)
	{
		nav_node_t	*n = &nav.nodes[i];
		int			j = 0;

		while (j < n->num_links)
		{
			nav_link_t	*l = &n->links[j];
			int			to = l->to;
			int			k;
			float		dz;

			if (l->type != NAV_LINK_WALK || to < 0 || to >= nav.num_nodes)
			{
				j++;
				continue;
			}
			dz = nav.nodes[to].origin[2] - n->origin[2];
			if (dz < 0)
				dz = -dz;
			if (dz <= NAV_FLUKE_DZ || Nav_HasWalkLink (to, i))
			{
				j++;
				continue;	// gentle slope, or a real bidirectional walk -- keep
			}
			// never nuke a lift column: a steep one-way walk whose endpoints sit
			// inside one real func_plat footprint is a plat ride the learner
			// stored as WALK before Nav_TagPlatLinks retags it (and with bot_lift
			// off it stays WALK).  Same entity test the retagger uses.
			{
				edict_t *p = Bot_FindPlatAt (n->origin);
				if (p && Bot_FindPlatAt (nav.nodes[to].origin) == p)
				{
					j++;
					continue;
				}
			}

			gi.dprintf ("ozbot: navvalidate drop %d->%d (%.0f %.0f %.0f)->(%.0f %.0f %.0f) dz=%.0f\n",
				i, to, n->origin[0], n->origin[1], n->origin[2],
				nav.nodes[to].origin[0], nav.nodes[to].origin[1],
				nav.nodes[to].origin[2], dz);

			// drop link j (Nav_PenalizeLink removal mechanics: compact the array,
			// keep link_fails aligned, decrement the destination's in-degree)
			for (k = j + 1; k < n->num_links; k++)
			{
				n->links[k - 1] = n->links[k];
				link_fails[i][k - 1] = link_fails[i][k];
			}
			n->num_links--;
			link_fails[i][n->num_links] = 0;
			if (nav_indegree[to] > 0)
				nav_indegree[to]--;
			dropped++;
			nav_dirty = true;
			// leave j where it is: the compacted link now occupies index j
		}
	}

	if (dropped)
		gi.dprintf ("ozbot: navvalidate dropped %d steep one-way walk link(s)\n", dropped);
}

/*
=================
Nav_FlagHazardNodes

bot_hazard load-time sweep: cache the map's active trigger_hurt volumes, then
stamp NAV_FLAG_HAZARD (lava/hurt volume: A* forbids) or NAV_FLAG_SLIME
(survivable wade: A* prices 4x) on every node, clearing stale flags so a
graph re-judged after a rules change heals.
Runs from the per-map setup (needs spawned entities), AFTER Goal_SeedNavNodes
so seeded item nodes get judged too, BEFORE Goal_ReachSweep so the oracle's
load verdicts reflect the exclusion the bots will actually route under.

Flags, not removals: the graph data survives intact (turning the cvar off
restores every route bit-exactly), and A*'s destination-node test does the
work -- the same mechanism the WATER capability mask already uses.
=================
*/
void Nav_FlagHazardNodes (void)
{
	int		i, flagged = 0, cleared = 0;
	edict_t	*e;

	num_hurt_volumes = 0;
	if (bot_hazard->value == 0)
		return;

	// active hurt volumes only: a START_OFF trigger_hurt spawns SOLID_NOT and
	// most kill-pits are always-on.  dmg <= 0 would be a healing brush.
	for (e = NULL; (e = G_Find (e, FOFS(classname), "trigger_hurt")) != NULL; )
	{
		if (e->solid != SOLID_TRIGGER || e->dmg <= 0)
			continue;
		if (num_hurt_volumes >= NAV_MAX_HURT_VOLUMES)
			break;
		VectorCopy (e->absmin, hurt_mins[num_hurt_volumes]);
		VectorCopy (e->absmax, hurt_maxs[num_hurt_volumes]);
		num_hurt_volumes++;
	}

	for (i = 0; i < nav.num_nodes; i++)
	{
		byte	is = Nav_PointHazardFlags (nav.nodes[i].origin);
		byte	was = nav.nodes[i].flags & (NAV_FLAG_HAZARD | NAV_FLAG_SLIME);

		if (is == was)
			continue;
		nav.nodes[i].flags = (nav.nodes[i].flags & ~(NAV_FLAG_HAZARD | NAV_FLAG_SLIME)) | is;
		if (is)
			flagged++;
		else
			cleared++;
		nav_dirty = true;
	}

	if (flagged || cleared || num_hurt_volumes)
		gi.dprintf ("ozbot: hazard sweep: %d node(s) flagged, %d cleared, %d hurt volume(s)\n",
			flagged, cleared, num_hurt_volumes);
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
	memset (nav_indegree, 0, sizeof(nav_indegree));
	nav_dirty = false;
	nav_frozen = false;		// Nav_Load sets it from the ONAV header if present
	nav_flags = 0;
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
