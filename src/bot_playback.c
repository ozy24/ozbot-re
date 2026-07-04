/*
ozbot - self-learning q2dm1 bot

bot_playback.c -- playbooks: recorded map-specific maneuvers (Phase R4).

A playbook is a set of recorded input sequences (from a human capture via
bot_inputlog, baked by tools/make_playbook.py) that the bot can replay to
execute moves its regular locomotion cannot -- e.g. the q2dm1 Megahealth
box+strafe jump.  Each entry surfaces in the nav graph as a NAV_LINK_PLAYBOOK
link between a node at its anchor and a node at its exit, so A* routes through
recorded moves like any other capability link.  Execution is a three-stage
controller (the lesson of the lift work: the controller IS the capability,
the graph edit is trivial):

  ALIGN   steer to the anchor: position within tolerance, facing the recorded
          yaw, speed preconditions met.  Times out fast.
  REPLAY  feed the recorded usercmds verbatim, one per tick, with a per-tick
          drift monitor against the recorded origin timeline.  Divergence
          aborts the attempt and penalizes the link so A* stops proposing a
          maneuver this bot cannot land.
  FAILED  sticks for the rest of the goal attempt (the giveup machinery
          resolves it); cleared on goal exit/respawn like the lift states.

File format (<gamedir>/playbooks/<map>.pbk, text, written by make_playbook.py):

  playbook 1
  entry <name>
  tickrate <hz>
  anchor <x> <y> <z> <yaw> <pos_tol> <yaw_tol> <min_speed> <max_speed>
  exit <x> <y> <z>
  tick <fwd> <side> <up> <yaw> <pitch> <x> <y> <z>
  ... (one per tick)
  end

Entries whose tickrate does not match the running game rate are skipped (the
baker resamples to the target rate; replaying across rates would replay a
different maneuver).
*/

#include "g_local.h"
#include "bot.h"
#include "bot_nav.h"

cvar_t	*bot_playbook;	// master gate (inert when no playbook file exists)

#define PB_MAX_ENTRIES		16
#define PB_MAX_TICKS		600		// 15s at 40Hz
#define PB_ENGAGE_RANGE		220.0f	// start aligning this close to the anchor node
#define PB_ALIGN_TIMEOUT	6.0f	// seconds to satisfy the anchor preconditions
#define PB_DRIFT_MAX		56.0f	// live-vs-recorded origin divergence abort
#define PB_COOLDOWN			1.0f	// after a completed replay (no instant re-engage)

typedef struct
{
	short	fwd, side, up;
	float	yaw, pitch;
	vec3_t	org;			// recorded origin AFTER this tick (drift reference)
} pb_tick_t;

typedef struct
{
	char		name[32];
	vec3_t		anchor;
	float		anchor_yaw;
	float		pos_tol, yaw_tol;
	float		min_speed, max_speed;
	vec3_t		exit;
	int			tick_rate;
	int			num_ticks;
	pb_tick_t	ticks[PB_MAX_TICKS];
	int			start_node, exit_node;	// resolved at registration
} pb_entry_t;

static pb_entry_t	pb_entries[PB_MAX_ENTRIES];
static int			pb_num_entries;

/*
=================
Playbook_Load

Parse <gamedir>/playbooks/<map>.pbk into pb_entries.  Tolerant line format;
unknown lines are ignored so the format can grow.
=================
*/
void Playbook_Load (const char *mapname)
{
	char		path[MAX_OSPATH];
	FILE		*f;
	char		line[256];
	pb_entry_t	*e = NULL;
	int			skipped_rate = 0;

	pb_num_entries = 0;

	if (bot_playbook->value == 0)
		return;

	Com_sprintf (path, sizeof(path), "%s/playbooks/%s.pbk", Bot_GameDir (), mapname);
	f = fopen (path, "r");
	if (!f)
		return;					// no playbook for this map: stay inert

	while (fgets (line, sizeof(line), f))
	{
		if (!strncmp (line, "entry ", 6))
		{
			if (pb_num_entries >= PB_MAX_ENTRIES)
				break;
			e = &pb_entries[pb_num_entries];
			memset (e, 0, sizeof(*e));
			e->start_node = e->exit_node = -1;
			sscanf (line + 6, "%31s", e->name);
		}
		else if (!e)
			continue;
		else if (!strncmp (line, "tickrate ", 9))
			e->tick_rate = atoi (line + 9);
		else if (!strncmp (line, "anchor ", 7))
			sscanf (line + 7, "%f %f %f %f %f %f %f %f",
				&e->anchor[0], &e->anchor[1], &e->anchor[2], &e->anchor_yaw,
				&e->pos_tol, &e->yaw_tol, &e->min_speed, &e->max_speed);
		else if (!strncmp (line, "exit ", 5))
			sscanf (line + 5, "%f %f %f", &e->exit[0], &e->exit[1], &e->exit[2]);
		else if (!strncmp (line, "tick ", 5))
		{
			if (e->num_ticks < PB_MAX_TICKS)
			{
				pb_tick_t *t = &e->ticks[e->num_ticks];
				int fwd, side, up;
				if (sscanf (line + 5, "%d %d %d %f %f %f %f %f",
						&fwd, &side, &up, &t->yaw, &t->pitch,
						&t->org[0], &t->org[1], &t->org[2]) == 8)
				{
					t->fwd = (short)fwd;
					t->side = (short)side;
					t->up = (short)up;
					e->num_ticks++;
				}
			}
		}
		else if (!strncmp (line, "end", 3))
		{
			if (e->num_ticks > 0 && e->tick_rate == game.framerate)
				pb_num_entries++;
			else if (e->num_ticks > 0)
				skipped_rate++;
			e = NULL;
		}
	}
	fclose (f);

	if (pb_num_entries || skipped_rate)
		gi.dprintf ("ozbot: playbook %s: %d entr%s loaded%s\n", path,
			pb_num_entries, pb_num_entries == 1 ? "y" : "ies",
			skipped_rate ? va (" (%d skipped: tickrate != sv_fps)", skipped_rate) : "");
}

/*
=================
Playbook_Register

Surface each loaded entry in the nav graph: a node at the anchor, a node at
the exit, and a one-way NAV_LINK_PLAYBOOK between them.  Runs in the per-map
setup after the graph is loaded and item spots are seeded (the same slot as
Nav_TagPlatLinks).  The links are never saved (Nav_Save filters the type), so
with bot_playbook off -- or the file absent -- the on-disk graph is untouched.
=================
*/
void Playbook_Register (void)
{
	int	i;

	for (i = 0; i < pb_num_entries; i++)
	{
		pb_entry_t *e = &pb_entries[i];

		e->start_node = Nav_SeedNode (e->anchor);
		e->exit_node  = Nav_SeedNode (e->exit);
		if (e->start_node < 0 || e->exit_node < 0 || e->start_node == e->exit_node)
		{
			gi.dprintf ("ozbot: playbook entry %s: could not seed nav nodes\n", e->name);
			e->start_node = e->exit_node = -1;
			continue;
		}
		Nav_AddLinkType (e->start_node, e->exit_node, NAV_LINK_PLAYBOOK);
		if (Bot_LinkType (e->start_node, e->exit_node) != NAV_LINK_PLAYBOOK)
		{
			// full link table (or a learned link already claims the pair --
			// which would shadow the maneuver with a walk that can't do it)
			gi.dprintf ("ozbot: playbook entry %s: link %d -> %d NOT registered\n",
				e->name, e->start_node, e->exit_node);
			e->start_node = e->exit_node = -1;
			continue;
		}
		gi.dprintf ("ozbot: playbook %s: node %d -> %d (%d ticks)\n",
			e->name, e->start_node, e->exit_node, e->num_ticks);
	}
}

/*
=================
Playbook_FindEntry

The entry whose registered link is the upcoming hop on this bot's path.
=================
*/
static pb_entry_t *Playbook_FindEntry (int from_node, int to_node)
{
	int	i;

	for (i = 0; i < pb_num_entries; i++)
		if (pb_entries[i].start_node == from_node && pb_entries[i].exit_node == to_node)
			return &pb_entries[i];
	return NULL;
}

/*
=================
Bot_PlaybackReset
=================
*/
void Bot_PlaybackReset (bot_t *b)
{
	b->pb_state = PB_NONE;
	b->pb_entry = -1;
	b->pb_tick = 0;
}

static void Playbook_Abort (bot_t *b, pb_entry_t *e, const char *why)
{
	Bot_LogEvent (b, why);
	if (e && e->start_node >= 0)
		Nav_PenalizeLink (e->start_node, e->exit_node);
	b->pb_state = PB_FAILED;	// sticks until the goal resolves
}

/*
=================
Bot_PlaybackThink

Returns true while the playback controller owns this frame's movement intent
and facing.  Call order in Bot_Navigate mirrors the lift controller: before
path following and stuck recovery; the caller freezes the goal-budget clock
while we return true (align waiting must not be billed).
=================
*/
qboolean Bot_PlaybackThink (bot_t *b)
{
	edict_t		*ent = b->ent;
	pb_entry_t	*e;
	vec3_t		d, vel;
	float		hdist, speed, dyaw;
	int			hop;

	if (bot_playbook->value == 0 || pb_num_entries == 0)
		return false;
	if (b->pb_state == PB_FAILED)
		return false;

	// mid-replay: keep going regardless of what the path looks like now
	if (b->pb_state == PB_REPLAY)
	{
		pb_tick_t	*t;
		int			k, lo, hi, best_k;
		float		dd, best_d;

		if (b->pb_entry < 0 || b->pb_entry >= pb_num_entries)
		{
			Bot_PlaybackReset (b);
			return false;
		}
		e = &pb_entries[b->pb_entry];

		// rail-match with cursor slip: execute the recorded tick whose
		// pre-tick position best matches where we actually are.  An entry
		// speed a few percent off the recording makes the bot lead/lag the
		// timeline (measured: 60u of pure phase error 9 ticks in) -- letting
		// the cursor slip a little each frame absorbs that, and drift becomes
		// a true distance-to-rail measure.
		lo = b->pb_tick - 1;
		if (lo < 0) lo = 0;
		hi = b->pb_tick + 3;
		if (hi > e->num_ticks) hi = e->num_ticks;
		best_k = b->pb_tick;
		best_d = 1e18f;
		for (k = lo; k <= hi; k++)
		{
			float *ref = (k == 0) ? e->anchor : e->ticks[k - 1].org;
			VectorSubtract (ent->s.origin, ref, d);
			dd = VectorLength (d);
			if (dd < best_d)
			{
				best_d = dd;
				best_k = k;
			}
		}
		if (best_d > PB_DRIFT_MAX)
		{
			gi.dprintf ("ozbot: pb_abort_drift %s: tick %d/%d drift %.0f\n",
				e->name, best_k, e->num_ticks, best_d);
			Playbook_Abort (b, e, "pb_abort_drift");
			return false;
		}

		// forward-progress watchdog: cursor slip may hold position briefly
		// (speed phase, wedge recovery), but a replay that stops making NEW
		// progress is a bot pinned on the rail -- and the caller freezes the
		// goal budget while we own the frame, so without this it would replay
		// in place forever.  Only a fresh high-water tick counts as progress.
		if (best_k > b->pb_hiwater)
		{
			b->pb_hiwater = best_k;
			b->pb_deadline = level.time + 0.75f;
		}
		else if (level.time > b->pb_deadline)
		{
			gi.dprintf ("ozbot: pb_abort_stall %s: tick %d/%d\n",
				e->name, best_k, e->num_ticks);
			Playbook_Abort (b, e, "pb_abort_stall");
			return false;
		}
		b->pb_tick = best_k;

		// wedge recovery: stopped dead while off the rail means we clipped
		// geometry the recording cleared by inches (rails can hug walls).
		// Spend this frame steering straight back onto the rail; the cursor
		// holds, the watchdog above bounds how long we may try.
		{
			vec3_t	vv;
			VectorCopy (ent->velocity, vv);
			vv[2] = 0;
			if (VectorLength (vv) < 30 && best_d > 6 && ent->groundentity
				&& best_k < e->num_ticks)
			{
				float *ref = (best_k == 0) ? e->anchor : e->ticks[best_k - 1].org;
				VectorSubtract (ref, ent->s.origin, d);
				d[2] = 0;
				if (VectorLength (d) > 1)
				{
					b->pb_yaw = vectoyaw (d);
					b->pb_pitch = 0;
					b->pb_cmd_fwd = 400;
					b->pb_cmd_side = 0;
					b->pb_cmd_up = 0;
					VectorNormalize (d);
					VectorCopy (d, b->move_dir);
					Bot_SetMoveYaw (b, b->pb_yaw);
					return true;
				}
			}
		}

		if (b->pb_tick >= e->num_ticks)
		{
			// replay complete: hand the path back pointing past the exit node
			Bot_LogEvent (b, "pb_done");
			while (b->path_idx < b->path_len && b->path[b->path_idx] != e->exit_node)
				b->path_idx++;
			if (b->path_idx < b->path_len)
				b->path_idx++;			// step past the exit node itself
			Bot_PlaybackReset (b);
			b->sj_cooldown = level.time + PB_COOLDOWN;	// no instant SJ re-grab
			return false;				// normal following resumes this frame
		}

		t = &e->ticks[b->pb_tick++];
		b->pb_cmd_fwd  = t->fwd;
		b->pb_cmd_side = t->side;
		b->pb_cmd_up   = t->up;
		b->pb_yaw      = t->yaw;
		b->pb_pitch    = t->pitch;

		// closed-loop rail correction: pure open-loop replay tolerates zero
		// entry error (measured: every engage drifted out within the run).
		// Bias the applied yaw a few degrees toward the recorded position --
		// enough to home onto the rail, too small to corrupt the maneuver.
		if (b->pb_tick > 1)
		{
			vec3_t	aim_ang = {0, 0, 0}, f2, r2, err;
			float	lat, corr;

			aim_ang[YAW] = t->yaw;
			AngleVectors (aim_ang, f2, r2, NULL);
			VectorSubtract (e->ticks[b->pb_tick - 2].org, ent->s.origin, err);
			err[2] = 0;
			lat = DotProduct (err, r2);	// + = the rail is to our right
			corr = lat * 0.15f;
			if (corr > 8.0f)  corr = 8.0f;
			if (corr < -8.0f) corr = -8.0f;
			b->pb_yaw = t->yaw - corr;	// turning right = decreasing yaw
		}

		b->progress_time = level.time;	// replay is progress; no stuck recovery
		Bot_SetMoveYaw (b, b->pb_yaw);	// sane world-space intent for debug/fallback
		return true;
	}

	// not replaying: engage only when a playbook hop is next on the path
	if (b->path_len <= 0 || b->path_idx >= b->path_len || b->path_idx < 1)
		hop = -1;
	else if (Bot_LinkType (b->path[b->path_idx - 1], b->path[b->path_idx]) == NAV_LINK_PLAYBOOK)
		hop = b->path_idx - 1;	// we're "between" the anchor and exit nodes
	else
		hop = -1;
	if (hop < 0 && b->path_idx + 1 < b->path_len
		&& Bot_LinkType (b->path[b->path_idx], b->path[b->path_idx + 1]) == NAV_LINK_PLAYBOOK)
		hop = b->path_idx;

	if (hop < 0)
	{
		if (b->pb_state != PB_NONE)
			Bot_PlaybackReset (b);	// path no longer runs through a playbook
		return false;
	}

	e = Playbook_FindEntry (b->path[hop], b->path[hop + 1]);
	if (!e)
	{
		if (b->pb_state != PB_NONE)
			Bot_PlaybackReset (b);
		return false;
	}

	// only take over near the anchor; approach normally until then
	VectorSubtract (e->anchor, ent->s.origin, d);
	if (VectorLength (d) > PB_ENGAGE_RANGE)
	{
		if (b->pb_state != PB_NONE)
			Bot_PlaybackReset (b);
		return false;
	}

	if (b->pb_state != PB_ALIGN)
	{
		Bot_LogEvent (b, "pb_align");
		b->pb_state = PB_ALIGN;
		b->pb_entry = (int)(e - pb_entries);
		b->pb_deadline = level.time + PB_ALIGN_TIMEOUT;
		b->pb_tick = 0;		// scratch during ALIGN: 1 = run-up latched
		VectorCopy (ent->s.origin, b->lift_move_pos);	// reuse: stall detector
		b->lift_move_time = level.time;
	}

	// immobile during ALIGN = blocked by geometry/another body: fail fast
	// (the controller refreshes progress_time, so normal stuck recovery is
	// off while we own the frame -- we must police our own stalls)
	VectorSubtract (ent->s.origin, b->lift_move_pos, d);
	if (VectorLength (d) > 20)
	{
		VectorCopy (ent->s.origin, b->lift_move_pos);
		b->lift_move_time = level.time;
	}
	else if (level.time - b->lift_move_time > 1.5f)
	{
		Playbook_Abort (b, e, "pb_abort_align");
		return false;
	}

	if (level.time > b->pb_deadline)
	{
		// diagnosis: which precondition could not be met
		{
			vec3_t	dd, vv;
			float	hd, sp;
			VectorSubtract (e->anchor, ent->s.origin, dd);
			dd[2] = 0;
			hd = VectorLength (dd);
			VectorCopy (ent->velocity, vv);
			vv[2] = 0;
			sp = VectorLength (vv);
			gi.dprintf ("ozbot: pb_abort_align %s: hdist=%.0f (tol %.0f) speed=%.0f (%.0f..%.0f) "
				"viewyaw=%.0f velyaw=%.0f (want %.0f +/-%.0f) runup=%d ground=%d\n",
				e->name, hd, e->pos_tol, sp, e->min_speed, e->max_speed,
				ent->client->ps.viewangles[YAW], sp > 1 ? vectoyaw (vv) : 0,
				e->anchor_yaw, e->yaw_tol, b->pb_tick, ent->groundentity != NULL);
		}
		Playbook_Abort (b, e, "pb_abort_align");
		return false;
	}

	// ---- ALIGN: get onto the anchor, facing the recorded way, at speed ----
	VectorSubtract (e->anchor, ent->s.origin, d);
	d[2] = 0;
	hdist = VectorLength (d);

	VectorCopy (ent->velocity, vel);
	vel[2] = 0;
	speed = VectorLength (vel);

	dyaw = e->anchor_yaw - ent->client->ps.viewangles[YAW];
	while (dyaw > 180)  dyaw -= 360;
	while (dyaw < -180) dyaw += 360;

	// the VELOCITY heading must match too, not just the facing: arriving at
	// the anchor at speed but moving the wrong way diverges from the recorded
	// trajectory on the first ticks (measured: every engage drift-aborted).
	// Captures start mid-run, so velocity ~ view heading is the right gate.
	if (speed > 60)
	{
		float dvel = vectoyaw (vel) - e->anchor_yaw;
		while (dvel > 180)  dvel -= 360;
		while (dvel < -180) dvel += 360;
		if (dvel < -e->yaw_tol || dvel > e->yaw_tol)
			dyaw = 999;			// fail the precondition check below
	}

	// the gate is LATERAL offset from the recorded line (longitudinal error
	// is absorbed by the replay's cursor slip; radial gating rejected good
	// crossings sampled a step early/late at 25ms x 300ups = 8u per tick)
	{
		vec3_t	ax = {0, 0, 0}, af, ar;
		float	lat, lon;

		ax[YAW] = e->anchor_yaw;
		AngleVectors (ax, af, ar, NULL);
		lat = (float)fabs (DotProduct (d, ar));
		lon = (float)fabs (DotProduct (d, af));
		hdist = (lon <= 24.0f) ? lat : hdist;
	}

	if (hdist <= e->pos_tol && ent->groundentity
		&& dyaw >= -e->yaw_tol && dyaw <= e->yaw_tol
		&& speed >= e->min_speed && (e->max_speed <= 0 || speed <= e->max_speed))
	{
		Bot_LogEvent (b, "pb_engage");
		b->pb_state = PB_REPLAY;
		b->pb_tick = 0;
		b->pb_hiwater = -1;
		b->pb_deadline = level.time + 0.75f;	// forward-progress watchdog
		// facing/cmds start next call -- this frame just holds the anchor
		b->pb_cmd_fwd = b->pb_cmd_side = b->pb_cmd_up = 0;
		b->pb_yaw = e->anchor_yaw;
		b->pb_pitch = 0;
		return true;
	}

	// ---- ALIGN steering ----
	// A moving-start maneuver can't be entered by walking AT the anchor and
	// stopping (speed + heading preconditions would never hold at once).
	// Stage a run-up: get to a point behind the anchor on the recorded
	// heading's axis, then run through the anchor at full speed along it --
	// the precondition check above fires as we cross.  Standing-start
	// entries (min_speed ~0) just walk on and settle.
	b->pb_pitch = 0;
	b->pb_cmd_fwd = b->pb_cmd_side = b->pb_cmd_up = 0;

	if (e->min_speed < 60)
	{
		// standing start: settle onto the anchor, facing the recorded way
		b->pb_yaw = e->anchor_yaw;
		if (hdist > 4)
		{
			float	mag = (hdist > e->pos_tol) ? 0.8f : 0.3f;
			VectorNormalize (d);
			VectorScale (d, mag, b->move_dir);
			b->move_yaw = vectoyaw (d);
		}
		else
			VectorClear (b->move_dir);
	}
	else
	{
		vec3_t	fwd, stage, tostage;
		vec3_t	aim_ang = {0, 0, 0};
		float	sdist;
		static const float stage_dists[3] = {140.0f, 100.0f, 60.0f};
		int		si;

		aim_ang[YAW] = e->anchor_yaw;
		AngleVectors (aim_ang, fwd, NULL, NULL);
		fwd[2] = 0;
		VectorNormalize (fwd);

		// how far behind the anchor to stage: as much run-up as the geometry
		// allows (captures start mid-run -- the back-extrapolated line can
		// poke into a wall; measured: bots camped a solid stage point until
		// the align deadline)
		for (si = 0; si < 3; si++)
		{
			VectorMA (e->anchor, -stage_dists[si], fwd, stage);
			if (Nav_CanWalk (e->anchor, stage, ent))
				break;
		}
		if (si == 3)
			VectorMA (e->anchor, -60.0f, fwd, stage);	// best effort
		VectorSubtract (stage, ent->s.origin, tostage);
		tostage[2] = 0;
		sdist = VectorLength (tostage);

		// reaching the staging point LATCHES the run-up (pb_tick=1): the
		// staging distance grows the moment we sprint forward, so the
		// condition can't be re-evaluated per tick (measured: the bot
		// turned back every tick and timed out)
		if (DotProduct (d, fwd) > 0 && sdist < 48)
			b->pb_tick = 1;
		if (DotProduct (d, fwd) <= 0)
			b->pb_tick = 0;		// overshot the anchor: stage again

		if (b->pb_tick == 1)
		{
			// run AT the anchor (not just along the heading): this converges
			// the lateral offset, so the replay starts nearly on the rail
			if (hdist > 24)
			{
				vec3_t	toa;
				VectorCopy (d, toa);
				VectorNormalize (toa);
				b->pb_yaw = vectoyaw (toa);
				VectorCopy (toa, b->move_dir);
			}
			else
			{
				b->pb_yaw = e->anchor_yaw;
				VectorCopy (fwd, b->move_dir);
			}
			b->move_yaw = b->pb_yaw;
		}
		else
		{
			// still getting to the staging point
			b->pb_yaw = (sdist > 1) ? vectoyaw (tostage) : e->anchor_yaw;
			if (sdist > 1)
			{
				VectorNormalize (tostage);
				VectorCopy (tostage, b->move_dir);
				b->move_yaw = b->pb_yaw;
			}
			else
				VectorClear (b->move_dir);
		}
	}

	// during ALIGN the normal projection moves us; the facing comes from
	// pb_yaw via Bot_Think's override
	b->progress_time = level.time;
	return true;
}
