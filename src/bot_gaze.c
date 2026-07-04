/*
ozbot - self-learning q2dm1 bot

bot_gaze.c -- humanization: out-of-combat gaze and view-turn dynamics
(plans/humanization.md Phases 1+).

Out of combat the stock bot bolts its view to its travel direction (yaw =
move_yaw, pitch = 0) and applies any new facing in a single 0.1s tick.  The
Phase-0 humanness profiler (tools/humanness.py, 1299 q2dm1 demos) measured
those as the loudest tells: human pitch sits at ~+8 deg (down) and moves
constantly, ~46% of human moving time is spent looking >30 deg away from the
travel direction, and human yaw motion is autocorrelated (lag-1 0.57 vs our
-0.02) with tails near 600 deg/s -- while the stock bot mixes dead-still view
with 1600 deg/s snaps.

Two independently-gated behaviors:
  bot_gaze     -- WHAT to look at: lead the path a node ahead, glance at
                  items/openings/shoulders, pitch follows the look point plus
                  a slow wander.
  bot_turnrate -- HOW the view moves: every facing change goes through an
                  exponential-approach slew (gain x remaining delta, capped),
                  which reproduces the human autocorrelation + rate envelope.

Movement is decoupled from facing (Bot_ApplyMovement projects the world-space
move intent onto whatever the facing is), so none of this changes where the
bot goes -- only where it looks.  Combat aim is untouched (bot_combat.c owns
the facing while engaged).
*/

#include "g_local.h"
#include "bot.h"
#include "bot_nav.h"

cvar_t	*bot_gaze;
cvar_t	*bot_turnrate;
cvar_t	*bot_humantest;	// head-to-head: even bot ids run the humanization
						// behaviors, odd ids run stock -- the parity harness
						// for measuring what humanness costs in kills

// numbers below are fitted to the Phase-0 profiler quantiles (see
// demos/derived/humanness/report_q2dm1.json):
//   human yaw rate while moving: p50 39, p90 243, p95 340, p99 606 deg/s
//   human pitch: p25 0.5, p50 8.3, p75 16.5 (positive = down)
#define GAZE_TURN_GAIN		0.38f	// fraction of remaining yaw delta per tick
#define GAZE_CAP_MIN		22.0f	// per-turn speed draw (deg/tick): humans
#define GAZE_CAP_SPAN		38.0f	// turn at many speeds, so a fixed cap
									// piles the rate distribution at one value
#define GAZE_DEADBAND		2.0f	// hold the view when this close (real
									// zero-rate frames; human p25 is ~1 deg/s)
#define GAZE_PITCH_GAIN		0.30f
#define GAZE_PITCH_CAP		22.0f
#define GAZE_PITCH_BIAS		4.0f	// resting look-down on top of geometry
#define GAZE_PITCH_MAX		50.0f
#define GAZE_WANDER_SIGMA	0.7f	// slow pitch texture (OU step / decay)
#define GAZE_WANDER_DECAY	0.08f
#define GAZE_WANDER_MAX		8.0f

/*
=================
Bot_Humanized

The humanization cvars apply to this bot.  With bot_humantest set, only even
bot ids run the behaviors (odd ids play stock), so kills-by-parity measures
the strength cost of humanness inside one match -- the bot_skilltest pattern.
=================
*/
qboolean Bot_Humanized (bot_t *b)
{
	if (bot_humantest->value != 0)
		return (b->id & 1) == 0;
	return true;
}

/*
=================
Bot_SlewAngle

Exponential approach with a rate cap: take 'gain' of the remaining delta each
tick, never more than 'cap' degrees.  The decaying steps are what gives the
view its human autocorrelation signature; the cap sets the flick ceiling.
=================
*/
float Bot_SlewAngle (float cur, float target, float gain, float cap)
{
	float	d = target - cur;

	while (d > 180)  d -= 360;
	while (d < -180) d += 360;

	if (d > -GAZE_DEADBAND && d < GAZE_DEADBAND)
		return cur;				// close enough: hold genuinely still

	// 40Hz adaptation: gain and cap arrive in 10Hz-tick units from every
	// caller (the profiler constants were fitted at 10Hz); convert here so
	// deg/sec behavior is tick-rate invariant
	d *= Bot_TickGain (gain);
	cap *= BOT_TICK_RATIO;
	if (d > cap)  d = cap;
	if (d < -cap) d = -cap;
	return cur + d;
}

/*
=================
Gaze_RedrawTurnSpeed

Each deliberate look (glance start, big look-target change) gets its own turn
speed, drawn low-skewed from the human envelope -- 220..600 deg/s with most
turns lazy and a few flicks.
=================
*/
static void Gaze_RedrawTurnSpeed (bot_t *b)
{
	float r = random ();
	b->gaze_cap = GAZE_CAP_MIN + r * r * GAZE_CAP_SPAN;
}

/*
=================
Gaze_PitchToward

Q2 pitch toward a point (positive = down), from eye height.
=================
*/
static float Gaze_PitchToward (edict_t *ent, vec3_t point, float hdist)
{
	float	dz = point[2] - (ent->s.origin[2] + ent->viewheight);

	if (hdist < 8)
		return 0;
	return (float)(-atan2 (dz, hdist) * 180.0 / M_PI);
}

/*
=================
Gaze_StartGlance

Pick a new glance target: a side look, a shoulder check, the goal item, or a
vertical scan.  Frequencies/durations sized so the time-looking-away duty
cycle lands near the human ~46% (together with corner leading and slew lag).
=================
*/
static void Gaze_StartGlance (bot_t *b)
{
	float	r = random ();
	float	dur;

	b->glance_pitch = 0;

	if (r < 0.45f)
	{
		// side glance off the travel direction
		float side = 20.0f + random () * 55.0f;
		b->glance_yaw = b->move_yaw + (random () < 0.5f ? side : -side);
		b->glance_pitch = crandom () * 6.0f;
		dur = 0.3f + random () * 0.5f;
	}
	else if (r < 0.60f)
	{
		// shoulder check
		b->glance_yaw = b->move_yaw + 180.0f + crandom () * 40.0f;
		dur = 0.3f + random () * 0.3f;
	}
	else if (r < 0.85f && b->goal_item)
	{
		// look at where we're headed (the item we intend to take)
		vec3_t	d;
		VectorSubtract (b->goal_item->s.origin, b->ent->s.origin, d);
		d[2] = 0;
		if (VectorLength (d) > 64)
		{
			b->glance_yaw = vectoyaw (d);
			b->glance_pitch = Gaze_PitchToward (b->ent, b->goal_item->s.origin,
				VectorLength (d));
			dur = 0.4f + random () * 0.4f;
		}
		else
		{
			b->glance_yaw = b->move_yaw + crandom () * 120.0f;
			dur = 0.3f + random () * 0.4f;
		}
	}
	else
	{
		// vertical scan (ledges above, drops below)
		b->glance_yaw = b->move_yaw + crandom () * 30.0f;
		b->glance_pitch = -20.0f + random () * 35.0f;
		dur = 0.3f + random () * 0.4f;
	}

	b->glance_until = level.time + dur;
	b->next_glance_time = b->glance_until + 2.0f + random () * 4.0f;
	Gaze_RedrawTurnSpeed (b);
	if (b->gaze_cap > 40.0f)
		b->gaze_cap = 40.0f;	// glances are casual looks, not flicks
}

/*
=================
Bot_GazeThink

Out-of-combat facing for this frame.  With bot_gaze: look ahead down the path
(leading corners), pitch to the look point + slow wander, interleaved with
glances.  With bot_turnrate: slew from the current view toward the desired
facing instead of snapping.  Either can run without the other.
=================
*/
void Bot_GazeThink (bot_t *b, float *facing_yaw, float *facing_pitch)
{
	edict_t		*ent = b->ent;
	float		want_yaw = b->move_yaw;
	float		want_pitch = 0;
	qboolean	human = Bot_Humanized (b);

	if (bot_gaze->value != 0 && human)
	{
		vec3_t		look;
		qboolean	have_look = false;

		// look-ahead point: partway toward the node PAST the next waypoint,
		// so the view leads upcoming corners the way a player's does (full
		// lead overshoots the human view-vs-travel offset; blend it)
		if (b->path_len > 0 && b->path_idx < b->path_len)
		{
			int node = b->path[b->path_idx];
			int ahead = (b->path_idx + 1 < b->path_len) ? b->path[b->path_idx + 1] : node;
			if (node >= 0 && node < nav.num_nodes
				&& ahead >= 0 && ahead < nav.num_nodes)
			{
				VectorAdd (nav.nodes[node].origin, nav.nodes[ahead].origin, look);
				VectorScale (look, 0.5f, look);
				have_look = true;
				if (ahead != b->gaze_last_node)
				{
					// a fresh corner came into play: this look is a new
					// deliberate turn, give it its own speed
					b->gaze_last_node = ahead;
					Gaze_RedrawTurnSpeed (b);
				}
			}
		}
		if (!have_look)
		{
			// no path (wander/wait): project the travel direction out ahead
			vec3_t	a = {0, 0, 0}, f, rt, up;
			a[YAW] = b->move_yaw;
			AngleVectors (a, f, rt, up);
			VectorMA (ent->s.origin, 192, f, look);
		}

		{
			vec3_t	d;
			float	hdist;
			VectorSubtract (look, ent->s.origin, d);
			d[2] = 0;
			hdist = VectorLength (d);
			if (hdist > 8)
				want_yaw = vectoyaw (d);
			want_pitch = Gaze_PitchToward (ent, look, hdist) + GAZE_PITCH_BIAS;
		}

		// slow pitch texture: an intermittent OU wander -- humans hold their
		// pitch still much of the time, then drift it (p25 of human pitch
		// rate is ~1 deg/s), so don't step the noise every tick.  Trigger
		// probability is per-tick: scale it to keep steps/sec constant.
		if (random () < 0.25f * BOT_TICK_RATIO)
		{
			b->gaze_pitch_wander += crandom () * GAZE_WANDER_SIGMA
				- b->gaze_pitch_wander * GAZE_WANDER_DECAY;
			if (b->gaze_pitch_wander > GAZE_WANDER_MAX)
				b->gaze_pitch_wander = GAZE_WANDER_MAX;
			if (b->gaze_pitch_wander < -GAZE_WANDER_MAX)
				b->gaze_pitch_wander = -GAZE_WANDER_MAX;
		}
		want_pitch += b->gaze_pitch_wander;

		// glances override the look-ahead
		if (level.time < b->glance_until)
		{
			want_yaw = b->glance_yaw;
			want_pitch = b->glance_pitch + b->gaze_pitch_wander;
			b->gaze_hold_yaw = want_yaw;
			b->gaze_hold_pitch = want_pitch;
		}
		else
		{
			if (level.time >= b->next_glance_time)
				Gaze_StartGlance (b);

			// fixation: hold the current look target until the fresh one has
			// drifted meaningfully.  Without this the look point sweeps a few
			// degrees every tick as the bot runs, and the view never rests --
			// a constant ~30 deg/s hum humans don't have (they fixate, then
			// catch up in one motion).
			{
				float dy = want_yaw - b->gaze_hold_yaw;
				while (dy > 180)  dy -= 360;
				while (dy < -180) dy += 360;
				if (dy > 8.0f || dy < -8.0f)
					b->gaze_hold_yaw = want_yaw;
				if (fabs (want_pitch - b->gaze_hold_pitch) > 4.0f)
					b->gaze_hold_pitch = want_pitch;
			}
			want_yaw = b->gaze_hold_yaw;
			want_pitch = b->gaze_hold_pitch;
		}

		if (want_pitch > GAZE_PITCH_MAX)  want_pitch = GAZE_PITCH_MAX;
		if (want_pitch < -GAZE_PITCH_MAX) want_pitch = -GAZE_PITCH_MAX;
	}

	if (bot_turnrate->value != 0 && human)
	{
		if (b->gaze_cap < GAZE_CAP_MIN)
			Gaze_RedrawTurnSpeed (b);	// fresh bot: no speed drawn yet
		*facing_yaw = Bot_SlewAngle (ent->client->ps.viewangles[YAW],
			want_yaw, GAZE_TURN_GAIN, b->gaze_cap);
		*facing_pitch = Bot_SlewAngle (ent->client->ps.viewangles[PITCH],
			want_pitch, GAZE_PITCH_GAIN, GAZE_PITCH_CAP);
	}
	else
	{
		*facing_yaw = want_yaw;
		*facing_pitch = want_pitch;
	}
}
