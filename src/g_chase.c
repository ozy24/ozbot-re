/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
#include "g_local.h"

static void Chase_ClearGun (gclient_t *cl)
{
	cl->ps.gunindex = 0;
	cl->ps.gunframe = 0;
	VectorClear (cl->ps.kick_angles);
}

static void Chase_SetClientNum (edict_t *ent)
{
	gclient_t	*cl = ent->client;
	int			selfnum = ent - g_edicts - 1;

	if (cl->chase_target && cl->chase_eyecam)
		cl->clientNum = cl->chase_target - g_edicts - 1;
	else
		cl->clientNum = selfnum;
}

void UpdateChaseCam(edict_t *ent)
{
	vec3_t o, ownerv, goal;
	edict_t *targ;
	vec3_t forward, right;
	trace_t trace;
	int i;
	vec3_t oldgoal;
	vec3_t angles;

	// is our chase target gone?
	if (!ent->client->chase_target->inuse
		|| ent->client->chase_target->client->resp.spectator) {
		edict_t *old = ent->client->chase_target;
		ChaseNext(ent);
		if (ent->client->chase_target == old) {
			ent->client->chase_target = NULL;
			ent->client->ps.pmove.pm_flags &= ~PMF_NO_PREDICTION;
			Chase_SetClientNum (ent);
			Chase_ClearGun (ent->client);
			return;
		}
	}

	targ = ent->client->chase_target;
	Chase_SetClientNum (ent);

	if (ent->client->chase_eyecam) {
		// First-person: mirror the target's playerstate so the view weapon
		// renders correctly.  q2pro hides the POV entity when clientNum matches.
		ent->client->ps = targ->client->ps;
		ent->client->ps.pmove.pm_flags |= PMF_NO_PREDICTION;

		if (targ->deadflag)
			ent->client->ps.pmove.pm_type = PM_DEAD;
		else
			ent->client->ps.pmove.pm_type = PM_FREEZE;

		VectorCopy (targ->client->v_angle, ent->client->v_angle);
		VectorCopy (targ->client->v_angle, ent->client->ps.viewangles);
		// pmove.origin is short[3] in 1/8 units -- VectorScale would read it as floats
		ent->s.origin[0] = ent->client->ps.pmove.origin[0] * 0.125f;
		ent->s.origin[1] = ent->client->ps.pmove.origin[1] * 0.125f;
		ent->s.origin[2] = ent->client->ps.pmove.origin[2] * 0.125f;
		ent->viewheight = targ->viewheight;

		for (i = 0; i < 3; i++)
			ent->client->ps.pmove.delta_angles[i] =
				ANGLE2SHORT(targ->client->v_angle[i] - ent->client->resp.cmd_angles[i]);

		gi.linkentity (ent);
		return;
	}

	Chase_ClearGun (ent->client);

	VectorCopy(targ->s.origin, ownerv);
	VectorCopy(ent->s.origin, oldgoal);

	ownerv[2] += targ->viewheight;

	VectorCopy(targ->client->v_angle, angles);
	if (angles[PITCH] > 56)
		angles[PITCH] = 56;
	AngleVectors (angles, forward, right, NULL);
	VectorNormalize(forward);
	VectorMA(ownerv, -30, forward, o);

	if (o[2] < targ->s.origin[2] + 20)
		o[2] = targ->s.origin[2] + 20;

	// jump animation lifts
	if (!targ->groundentity)
		o[2] += 16;

	trace = gi.trace(ownerv, vec3_origin, vec3_origin, o, targ, MASK_SOLID);

	VectorCopy(trace.endpos, goal);

	VectorMA(goal, 2, forward, goal);

	// pad for floors and ceilings
	VectorCopy(goal, o);
	o[2] += 6;
	trace = gi.trace(goal, vec3_origin, vec3_origin, o, targ, MASK_SOLID);
	if (trace.fraction < 1) {
		VectorCopy(trace.endpos, goal);
		goal[2] -= 6;
	}

	VectorCopy(goal, o);
	o[2] -= 6;
	trace = gi.trace(goal, vec3_origin, vec3_origin, o, targ, MASK_SOLID);
	if (trace.fraction < 1) {
		VectorCopy(trace.endpos, goal);
		goal[2] += 6;
	}

	if (targ->deadflag)
		ent->client->ps.pmove.pm_type = PM_DEAD;
	else
		ent->client->ps.pmove.pm_type = PM_FREEZE;

	VectorCopy(goal, ent->s.origin);
	for (i=0 ; i<3 ; i++)
		ent->client->ps.pmove.delta_angles[i] = ANGLE2SHORT(targ->client->v_angle[i] - ent->client->resp.cmd_angles[i]);

	if (targ->deadflag) {
		ent->client->ps.viewangles[ROLL] = 40;
		ent->client->ps.viewangles[PITCH] = -15;
		ent->client->ps.viewangles[YAW] = targ->client->killer_yaw;
	} else {
		VectorCopy(targ->client->v_angle, ent->client->ps.viewangles);
		VectorCopy(targ->client->v_angle, ent->client->v_angle);
	}

	ent->viewheight = 0;
	ent->client->ps.pmove.pm_flags |= PMF_NO_PREDICTION;
	gi.linkentity(ent);
}

void ChaseNext(edict_t *ent)
{
	int i;
	edict_t *e;

	if (!ent->client->chase_target)
		return;

	i = ent->client->chase_target - g_edicts;
	do {
		i++;
		if (i > maxclients->value)
			i = 1;
		e = g_edicts + i;
		if (!e->inuse)
			continue;
		if (!e->client->resp.spectator)
			break;
	} while (e != ent->client->chase_target);

	ent->client->chase_target = e;
	Chase_SetClientNum (ent);
}

void ChasePrev(edict_t *ent)
{
	int i;
	edict_t *e;

	if (!ent->client->chase_target)
		return;

	i = ent->client->chase_target - g_edicts;
	do {
		i--;
		if (i < 1)
			i = maxclients->value;
		e = g_edicts + i;
		if (!e->inuse)
			continue;
		if (!e->client->resp.spectator)
			break;
	} while (e != ent->client->chase_target);

	ent->client->chase_target = e;
	Chase_SetClientNum (ent);
}

void GetChaseTarget(edict_t *ent)
{
	int i;
	edict_t *other;

	for (i = 1; i <= maxclients->value; i++) {
		other = g_edicts + i;
		if (other->inuse && !other->client->resp.spectator) {
			ent->client->chase_target = other;
			ent->client->chase_eyecam = true;
			Chase_SetClientNum (ent);
			return;
		}
	}
	gi.centerprintf(ent, "No other players to chase.");
}

/*
=================
ChaseEndServerFrame

Run after all normal ClientEndServerFrames so eyecam mirrors the
target's finalized playerstate (viewoffset, gun, position).  Running
ClientEndServerFrame on a chasing spectator would stomp that state.
=================
*/
void ChaseEndServerFrame(edict_t *ent)
{
	gclient_t	*cl = ent->client;

	if (!cl->chase_target)
		return;

	if (!cl->chase_target->inuse || cl->chase_target->client->resp.spectator) {
		edict_t *old = cl->chase_target;
		ChaseNext (ent);
		if (cl->chase_target == old) {
			cl->chase_target = NULL;
			cl->ps.pmove.pm_flags &= ~PMF_NO_PREDICTION;
			Chase_SetClientNum (ent);
			Chase_ClearGun (cl);
			return;
		}
	}

	UpdateChaseCam (ent);
	G_SetSpectatorStats (ent);
}
