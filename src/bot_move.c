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

	if (level.time >= b->next_wander_time ||
		(speed < 20 && ent->groundentity && (level.time - b->last_repick_time) > 0.4))
	{
		b->desired_yaw = crandom() * 180.0;
		b->next_wander_time = level.time + 1.5 + random() * 2.0;
		b->last_repick_time = level.time;
	}

	Bot_SetMoveYaw (b, b->desired_yaw);

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
