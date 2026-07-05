/*
ozbot - self-learning q2dm1 bot

bot_combat.c -- combat: enemy selection, skill-based aim (reaction + error),
weapon selection, fire, and dodge.  When an enemy is engaged this overrides
navigation for the frame.
*/

#include "g_local.h"
#include "bot.h"

cvar_t	*bot_skill;		// 0 (easy) .. 1 (hard)
cvar_t	*bot_skilltest;	// head-to-head skill A/B; see Bot_Add in bot_main.c
cvar_t	*bot_lead;		// lead moving targets by projectile flight time
cvar_t	*bot_leadtest;	// head-to-head lead A/B: even bot ids lead, odd don't
cvar_t	*bot_flee;		// retreat + fetch health/armor when clearly outmatched
cvar_t	*bot_fleetest;	// head-to-head flee A/B: even bot ids flee, odd don't
cvar_t	*bot_aimtest;	// head-to-head aim-formula A/B: even bot ids apply the
cvar_t	*bot_aimreact;	//   bot_aim* multipliers below, odd use the stock
cvar_t	*bot_aimturn;	//   formula -- for sweeping which aim constant
cvar_t	*bot_aimerr;	//   (reaction/turn rate/error/fire threshold) actually
cvar_t	*bot_aimfire;	//   buys kills at a given nominal skill
cvar_t	*bot_aimtexture;	// humanization: autocorrelated aim error + reversal
							// overshoot instead of per-frame white noise
							// (plans/humanization.md Phase 2)
cvar_t	*bot_survive;		// survival instinct: health-need urgency + low-hp
							// caution (break off + heal instead of dying mid-fight)
cvar_t	*bot_gazelife;		// humanization: glance around between fire windows
							// (threat-check / navigation looks) then snap back to
							// aim -- adds view liveliness at ~no lethality cost
cvar_t	*bot_aimflick;		// flick-speed multiplier on the aim turn cap
							// (acquisition swings; smoothed by bot_aimsmooth)
cvar_t	*bot_aimsmooth;		// 40Hz view smoothing: the aim DECISION is 10Hz
							// (FRAMESYNC); glide the sent view toward it every
							// frame instead of snapping+holding (kills the 10Hz
							// view judder that reads as jerky/robotic aim)
cvar_t	*bot_fov;		// humanization: enemy acquisition needs the target in
						// a ~120 deg view cone (or a recent pain event -- the
						// turn-toward-attacker reflex).  Ends 360-degree
						// vision; pairs with bot_gaze scanning (Phase 3)
cvar_t	*bot_hop;		// humanization: combat movement rhythm -- jump rate
						// and strafe-leg lengths from the demo stats, momentum
						// dip on reversals, commit to close fights (Phase 4)

static float Skill (bot_t *b)
{
	float s = (b->skill_ovr >= 0.0f) ? b->skill_ovr : (bot_skill ? bot_skill->value : 0.6f);
	if (s < 0) s = 0;
	if (s > 1) s = 1;
	return s;
}

static float AngleDelta (float target, float cur)
{
	float d = target - cur;
	while (d > 180) d -= 360;
	while (d < -180) d += 360;
	return d;
}

static float ApproachAngle (float cur, float target, float step)
{
	float d = AngleDelta (target, cur);
	if (d > step) d = step;
	if (d < -step) d = -step;
	return cur + d;
}

/*
=================
Combat_InFov

Target inside a ~120 deg horizontal view cone (or close enough to hear).
Vertical is deliberately not gated: peripheral vision plus sound covers it,
and q2dm1's stacked floors would make a pitch gate blind bots to fights one
step up a ramp.
=================
*/
#define BOT_FOV_HALF	60.0f
#define BOT_FOV_NEAR	100.0f	// within earshot: you know they're there

static qboolean Combat_InFov (edict_t *self, edict_t *other)
{
	vec3_t	d;
	float	dy;

	VectorSubtract (other->s.origin, self->s.origin, d);
	d[2] = 0;
	if (VectorLength (d) < BOT_FOV_NEAR)
		return true;
	dy = AngleDelta (vectoyaw (d), self->client->ps.viewangles[YAW]);
	return dy >= -BOT_FOV_HALF && dy <= BOT_FOV_HALF;
}

/*
=================
Combat_FindEnemy

Nearest visible, living player/bot (deathmatch: everyone is fair game).

With bot_fov (humanization Phase 3): only targets inside the view cone are
acquired -- plus whoever hurt us recently (the pain reflex, Bot_NotePain) and
the enemy we are already fighting (aim tracks them, so they stay "seen").
This is where humanness deliberately costs strength: no more eyes in the back
of the head.  The gaze layer's scanning (bot_gaze) is what finds targets now.
=================
*/
static edict_t *Combat_FindEnemy (bot_t *b)
{
	edict_t		*self = b->ent;
	edict_t		*best = NULL;
	float		bestd = 1e18f;
	int			i;
	qboolean	fovgate = (bot_fov->value != 0 && Bot_Humanized (b));

	for (i = 1; i <= (int)maxclients->value; i++)
	{
		edict_t	*o = g_edicts + i;
		vec3_t	d;
		float	dd;

		if (o == self || !o->inuse || !o->client)
			continue;
		if (o->deadflag || o->health <= 0)
			continue;
		if (o->client->resp.spectator)
			continue;
		if (!visible (self, o))
			continue;
		VectorSubtract (o->s.origin, self->s.origin, d);
		dd = VectorLength (d);
		// outside the cone, a target is still acquirable if it hurt us
		// recently (pain reflex) or fired an unsilenced weapon in earshot
		// (Q2 guns are loud; humans turn toward gunfire behind them)
		if (fovgate && o != b->enemy
			&& !(b->threat_ent == o && level.time - b->threat_time < 2.0f)
			&& !(dd < 700 && level.time - Bot_NoiseTime (o) < 1.0f)
			&& !Combat_InFov (self, o))
			continue;
		if (dd < bestd)
		{
			bestd = dd;
			best = o;
		}
	}
	return best;
}

/*
=================
Combat_HasWeaponAmmo
=================
*/
static qboolean Combat_HasWeaponAmmo (edict_t *ent, gitem_t *w)
{
	gitem_t	*am;

	if (!w || ent->client->pers.inventory[ITEM_INDEX(w)] <= 0)
		return false;
	if (!w->ammo)
		return true;			// e.g. Blaster
	am = FindItem (w->ammo);
	if (!am)
		return true;
	return ent->client->pers.inventory[ITEM_INDEX(am)] > 0;
}

/*
=================
Combat_SelectWeapon

Switch to the best weapon we own and have ammo for (engine handles the actual
swap via client->newweapon on the next weapon think).
=================
*/
static void Combat_SelectWeapon (bot_t *b)
{
	static const char *prio[] = {
		"railgun", "rocket launcher", "hyperblaster", "chaingun",
		"super shotgun", "machinegun", "shotgun", "blaster"
	};
	edict_t	*ent = b->ent;
	int		i;

	for (i = 0; i < 8; i++)
	{
		gitem_t *w = FindItem ((char *)prio[i]);
		if (w && Combat_HasWeaponAmmo (ent, w))
		{
			if (ent->client->pers.weapon != w && ent->client->newweapon != w)
				ent->client->newweapon = w;
			return;
		}
	}
}

/*
=================
Combat_Strength

Effective toughness for fight-or-flight comparisons: health plus armor at the
armor system's rough absorption weight.
=================
*/
float Combat_Strength (edict_t *e)
{
	float	s = e->health;
	int		ai;

	if (e->client)
	{
		ai = ArmorIndex (e);
		if (ai)
			s += e->client->pers.inventory[ai] * 0.5f;
	}
	return s;
}

/*
=================
Combat_ProjectileSpeed

Muzzle speed of the current weapon's projectile, or 0 for hitscan weapons
(which need no lead) and grenades (whose lobbed arc we don't model).
Speeds match the fire_* calls in p_weapon.c.
=================
*/
static float Combat_ProjectileSpeed (gitem_t *w)
{
	const char *nm = (w && w->pickup_name) ? w->pickup_name : "";
	if (strstr (nm, "Rocket"))
		return 650;
	if (strstr (nm, "Blaster"))		// Blaster and HyperBlaster bolts
		return 1000;
	return 0;
}

/*
=================
Combat_Aim

If an enemy is visible: aim toward it (returning the facing in *facing_yaw/
*facing_pitch), fire, pick a weapon, and blend a strafe/range component into
b->move_dir (so the bot dodges and adjusts range while still pursuing its nav
goal).  Returns true if engaged.
=================
*/
qboolean Combat_Aim (bot_t *b, usercmd_t *cmd, float *facing_yaw, float *facing_pitch)
{
	edict_t	*self = b->ent;
	edict_t	*enemy;
	float	skill = Skill (b);
	vec3_t	eyes, teyes, dir, ang, toenemy, strafe, comb;
	float	turnstep, firethresh, reaction, err, range, aimoff, rc;
	qboolean aimtweak;

	// FRAMESYNC (40Hz adaptation): the whole combat DECISION layer --
	// acquisition, flee, weapon choice, target sampling, error step, tracking
	// step, trigger -- runs at the authored 10Hz, exactly the cadence every
	// Phase 4-18 constant (and the humanness KS profile) was tuned and
	// validated at.  Per-tick decisions, even per-second-normalized, gave the
	// bot fresher combat information and measured +28..41% frags.  Off-frames
	// hold the aim and the engagement, and only keep the dodge/range movement
	// blend running (the "10Hz brain, 40Hz body" split).
	if (!FRAMESYNC)
	{
		enemy = b->enemy;
		if (!enemy || !enemy->inuse || !enemy->client
			|| enemy->deadflag || enemy->health <= 0
			|| enemy->client->resp.spectator)
			return false;		// next keyframe re-evaluates properly

		VectorSubtract (enemy->s.origin, self->s.origin, dir);
		range = VectorLength (dir);
		goto aim_held;
	}

	enemy = Combat_FindEnemy (b);
	if (!enemy)
	{
		b->enemy = NULL;
		return false;
	}

	aimtweak = (bot_aimtest->value != 0) && ((b->id & 1) == 0);

	if (b->enemy != enemy)
	{
		reaction = 0.1f + (1.0f - skill) * 0.4f;
		if (aimtweak)
			reaction *= bot_aimreact->value;
		b->enemy = enemy;
		b->reaction_until = level.time + reaction;
		b->aim[YAW]   = self->client->ps.viewangles[YAW];
		b->aim[PITCH] = self->client->ps.viewangles[PITCH];
		b->aim_view[YAW]   = self->client->ps.viewangles[YAW];	// view starts
		b->aim_view[PITCH] = self->client->ps.viewangles[PITCH];	// where we look
		b->aim_err[YAW] = b->aim_err[PITCH] = 0;	// no stale texture error
		b->aim_sweep_sign = 0;						// from an earlier fight
		b->cglance_until = 0;						// no glance mid-acquisition;
		b->cglance_next = level.time + 0.8f;		// settle on the new target first
		// seed with the target's actual bearing, not our own facing -- the
		// acquisition offset is not a "sweep" and must not prime the
		// reversal-overshoot detector
		{
			vec3_t	bd;
			VectorSubtract (enemy->s.origin, self->s.origin, bd);
			bd[2] = 0;
			b->aim_bearing_prev = (VectorLength (bd) > 1)
				? vectoyaw (bd) : b->aim[YAW];
		}
	}

	// fight-or-flight: when clearly outmatched, retreat (still firing) and let
	// the goal layer fetch health/armor.  Hysteresis so it doesn't flap.
	{
		qboolean can_flee = (bot_fleetest->value != 0)
			? ((b->id & 1) == 0)
			: (bot_flee->value != 0);
		if (!can_flee)
			b->flee = false;
		else
		{
			float mine   = Combat_Strength (self);
			float theirs = Combat_Strength (enemy);
			qboolean survive = (bot_survive && bot_survive->value != 0);
			if (b->flee)
			{
				// with survive, re-engage once healed to ~60 (hysteresis vs the
				// <40 low-hp trigger below) instead of waiting for full recovery
				if (mine > (survive ? 60.0f : 100.0f) || mine > theirs * 0.95f)
					b->flee = false;	// recovered (or they got hurt too)
			}
			else
			{
				// low-absolute-hp caution (bot_survive): break off and heal even
				// in an even fight -- the attrition deaths were bots at <40hp
				// fighting on because they were not "outmatched"
				if ((mine < 75 && mine < theirs * 0.65f) || (survive && mine < 40.0f))
					b->flee = true;
			}
		}
	}

	Combat_SelectWeapon (b);

	VectorCopy (self->s.origin, eyes);
	eyes[2] += self->viewheight;
	VectorCopy (enemy->s.origin, teyes);
	teyes[2] += enemy->viewheight;

	// lead a moving target by the projectile's flight time, scaled by skill
	// (hitscan weapons need no lead; aim error below still applies on top)
	{
		qboolean lead = (bot_leadtest->value != 0)
			? ((b->id & 1) == 0)
			: (bot_lead->value != 0);
		float pspeed = lead ? Combat_ProjectileSpeed (self->client->pers.weapon) : 0;
		if (pspeed > 0)
		{
			VectorSubtract (teyes, eyes, dir);
			VectorMA (teyes, skill * VectorLength (dir) / pspeed,
			          enemy->velocity, teyes);
		}
	}

	VectorSubtract (teyes, eyes, dir);
	range = VectorLength (dir);
	vectoangles (dir, ang);

	turnstep = 20.0f + skill * 40.0f;
	// flick speed (bot_aimflick): the stock 20-60 deg per 10Hz tick caps
	// acquisition at ~600 deg/s -- a bot needs ~3 ticks to spin 180 to a target
	// behind it, which looks robotically slow.  Humans flick far faster (demo
	// yaw p90 ~227 deg/tick, up to a 180 turn in one 100ms sample).  Scale the
	// cap up so big acquisition swings match human flicks; bot_aimsmooth glides
	// them so faster does NOT mean jerkier.  Fine tracking is gain-limited, not
	// cap-limited, so this only speeds the big swings.
	turnstep *= bot_aimflick->value;
	if (aimtweak)
		turnstep *= bot_aimturn->value;

	if (bot_aimtexture->value != 0 && Bot_Humanized (b))
	{
		// --- humanization (Phase 2): the stock error below is fresh white
		// noise every 100ms, which reads as a 10Hz vibration around the
		// target.  Human aim error *wanders*: it drifts off and gets pulled
		// back (pursuit lag + correction).  Model it as an OU process whose
		// stationary spread matches the stock magnitude, so bot_skill keeps
		// its meaning: sigma (error size) shrinks and theta (correction
		// speed) grows with skill.
		// NOT the stock magnitude: correlated error produces miss STREAKS
		// (the wandering aim point stays wrong for several frames), so at
		// equal spread it fights far worse than white noise, which averages
		// out shot to shot -- measured 38% relative kill cost at 0.577x.
		// Shrink the spread and correct faster to buy the texture for free.
		// (constants are per authored 10Hz frame; the FRAMESYNC gate above
		// keeps that cadence at any server tick rate)
		float	sd    = 0.26f * (1.0f - skill) * 7.0f;
		float	theta = 0.35f + 0.30f * skill;
		float	sigma = sd * (float)sqrt (theta * (2.0f - theta));
		float	sweep, sweep_sign;

		if (aimtweak)
			sigma *= bot_aimerr->value;
		b->aim_err[YAW]   += -theta * b->aim_err[YAW]
			+ sigma * (crandom () + crandom () + crandom ());
		b->aim_err[PITCH] += -theta * b->aim_err[PITCH]
			+ sigma * (crandom () + crandom () + crandom ());

		// overshoot + a beat of re-reaction when the target reverses: humans
		// keep sweeping the way the target WAS going for a moment
		sweep = AngleDelta (ang[YAW], b->aim_bearing_prev);
		sweep_sign = (sweep > 1.5f) ? 1.0f : (sweep < -1.5f) ? -1.0f : 0.0f;
		if (sweep_sign != 0 && b->aim_sweep_sign != 0
			&& sweep_sign != b->aim_sweep_sign
			&& level.time - b->aim_flip_time > 0.6f)
		{
			// rate-limited: hopping targets flip the bearing every few ticks,
			// and overshooting on every flip bled ~6% of stack frags -- the
			// overshoot is for deliberate strafe reversals, not bounce jitter
			b->aim_flip_time = level.time;
			b->aim_err[YAW] += b->aim_sweep_sign * (1.5f + (1.0f - skill) * 3.0f);
			if (b->reaction_until < level.time + 0.1f)
				b->reaction_until = level.time + 0.1f + (1.0f - skill) * 0.2f;
		}
		if (sweep_sign != 0)
			b->aim_sweep_sign = sweep_sign;
		b->aim_bearing_prev = ang[YAW];

		ang[YAW]   += b->aim_err[YAW];
		ang[PITCH] += b->aim_err[PITCH] * 0.6f;	// humans stabilize pitch better

		// gain-based tracking (a fraction of the remaining delta per tick)
		// instead of the stock constant-rate chase: same convergence, but the
		// decaying steps give the human autocorrelation signature
		{
			float	gain = 0.65f + 0.20f * skill;
			float	d;
			d = AngleDelta (ang[YAW], b->aim[YAW]) * gain;
			if (d > turnstep)  d = turnstep;
			if (d < -turnstep) d = -turnstep;
			b->aim[YAW] += d;
			d = AngleDelta (ang[PITCH], b->aim[PITCH]) * gain;
			if (d > turnstep)  d = turnstep;
			if (d < -turnstep) d = -turnstep;
			b->aim[PITCH] += d;
		}
	}
	else
	{
		err = (1.0f - skill) * 7.0f;
		if (aimtweak)
			err *= bot_aimerr->value;
		ang[YAW]   += crandom () * err;
		ang[PITCH] += crandom () * err;

		b->aim[YAW]   = ApproachAngle (b->aim[YAW],   ang[YAW],   turnstep);
		b->aim[PITCH] = ApproachAngle (b->aim[PITCH], ang[PITCH], turnstep);
	}

	firethresh = 3.0f + (1.0f - skill) * 12.0f;
	if (aimtweak)
		firethresh *= bot_aimfire->value;
	aimoff = (float)(fabs (AngleDelta (ang[YAW], b->aim[YAW]))
	               + fabs (AngleDelta (ang[PITCH], b->aim[PITCH])));

	// combat gaze-life (bot_gazelife): a human doesn't stare down one target --
	// between shots they flick to check a flank or glance where they're running,
	// then snap back to fire.  Run a brief, spaced glance: the VIEW leaves the
	// enemy (aim_look) while b->aim keeps tracking, and FIRE is held during the
	// glance (you don't shoot where you're not looking) -- so no view is ever
	// snapped/teleported back mid-glance, and when it ends the view glides home
	// and shooting resumes.  Adds the human look-away texture (45% of moving
	// frames vs the bot's 17%) for a small, bounded fire-downtime cost.
	b->aim_look[YAW]   = b->aim[YAW];		// default: hold the view on the aim
	b->aim_look[PITCH] = b->aim[PITCH];
	{
		qboolean	glancing = false;

		if (bot_gazelife->value != 0 && Bot_Humanized (b) && !b->flee
			&& level.time >= b->reaction_until)
		{
			if (level.time < b->cglance_until)
				glancing = true;
			else if (level.time >= b->cglance_next)
			{
				// start a brief glance off the aim (a flank/path check), or
				// toward where we're running.  Kept mild + spaced: an aggressive
				// version cost ~11% kills for no metric gain (the human
				// look-away is mostly BACKPEDAL, a movement trait, not a glance).
				float	spd = VectorLength (self->velocity);
				float	base, r = random ();
				if (spd > 120.0f && r < 0.4f)
					base = b->move_yaw;					// look where we're running
				else
				{
					float off = 45.0f + random () * 95.0f;	// 45-140 deg off aim
					base = b->aim[YAW] + (random () < 0.5f ? off : -off);
				}
				b->cglance_yaw   = base;
				b->cglance_pitch = crandom () * 7.0f;			// near level
				b->cglance_until = level.time + 0.14f + random () * 0.16f;
				b->cglance_next  = b->cglance_until + 1.1f + random () * 1.8f;
				glancing = true;
			}
			if (glancing)
			{
				b->aim_look[YAW]   = b->cglance_yaw;
				b->aim_look[PITCH] = b->cglance_pitch;
			}
		}

		// fire is held during a glance (you don't shoot where you aren't
		// looking); on non-glance frames aim_look == b->aim so the aim_view snap
		// on the firing frame is exact.
		if (!glancing && level.time >= b->reaction_until && aimoff < firethresh)
			cmd->buttons |= BUTTON_ATTACK;
	}

aim_held:
	// 40Hz view smoothing (bot_aimsmooth): b->aim is the aim the 10Hz decision
	// layer committed to; snapping the view to it on the keyframe and holding it
	// for the 3 off-frames makes the view lurch 10x/sec -- the "jerky/robotic"
	// aim.  Glide the sent view toward the committed aim every frame instead
	// (smooth pursuit, like a hand on a mouse).  Fire keys off b->aim vs the
	// bearing at FRAMESYNC, and when settled-on-target aim barely moves so the
	// view is already there -- accuracy is unchanged, only the between-commit
	// swing is smoothed.  At 10Hz (BOT_TICK_RATIO>=1) g=1: exact, a no-op.
	if (bot_aimsmooth->value != 0 && BOT_TICK_RATIO < 1.0f)
	{
		// gap-dependent gain: a BIG committed step (a 40-deg acquisition snap,
		// held 3 frames = the robotic jerk) glides over several frames; a SMALL
		// one (fine tracking, where the trigger pulls) is followed tightly so
		// the shot still goes where the 10Hz layer aimed.  g -> 1 as the gap
		// shrinks, so settled aim == b->aim (no accuracy cost).
		// glide toward aim_look (= b->aim while tracking, or a glance point
		// during a gaze-life look); fire is suppressed during a glance, so on a
		// firing frame aim_look is always b->aim and the snap below is exact.
		float	dy = AngleDelta (b->aim_look[YAW],   b->aim_view[YAW]);
		float	dp = AngleDelta (b->aim_look[PITCH], b->aim_view[PITCH]);
		float	gap = (float)(fabs (dy) + fabs (dp));
		float	g = 1.0f / (1.0f + gap * 0.06f);
		if (g < 0.30f) g = 0.30f;
		// on a firing frame the shot travels down the sent view, so snap it to
		// the committed aim -- accuracy is never traded for smoothness, only the
		// between-shot swing glides (firing frames are a fraction of combat)
		if (cmd->buttons & BUTTON_ATTACK) g = 1.0f;
		b->aim_view[YAW]   += dy * g;
		b->aim_view[PITCH] += dp * g;
		*facing_yaw   = b->aim_view[YAW];
		*facing_pitch = b->aim_view[PITCH];
	}
	else
	{
		*facing_yaw   = b->aim[YAW];
		*facing_pitch = b->aim[PITCH];
	}

	// --- blend combat movement into the existing (nav) move intent ---
	// not while boarding/riding a lift: the lift controller owns movement
	// there (a dodge would carry the bot off the plat); aiming and firing
	// above still apply -- movement/aim decoupling makes that free.  The
	// bot_lift check matters: turning the cvar off mid-run strands
	// lift_state (nothing else advances it), which must not keep gating this.
	if (bot_lift->value != 0
		&& (b->lift_state == LIFT_BOARD || b->lift_state == LIFT_RIDE))
		return true;

	// (combat movement stays per-tick: an A/B of keyframing this blend showed
	// no lethality change and a pickups cost -- the 40Hz "body" keeps its
	// smooth dodging; only the decision layer above is keyframed)
	VectorSubtract (enemy->s.origin, self->s.origin, toenemy);
	toenemy[2] = 0;
	VectorNormalize (toenemy);

	// humanization (Phase 4): combat movement rhythm from the demo stats
	{
		qboolean	rhythm = (bot_hop->value != 0 && Bot_Humanized (b));
		float		sw, navw;

		if (level.time >= b->dodge_until)
		{
			b->dodge_dir = (random () < 0.5f) ? -1 : 1;
			if (rhythm)
			{
				// strafe legs from the human reversal-interval distribution
				// (right-skewed: p50 0.7s, p90 2.3s) instead of uniform
				// 0.5-1.1s -- short jinks mixed with long committed runs
				float u = random ();
				if (u > 0.98f)
					u = 0.98f;
				b->dodge_until = level.time + 0.25f - 0.65f * (float)log (1.0 - u);
				b->dodge_flip_time = level.time;
			}
			else
				b->dodge_until = level.time + 0.5f + random () * 0.6f;
		}
		strafe[0] = -toenemy[1] * b->dodge_dir;	// perpendicular to enemy dir
		strafe[1] =  toenemy[0] * b->dodge_dir;
		strafe[2] = 0;

		// momentum: a fresh reversal starts slow (humans can't flip
		// velocity in one tick; the stock bot does)
		sw = 1.0f;
		if (rhythm && level.time - b->dodge_flip_time < 0.2f)
			sw = 0.5f;

		// approach if far, back off if too close; when fleeing, always
		// disengage (movement is decoupled from aim, so the bot retreats
		// while firing back)
		if (b->flee)
		{
			rc = -0.9f;
			comb[0] = b->move_dir[0] * 1.0f + strafe[0] * 0.5f * sw + toenemy[0] * rc;
			comb[1] = b->move_dir[1] * 1.0f + strafe[1] * 0.5f * sw + toenemy[1] * rc;
		}
		else
		{
			rc = (range < 200) ? -0.8f : (range > 650) ? 0.6f : 0.0f;

			// commit to close fights instead of half-jogging toward the nav
			// goal mid-duel
			navw = (rhythm && range < 250) ? 0.45f : 0.7f;

			// nav goal (already in move_dir) + dodge + range adjustment
			comb[0] = b->move_dir[0] * navw + strafe[0] * 0.7f * sw + toenemy[0] * rc;
			comb[1] = b->move_dir[1] * navw + strafe[1] * 0.7f * sw + toenemy[1] * rc;
		}
		comb[2] = 0;
		if (VectorLength (comb) < 0.1f)
			VectorCopy (strafe, comb);
		VectorNormalize (comb);
		VectorCopy (comb, b->move_dir);

		// humans jump a LOT in Q2 fights (demo corpus: ~15 jumps/min overall
		// vs the stock bot's ~4); airborne targets are also harder to hit.
		// per-tick probability, tick-rate scaled to keep jumps/min constant
		if (self->groundentity && random () < (rhythm ? 0.09f : 0.03f) * BOT_TICK_RATIO)
			b->want_jump = true;
	}

	return true;
}
