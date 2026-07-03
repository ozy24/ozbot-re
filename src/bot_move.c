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

static void Bot_SetMoveYaw (bot_t *b, float yaw)
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

	if (ent->groundentity && random() < 0.02)
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
#define ROLLOUT_TICKS	5
#define ROLLOUT_MSEC	100		// matches the 10Hz server frame these bots run at

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
		memset (&pm.cmd, 0, sizeof(pm.cmd));
		pm.cmd.msec        = ROLLOUT_MSEC;
		pm.cmd.forwardmove = steps[i].forward;
		pm.cmd.sidemove    = steps[i].side;
		pm.cmd.upmove      = steps[i].up;
		pm.cmd.angles[YAW]   = (short)(ANGLE2SHORT(yaw) - pm.s.delta_angles[YAW]);
		pm.cmd.angles[PITCH] = (short)(0 - pm.s.delta_angles[PITCH]);
		pm.snapinitial = (i == 0);
		gi.Pmove (&pm);
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
static int Bot_LinkType (int from, int to)
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
Lift_UpcomingPlatHop

Index into b->path of the FROM node of the plat hop in play: the hop we are
currently traversing, or the next one once we're near its base.  -1 if none.
=================
*/
static int Lift_UpcomingPlatHop (bot_t *b)
{
	if (b->path_len <= 0)
		return -1;

	if (b->path_idx >= 1 && b->path_idx < b->path_len
		&& Bot_LinkType (b->path[b->path_idx - 1], b->path[b->path_idx]) == NAV_LINK_PLAT)
		return b->path_idx - 1;

	if (b->path_idx >= 0 && b->path_idx + 1 < b->path_len
		&& Bot_LinkType (b->path[b->path_idx], b->path[b->path_idx + 1]) == NAV_LINK_PLAT)
	{
		vec3_t	d;
		float	range = (b->lift_state != LIFT_NONE) ? LIFT_RELEASE_RANGE
													  : LIFT_ENGAGE_RANGE;
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

	hop = Lift_UpcomingPlatHop (b);
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

	if (b->lift_state == LIFT_NONE)
		b->lift_deadline = level.time + LIFT_WAIT_TIMEOUT;

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
		&& random () < 0.03f)
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

void Bot_ApplyMovement (bot_t *b, usercmd_t *cmd, float facing_yaw)
{
	vec3_t	a = {0, 0, 0}, f, r, u;
	float	speed = bot_forwardspeed->value;
	float	fm, sm;

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
