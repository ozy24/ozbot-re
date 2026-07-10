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
cvar_t	*bot_dodge;			// directed rocket dodge: sidestep an incoming rocket's
							// path (perpendicular to its travel).  RE-TESTED at 40Hz
							// (25ms ticks) after two 10Hz rejections -- and rejected a
							// THIRD time: 16-seed paired parity, aggressive step kills
							// -40 kill-differential for -3 deaths; gentle step -25
							// kills AND +14 deaths.  The existing constant combat
							// strafe (bot_hop) already captures evasion; a directed
							// step only disrupts offense and a ~90u step in 0.3s
							// rarely clears the ~150u splash.  Default OFF (infra kept
							// as a "don't re-try rocket dodging" marker, like bot_survive).
cvar_t	*bot_dodgetest;		// id-parity A/B for bot_dodge (even ids dodge, odd control)
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
cvar_t	*bot_wpntactic;		// weapon-aware combat: engagement range + style per weapon
cvar_t	*bot_wpntactictest;	// id-parity A/B for bot_wpntactic (even ids get it)
cvar_t	*bot_wpnlog;		// per-engagement telemetry (weapon/range/intent)
cvar_t	*bot_wpnselect;		// range-aware firing-weapon choice (demo kill-range bands)
cvar_t	*bot_wpnselecttest;	// id-parity A/B for bot_wpnselect (even ids get it)
cvar_t	*bot_wpnsellog;		// diagnostic: chosen weapon vs target distance
cvar_t	*bot_blastertransit;	// blaster-only: keep going to the weapon/armor goal, fire defensively
cvar_t	*bot_blastertransittest;	// id-parity A/B for bot_blastertransit (even ids get it)
cvar_t	*bot_watersight;	// the water surface is opaque: no sightline across it
cvar_t	*bot_aimlog;		// per-shot aim-error telemetry (calibration diagnostic)
cvar_t	*bot_aimprec;		// scale precision-weapon aim error toward human (0=off, 1=full)
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
Combat_WaterSurfaceBlocks  (bot_watersight)

The water surface is opaque: you cannot see (or shoot) across it.  The engine's
visible() sightline is a MASK_OPAQUE trace, which passes straight THROUGH liquid
volumes -- so a bot with its eyes underwater could acquire (and railgun) a target
standing on the steps, and a bot on the steps could pick off someone submerged.
Block the sightline whenever the two eye points sit on opposite sides of a liquid
boundary (one submerged, one not): the line between them must then cross the
surface.  A bot in the same medium as its target (both underwater, or both in
air) is unaffected, so the off-state is byte-identical.
=================
*/
static qboolean Combat_WaterSurfaceBlocks (edict_t *self, edict_t *other)
{
	vec3_t		e1, e2;
	qboolean	w1, w2;

	if (!bot_watersight || bot_watersight->value == 0)
		return false;
	VectorCopy (self->s.origin, e1);
	e1[2] += self->viewheight;
	VectorCopy (other->s.origin, e2);
	e2[2] += other->viewheight;
	w1 = (gi.pointcontents (e1) & MASK_WATER) != 0;
	w2 = (gi.pointcontents (e2) & MASK_WATER) != 0;
	return w1 != w2;		// eyes on opposite sides of the surface -> no sightline
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
		if (Combat_WaterSurfaceBlocks (self, o))
			continue;			// opaque water surface between our eyes and theirs
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
Combat_BlasterOnly

True when the bot has NOTHING better than the blaster to fight with -- it owns no
other weapon it currently has ammo for (a fresh spawn, or shot dry).  In that
state committing to a fight is a bad trade: the right play is to keep travelling
to the weapon/armor goal and fire the blaster only defensively en route.
=================
*/
static qboolean Combat_BlasterOnly (edict_t *ent)
{
	static const char *better[] = {
		"railgun", "rocket launcher", "hyperblaster", "chaingun",
		"super shotgun", "machinegun", "shotgun"
	};
	int	i;

	for (i = 0; i < 7; i++)
	{
		gitem_t	*w = FindItem ((char *)better[i]);
		if (w && Combat_HasWeaponAmmo (ent, w))
			return false;		// has a real weapon to fight with
	}
	return true;				// only the blaster
}

/*
=================
Combat_SelectWeapon

Switch to the best weapon we own and have ammo for (engine handles the actual
swap via client->newweapon on the next weapon think).
=================
*/
// both defined later in the file (after Combat_SelectWeapon)
static qboolean Combat_WpnSelectOn (bot_t *b);
static void Combat_WeaponProfile (gitem_t *w, float *lo, float *hi, float *bias);

static void Combat_SelectWeapon (bot_t *b)
{
	static const char *prio[] = {
		"railgun", "rocket launcher", "hyperblaster", "chaingun",
		"super shotgun", "machinegun", "shotgun", "blaster"
	};
	edict_t	*ent = b->ent;
	int		i;

	// range-aware selection (bot_wpnselect): among the weapons we own + have ammo
	// for, pick the one whose demo kill-range band [lo,hi] best fits the current
	// distance to the target (Combat_WeaponProfile -- the same bands bot_wpntactic
	// positions to).  A weapon in-band scores 0; otherwise its penalty is how far
	// the target sits outside the band.  prio[] is scanned strong-first with a
	// strict-less compare, so genuine ties (e.g. several weapons in-band at mid
	// range) still break toward the stronger weapon -- reproducing the old
	// rail-first choice everywhere EXCEPT where distance makes it wrong (a foe in
	// your face drops the railgun for SSG/RL/chaingun).  Off-state falls straight
	// through to the fixed priority below, byte-identical.
	if (Combat_WpnSelectOn (b) && b->enemy && b->enemy->inuse)
	{
		vec3_t	d;
		float	dist, bestpen = 1e18f;
		gitem_t	*best = NULL;

		VectorSubtract (b->enemy->s.origin, ent->s.origin, d);
		dist = VectorLength (d);
		for (i = 0; i < 8; i++)
		{
			gitem_t	*w = FindItem ((char *)prio[i]);
			float	lo, hi, bias, pen;

			if (!w || !Combat_HasWeaponAmmo (ent, w))
				continue;
			Combat_WeaponProfile (w, &lo, &hi, &bias);
			pen = (dist < lo) ? (lo - dist) : (dist > hi) ? (dist - hi) : 0.0f;
			if (pen < bestpen)
			{
				bestpen = pen;
				best = w;
			}
		}
		// hysteresis: re-selecting every 10Hz keyframe makes a target dancing
		// across a band edge flip the weapon repeatedly, and each raise/lower
		// animation is dead time the bot can't fire.  Stay on the held weapon
		// unless a candidate fits the current distance markedly better (by
		// WPNSEL_HYST units) -- so only a decisive range change (a foe closing
		// into your face) forces the swap, not boundary jitter.
		#define WPNSEL_HYST	100.0f
		if (best)
		{
			gitem_t	*held = ent->client->pers.weapon;
			if (held && held != best && Combat_HasWeaponAmmo (ent, held))
			{
				float	lo, hi, bias, hpen;
				Combat_WeaponProfile (held, &lo, &hi, &bias);
				hpen = (dist < lo) ? (lo - dist) : (dist > hi) ? (dist - hi) : 0.0f;
				if (hpen <= bestpen + WPNSEL_HYST)
					best = held;		// held is still good enough; don't thrash
			}
			if (bot_wpnsellog && bot_wpnsellog->value != 0)
				Bot_LogWpnSel (b, best->pickup_name ? best->pickup_name : "",
					(ent->client->pers.weapon && ent->client->pers.weapon->pickup_name)
						? ent->client->pers.weapon->pickup_name : "", dist);
			if (ent->client->pers.weapon != best && ent->client->newweapon != best)
				ent->client->newweapon = best;
			return;
		}
	}

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
Combat_AmmoFracForItem

Fractional ammo fill (0..1) for the highest-PRIORITY owned weapon that consumes
`ammo_item`, or -1 if the bot owns no weapon that uses it (so the goal scorer
keeps ammo need low instead of hoarding fuel for guns it lacks).  frac = current
ammo / T, clamped to 1, where T is the "comfortable" fill for that ammo type --
the p50 ammo count at which pros actually refilled it (tools/dm2_combat.py need;
demos/derived/combat_need/thresholds.json, 5859 demos: rockets 8, slugs 16,
shells 10, grenades 6, cells 50, bullets 103).  Scanning prio[] in order means
the fill reported is for the best gun this ammo feeds (a near-empty rail's slugs
outrank a full shotgun's shells).
=================
*/
float Combat_AmmoFracForItem (edict_t *ent, gitem_t *ammo_item)
{
	static const char *prio[] = {
		"railgun", "rocket launcher", "hyperblaster", "chaingun",
		"super shotgun", "machinegun", "shotgun", "blaster"
	};
	static const struct { const char *name; float thresh; } tbl[] = {
		{ "Rockets", 8.0f }, { "Slugs", 16.0f }, { "Cells", 50.0f },
		{ "Bullets", 103.0f }, { "Shells", 10.0f }, { "Grenades", 6.0f },
	};
	const char *an = ammo_item->pickup_name ? ammo_item->pickup_name : "";
	float	thresh = 0.0f;
	int		i;

	for (i = 0; i < (int)(sizeof(tbl) / sizeof(tbl[0])); i++)
		if (strstr (an, tbl[i].name)) { thresh = tbl[i].thresh; break; }
	if (thresh <= 0.0f)
		return -1.0f;			// not an ammo type we have a threshold for

	for (i = 0; i < 8; i++)
	{
		gitem_t	*w = FindItem ((char *)prio[i]);
		float	frac;
		int		cur;

		if (!w || ent->client->pers.inventory[ITEM_INDEX(w)] <= 0)
			continue;			// don't own this weapon
		if (!w->ammo || FindItem (w->ammo) != ammo_item)
			continue;			// this weapon doesn't consume this ammo
		cur = ent->client->pers.inventory[ITEM_INDEX(ammo_item)];
		frac = cur / thresh;
		if (frac > 1.0f) frac = 1.0f;
		if (frac < 0.0f) frac = 0.0f;
		return frac;
	}
	return -1.0f;				// no owned weapon consumes this ammo
}

// engagement intent (bot_wpnlog telemetry; the movement bias is band-driven)
#define ENG_HOLD		0	// in the preferred band: strafe, keep range
#define ENG_PRESS		1	// beyond the band: close in
#define ENG_REPOSITION	2	// inside the band's near edge: give ground to it
#define ENG_BREAK		3	// held weapon dry: disengage
#define ENG_RETREAT		4	// fleeing (outmatched)

static float Clampf (float v, float lo, float hi)
{
	return (v < lo) ? lo : (v > hi) ? hi : v;
}

/*
=================
Combat_Tactic

Whether this bot runs weapon-aware combat tactics this frame.  bot_wpntactictest
gives the id-parity head-to-head (even ids get it, odd are the control);
otherwise bot_wpntactic.  Gated by Bot_Humanized, like the other combat-feel
behaviors, so it travels with the humanization profile.
=================
*/
static qboolean Combat_Tactic (bot_t *b)
{
	qboolean on = (bot_wpntactictest && bot_wpntactictest->value != 0)
		? ((b->id & 1) == 0)
		: (bot_wpntactic && bot_wpntactic->value != 0);
	return on && Bot_Humanized (b);
}

/*
=================
Combat_WpnSelectOn

Whether this bot runs range-aware firing-weapon selection this frame
(bot_wpnselect).  bot_wpnselecttest gives the id-parity head-to-head (even ids
get it, odd are the control); otherwise bot_wpnselect.  Gated by Bot_Humanized
so it travels with the humanization profile, exactly like Combat_Tactic.
=================
*/
static qboolean Combat_WpnSelectOn (bot_t *b)
{
	qboolean on = (bot_wpnselecttest && bot_wpnselecttest->value != 0)
		? ((b->id & 1) == 0)
		: (bot_wpnselect && bot_wpnselect->value != 0);
	return on && Bot_Humanized (b);
}

/*
=================
Combat_BlasterTransit  (bot_blastertransit)

Whether this bot, RIGHT NOW, should treat a fight as a nuisance to travel through
rather than commit to -- true only when it has nothing but the blaster to fire.
The behavior (in the Combat_Aim movement blend): hold full nav weight toward the
weapon/armor goal and drop the range-closing pull, so the bot keeps heading to a
real weapon while its aim still tracks and fires the blaster defensively (movement
is relative to facing, so it back-pedals/strafes to the goal while shooting back).
bot_blastertransittest gives the id-parity head-to-head; gated by Bot_Humanized
like the other combat-feel levers.  A bot that already owns a real weapon is
unaffected (Combat_BlasterOnly false), so the off-state is unchanged.
=================
*/
static qboolean Combat_BlasterTransit (bot_t *b)
{
	qboolean on = (bot_blastertransittest && bot_blastertransittest->value != 0)
		? ((b->id & 1) == 0)
		: (bot_blastertransit && bot_blastertransit->value != 0);
	return on && Bot_Humanized (b) && Combat_BlasterOnly (b->ent);
}

/*
=================
Combat_WeaponProfile

Preferred engagement band [lo,hi] (world units) and style bias for a weapon,
calibrated from the pro demo corpus (tools/dm2_combat.py tactics over 5859 demos:
kill-range p25/p75 for the band; advance-vs-retreat tendency for the bias).
Railgun holds long (238-606); the rest brawl mid-close; chaingun/super shotgun
press (bias>0), railgun/grenade launcher give ground (bias<0).
=================
*/
static void Combat_WeaponProfile (gitem_t *w, float *lo, float *hi, float *bias)
{
	const char *nm = (w && w->pickup_name) ? w->pickup_name : "";
	if      (strstr (nm, "Railgun"))         { *lo = 238; *hi = 606; *bias = -0.10f; }
	else if (strstr (nm, "Super Shotgun"))   { *lo = 139; *hi = 328; *bias =  0.20f; }
	else if (strstr (nm, "Rocket"))          { *lo = 134; *hi = 335; *bias =  0.10f; }
	else if (strstr (nm, "Chaingun"))        { *lo = 157; *hi = 362; *bias =  0.25f; }
	else if (strstr (nm, "Hyper"))           { *lo = 130; *hi = 311; *bias =  0.15f; }
	// GL lobs from range: its kill-range is close (splash lands near the victim)
	// but players POSITION back, so anchor a far hold band, not the kill-range.
	else if (strstr (nm, "Grenade Launcher")){ *lo = 350; *hi = 800; *bias = -0.20f; }
	else if (strstr (nm, "Machinegun"))      { *lo = 171; *hi = 398; *bias =  0.00f; }
	else if (strstr (nm, "BFG"))             { *lo = 106; *hi = 297; *bias =  0.15f; }
	else                                     { *lo = 139; *hi = 339; *bias =  0.00f; } // shotgun/blaster
}

/*
=================
Combat_HeldLowAmmo

True if the weapon in hand is nearly out of ammo (below the demo refill fraction)
-- the cue to break off instead of dancing in the enemy's face empty.  Ammo-less
weapons (blaster) are never "low".
=================
*/
static qboolean Combat_HeldLowAmmo (edict_t *ent)
{
	gitem_t	*w = ent->client->pers.weapon;
	gitem_t	*am;
	float	f;

	if (!w || !w->ammo)
		return false;
	am = FindItem (w->ammo);
	if (!am)
		return false;
	f = Combat_AmmoFracForItem (ent, am);
	return (f >= 0.0f && f < 0.25f);
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

// bot_aimprec calibration (at strength 1.0): the aim-error multiplier for a
// precision weapon is 1 + wprec * (BASE + RANGE*range/1000 + LAT*lat/300), so a
// railgun (wprec 1) against a still target at 500u gets ~BASE+RANGE/2, and more
// far/against strafers -- restoring the human range+motion degradation the
// weapon-agnostic error lacks.  Tuned by sweeping bot_aimprec vs the human curve
// (demos/derived/combat_aim); see the ozbot-re aim-accuracy memory.
#define AIMPREC_BASE	1.6f
#define AIMPREC_RANGE	1.4f
#define AIMPREC_LAT		2.2f

/*
=================
Combat_WeaponPrec  (bot_aimprec)

How much a weapon's aim error should scale toward human fallibility.  Precision
hitscan / fast-bolt weapons (railgun, blaster, hyperblaster) read as uncanny
because the aim error is weapon-agnostic and tiny, so a thin beam / fast bolt
essentially never misses; the demo corpus shows humans miss these FAR more,
especially at range and against moving targets (demos/derived/combat_aim).
Spread (shotgun) and splash (rocket/GL) weapons hide the error, so they don't
read as uncanny -- leave them at 0 (adding error there is a pure lethality
loss for no feel gain).  Chaingun/machinegun are hitscan but spray-fire, so a
mild factor only.
=================
*/
static float Combat_WeaponPrec (gitem_t *w)
{
	const char *nm = (w && w->pickup_name) ? w->pickup_name : "";
	if (strstr (nm, "Railgun"))			return 1.00f;
	if (strstr (nm, "HyperBlaster"))	return 0.70f;	// before "Blaster" (substring)
	if (strstr (nm, "Blaster"))			return 0.85f;
	if (strstr (nm, "Chaingun") || strstr (nm, "Machinegun"))	return 0.30f;
	return 0.0f;
}

/*
=================
Combat_TargetLateral

Horizontal speed of 'enemy' perpendicular to the line of sight (the strafe
component a shooter has to track).  Matches the bot_aimlog / demo metric.
=================
*/
static float Combat_TargetLateral (edict_t *self, edict_t *enemy)
{
	vec3_t	los;
	float	horiz, along, px, py;

	VectorSubtract (enemy->s.origin, self->s.origin, los);
	horiz = (float)sqrt (los[0]*los[0] + los[1]*los[1]);
	if (horiz < 1.0f)
		return 0.0f;
	along = (enemy->velocity[0]*los[0] + enemy->velocity[1]*los[1]) / horiz;
	px = enemy->velocity[0] - along * los[0] / horiz;
	py = enemy->velocity[1] - along * los[1] / horiz;
	return (float)sqrt (px*px + py*py);
}

/*
=================
Combat_RocketThreat  (bot_dodge)

Scan for an incoming enemy rocket whose flight path passes close to this bot;
if found, set a world-space step-away direction (perpendicular to the rocket's
horizontal travel, on the side that widens the miss) and a short commit window.
The 10Hz brain could never time a sidestep (100ms reaction, rejected twice);
the 40Hz body has 25ms granularity, so this is the re-test.  Returns true if a
fresh threat set a dodge this tick.
=================
*/
#define DODGE_RANGE			650.0f	// only rockets within this many units matter
#define DODGE_MISS_DIST		150.0f	// step if the path passes within this of us
#define DODGE_CLOSING		0.5f	// rocket heading roughly toward us (cos)
#define DODGE_MAX_TTI		1.0f	// only react inside ~1s to impact
#define DODGE_HOLD			0.30f	// commit to a chosen step this long

static qboolean Combat_RocketThreat (edict_t *self, bot_t *b)
{
	edict_t	*e, *best = NULL;
	float	best_tti = 99999.0f;
	int		i;
	vec3_t	rvel, rel, perp, torus, vn, closest;
	float	speed, tti, miss, side;

	for (i = (int)game.maxclients + 1; i < globals.num_edicts; i++)
	{
		e = g_edicts + i;
		if (!e->inuse || !e->classname || strcmp (e->classname, "rocket") != 0)
			continue;
		if (e->owner == self)		// don't dodge our own rocket
			continue;

		VectorSubtract (e->s.origin, self->s.origin, rel);	// rocket - self
		if (VectorLength (rel) > DODGE_RANGE)
			continue;

		VectorCopy (e->velocity, rvel);
		speed = VectorLength (rvel);
		if (speed < 1.0f)
			continue;

		// closing? rocket velocity direction vs direction from rocket to us
		VectorScale (rel, -1.0f, torus);
		VectorNormalize (torus);
		VectorScale (rvel, 1.0f / speed, vn);
		if (DotProduct (vn, torus) < DODGE_CLOSING)
			continue;

		// time to closest approach and the miss distance at that time.
		// p(t) = rel + rvel*t ; |p| minimized at t = -(rel.rvel)/|rvel|^2
		tti = -DotProduct (rel, rvel) / (speed * speed);
		if (tti < 0.0f || tti > DODGE_MAX_TTI)
			continue;
		VectorMA (rel, tti, rvel, closest);
		miss = VectorLength (closest);
		if (miss > DODGE_MISS_DIST)
			continue;

		if (tti < best_tti)
		{
			best_tti = tti;
			best = e;
		}
	}

	if (!best)
		return false;

	// horizontal perpendicular to the rocket's travel; step toward the side we
	// are already on so the miss widens (dead-on picks a stable side by id)
	VectorCopy (best->velocity, rvel);
	rvel[2] = 0;
	if (VectorNormalize (rvel) < 1.0f)
		return false;
	perp[0] = -rvel[1];
	perp[1] =  rvel[0];
	perp[2] = 0;

	VectorSubtract (self->s.origin, best->s.origin, rel);
	side = DotProduct (rel, perp);
	if (side < 0.0f || (side == 0.0f && (b->id & 1)))
		VectorScale (perp, -1.0f, perp);

	VectorCopy (perp, b->dodge_rkt_dir);
	b->dodge_rkt_until = level.time + DODGE_HOLD;
	return true;
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
	float	turnstep, firethresh, reaction, err, range, aimoff, rc, precm;
	qboolean aimtweak;
	qboolean blaster_transit = false;	// blaster-only: travel to a real weapon, fire defensively

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
			qboolean survive = Bot_Survives (b);
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

	// weapon-aware engagement intent (10Hz-committed): the per-tick movement
	// blend below reads eng_lo/eng_hi/eng_bias to hold each weapon at its
	// demo-preferred range and press / give ground per its style, replacing the
	// old weapon-blind 200-650 dead-band that made every weapon circle alike.
	if (Combat_Tactic (b))
	{
		float lo, hi, bias;
		Combat_WeaponProfile (self->client->pers.weapon, &lo, &hi, &bias);
		if (b->flee)
			b->eng_intent = ENG_RETREAT;
		else if (Combat_HeldLowAmmo (self))
		{
			bias -= 0.4f;					// out of ammo: disengage
			b->eng_intent = ENG_BREAK;
		}
		else
			b->eng_intent = (range > hi) ? ENG_PRESS
			              : (range < lo) ? ENG_REPOSITION : ENG_HOLD;
		b->eng_lo = lo;
		b->eng_hi = hi;
		b->eng_bias = bias;
	}
	else
	{
		b->eng_lo = b->eng_hi = b->eng_bias = 0.0f;
		b->eng_intent = ENG_HOLD;
	}

	// bot_wpnlog: record the engagement range/weapon/intent at 10Hz (gated, so
	// off-state telemetry is unchanged).  Logged regardless of bot_wpntactic so
	// the baseline (tactic off) range distribution is measurable for the A/B.
	if (bot_wpnlog && bot_wpnlog->value != 0)
		Bot_LogEngage (b,
			(self->client->pers.weapon && self->client->pers.weapon->pickup_name)
				? self->client->pers.weapon->pickup_name : "Blaster",
			range, b->eng_intent);

	// bot_aimprec: scale precision-weapon (railgun/blaster) aim error up toward
	// human fallibility, with a range + target-lateral-speed term (the demo
	// corpus shows humans miss these far more, and more when far / vs strafers).
	// Applied to the error magnitude in both aim paths below.  precm==1 when off
	// or for spread/splash weapons, so the off-state is byte-identical.
	{
		float precm_wp;
		precm = 1.0f;
		if (bot_aimprec->value != 0
			&& (precm_wp = Combat_WeaponPrec (self->client->pers.weapon)) > 0.0f)
		{
			float lat = Combat_TargetLateral (self, enemy);
			precm = 1.0f + bot_aimprec->value * precm_wp
				* (AIMPREC_BASE + AIMPREC_RANGE * range / 1000.0f
				   + AIMPREC_LAT * lat / 300.0f);
		}
	}

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
		sigma *= precm;		// bot_aimprec: precision-weapon error scale-up
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
		err *= precm;		// bot_aimprec: precision-weapon error scale-up
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
		{
			cmd->buttons |= BUTTON_ATTACK;

			// bot_aimlog: record this shot's aim error vs the enemy's TRUE
			// bearing (no lead, matching the human demo metric) + target lateral
			// speed, so the bot's accuracy distribution is comparable to
			// demos/derived/combat_aim.  Gated -> off-state telemetry unchanged.
			if (bot_aimlog->value != 0)
			{
				vec3_t	tb, teye;
				float	tby, tbp, horiz, latsp = 0.0f, rng;
				VectorCopy (enemy->s.origin, teye);
				teye[2] += enemy->viewheight;
				VectorSubtract (teye, eyes, tb);
				horiz = (float)sqrt (tb[0]*tb[0] + tb[1]*tb[1]);
				rng   = VectorLength (tb);
				tby = (horiz > 1.0f) ? (float)(atan2 (tb[1], tb[0]) * 57.29578)
				                     : b->aim[YAW];
				tbp = (horiz > 1.0f) ? -(float)(atan2 (tb[2], horiz) * 57.29578)
				                     : b->aim[PITCH];
				if (horiz > 1.0f)
				{
					float hx = tb[0] / horiz, hy = tb[1] / horiz;
					float along = enemy->velocity[0]*hx + enemy->velocity[1]*hy;
					float px = enemy->velocity[0] - along*hx;
					float py = enemy->velocity[1] - along*hy;
					latsp = (float)sqrt (px*px + py*py);
				}
				Bot_LogAimShot (b,
					(self->client->pers.weapon
					 && self->client->pers.weapon->pickup_name)
						? self->client->pers.weapon->pickup_name : "Blaster",
					rng, latsp,
					AngleDelta (b->aim[YAW], tby),
					AngleDelta (b->aim[PITCH], tbp));
			}
		}
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
		qboolean	rkt_dodge = false;
		float		sw, navw, sfw;

		// bot_dodge: an incoming rocket overrides the strafe with a directed
		// sidestep out of its path (perpendicular to the rocket's travel).
		// Bot_Dodges folds in both the cvar and the id-parity test harness.
		if (Bot_Dodges (b))
		{
			Combat_RocketThreat (self, b);		// refreshes dir/until on a threat
			rkt_dodge = (level.time < b->dodge_rkt_until);
		}

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
		if (rkt_dodge)
		{
			// step out of the rocket's line, not perpendicular to the enemy
			strafe[0] = b->dodge_rkt_dir[0];
			strafe[1] = b->dodge_rkt_dir[1];
			strafe[2] = 0;
		}
		else
		{
			strafe[0] = -toenemy[1] * b->dodge_dir;	// perpendicular to enemy dir
			strafe[1] =  toenemy[0] * b->dodge_dir;
			strafe[2] = 0;
		}

		// momentum: a fresh reversal starts slow (humans can't flip
		// velocity in one tick; the stock bot does).  A rocket dodge commits
		// at full speed -- it's an evasion, not a rhythm jink.
		sw = 1.0f;
		if (!rkt_dodge && rhythm && level.time - b->dodge_flip_time < 0.2f)
			sw = 0.5f;

		// approach if far, back off if too close; when fleeing, always
		// disengage (movement is decoupled from aim, so the bot retreats
		// while firing back)
		blaster_transit = Combat_BlasterTransit (b);
		if (b->flee)
		{
			rc = -0.9f;
			sfw = rkt_dodge ? 0.9f : 0.5f;	// rocket dodge biases the strafe
			comb[0] = b->move_dir[0] * 1.0f + strafe[0] * sfw * sw + toenemy[0] * rc;
			comb[1] = b->move_dir[1] * 1.0f + strafe[1] * sfw * sw + toenemy[1] * rc;
		}
		else if (blaster_transit)
		{
			// only the blaster to fight with (fresh spawn / shot dry): do NOT
			// commit -- keep full nav weight toward the weapon/armor goal and drop
			// the range-closing pull, so the bot travels to a real weapon while its
			// aim still tracks and fires the blaster defensively.  Movement is
			// relative to facing, so it back-pedals/strafes to the goal while
			// shooting back.  A light strafe keeps it from being a straight-line
			// sitting duck; a rocket dodge still gets its full step.
			rc = 0.0f;
			navw = 1.0f;
			sfw = rkt_dodge ? 0.9f : 0.35f;
			b->eng_intent = ENG_BREAK;
			comb[0] = b->move_dir[0] * navw + strafe[0] * sfw * sw + toenemy[0] * rc;
			comb[1] = b->move_dir[1] * navw + strafe[1] * sfw * sw + toenemy[1] * rc;
		}
		else
		{
			if (Combat_Tactic (b) && b->eng_hi > 0.0f)
			{
				// weapon-calibrated (eng_* committed at 10Hz): back off below the
				// band's near edge, close above the far edge, strafe in-band; then
				// add the weapon's style bias (chaingun/SSG keep +rc pressure,
				// railgun/grenade launcher give ground).  This replaces the old
				// weapon-blind 200-650 dead-band that made every weapon circle.
				float lo = b->eng_lo, hi = b->eng_hi;
				if (range < lo)
					rc = -0.8f * Clampf ((lo - range) / (lo + 1.0f), 0.0f, 1.0f);
				else if (range > hi)
					rc =  0.8f * Clampf ((range - hi) / (hi + 1.0f), 0.0f, 1.0f);
				else
					rc = 0.0f;
				rc = Clampf (rc + b->eng_bias, -0.9f, 0.9f);
			}
			else
				rc = (range < 200) ? -0.8f : (range > 650) ? 0.6f : 0.0f;

			// commit to close fights instead of half-jogging toward the nav
			// goal mid-duel
			navw = (rhythm && range < 250) ? 0.45f : 0.7f;

			// nav goal (already in move_dir) + dodge + range adjustment.
			// a rocket dodge biases the strafe toward the step-away line
			// (gentle: a hard commit wrecks offense for little splash gain)
			sfw = rkt_dodge ? 1.0f : 0.7f;
			comb[0] = b->move_dir[0] * navw + strafe[0] * sfw * sw + toenemy[0] * rc;
			comb[1] = b->move_dir[1] * navw + strafe[1] * sfw * sw + toenemy[1] * rc;
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
