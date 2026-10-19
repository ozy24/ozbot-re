/*
oz_arena.c -- arena play (Rocket Arena and the like) for ozbot on q2red.

An arena map has no items: every player is handed a full loadout when a round
starts, so the item economy that drives deathmatch has nothing to offer, and
the roam that fills in for it picks any node on the map -- through the
lobby's teleporters into an arena where the bot has no business.  On a map
with no items (and no CTF flags) this layer:

  - hunts: the bot's objective is the nearest live enemy in its own arena,
    routed and re-routed as it moves (oz_ctf.c's CTF_HUNT leg, which already
    chases a moving carrier);
  - keeps a roam inside the arena: an arena is the part of the graph joined
    by walking, jumping and falling links, without teleports (the mod moves
    players between arenas itself).

Observers and the lobby are the mod's business: it publishes them as
spectators, which Bot_Think and the target filters skip.  Nothing here is
compiled into the q2repro DLL.
*/

#include "../src/g_local.h"
#include "../src/bot.h"
#include "../src/bot_nav.h"

static qboolean	arena_active;
static int		arena_label[NAV_MAX_NODES];	// arena (component) of each node
static int		arena_labelled_nodes = -1;	// nav.num_nodes when labelled
static int		arena_client_node[MAX_CLIENTS + 1];	// by edict number, -1 none

void Arena_MapBegin (void)
{
	arena_active = false;
	arena_labelled_nodes = -1;
}

static int Arena_Find (int n)
{
	while (arena_label[n] != n)
		n = arena_label[n] = arena_label[arena_label[n]];
	return n;
}

/* Label each node with its arena: union over every link but teleports. */
static void Arena_Label (void)
{
	int	i, k;

	for (i = 0; i < nav.num_nodes; i++)
		arena_label[i] = i;
	for (i = 0; i < nav.num_nodes; i++)
		for (k = 0; k < nav.nodes[i].num_links; k++)
		{
			nav_link_t	*l = &nav.nodes[i].links[k];
			int			a, c;
			if (l->type == NAV_LINK_TELEPORT || l->to < 0 || l->to >= nav.num_nodes)
				continue;
			a = Arena_Find (i);
			c = Arena_Find (l->to);
			if (a != c)
				arena_label[a] = c;
		}
	for (i = 0; i < nav.num_nodes; i++)
		arena_label[i] = Arena_Find (i);
	arena_labelled_nodes = nav.num_nodes;
}

static int Arena_Of (int node)
{
	return (node >= 0 && node < arena_labelled_nodes) ? arena_label[node] : -1;
}

/*
=================
Arena_Frame

Once per frame, after the world is mirrored: is this an arena map, and which
node does each player stand at.
=================
*/
void Arena_Frame (void)
{
	int	i;

	arena_active = false;
	if (Ctf_Active () || nav.num_nodes < 2)
		return;
	for (i = (int)game.maxclients + 1; i < globals.num_edicts; i++)
		if (g_edicts[i].inuse && g_edicts[i].item)
			return;		// items: the deathmatch economy applies
	arena_active = true;

	if (arena_labelled_nodes != nav.num_nodes)
		Arena_Label ();
	for (i = 1; i <= (int)game.maxclients && i <= MAX_CLIENTS; i++)
	{
		edict_t	*e = g_edicts + i;
		arena_client_node[i] = (e->inuse && e->client && !e->client->resp.spectator)
			? Nav_NearestNode (e->s.origin) : -1;
	}
}

qboolean Arena_Active (void)
{
	return arena_active;
}

/*
=================
Arena_Target

The enemy to hunt: the nearest live, playing opponent standing in the bot's
own arena.  NULL if there is none (between rounds, or alone).
=================
*/
edict_t *Arena_Target (bot_t *b)
{
	edict_t	*self = b->ent, *best = NULL;
	int		i, mine, self_num;
	float	bestd = 0;
	vec3_t	d;

	if (!arena_active || !self || !self->client)
		return NULL;
	self_num = (int)(self - g_edicts);
	if (self_num < 1 || self_num > MAX_CLIENTS || (mine = Arena_Of (arena_client_node[self_num])) < 0)
		return NULL;
	for (i = 1; i <= (int)game.maxclients && i <= MAX_CLIENTS; i++)
	{
		edict_t	*o = g_edicts + i;
		float	dd;

		if (o == self || !o->inuse || !o->client || o->deadflag || o->health <= 0
			|| o->client->resp.spectator || OzHost_IsTeammate (self, o)
			|| Arena_Of (arena_client_node[i]) != mine)
			continue;
		VectorSubtract (o->s.origin, self->s.origin, d);
		dd = VectorLength (d);
		if (!best || dd < bestd)
		{
			best = o;
			bestd = dd;
		}
	}
	return best;
}

/*
=================
Arena_RoamNode

A random node in the same arena as start (-1 if start is not on the graph).
=================
*/
int Arena_RoamNode (int start)
{
	int	mine = Arena_Of (start), idx, tries;

	if (mine < 0)
		return -1;
	for (tries = 0; tries < 64; tries++)
	{
		idx = (int)(random () * nav.num_nodes);
		if (idx >= 0 && idx < nav.num_nodes && idx != start && arena_label[idx] == mine
			&& !(nav.nodes[idx].flags & (NAV_FLAG_HAZARD | NAV_FLAG_SLIME)))
			return idx;
	}
	return -1;
}
