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
Combat_FindEnemy

Nearest visible, living player/bot (deathmatch: everyone is fair game).
=================
*/
static edict_t *Combat_FindEnemy (bot_t *b)
{
	edict_t	*self = b->ent;
	edict_t	*best = NULL;
	float	bestd = 1e18f;
	int		i;

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
			if (b->flee)
			{
				if (mine > 100 || mine > theirs * 0.95f)
					b->flee = false;	// recovered (or they got hurt too)
			}
			else
			{
				if (mine < 75 && mine < theirs * 0.65f)
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

	err = (1.0f - skill) * 7.0f;
	if (aimtweak)
		err *= bot_aimerr->value;
	ang[YAW]   += crandom () * err;
	ang[PITCH] += crandom () * err;

	turnstep = 20.0f + skill * 40.0f;
	if (aimtweak)
		turnstep *= bot_aimturn->value;
	b->aim[YAW]   = ApproachAngle (b->aim[YAW],   ang[YAW],   turnstep);
	b->aim[PITCH] = ApproachAngle (b->aim[PITCH], ang[PITCH], turnstep);

	*facing_yaw   = b->aim[YAW];
	*facing_pitch = b->aim[PITCH];

	firethresh = 3.0f + (1.0f - skill) * 12.0f;
	if (aimtweak)
		firethresh *= bot_aimfire->value;
	aimoff = (float)(fabs (AngleDelta (ang[YAW], b->aim[YAW]))
	               + fabs (AngleDelta (ang[PITCH], b->aim[PITCH])));
	if (level.time >= b->reaction_until && aimoff < firethresh)
		cmd->buttons |= BUTTON_ATTACK;

	// --- blend combat movement into the existing (nav) move intent ---
	// not while boarding/riding a lift: the lift controller owns movement
	// there (a dodge would carry the bot off the plat); aiming and firing
	// above still apply -- movement/aim decoupling makes that free.  The
	// bot_lift check matters: turning the cvar off mid-run strands
	// lift_state (nothing else advances it), which must not keep gating this.
	if (bot_lift->value != 0
		&& (b->lift_state == LIFT_BOARD || b->lift_state == LIFT_RIDE))
		return true;

	VectorSubtract (enemy->s.origin, self->s.origin, toenemy);
	toenemy[2] = 0;
	VectorNormalize (toenemy);

	if (level.time >= b->dodge_until)
	{
		b->dodge_dir = (random () < 0.5f) ? -1 : 1;
		b->dodge_until = level.time + 0.5f + random () * 0.6f;
	}
	strafe[0] = -toenemy[1] * b->dodge_dir;	// perpendicular to enemy dir
	strafe[1] =  toenemy[0] * b->dodge_dir;
	strafe[2] = 0;

	// approach if far, back off if too close; when fleeing, always disengage
	// (movement is decoupled from aim, so the bot retreats while firing back)
	if (b->flee)
	{
		rc = -0.9f;
		comb[0] = b->move_dir[0] * 1.0f + strafe[0] * 0.5f + toenemy[0] * rc;
		comb[1] = b->move_dir[1] * 1.0f + strafe[1] * 0.5f + toenemy[1] * rc;
	}
	else
	{
		rc = (range < 200) ? -0.8f : (range > 650) ? 0.6f : 0.0f;

		// nav goal (already in move_dir) + dodge + range adjustment
		comb[0] = b->move_dir[0] * 0.7f + strafe[0] * 0.7f + toenemy[0] * rc;
		comb[1] = b->move_dir[1] * 0.7f + strafe[1] * 0.7f + toenemy[1] * rc;
	}
	comb[2] = 0;
	if (VectorLength (comb) < 0.1f)
		VectorCopy (strafe, comb);
	VectorNormalize (comb);
	VectorCopy (comb, b->move_dir);

	if (self->groundentity && random () < 0.03f)
		b->want_jump = true;

	return true;
}
