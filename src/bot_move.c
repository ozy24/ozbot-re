/*
ozbot - self-learning q2dm1 bot

bot_move.c -- steering.

Steering sets a world-space movement *intent* (b->move_dir, move_yaw,
want_jump) rather than writing the usercmd directly.  Bot_ApplyMovement later
projects that intent onto whatever facing the bot ends up using (its travel
direction, or its aim during combat), so the bot can move toward a goal while
looking/shooting somewhere else.
*/

#include "g_local.h"
#include "bot.h"
#include "bot_nav.h"

cvar_t	*bot_fidget;	// humanization: locomotion texture -- idle fidget at
						// deliberate holds, fast turn-away instead of wall
						// dithering, travel hops (plans/humanization.md Ph 5)

/*
=================
Bot_Swimming

True when the bot should steer in 3D: submerged, or in water without footing
(surface-bobbing under a ledge -- keeping the vertical intent there lets
Pmove's water-jump fire to climb out).
=================
*/
static qboolean Bot_Swimming (edict_t *ent)
{
	if (bot_swim->value == 0)
		return false;
	return ent->waterlevel >= 2
		|| (ent->waterlevel >= 1 && !ent->groundentity);
}

/*
=================
Bot_SetMoveToward / Bot_SetMoveYaw
=================
*/
static void Bot_SetMoveToward (bot_t *b, vec3_t target)
{
	vec3_t	d, h;
	VectorSubtract (target, b->ent->s.origin, d);

	// riding a lift: the waypoint is straight up (a learned plat column).
	// Keep the 3D intent -- the horizontal projection goes ~zero, so the bot
	// stands still on the plat and lets it do the climbing, instead of the
	// flattened+normalized intent turning a few units of offset into a
	// full-speed push that walks it off the platform.
	if (bot_lift->value != 0 && !Bot_Swimming (b->ent) && d[2] > 48)
	{
		VectorCopy (d, h);
		h[2] = 0;
		if (VectorLength (h) < 72)
		{
			VectorNormalize (d);
			VectorCopy (d, b->move_dir);
			if (VectorLength (h) > 1)
				b->move_yaw = vectoyaw (h);
			return;
		}
	}

	if (Bot_Swimming (b->ent))
	{
		// keep the vertical component: in water the intent is genuinely 3D
		// (Bot_ApplyMovement turns move_dir[2] into swim upmove)
		if (VectorLength (d) < 1)
		{
			VectorClear (b->move_dir);
			return;
		}
		VectorCopy (d, h);
		h[2] = 0;
		VectorNormalize (d);
		VectorCopy (d, b->move_dir);
		if (VectorLength (h) > 1)
			b->move_yaw = vectoyaw (h);
		return;
	}

	d[2] = 0;
	if (VectorLength (d) < 1)
	{
		VectorClear (b->move_dir);
		return;
	}
	VectorNormalize (d);
	VectorCopy (d, b->move_dir);
	b->move_yaw = vectoyaw (d);
}

void Bot_SetMoveYaw (bot_t *b, float yaw)
{
	vec3_t	a = {0, 0, 0}, f, r, u;
	a[YAW] = yaw;
	AngleVectors (a, f, r, u);
	f[2] = 0;
	VectorNormalize (f);
	VectorCopy (f, b->move_dir);
	b->move_yaw = yaw;
}

/*
=================
Bot_SteerToPoint
=================
*/
void Bot_SteerToPoint (bot_t *b, vec3_t target)
{
	Bot_SetMoveToward (b, target);
}

/*
=================
Bot_Wander

Explore-mode steering: random heading on a timer, re-pick sooner when stuck,
occasional hop to discover jump links.
=================
*/
void Bot_Wander (bot_t *b)
{
	edict_t	*ent = b->ent;
	float	speed = VectorLength (ent->velocity);
	float	use_yaw;
	qboolean texture = (bot_fidget->value != 0 && Bot_Humanized (b));

	if (speed < 20 && ent->groundentity
		&& (level.time - b->last_repick_time) > (texture ? 0.15 : 0.4))
	{
		// blocked (wall / ledge stop).  Humanization: re-pick fast and TURN
		// AWAY (60-180 deg off the blocked heading) instead of waiting 0.4s
		// on a fresh uniform draw that faces the same wall half the time --
		// the stock dithering is ~46% of all bot standing-still frames.
		if (texture)
			b->desired_yaw = b->move_yaw
				+ (random () < 0.5f ? 1.0f : -1.0f) * (60.0f + random () * 120.0f);
		else
			b->desired_yaw = crandom() * 180.0;
		b->next_wander_time = level.time + 1.5 + random() * 2.0;
		b->last_repick_time = level.time;
	}
	else if (level.time >= b->next_wander_time)
	{
		b->desired_yaw = crandom() * 180.0;
		b->next_wander_time = level.time + 1.5 + random() * 2.0;
		b->last_repick_time = level.time;
	}

	// (read AFTER the re-pick above -- a fresh heading must apply this frame,
	// not next; hoisting this cost a one-frame lag that broke stock parity)
	use_yaw = b->desired_yaw;

	// humanization (bot_turnrate): approach a re-picked wander heading
	// smoothly instead of snapping the whole body 90-180 deg in one tick --
	// the heading is random anyway, so the arc through intermediate headings
	// explores just as well, and the view (which tracks the travel direction)
	// stops whipping.  Keep the snap when crawling/stuck: boxed-in geometry
	// genuinely demands abrupt turns.
	if (bot_turnrate->value != 0 && Bot_Humanized (b) && speed >= 20)
		use_yaw = Bot_SlewAngle (b->move_yaw, b->desired_yaw, 0.35f, 8.0f);

	Bot_SetMoveYaw (b, use_yaw);

	if (ent->groundentity && random() < 0.02f * BOT_TICK_RATIO)
	{
		b->want_jump = true;
		b->did_jump = true;
	}
}

/*
=================
Bot_RolloutRecover

Short-horizon forward search: instead of guessing a recovery heading, roll a
handful of candidate input sequences forward through the REAL movement code
(gi.Pmove, the same function the engine uses to resolve every client's usercmd)
and commit to whichever one makes the most progress toward the current path
waypoint.  This replaces hand-authored recovery primitives (blind unstick
weave, a single-ledge hop) with a search that works for whatever the local
geometry actually is -- stairs, a ledge, a lift lip, a tight corner -- because
it's asking the physics itself "which of these gets me closer", not pattern-
matching a shape.

Only the FIRST tick of the winning candidate is ever applied (via move_dir /
want_jump, same as any other steering call); next frame, if still stuck, we
re-simulate fresh from wherever the bot actually ended up.  This makes it a
receding-horizon controller rather than a committed multi-step plan, so a bad
prediction (the world is only approximated by Pmove -- no other entities move
during the rollout) self-corrects within a tick instead of compounding.
=================
*/
#define ROLLOUT_TICKS	5		// candidate rows: one per authored 10Hz step
// each row is simulated for FRAMEDIV engine ticks of FRAMETIME each, so the
// candidates keep their authored 0.5s horizon and 100ms input shape at any
// tick rate (exact no-op at 10Hz)

typedef struct
{
	short	forward, side, up;
} rollout_step_t;

// each row is one tick; candidates cover the recovery shapes a stuck bot
// actually needs: push straight, jump now vs after a run-up (gap clears need
// speed before leaving the ground), or step diagonally around an edge.
static const rollout_step_t rollout_candidates[][ROLLOUT_TICKS] =
{
	{ {400,0,0},   {400,0,0},   {400,0,0},   {400,0,0},   {400,0,0}   },	// straight
	{ {400,0,350}, {400,0,0},   {400,0,0},   {400,0,0},   {400,0,0}   },	// jump now
	{ {400,0,0},   {400,0,0},   {400,0,350}, {400,0,0},   {400,0,0}   },	// run-up then jump
	{ {300,-300,0},{300,-300,0},{300,-300,0},{300,-300,0},{300,-300,0}},	// strafe left
	{ {300,300,0}, {300,300,0}, {300,300,0}, {300,300,0}, {300,300,0} },	// strafe right
	{ {350,-250,350},{350,-250,0},{350,0,0}, {350,0,0},   {350,0,0}   },	// diagonal-left + jump
	{ {350,250,350},{350,250,0}, {350,0,0},  {350,0,0},   {350,0,0}   },	// diagonal-right + jump
};
#define NUM_ROLLOUT_CANDIDATES (sizeof(rollout_candidates) / sizeof(rollout_candidates[0]))

// Simulates one candidate from the bot's real current state without touching
// any real entity/client state -- gi.Pmove only reads/writes the pmove_t we
// pass it (trace/pointcontents are read-only world queries), so this is safe
// to call repeatedly and discard.
static void Bot_SimulateCandidate (edict_t *ent, float yaw, const rollout_step_t *steps, vec3_t out_origin)
{
	pmove_t	pm;
	int		i;

	memset (&pm, 0, sizeof(pm));
	pm.s.pm_type = ent->deadflag ? PM_DEAD : PM_NORMAL;
	pm.s.gravity = (short)sv_gravity->value;
	for (i = 0; i < 3; i++)
	{
		pm.s.origin[i]   = (short)(ent->s.origin[i] * 8);
		pm.s.velocity[i] = (short)(ent->velocity[i] * 8);
	}
	pm.trace = PM_trace;
	pm.pointcontents = gi.pointcontents;
	pm_passent = ent;

	for (i = 0; i < ROLLOUT_TICKS; i++)
	{
		int	j;

		for (j = 0; j < FRAMEDIV; j++)
		{
			memset (&pm.cmd, 0, sizeof(pm.cmd));
			pm.cmd.msec        = (byte)(FRAMETIME * 1000);
			pm.cmd.forwardmove = steps[i].forward;
			pm.cmd.sidemove    = steps[i].side;
			// a jump command only needs the first engine tick of its row
			pm.cmd.upmove      = (j == 0) ? steps[i].up : (short)((steps[i].up > 0) ? 0 : steps[i].up);
			pm.cmd.angles[YAW]   = (short)(ANGLE2SHORT(yaw) - pm.s.delta_angles[YAW]);
			pm.cmd.angles[PITCH] = (short)(0 - pm.s.delta_angles[PITCH]);
			pm.snapinitial = (i == 0 && j == 0);
			gi.Pmove (&pm);
		}
	}

	for (i = 0; i < 3; i++)
		out_origin[i] = pm.s.origin[i] * 0.125f;
}

void Bot_RolloutRecover (bot_t *b)
{
	edict_t	*ent = b->ent;
	vec3_t	target, toward, end;
	float	yaw, start_hdist, start_vdist, best_score;
	int		i, best;

	if (b->path_idx >= b->path_len)
	{
		Bot_Unstick (b);
		return;
	}
	VectorCopy (nav.nodes[b->path[b->path_idx]].origin, target);

	VectorSubtract (target, ent->s.origin, toward);
	start_vdist = (float)fabs (toward[2]);
	toward[2] = 0;
	start_hdist = VectorLength (toward);
	if (start_hdist < 1 && start_vdist < 1)
	{
		Bot_Unstick (b);
		return;
	}
	yaw = vectoyaw (toward);

	// Score on horizontal + vertical progress separately rather than raw 3D
	// distance: node "arrival" (Bot_FollowPath) is a horizontal-only check, and
	// a big vertical gap (e.g. a lift shaft) can't close in one ~0.5s rollout
	// regardless of candidate, which would otherwise swamp the comparison and
	// make it noise between candidates that actually differ a lot horizontally.
	best = -1;
	best_score = -1e18f;
	for (i = 0; i < (int)NUM_ROLLOUT_CANDIDATES; i++)
	{
		vec3_t	d;
		float	hdist, vdist, score;

		Bot_SimulateCandidate (ent, yaw, rollout_candidates[i], end);
		VectorSubtract (target, end, d);
		vdist = (float)fabs (d[2]);
		d[2] = 0;
		hdist = VectorLength (d);
		score = (start_hdist - hdist) + 0.5f * (start_vdist - vdist);
		if (score > best_score)
		{
			best_score = score;
			best = i;
		}
	}

	if (best < 0 || best_score <= 0)
	{
		// nothing tried made progress -- fall back to the old blind unstick
		Bot_Unstick (b);
		return;
	}

	Bot_SetMoveYaw (b, yaw);
	{
		const rollout_step_t *step = &rollout_candidates[best][0];
		vec3_t	a = {0, 0, 0}, f, r, u;
		a[YAW] = yaw;
		AngleVectors (a, f, r, u);
		f[2] = 0; r[2] = 0;
		VectorNormalize (f);
		VectorNormalize (r);
		VectorScale (f, step->forward, f);
		VectorScale (r, step->side, r);
		VectorAdd (f, r, b->move_dir);
		if (VectorLength (b->move_dir) > 0.1f)
			VectorNormalize (b->move_dir);
		if (step->up > 0)
		{
			b->want_jump = true;
			b->did_jump = true;
		}
	}
}

/*
=================
Bot_Unstick

Pick a fresh random heading (and occasionally hop) to dislodge from geometry.
=================
*/
void Bot_Unstick (bot_t *b)
{
	edict_t	*ent = b->ent;

	if (level.time >= b->next_wander_time)
	{
		b->desired_yaw = crandom() * 180.0;
		b->next_wander_time = level.time + 0.4;
	}

	Bot_SetMoveYaw (b, b->desired_yaw);

	if (ent->groundentity && random() < 0.15)
	{
		b->want_jump = true;
		b->did_jump = true;
	}
}

/*
=================
Bot_LinkType
=================
*/
int Bot_LinkType (int from, int to)
{
	nav_node_t	*n;
	int			i;

	if (from < 0 || from >= nav.num_nodes)
		return NAV_LINK_WALK;
	n = &nav.nodes[from];
	for (i = 0; i < n->num_links; i++)
		if (n->links[i].to == to)
			return n->links[i].type;
	return NAV_LINK_WALK;
}

//==========================================================================
// lift riding (bot_lift) -- see plans/lift-riding.md
//
// Boarding a func_plat is physically trivial (it rests at the bottom flush
// with the floor; walking on triggers the ride), but it takes seconds of
// deliberate waiting, which every other subsystem punishes as being stuck.
// This controller owns the movement intent while a plat hop is in play:
// WAIT clear of the footprint until the plat is down (standing inside the
// footprint would hold the plat up top forever via the shaft-high touch
// trigger -- measured in Phase 0), BOARD to the plat center, RIDE holding
// the middle, then hand the path back past the learned column.
//==========================================================================

// func_plat move states -- mirrors the file-scope defines in g_func.c
#define PLAT_STATE_TOP		0
#define PLAT_STATE_BOTTOM	1
#define PLAT_STATE_UP		2
#define PLAT_STATE_DOWN		3

#define LIFT_ENGAGE_RANGE	160.0f	// engage when this close to the column base
#define LIFT_RELEASE_RANGE	280.0f	// disengage hysteresis (combat dodging
									// must not flap the controller on and off)
#define LIFT_WAIT_TIMEOUT	10.0f	// covers one full plat cycle (~9s on q2dm1)
#define LIFT_RIDE_TIMEOUT	15.0f	// safety only; a ride is ~3s
#define LIFT_BOARD_STALL	2.5f	// boarding is a short walk; longer without
									// moving means geometry blocks this approach
#define LIFT_MARGIN			24.0f	// footprint clearance while waiting

static qboolean Lift_IsPlat (edict_t *e)
{
	return (e && e->inuse && e->classname
		&& strcmp (e->classname, "func_plat") == 0);
}

/*
=================
Bot_FindPlatAt

The func_plat whose horizontal footprint contains pos (a plat only travels
vertically, so its absmin/absmax x,y are position-independent).
=================
*/
edict_t *Bot_FindPlatAt (vec3_t pos)
{
	int		i;

	for (i = (int)game.maxclients + 1; i < globals.num_edicts; i++)
	{
		edict_t *e = g_edicts + i;
		if (!Lift_IsPlat (e))
			continue;
		if (pos[0] >= e->absmin[0] - 16 && pos[0] <= e->absmax[0] + 16 &&
			pos[1] >= e->absmin[1] - 16 && pos[1] <= e->absmax[1] + 16)
			return e;
	}
	return NULL;
}

/*
=================
Bot_UpcomingHop

Index into b->path of the FROM node of the next hop of link type 'type' in
play: the hop being traversed now, or the upcoming one once the bot is
within 'engage' range of its base ('release' > 'engage' gives an already-
engaged controller disengage hysteresis, so e.g. combat dodging can't flap
it on and off).  This is the generic query behind typed-link traversal
controllers -- the lift today, any future capability keyed to a link type
(plans/nav-oracle.md Phase B).  -1 if none.
=================
*/
int Bot_UpcomingHop (bot_t *b, int type, float engage, float release, qboolean engaged)
{
	if (b->path_len <= 0)
		return -1;

	if (b->path_idx >= 1 && b->path_idx < b->path_len
		&& Bot_LinkType (b->path[b->path_idx - 1], b->path[b->path_idx]) == type)
		return b->path_idx - 1;

	if (b->path_idx >= 0 && b->path_idx + 1 < b->path_len
		&& Bot_LinkType (b->path[b->path_idx], b->path[b->path_idx + 1]) == type)
	{
		vec3_t	d;
		float	range = engaged ? release : engage;
		VectorSubtract (nav.nodes[b->path[b->path_idx]].origin, b->ent->s.origin, d);
		d[2] = 0;
		if (VectorLength (d) < range)
			return b->path_idx;
	}

	return -1;
}

/*
=================
Bot_LiftReset
=================
*/
void Bot_LiftReset (bot_t *b)
{
	b->lift_state = LIFT_NONE;
	b->lift_plat = NULL;
	b->lift_deadline = 0;
}

/*
=================
Bot_LiftThink

Returns true while the lift controller owns this frame's movement intent.
The caller (Bot_Navigate) then skips path following, stuck recovery, and
freezes the goal-budget clock, so waiting cannot be punished.  LIFT_FAILED
(state timeout) sticks for the rest of the goal attempt: the controller
stands aside and the normal giveup machinery resolves the goal.
=================
*/
qboolean Bot_LiftThink (bot_t *b)
{
	edict_t	*ent = b->ent;
	edict_t	*plat;
	vec3_t	c, d;
	int		hop;

	if (b->lift_state == LIFT_FAILED)
		return false;

	hop = Bot_UpcomingHop (b, NAV_LINK_PLAT, LIFT_ENGAGE_RANGE, LIFT_RELEASE_RANGE,
		b->lift_state != LIFT_NONE);
	if (hop < 0)
	{
		if (b->lift_state != LIFT_NONE)
			Bot_LiftReset (b);		// path no longer has a lift hop in play
		return false;
	}

	// resolve the plat entity for this column (cached per attempt)
	if (!b->lift_plat)
	{
		b->lift_plat = Bot_FindPlatAt (nav.nodes[b->path[hop + 1]].origin);
		if (!b->lift_plat)
		{
			// a plat-typed link with no real plat under it: stand aside
			Bot_LogEvent (b, "lift_noplat");
			b->lift_state = LIFT_FAILED;
			return false;
		}
	}
	plat = b->lift_plat;

	// ascent-only engage: a bot already at/above the column top is transiting
	// or descending, NOT boarding.  Entering a WAIT there is a trap -- standing
	// at the shaft top holds the plat up (the shaft-high touch trigger), so the
	// ride it waits for never comes and it burns the full 10s timeout, with
	// stuck-detection and the goal budget both frozen.  This is q2dm1's "stuck
	// above the chaingun": bots descending the NE platform (z920) got captured
	// by the CG lift's top landing (node 251) and timed out ~8x/run, ~45s of
	// dead standing.  Stand aside so normal follow + stuck-penalize erodes the
	// (usually fluke) descent link and reroutes.  Only gated on a fresh engage
	// so an in-progress RIDE is never interrupted.  (Descent-by-riding-the-plat-
	// down is not a q2dm1 route; revisit if a map needs it.)
	if (b->lift_state == LIFT_NONE)
	{
		int	k = hop + 1, top;
		while (k + 1 < b->path_len
			&& Bot_LinkType (b->path[k], b->path[k + 1]) == NAV_LINK_PLAT)
			k++;
		top = b->path[k];
		if (ent->s.origin[2] > nav.nodes[top].origin[2] - 24.0f)
			return false;
		b->lift_deadline = level.time + LIFT_WAIT_TIMEOUT;
	}

	if (level.time > b->lift_deadline)
	{
		Bot_LogEvent (b, "lift_timeout");
		b->lift_state = LIFT_FAILED;
		return false;
	}

	// plat footprint center; the boarding/riding target
	c[0] = (plat->absmin[0] + plat->absmax[0]) * 0.5f;
	c[1] = (plat->absmin[1] + plat->absmax[1]) * 0.5f;
	c[2] = ent->s.origin[2];

	b->want_jump = false;

	if (ent->groundentity == plat)
	{
		// ---- RIDE: the plat has us; it does the climbing ----
		int		k, top;

		if (b->lift_state != LIFT_RIDE)
		{
			Bot_LogEvent (b, "lift_ride");
			b->lift_state = LIFT_RIDE;
			b->lift_deadline = level.time + LIFT_RIDE_TIMEOUT;
		}

		// top of the consecutive plat-link chain (the learned column)
		k = hop + 1;
		while (k + 1 < b->path_len
			&& Bot_LinkType (b->path[k], b->path[k + 1]) == NAV_LINK_PLAT)
			k++;
		top = b->path[k];

		// topped out (plat parked, or risen to the column top): hand the path
		// back to the normal follower pointing PAST the column -- the top
		// node hangs over the shaft mouth, and re-targeting it would just
		// re-engage this controller every frame
		if (plat->moveinfo.state == PLAT_STATE_TOP
			|| ent->s.origin[2] > nav.nodes[top].origin[2] - 24.0f)
		{
			Bot_LogEvent (b, "lift_exit");
			b->path_idx = k + 1;
			Bot_LiftReset (b);
			return false;			// normal following resumes this frame
		}

		// still travelling: hold the middle so we can't drift off the edge
		// (gentle correction only -- move_dir magnitude scales the speed)
		VectorSubtract (c, ent->s.origin, d);
		d[2] = 0;
		if (VectorLength (d) > 24)
		{
			b->move_yaw = vectoyaw (d);
			VectorNormalize (d);
			VectorScale (d, 0.35f, b->move_dir);
		}
		else
			VectorClear (b->move_dir);
		return true;
	}

	if (plat->moveinfo.state == PLAT_STATE_BOTTOM)
	{
		// ---- BOARD: it's down; walk onto it (the touch trigger fires it) ----
		if (b->lift_state != LIFT_BOARD)
		{
			Bot_LogEvent (b, "lift_board");
			b->lift_state = LIFT_BOARD;
			VectorCopy (ent->s.origin, b->lift_move_pos);
			b->lift_move_time = level.time;
		}

		// boarding is a short unobstructed walk; stalling means geometry
		// blocks this approach (e.g. a railing above the shaft -- Phase 0
		// found paths entering columns from adjacent upper ledges).  Fail
		// over fast so the normal machinery erodes the bad approach link.
		VectorSubtract (ent->s.origin, b->lift_move_pos, d);
		if (VectorLength (d) > 24)
		{
			VectorCopy (ent->s.origin, b->lift_move_pos);
			b->lift_move_time = level.time;
		}
		else if (level.time - b->lift_move_time > LIFT_BOARD_STALL)
		{
			Bot_LogEvent (b, "lift_timeout");
			b->lift_state = LIFT_FAILED;
			return false;
		}

		VectorSubtract (c, ent->s.origin, d);
		d[2] = 0;
		if (VectorLength (d) > 1)
		{
			VectorNormalize (d);
			VectorCopy (d, b->move_dir);
			b->move_yaw = vectoyaw (d);
		}
		return true;
	}

	// ---- WAIT: the plat is away.  Hold CLEAR of the footprint: standing
	// inside it blocks the plat's descent (our own touch keeps it up) and
	// risks a crush when it comes down.
	if (b->lift_state != LIFT_WAIT)
	{
		Bot_LogEvent (b, "lift_wait");
		b->lift_state = LIFT_WAIT;
	}
	if (ent->s.origin[0] > plat->absmin[0] - LIFT_MARGIN
		&& ent->s.origin[0] < plat->absmax[0] + LIFT_MARGIN
		&& ent->s.origin[1] > plat->absmin[1] - LIFT_MARGIN
		&& ent->s.origin[1] < plat->absmax[1] + LIFT_MARGIN)
	{
		// inside: back straight out, away from the shaft
		VectorSubtract (ent->s.origin, c, d);
		d[2] = 0;
		if (VectorLength (d) < 1)
			d[0] = 1;				// dead center: any way out will do
		VectorNormalize (d);
		VectorCopy (d, b->move_dir);
		b->move_yaw = vectoyaw (d);
	}
	else
	{
		// hold position facing the lift; the ride is coming to us
		VectorClear (b->move_dir);
		VectorSubtract (c, ent->s.origin, d);
		d[2] = 0;
		if (VectorLength (d) > 1)
			b->move_yaw = vectoyaw (d);
	}
	return true;
}

//==========================================================================
// strafe jumping (bot_strafejump) -- Phase 19
//
// Chained bunny hops with forward+side both held and a per-tick yaw sweep
// build speed past the 300 run cap (calibrated from a human input capture:
// 300 -> 557 over 5 hops).  The physics is tick-rate neutral -- air accel
// grants accel*frametime*wishspeed = 30 ups per 100ms tick, the same
// per-second budget a 30Hz human gets -- and a re-jump on the first grounded
// tick skips ground friction entirely (PM_CheckJump nulls groundentity
// before PM_Friction runs).  Holding jump across the landing is safe:
// PMF_JUMP_HELD cannot latch while airborne, so no landing-time prediction
// is needed at 100ms quantization.
//
// The one physics gap vs a real client: bot-initiated Pmove runs with the
// engine's DEFAULT pmove params, which lack the "strafehack" q2pro grants
// real clients (sv_strafejump_hack, default 1).  Without it every hop
// landing (vz ~ -290) sets PMF_TIME_LAND, a 144ms jump lockout that forces
// a full 100ms ground-friction frame.  The game owns ps.pmove between
// frames, so Bot_RunFrame clears that flag for a bot mid-chain -- exactly
// reproducing the physics humans already play under, on any stock engine.
//
// The controller only engages on a QUALIFIED RUNWAY: a stretch of the
// committed A* path made of near-collinear flat WALK links, hull-trace
// verified for width, headroom, and floor at hop height.  While active it
// owns movement intent and facing (the gaze slew would corrupt the wishdir
// angle), writes the usercmd directly (the unit-projection path caps
// diagonals), and hands back cleanly at the runway's end with the momentum
// kept.
//==========================================================================

#define SJ_MIN_RUNWAY		256.0f	// min qualified spine length to engage
#define SJ_SEG_MAX_TURN		35.0f	// per-segment heading change (gentle curves ok)
#define SJ_SEG_MAX_DZ		24.0f	// near-flat (hop rhythm breaks on real stairs)
#define SJ_ENGAGE_SPEED		230.0f	// ground speed needed to start hopping
#define SJ_ABORT_SPEED		200.0f	// grounded below this mid-chain = blocked
#define SJ_COOLDOWN			1.5f	// after an abort / failed qualify
#define SJ_CHECK_INTERVAL	0.3f	// qualification throttle
#define SJ_LANE_OFFSET		30.0f	// lateral clearance lanes (hull covers +/-46)
#define SJ_HEAD_ERR_MAX		55.0f	// velocity heading vs spine target = drifted off
#define SJ_LOOKAHEAD		340.0f	// spine target distance (smooths node zig-zag)
#define SJ_WISH_CAP			270.0f	// theta* = acos(cap/speed) keeps the full
									// 30 ups accel quantum granted every tick

// qualification funnel counters (bot_sjlog >= 2 dumps them periodically):
// where runway candidates die, so thresholds can be tuned against reality
int sj_diag[SJ_DIAG_MAX];

static float SJ_AngleNorm (float a)
{
	while (a > 180.0f)
		a -= 360.0f;
	while (a < -180.0f)
		a += 360.0f;
	return a;
}

/*
=================
SJ_FacingYaw

The per-tick yaw law.  Air accel adds speed only while the projection of
velocity onto wishdir is below the 300 cap, so as speed grows the wishdir
must sit further off the velocity: theta* = acos(SJ_WISH_CAP/speed).  With
forward+side both held the wishdir is 45 deg off the facing toward the strafe
side, so facing = vel_heading + side*(45 - theta*).  Below the cap this
degenerates to pushing straight along the velocity (max accel), and as speed
rises it reproduces the accelerating yaw sweep seen in the human capture.
side: +1 = sidemove +400 (strafing right pulls the velocity clockwise).
=================
*/
static float SJ_FacingYaw (vec3_t velocity, int side, float fallback_heading)
{
	vec3_t	v;
	float	speed, theta;

	VectorCopy (velocity, v);
	v[2] = 0;
	speed = VectorLength (v);
	if (speed < 60)
		return fallback_heading;

	if (speed <= SJ_WISH_CAP)
		theta = 0;
	else
		theta = (float)acos ((double)(SJ_WISH_CAP / speed)) * 57.295780f;

	return vectoyaw (v) + (float)side * (45.0f - theta);
}

/*
=================
SJ_SegmentClear

Traces one runway chord against the WORLD only (MASK_SOLID: other
players/bots on the runway are transient -- the live abort logic handles
them; qualifying against them starved engagement to ~nothing).  What
actually kills a chain is a wall ahead, a floor gap, or a hazard pool --
NOT low ceilings: an apex head-clip merely flattens the hop (Pmove clips
vertical velocity only) and the held-jump landing re-fires, so we
deliberately do NOT demand apex headroom (a hull lane at +46 rejected every
doorframe arch on the map and starved engagement).  Node origins sit 24
above the floor; lanes run at +26 so the 18u auto-steps a runner clears
don't read as walls, while real walls (floor to ceiling) still do.
=================
*/
static qboolean SJ_SegmentClear (edict_t *ent, vec3_t from, vec3_t to)
{
	vec3_t	dir, right, s, e, up = {0, 0, 1};
	trace_t	tr;
	int		i;

	VectorSubtract (to, from, dir);
	dir[2] = 0;
	if (VectorNormalize (dir) < 1)
		return true;			// co-located: nothing to check

	// center lane: the bot's hull at body height (walls, blockers)
	VectorCopy (from, s);
	s[2] += 26;
	VectorCopy (to, e);
	e[2] += 26;
	tr = gi.trace (s, ent->mins, ent->maxs, e, ent, MASK_SOLID);
	if (tr.fraction < 1.0f || tr.startsolid || tr.allsolid)
		return false;

	// side lanes: point probes at +/-SJ_LANE_OFFSET.  A wall on ONE side is
	// fine (humans strafe jump along walls; a graze only sheds a little
	// speed), but we need at least one open side to weave in -- and any
	// side that IS open must have real, non-hazard floor at its far end,
	// so a drifting hop can't carry the bot over a drop or into lava.
	CrossProduct (dir, up, right);
	{
		int	sides_clear = 0;

		for (i = -1; i <= 1; i += 2)
		{
			vec3_t	dn, lp;

			VectorMA (from, SJ_LANE_OFFSET * i, right, s);
			s[2] = from[2] + 26;
			VectorMA (to, SJ_LANE_OFFSET * i, right, e);
			e[2] = to[2] + 26;
			tr = gi.trace (s, vec3_origin, vec3_origin, e, ent, MASK_SOLID);
			if (tr.fraction < 1.0f || tr.startsolid)
				continue;		// walled: can't drift there, floor irrelevant

			VectorCopy (e, dn);
			dn[2] -= 96;
			tr = gi.trace (e, vec3_origin, vec3_origin, dn, ent, MASK_SOLID);
			if (tr.fraction == 1.0f)
				return false;	// open side over a drop: too dangerous
			VectorCopy (tr.endpos, lp);
			lp[2] += 8;
			if (gi.pointcontents (lp) & (CONTENTS_LAVA | CONTENTS_SLIME))
				return false;
			sides_clear++;
		}
		if (sides_clear == 0)
			return false;		// a slot exactly our width: no room to weave
	}
	return true;
}

/*
=================
SJ_QualifyRunway

Find the furthest path node reachable along a straight clear CHORD from the
bot: the spine of a runway is the corridor's shape, not the learned nodes'
zig-zag polyline (per-segment collinearity starved chains to one hop -- the
nodes wander ~30 deg down perfectly straight halls).  A node qualifies as
the runway end if every intermediate path node hugs the chord laterally,
all of it is near our floor height, and the chord traces clear.  Returns
the path index of the runway's last node, or -1 if the best chord is
shorter than SJ_MIN_RUNWAY.
=================
*/
static int SJ_QualifyRunway (bot_t *b)
{
	edict_t	*ent = b->ent;
	vec3_t	chord, right, d, up = {0, 0, 1};
	float	total = 0;
	int		k, j, last = -1;

	if (b->path_len <= 0 || b->path_idx >= b->path_len)
		return -1;

	sj_diag[SJ_DIAG_QUALIFY]++;
	for (k = b->path_idx; k < b->path_len; k++)
	{
		float	len;
		int		node = b->path[k];

		if (node < 0 || node >= nav.num_nodes)
			break;
		if (k > b->path_idx && Bot_LinkType (b->path[k - 1], node) != NAV_LINK_WALK)
		{
			sj_diag[SJ_DIAG_LINK]++;
			break;
		}
		if (fabs (nav.nodes[node].origin[2] - ent->s.origin[2]) > SJ_SEG_MAX_DZ)
		{
			sj_diag[SJ_DIAG_DZ]++;
			break;
		}

		VectorSubtract (nav.nodes[node].origin, ent->s.origin, chord);
		chord[2] = 0;
		len = VectorLength (chord);
		if (len < 64)
		{
			last = k;			// too close to define a chord: absorb it
			if (len > total)
				total = len;
			continue;
		}
		VectorScale (chord, 1.0f / len, chord);
		CrossProduct (chord, up, right);

		// every intermediate node must hug this chord
		for (j = b->path_idx; j < k; j++)
		{
			VectorSubtract (nav.nodes[b->path[j]].origin, ent->s.origin, d);
			if (fabs (DotProduct (d, right)) > 40)
				break;
		}
		if (j < k)
		{
			sj_diag[SJ_DIAG_TURN]++;
			break;
		}

		if (!SJ_SegmentClear (ent, ent->s.origin, nav.nodes[node].origin))
		{
			sj_diag[SJ_DIAG_TRACE]++;
			break;
		}

		last = k;
		total = len;
	}

	if (last < 0 || total < SJ_MIN_RUNWAY)
	{
		sj_diag[SJ_DIAG_SHORT]++;
		return -1;
	}
	return last;
}

/*
=================
SJ_SimFirstHop

Commit gate: forward-simulate the whole first hop (takeoff to landing)
through the real movement code with the exact per-tick cmds the live
controller would issue, and only engage if it lands grounded, dry, near the
intended line, with real forward progress.  Deterministic physics makes this
an exact prediction, so the controller never flails (same principle as
Bot_RolloutRecover / the Phase 17 lift pre-sim).
=================
*/
static qboolean SJ_SimFirstHop (bot_t *b, float heading, int side)
{
	edict_t	*ent = b->ent;
	pmove_t	pm;
	vec3_t	start, land, delta, ang = {0, 0, 0}, fwd, right, upv;
	float	fwd_prog, lateral;
	int		i, t;
	qboolean landed = false;

	memset (&pm, 0, sizeof(pm));
	pm.s.pm_type = PM_NORMAL;
	pm.s.gravity = (short)sv_gravity->value;
	for (i = 0; i < 3; i++)
	{
		pm.s.origin[i]   = (short)(ent->s.origin[i] * 8);
		pm.s.velocity[i] = (short)(ent->velocity[i] * 8);
	}
	pm.trace = PM_trace;
	pm.pointcontents = gi.pointcontents;
	pm_passent = ent;
	VectorCopy (ent->s.origin, start);

	// live-tick-exact simulation: msec and tick count follow the actual rate
	// (9 authored frames of hop = 9 * FRAMEDIV engine ticks)
	for (t = 0; t < 9 * FRAMEDIV; t++)
	{
		vec3_t	vel;
		float	yaw;

		// mirror the live controller's landing-lockout parity clear
		if ((pm.s.pm_flags & PMF_TIME_LAND)
			&& !(pm.s.pm_flags & (PMF_TIME_TELEPORT | PMF_TIME_WATERJUMP)))
		{
			pm.s.pm_flags &= ~PMF_TIME_LAND;
			pm.s.pm_time = 0;
		}

		for (i = 0; i < 3; i++)
			vel[i] = pm.s.velocity[i] * 0.125f;
		yaw = SJ_FacingYaw (vel, side, heading);

		memset (&pm.cmd, 0, sizeof(pm.cmd));
		pm.cmd.msec        = (byte)(FRAMETIME * 1000);
		pm.cmd.forwardmove = 400;
		pm.cmd.sidemove    = (short)(400 * side);
		pm.cmd.upmove      = (t == 1) ? 0 : 350;	// one release clears JUMP_HELD
		pm.cmd.angles[YAW]   = (short)(ANGLE2SHORT(yaw) - pm.s.delta_angles[YAW]);
		pm.cmd.angles[PITCH] = (short)(0 - pm.s.delta_angles[PITCH]);
		pm.snapinitial = (t == 0);
		gi.Pmove (&pm);

		if (t >= 2 && pm.groundentity)
		{
			landed = true;
			break;
		}
	}

	if (!landed || pm.waterlevel > 0)
		return false;

	for (i = 0; i < 3; i++)
		land[i] = pm.s.origin[i] * 0.125f;
	VectorSubtract (land, start, delta);
	if (fabs (delta[2]) > 40)
		return false;			// fell off / climbed something: not our runway
	delta[2] = 0;

	ang[YAW] = heading;
	AngleVectors (ang, fwd, right, upv);
	fwd_prog = DotProduct (delta, fwd);
	lateral  = (float)fabs (DotProduct (delta, right));
	return (fwd_prog > 100 && lateral < 48);
}

/*
=================
Bot_StrafeReset
=================
*/
void Bot_StrafeReset (bot_t *b)
{
	b->sj_state = SJ_NONE;
	b->sj_end_idx = -1;
	b->sj_air_ticks = 0;
	b->sj_hops = 0;
	b->sj_peak = 0;
	b->sj_cmd_fwd = b->sj_cmd_side = b->sj_cmd_up = 0;
}

static void SJ_Abort (bot_t *b, const char *why)
{
	Bot_LogSJ (b, why, b->sj_hops, b->sj_peak);
	Bot_StrafeReset (b);
	b->sj_cooldown = level.time + SJ_COOLDOWN;
}

/*
=================
Bot_StrafeThink

Returns true while the controller owns this frame's movement intent (the
caller then skips path following and stuck recovery; the goal budget keeps
billing -- travel is faster than budgeted).  NO random() calls in here:
capability-off runs must keep the RNG stream byte-identical to baseline.
=================
*/
qboolean Bot_StrafeThink (bot_t *b)
{
	edict_t	*ent = b->ent;
	vec3_t	v2, d, tgt;
	float	speed, desired_heading, yaw;
	int		node;

	if (bot_strafejump->value == 0)
	{
		if (b->sj_state != SJ_NONE)
			Bot_StrafeReset (b);	// cvar flipped off mid-chain
		return false;
	}

	// hard vetoes: combat owns the facing; water/lift own the movement
	if (b->enemy || ent->waterlevel > 0 || b->lift_state != LIFT_NONE)
	{
		if (b->sj_state == SJ_ACTIVE)
			SJ_Abort (b, b->enemy ? "abort_enemy" : "abort_env");
		return false;
	}

	v2[0] = ent->velocity[0];
	v2[1] = ent->velocity[1];
	v2[2] = 0;
	speed = VectorLength (v2);

	if (b->sj_state == SJ_NONE)
	{
		int		end, side;
		float	err;

		if (level.time < b->sj_cooldown || level.time < b->sj_next_check)
			return false;
		if (!ent->groundentity || speed < SJ_ENGAGE_SPEED)
			return false;
		// a pending landing lockout would eat the takeoff (the parity clear
		// only runs while the controller is active)
		if (ent->client->ps.pmove.pm_flags & PMF_TIME_LAND)
			return false;

		b->sj_next_check = level.time + SJ_CHECK_INTERVAL;
		end = SJ_QualifyRunway (b);
		if (end < 0)
			return false;

		VectorSubtract (nav.nodes[b->path[end]].origin, ent->s.origin, d);
		d[2] = 0;
		if (VectorLength (d) < 1)
			return false;
		desired_heading = vectoyaw (d);

		// initial strafe side: whichever pulls the velocity toward the spine
		err = SJ_AngleNorm (desired_heading - vectoyaw (v2));
		side = (err > 0) ? -1 : +1;

		if (!SJ_SimFirstHop (b, desired_heading, side))
		{
			sj_diag[SJ_DIAG_PRESIM]++;
			b->sj_cooldown = level.time + SJ_COOLDOWN;
			return false;
		}

		sj_diag[SJ_DIAG_ENGAGE]++;
		b->sj_state = SJ_ACTIVE;
		b->sj_end_idx = end;
		b->sj_side = side;
		b->sj_air_ticks = 0;
		b->sj_hops = 0;
		b->sj_peak = speed;
		// diag fields repurposed at engage: hops = runway node count,
		// peak = the qualified chord length
		Bot_LogSJ (b, "engage", end - b->path_idx + 1, VectorLength (d));
		// fall through: produce this frame's takeoff cmd below
	}

	// stale runway (a replan rebuilt the path under us)
	if (b->sj_end_idx < b->path_idx || b->sj_end_idx >= b->path_len)
	{
		SJ_Abort (b, "abort_path");
		return false;
	}

	// advance past nodes we've reached or overflown (the follower's 48u
	// arrival radius is too tight at 40-55u per tick)
	while (b->path_idx <= b->sj_end_idx)
	{
		node = b->path[b->path_idx];
		if (node < 0 || node >= nav.num_nodes)
		{
			SJ_Abort (b, "abort_path");
			return false;
		}
		VectorSubtract (nav.nodes[node].origin, ent->s.origin, d);
		d[2] = 0;
		if (VectorLength (d) < 80)
		{
			b->path_idx++;
			continue;
		}
		if (b->path_idx < b->sj_end_idx)
		{
			vec3_t	segd;
			VectorSubtract (nav.nodes[b->path[b->path_idx + 1]].origin,
							nav.nodes[node].origin, segd);
			segd[2] = 0;
			if (DotProduct (d, segd) < 0)
			{
				b->path_idx++;	// node fell behind us relative to the spine
				continue;
			}
		}
		break;
	}
	if (b->path_idx > b->sj_end_idx)
	{
		// runway consumed: clean hand-back, momentum kept
		Bot_LogSJ (b, "done", b->sj_hops, b->sj_peak);
		Bot_StrafeReset (b);
		b->sj_cooldown = level.time + 0.3f;
		return false;			// the follower steers this frame
	}

	// grounded with little chord left: try extending through a gentle bend
	// BEFORE deciding this hop.  A seamless extension keeps the chain (and
	// its speed) alive around a curve -- the user's capture carves exactly
	// this way; handing back instead costs a full-friction ground tick that
	// resets everything above ~300.
	if (ent->groundentity && speed > 60)
	{
		VectorSubtract (nav.nodes[b->path[b->sj_end_idx]].origin, ent->s.origin, d);
		d[2] = 0;
		if (VectorLength (d) < speed * 0.55f)
		{
			int nend = SJ_QualifyRunway (b);
			if (nend > b->sj_end_idx)
				b->sj_end_idx = nend;
		}
	}

	// spine target: the furthest runway node within the lookahead (smooths
	// the node-to-node zig-zag into one heading)
	{
		float	acc = 0;
		vec3_t	prev;
		int		k, tn = b->path[b->path_idx];

		VectorCopy (ent->s.origin, prev);
		for (k = b->path_idx; k <= b->sj_end_idx; k++)
		{
			VectorSubtract (nav.nodes[b->path[k]].origin, prev, d);
			d[2] = 0;
			acc += VectorLength (d);
			tn = b->path[k];
			if (acc > SJ_LOOKAHEAD)
				break;
			VectorCopy (nav.nodes[b->path[k]].origin, prev);
		}
		VectorCopy (nav.nodes[tn].origin, tgt);
	}
	VectorSubtract (tgt, ent->s.origin, d);
	d[2] = 0;
	if (VectorLength (d) < 1)
	{
		Bot_LogSJ (b, "done", b->sj_hops, b->sj_peak);
		Bot_StrafeReset (b);
		b->sj_cooldown = level.time + 0.5f;
		return false;
	}
	desired_heading = vectoyaw (d);

	// drifted off the spine?
	if (speed > 60
		&& fabs (SJ_AngleNorm (desired_heading - vectoyaw (v2))) > SJ_HEAD_ERR_MAX)
	{
		SJ_Abort (b, "abort_drift");
		return false;
	}

	if (ent->groundentity)
	{
		// between hops: decide whether the runway still funds another one
		float	remain;

		if (b->sj_hops > 0 && speed < SJ_ABORT_SPEED)
		{
			SJ_Abort (b, "abort_slow");
			return false;
		}

		VectorSubtract (nav.nodes[b->path[b->sj_end_idx]].origin, ent->s.origin, d);
		d[2] = 0;
		remain = VectorLength (d);
		if (remain < speed * 0.55f)
		{
			// not enough runway for a full hop (and no extension was found):
			// hand back with the momentum kept
			Bot_LogSJ (b, "done", b->sj_hops, b->sj_peak);
			Bot_StrafeReset (b);
			b->sj_cooldown = level.time + 0.3f;
			return false;
		}

		// re-verify the stretch we're about to fly over
		if (!SJ_SegmentClear (ent, ent->s.origin, tgt))
		{
			SJ_Abort (b, "abort_blocked");
			return false;
		}

		// pick this hop's strafe side: pull the velocity back toward the
		// spine (alternates naturally on a straight, holds through a curve)
		if (speed > 60)
		{
			float err = SJ_AngleNorm (desired_heading - vectoyaw (v2));
			b->sj_side = (err > 0) ? -1 : +1;
		}

		b->sj_air_ticks = 0;
		b->sj_hops++;
		b->sj_cmd_up = 350;		// takeoff (landing frames re-jump friction-free)
		if (bot_sjlog->value != 0)
			Bot_LogSJ (b, "hop", b->sj_hops, speed);
	}
	else
	{
		b->sj_air_ticks++;
		// one release tick clears PMF_JUMP_HELD; then hold jump so the
		// landing tick re-jumps regardless of where in the 100ms it lands
		b->sj_cmd_up = (b->sj_air_ticks == 1) ? 0 : 350;
	}

	yaw = SJ_FacingYaw (ent->velocity, b->sj_side, desired_heading);
	b->sj_cmd_fwd  = 400;
	b->sj_cmd_side = (short)(400 * b->sj_side);

	// keep a sane world-space intent too: it feeds the debug draw and the
	// one-frame combat-acquisition fallback before the abort lands
	Bot_SetMoveYaw (b, yaw);

	if (speed > b->sj_peak)
		b->sj_peak = speed;
	return true;
}

/*
=================
Bot_FollowPath

Sets movement intent along b->path toward the next waypoint, advancing past
reached/overshot nodes.  Returns false when the goal is reached or invalid.
=================
*/
qboolean Bot_FollowPath (bot_t *b)
{
	edict_t	*ent = b->ent;
	vec3_t	target, d, vel;
	float	hdist;
	int		node, prev;

	if (b->path_len <= 0)
		return false;

	while (b->path_idx < b->path_len)
	{
		qboolean	vertical;
		float		dz, vdist;

		node = b->path[b->path_idx];
		if (node < 0 || node >= nav.num_nodes)
		{
			b->path_idx++;
			continue;
		}
		VectorSubtract (nav.nodes[node].origin, ent->s.origin, d);
		dz = d[2];
		vdist = (float)fabs (dz);
		d[2] = 0;
		hdist = VectorLength (d);

		// vertical context: arrival must be 3D, else a shaft's nodes all
		// "arrive" at once (their horizontal distance is ~0) and the bot
		// targets the far side through the wall/floor.  Applies while
		// swimming (or heading for a water node), and -- with bot_lift --
		// when the waypoint is a plat-column node still well above us.
		vertical = (bot_swim->value != 0
			&& (ent->waterlevel >= 2 || (nav.nodes[node].flags & NAV_FLAG_WATER)))
			|| (bot_lift->value != 0 && dz > 48 && hdist < 72);

		if (hdist < NAV_ARRIVE_RADIUS && (!vertical || vdist < NAV_ARRIVE_RADIUS))
		{
			b->path_idx++;
			continue;
		}

		VectorCopy (ent->velocity, vel);
		vel[2] = 0;
		if (!vertical && hdist < 96 && VectorLength (vel) > 40 && DotProduct (vel, d) < 0)
		{
			b->path_idx++;
			continue;
		}
		break;
	}
	if (b->path_idx >= b->path_len)
		return false;

	node = b->path[b->path_idx];
	VectorCopy (nav.nodes[node].origin, target);

	Bot_SetMoveToward (b, target);

	if (b->path_idx > 0 && ent->groundentity)
	{
		prev = b->path[b->path_idx - 1];
		VectorSubtract (target, ent->s.origin, d);
		d[2] = 0;
		if (Bot_LinkType (prev, node) == NAV_LINK_JUMP && VectorLength (d) < 96)
		{
			b->want_jump = true;
			b->did_jump = true;
		}
	}

	// humanization (bot_fidget): travel hops.  Humans bounce down corridors
	// (demo corpus ~15 jumps/min; the stock bot ~4).  did_jump stays honest
	// so link learning records a JUMP if the hop happens to climb something.
	if (bot_fidget->value != 0 && Bot_Humanized (b)
		&& ent->groundentity && !b->enemy && !b->want_jump
		&& VectorLength (ent->velocity) > 220
		&& random () < 0.03f * BOT_TICK_RATIO)
	{
		b->want_jump = true;
		b->did_jump = true;
	}

	return true;
}

/*
=================
Bot_ApplyMovement

Projects the world-space move intent onto 'facing_yaw' to produce forward/side
movement, so the bot travels toward its goal regardless of where it's looking.
=================
*/
/*
=================
Bot_StepIsSafe

True unless the spot just ahead (in the bot's move direction) is a deadly drop
(void / very long fall) or lava/slime.  Only consulted while exploring -- learned
paths are known-traversable, so we don't second-guess them.
=================
*/
static qboolean Bot_StepIsSafe (bot_t *b)
{
	edict_t	*ent = b->ent;
	vec3_t	ahead, start, end;
	trace_t	tr;

	if (VectorLength (b->move_dir) < 0.1f)
		return true;

	VectorMA (ent->s.origin, 28, b->move_dir, ahead);
	VectorCopy (ahead, start);
	start[2] += 16;
	VectorCopy (ahead, end);
	end[2] -= 160;		// survivable step/drop distance

	tr = gi.trace (start, vec3_origin, vec3_origin, end, ent, MASK_PLAYERSOLID);
	if (tr.fraction == 1.0f)
		return false;	// no floor within range -> void or a long fall

	{
		vec3_t lp;
		VectorCopy (tr.endpos, lp);
		lp[2] += 8;
		if (gi.pointcontents (lp) & (CONTENTS_LAVA | CONTENTS_SLIME))
			return false;
	}
	return true;
}

/*
=================
Bot_Fidget

Humanization (bot_fidget): idle texture while deliberately holding a spot
(waiting for an item respawn).  Statue-stillness is a loud tell -- humans
shift their weight in small steps.  Micro-steps are leashed to the anchor and
safety-checked, and the intent magnitude stays small so the "hold" still does
its job.  Never during lift states: a wiggle near a plat footprint can hold
the lift up (see bot_lift's WAIT).
=================
*/
void Bot_Fidget (bot_t *b, vec3_t anchor)
{
	edict_t	*ent = b->ent;
	vec3_t	d;

	if (bot_fidget->value == 0 || !Bot_Humanized (b))
		return;
	if (!ent->groundentity || b->lift_state != LIFT_NONE)
		return;

	VectorSubtract (anchor, ent->s.origin, d);
	d[2] = 0;
	if (VectorLength (d) > 40)
		return;					// still traveling to the spot; don't meddle

	if (level.time >= b->fidget_until)
	{
		if (random () < 0.45f)
		{
			b->fidget_yaw = random () * 360.0f;
			b->fidget_mag = 0.2f + random () * 0.15f;
		}
		else
			b->fidget_mag = 0;	// stand for a beat
		b->fidget_until = level.time + 0.4f + random () * 1.3f;
	}

	if (b->fidget_mag > 0)
	{
		if (VectorLength (d) > 24)
		{
			// drifted off the spot: this step goes back toward it
			VectorNormalize (d);
			VectorScale (d, b->fidget_mag, b->move_dir);
			return;
		}
		{
			vec3_t	a = {0, 0, 0}, f, r, u;
			a[YAW] = b->fidget_yaw;
			AngleVectors (a, f, r, u);
			f[2] = 0;
			VectorNormalize (f);
			VectorScale (f, b->fidget_mag, b->move_dir);
		}
		if (!Bot_StepIsSafe (b))
			VectorClear (b->move_dir);
	}
}

/*
=================
Bot_WallSlide

Preventive de-pinning (bot_wallslide): if the move intent drives into a roughly
vertical wall, deflect it ALONG the wall instead of straight into it, so the bot
rounds the corner rather than pressing the wall until the 1s stuck timer trips
and recovery kicks in.  Where the approach is shallow the natural slide direction
is the projection of the intent onto the wall plane; where it's near-perpendicular
(projection ~0) we probe both tangents and take the one with more clearance.
Updates move_yaw too, so the look-ahead/facing follows the slide (the whole point
is that the bot stops staring at the wall).  Never during lifts/water/exploring --
those layers own the intent (plat columns, swim exits, void-edge stops).
=================
*/
#define WALLSLIDE_PROBE		24.0f
static void Bot_WallSlide (bot_t *b)
{
	edict_t	*ent = b->ent;
	vec3_t	fwd, start, end, tang, slide, up = {0, 0, 1};
	trace_t	tr;
	float	along, slen;

	if (bot_wallslide->value == 0)
		return;
	if (!ent->groundentity || Bot_Swimming (ent) || b->lift_state != LIFT_NONE)
		return;
	if (b->mode == BOT_MODE_EXPLORE)
		return;						// wander/StepIsSafe own explore steering

	VectorCopy (b->move_dir, fwd);
	fwd[2] = 0;
	if (VectorNormalize (fwd) < 0.1f)
		return;

	// box-trace the bot's own hull a short step ahead
	VectorCopy (ent->s.origin, start);
	VectorMA (start, WALLSLIDE_PROBE, fwd, end);
	tr = gi.trace (start, ent->mins, ent->maxs, end, ent, MASK_PLAYERSOLID);
	if (tr.fraction > 0.7f)
		return;						// mostly clear ahead: not pinned
	if (tr.plane.normal[2] > 0.5f || tr.plane.normal[2] < -0.5f)
		return;						// a ramp/step/ceiling, not a wall -- Pmove handles it

	// wall tangent (horizontal), signed toward the intent's glancing direction
	CrossProduct (up, tr.plane.normal, tang);
	tang[2] = 0;
	if (VectorNormalize (tang) < 0.1f)
		return;
	along = DotProduct (fwd, tang);
	if (along < 0)
		VectorScale (tang, -1, tang);

	// near-perpendicular hit: the signed tangent is ambiguous -- probe both ways
	// and slide toward whichever side is more open.
	if (fabs (along) < 0.35f)
	{
		vec3_t	le, re;
		trace_t	lt, rt;
		VectorMA (start, WALLSLIDE_PROBE, tang, re);
		lt = gi.trace (start, ent->mins, ent->maxs, re, ent, MASK_PLAYERSOLID);
		VectorMA (start, -WALLSLIDE_PROBE, tang, le);
		rt = gi.trace (start, ent->mins, ent->maxs, le, ent, MASK_PLAYERSOLID);
		if (rt.fraction > lt.fraction)
			VectorScale (tang, -1, tang);
	}

	// preserve the original horizontal speed magnitude along the new heading
	slen = sqrt (b->move_dir[0] * b->move_dir[0] + b->move_dir[1] * b->move_dir[1]);
	VectorScale (tang, slen, slide);
	slide[2] = b->move_dir[2];
	VectorCopy (slide, b->move_dir);
	b->move_yaw = vectoyaw (tang);
}

void Bot_ApplyMovement (bot_t *b, usercmd_t *cmd, float facing_yaw)
{
	vec3_t	a = {0, 0, 0}, f, r, u;
	float	speed = bot_forwardspeed->value;
	float	fm, sm;

	// playbook replay: the recorded usercmd is the maneuver -- apply verbatim
	// (facing was already set to the recorded view in Bot_Think, so these
	// move values mean exactly what they meant when captured)
	if (bot_playbook->value != 0 && b->pb_state == PB_REPLAY)
	{
		cmd->forwardmove = b->pb_cmd_fwd;
		cmd->sidemove    = b->pb_cmd_side;
		cmd->upmove      = b->pb_cmd_up;
		return;
	}

	// strafe jumping: the controller computed the exact usercmd (forward and
	// side BOTH saturated -- the unit projection below would cap the diagonal
	// at ~283/283 and kill the wishdir angle the yaw law depends on).  Only
	// while combat hasn't grabbed the facing; if an enemy appeared this frame
	// we fall through to the normal projection and abort next frame.
	if (bot_strafejump->value != 0 && b->sj_state == SJ_ACTIVE && !b->enemy)
	{
		cmd->forwardmove = b->sj_cmd_fwd;
		cmd->sidemove    = b->sj_cmd_side;
		cmd->upmove      = b->sj_cmd_up;
		return;
	}

	// avoid wandering into void/lava (explore only; combat & learned paths are
	// allowed to take known drops)
	if (b->mode == BOT_MODE_EXPLORE && !b->enemy && b->ent->groundentity
		&& !Bot_StepIsSafe (b))
	{
		cmd->forwardmove = 0;
		cmd->sidemove = 0;
		b->next_wander_time = level.time;	// turn away next frame
		return;
	}

	Bot_WallSlide (b);	// deflect intent along a wall it's about to pin against

	a[YAW] = facing_yaw;
	AngleVectors (a, f, r, u);
	f[2] = 0;
	r[2] = 0;
	VectorNormalize (f);
	VectorNormalize (r);

	fm = DotProduct (b->move_dir, f);
	sm = DotProduct (b->move_dir, r);

	cmd->forwardmove = (short)(fm * speed);
	cmd->sidemove    = (short)(sm * speed);

	// swimming: vertical intent becomes swim upmove (Pmove's water-jump also
	// keys off upmove when we surface against a ledge, which climbs us out)
	if (Bot_Swimming (b->ent))
		cmd->upmove = (short)(b->move_dir[2] * speed);

	if (b->want_jump && b->ent->groundentity)
		cmd->upmove = 350;
}
