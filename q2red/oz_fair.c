/*
bot_fair.c -- bot_fair: ozbot-re under a learning bot's senses (a sparring mode)

bot_fair 1 takes away what a human (or a fair learned bot) could not know:
  - items: an item's state comes from what this bot saw or heard, not from the
    server.  A pickup it saw (line of sight in its view cone) or heard (within
    FAIR_HEAR units) tells it when the item is back (players know the respawn
    times); an empty spot it sees with no pickup heard says "gone, back some
    time" (FAIR_GONE_GUESS seconds).  Otherwise it believes an item is there.
  - the enemy is seen bot_fairdelay seconds late (default 0.15 s, the learned
    bot's perception delay): aim and lead read the enemy's past origin and
    velocity, not the present.
  - enemy health: the sight-legal belief (bot_enemymodel's path), not the truth.
Off (0, the default) every path is the shipped one.
*/
#include "../src/g_local.h"
#include "../src/bot.h"

cvar_t	*bot_fair;
cvar_t	*bot_fairdelay;

#define FAIR_HIST		32			// samples per client (0.8 s at 40 Hz)
#define FAIR_HEAR		1000.0f		// a pickup sound (ATTN_NORM) carries about this far
#define FAIR_GONE_GUESS	10.0f		// seen empty, pickup unheard: believed gone this long
#define FAIR_LOOK_EVERY	0.2f		// how often a bot looks over the item spots it can see
#define FAIR_FOV_COS	0.5f		// a 120-degree view cone
#define FAIR_TELEPORT	200.0f		// a jump this big between samples is a teleport or respawn

typedef struct
{
	float	t;
	vec3_t	origin, velocity;
	float	viewheight;
} fair_sample_t;

static fair_sample_t	hist[MAX_CLIENTS + 1][FAIR_HIST];
static int				hist_n[MAX_CLIENTS + 1], hist_head[MAX_CLIENTS + 1];

static float	gone_until[MAX_CLIENTS + 1][MAX_EDICTS];	// believed taken until (0 = believed there)
static byte		prev_avail[MAX_EDICTS];
static float	next_look[MAX_CLIENTS + 1];
static float	last_frame = -1;

void Fair_Init (void)
{
	bot_fair      = gi.cvar ("bot_fair", "0", 0);
	bot_fairdelay = gi.cvar ("bot_fairdelay", "0.15", 0);
}

qboolean Fair_On (void)
{
	return bot_fair && bot_fair->value != 0;
}

static void Fair_Reset (void)
{
	memset (hist_n, 0, sizeof (hist_n));
	memset (hist_head, 0, sizeof (hist_head));
	memset (gone_until, 0, sizeof (gone_until));
	memset (prev_avail, 0, sizeof (prev_avail));
	memset (next_look, 0, sizeof (next_look));
}

static int BotSlot (bot_t *b)
{
	int n = (int)(b->ent - g_edicts);
	return (n >= 1 && n <= MAX_CLIENTS) ? n : 0;
}

// can this bot see the item's spot: in its view cone, nothing solid between
static qboolean Fair_SeesSpot (edict_t *self, vec3_t spot)
{
	vec3_t	eyes, d, fwd;
	trace_t	tr;
	float	len;

	VectorCopy (self->s.origin, eyes);
	eyes[2] += self->viewheight;
	VectorSubtract (spot, eyes, d);
	len = VectorNormalize (d);
	if (len > 1.0f)
	{
		AngleVectors (self->client->v_angle, fwd, NULL, NULL);
		if (DotProduct (fwd, d) < FAIR_FOV_COS)
			return false;
	}
	tr = gi.trace (eyes, vec3_origin, vec3_origin, spot, self, MASK_OPAQUE);
	return tr.fraction >= 1.0f;
}

static qboolean Fair_IsBot (edict_t *e)
{
	return e && e->inuse && e->client && Bot_ForEdict (e) != NULL;
}

static float Fair_RespawnAt (edict_t *it)
{
	if (it->think == DoRespawn && it->nextthink > level.time)
		return it->nextthink;		// the period a player knows, from the moment seen
	return level.time + 30.0f;
}

/*
=================
Fair_Frame

Once a server frame (Bot_RunFrame): sample every client's state for the
perception delay, and watch the item spots for pickups each bot saw or heard.
=================
*/
void Fair_Frame (void)
{
	int		i, c;

	if (!Fair_On ())
		return;
	if (level.time < last_frame)
		Fair_Reset ();				// a new map
	if (level.time == last_frame)
		return;
	last_frame = level.time;

	for (c = 1; c <= (int)game.maxclients && c <= MAX_CLIENTS; c++)
	{
		edict_t			*e = g_edicts + c;
		fair_sample_t	*s;

		if (!e->inuse || !e->client)
		{
			hist_n[c] = 0;
			continue;
		}
		hist_head[c] = (hist_head[c] + 1) % FAIR_HIST;
		s = &hist[c][hist_head[c]];
		s->t = level.time;
		VectorCopy (e->s.origin, s->origin);
		VectorCopy (e->velocity, s->velocity);
		s->viewheight = e->viewheight;
		if (hist_n[c] < FAIR_HIST)
			hist_n[c]++;
	}

	for (i = (int)game.maxclients + 1; i < globals.num_edicts && i < MAX_EDICTS; i++)
	{
		edict_t		*it = g_edicts + i;
		qboolean	avail;
		vec3_t		spot;

		if (!it->inuse || !it->item)
		{
			prev_avail[i] = 0;
			continue;
		}
		avail = Goal_ItemAvailable (it);
		VectorCopy (it->s.origin, spot);
		spot[2] += 16;
		for (c = 1; c <= (int)game.maxclients && c <= MAX_CLIENTS; c++)
		{
			edict_t	*self = g_edicts + c;
			vec3_t	d;

			if (!Fair_IsBot (self))
				continue;
			if (prev_avail[i] && !avail)
			{
				// just taken: heard (near) or seen, the bot knows when it's back
				VectorSubtract (it->s.origin, self->s.origin, d);
				if (VectorLength (d) < FAIR_HEAR || Fair_SeesSpot (self, spot))
					gone_until[c][i] = Fair_RespawnAt (it);
			}
			else if (level.time >= next_look[c])
			{
				// a look over the spots in view: back, or empty
				if (avail ? gone_until[c][i] > level.time : gone_until[c][i] <= level.time)
					if (Fair_SeesSpot (self, spot))
						gone_until[c][i] = avail ? 0 : level.time + FAIR_GONE_GUESS;
			}
		}
		prev_avail[i] = avail;
	}
	for (c = 1; c <= (int)game.maxclients && c <= MAX_CLIENTS; c++)
		if (level.time >= next_look[c])
			next_look[c] = level.time + FAIR_LOOK_EVERY;
}

/*
=================
Goal_Believed

Whether this bot believes the item is there to pick up: the truth with
bot_fair off, else what it saw and heard.
=================
*/
qboolean Goal_Believed (bot_t *b, edict_t *it)
{
	int	c, i;

	if (!Fair_On ())
		return Goal_ItemAvailable (it);
	if (!it || !it->inuse || !it->item)
		return false;
	c = BotSlot (b);
	i = (int)(it - g_edicts);
	if (!c || i < 0 || i >= MAX_EDICTS)
		return Goal_ItemAvailable (it);
	return level.time >= gone_until[c][i];
}

// seconds until this bot believes the item is back (0: believed there now)
float Fair_RespawnEta (bot_t *b, edict_t *it)
{
	int	c = BotSlot (b), i = (int)(it - g_edicts);

	if (!c || i < 0 || i >= MAX_EDICTS || level.time >= gone_until[c][i])
		return 0;
	return gone_until[c][i] - level.time;
}

/*
=================
Fair_Enemy

The enemy as this bot perceives it: its origin, velocity and viewheight
bot_fairdelay seconds ago (interpolated, not across a teleport).  False when
bot_fair is off or there is no history (callers use the present).
=================
*/
qboolean Fair_Enemy (edict_t *enemy, vec3_t origin, vec3_t velocity, float *viewheight)
{
	int				c, k, n;
	float			want;
	fair_sample_t	*a = NULL, *b2 = NULL;

	if (!Fair_On () || !enemy)
		return false;
	c = (int)(enemy - g_edicts);
	if (c < 1 || c > MAX_CLIENTS || hist_n[c] == 0)
		return false;
	want = level.time - bot_fairdelay->value;
	n = hist_n[c];
	// newest to oldest: b2 the newest sample at or after want, a the first before it
	for (k = 0; k < n; k++)
	{
		fair_sample_t *s = &hist[c][(hist_head[c] - k + FAIR_HIST) % FAIR_HIST];
		if (s->t >= want)
			b2 = s;
		else
		{
			a = s;
			break;
		}
	}
	if (!a)
		a = b2;						// history shorter than the delay: the oldest
	if (!b2 || b2 == a)
	{
		VectorCopy (a->origin, origin);
		VectorCopy (a->velocity, velocity);
		*viewheight = a->viewheight;
		return true;
	}
	{
		vec3_t	d;
		float	f = (b2->t > a->t) ? (want - a->t) / (b2->t - a->t) : 0;

		VectorSubtract (b2->origin, a->origin, d);
		if (VectorLength (d) > FAIR_TELEPORT)
			f = (f < 0.5f) ? 0 : 1;
		VectorMA (a->origin, f, d, origin);
		if (f < 0.5f)
			VectorCopy (a->velocity, velocity);
		else
			VectorCopy (b2->velocity, velocity);
		*viewheight = f < 0.5f ? a->viewheight : b2->viewheight;
	}
	return true;
}
