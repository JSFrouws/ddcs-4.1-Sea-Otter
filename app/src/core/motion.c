#include <math.h>
#include <string.h>
#include "motion.h"
#include "settings.h"
#include "../perf.h"

/* the FPGA plays its block queue back to back: the full 40 000 steps/s is sustainable */
#define SUSTAIN_SPS ((double)FPGA_MAX_SPS)
/* longest kinematic step inside one tick of a dry run (see motion_tick) */
#define MOTION_SUBSTEP 0.010

int rapid_override(int feed_ov) { return feed_ov < 100 ? feed_ov : 100; }

void motion_init(motion_t *m, fpga_t *f, const double mpos[NAXES])
{
	memset(m, 0, sizeof *m);
	m->f = f;
	m->fpga_mode = -1;                                   /* told on the first tick */
	m->feed_ov = m->spin_ov = m->last_ov = 100;
	m->replan_from = -1; m->applied_pct = -1;
	for (int a = 0; a < NAXES; a++) motion_set_pos(m, a, mpos[a]);
}

void motion_set_pos(motion_t *m, int a, double mm)
{
	m->pos[a] = mm;
	m->steps[a] = llround(mm * SET.ax[a].steps_per_mm);
	m->spm[a] = SET.ax[a].steps_per_mm;
	m->emit_pos[a] = mm;
	if (!m->qn) m->plan_end[a] = mm;
}

/* A steps/mm change must never move anything. pos[] is the commanded position in mm and
 * emit_block() sends pos * steps/mm - steps, so keeping pos across a new scale turns the difference
 * into one huge step stream with no ramp and no limits (2026-10-31, X ran +336.6 mm). The
 * steps are what the motors really did, so they stay and the mm position follows them. */
static void rescale(motion_t *m)
{
	bool any = false;
	for (int a = 0; a < NAXES; a++) {
		double spm = SET.ax[a].steps_per_mm;
		if (m->spm[a] == spm || spm <= 0) continue;
		m->spm[a] = spm;
		m->pos[a] = (double)m->steps[a] / spm;
		m->rescaled |= (uint8_t)(1u << a);
		any = true;
	}
	if (!any) return;
	motion_stop(m, true);                 /* anything planned was planned in the old scale */
	for (int a = 0; a < NAXES; a++) m->pos[a] = (double)m->steps[a] / m->spm[a];
	memcpy(m->plan_end, m->pos, sizeof m->plan_end);
	memcpy(m->emit_pos, m->pos, sizeof m->emit_pos);
	m->home.on = false;
}

void motion_queued_end(const motion_t *m, double out[NAXES])
{
	memcpy(out, (m->qn || (m->active && m->op.kind == OP_LINE)) ? m->plan_end : m->pos, sizeof(double) * NAXES);
}

int motion_space(const motion_t *m) { return MQ_LEN - m->qn; }

#define QI(m, i) (((m)->qh + (i)) % MQ_LEN)

/* ---- kinematics -------------------------------------------------------------------------
 * Planning (lookahead passes) uses v^2 = v0^2 + 2aL with each segment's EFFECTIVE acceleration
 * aplan = A*v/(v + A^2/J): the average acceleration of a full S-curve speed change up to its
 * nominal speed. (Charging a jerk ramp per segment instead would make chains of tiny segments
 * crawl.) Execution brakes with the precise jerk-aware distance of one continuous braking:
 *   d = (v^2 - ve^2) / (2A) + (v + ve) * A / (2J)  */
static double reach(double v0, double a, double L) { return sqrt(v0 * v0 + 2 * a * L); }

static double brake_dist(double v, double ve, double a, double j, double acur)
{
	if (v <= ve) return 0;
	double d = (v * v - ve * ve) / (2 * a) + (v + ve) * a / (2 * j);
	if (acur > 0) d += v * acur / j;          /* first undo the acceleration still in progress */
	return d;
}

/* ---- segment geometry: speed, acceleration and jerk limited per axis ---- */
bool motion_plan_setup(struct plan *p, const gc_op_t *op, const double start[NAXES])
{
	double d2 = 0;
	for (int a = 0; a < NAXES; a++) { p->u[a] = op->end[a] - start[a]; d2 += p->u[a] * p->u[a]; }
	p->len = sqrt(d2);
	if (p->len < 1e-7) return false;
	double vcap = 1e9, ac = 1e9, jk = 1e12;
	for (int a = 0; a < NAXES; a++) {
		p->u[a] /= p->len;
		double c = fabs(p->u[a]);
		if (c < 1e-9) continue;
		double cap = fmin(SET.ax[a].max_speed, SUSTAIN_SPS / SET.ax[a].steps_per_mm);   /* FPGA limit */
		vcap = fmin(vcap, cap / c);
		ac = fmin(ac, SET.ax[a].accel / c);
		jk = fmin(jk, SET.ax[a].jerk / c);
	}
	p->line = true; p->rapid = op->rapid;
	double v = fmin(op->rapid ? 1e9 : op->feed / 60.0, vcap);
	p->vnom = v; p->vcap = vcap; p->acc = ac; p->jerk = jk;
	p->aplan = ac * v / (v + ac * ac / jk);
	return true;
}

/* Corner limit between two segments. Each axis may change speed instantly by at most its
 * corner jump: an X-only move turning 10 degrees towards Y asks Y for v*sin(10) from zero,
 * so it keeps most of its speed; a full reversal asks X for 2v, so it nearly stops. A
 * junction-deviation tolerance adds the usual centripetal limit for real corners. */
double motion_corner_limit(const struct plan *A, const struct plan *B)
{
	double v = 1e9, dot = 0;
	for (int a = 0; a < NAXES; a++) {
		double du = fabs(B->u[a] - A->u[a]);
		dot += A->u[a] * B->u[a];
		if (du > 1e-9) v = fmin(v, SET.ax[a].corner_speed / du);
	}
	if (SET.junction_dev > 0 && dot < 0.999999) {
		double sh = sqrt(fmax(0.0, 0.5 * (1 + dot)));      /* sin of half the angle between the paths' normals */
		double acc = fmin(A->acc, B->acc);
		if (sh < 0.999999) v = fmin(v, sqrt(acc * SET.junction_dev * sh / (1 - sh)));
	}
	return v;
}

static double ov_of(const motion_t *m, const struct plan *p)
{
	return (p->rapid ? rapid_override(m->feed_ov) : m->feed_ov) / 100.0;
}

/* speed with the override applied, never above what the axes / FPGA can sustain */
static double vrun(const motion_t *m, const struct plan *p) { return fmin(p->vnom * ov_of(m, p), p->vcap); }

bool motion_push(motion_t *m, const gc_op_t *op)
{
	if (m->qn >= MQ_LEN) return false;
	if (!m->qn && !(m->active && m->op.kind == OP_LINE)) memcpy(m->plan_end, m->pos, sizeof m->plan_end);
	struct plan p = { 0 };
	if (op->kind == OP_LINE) {
		if (!motion_plan_setup(&p, op, m->plan_end)) return true;          /* zero length: nothing to do */
		const struct plan *prev = m->qn ? &m->pl[QI(m, m->qn - 1)] : (m->active && m->op.kind == OP_LINE ? &m->cur : NULL);
		p.vj = prev && prev->line ? motion_corner_limit(prev, &p) : 0;
		memcpy(m->plan_end, op->end, sizeof m->plan_end);
		m->qdist += p.len;
	}
	int i = m->qn++;
	m->q[QI(m, i)] = *op;
	m->pl[QI(m, i)] = p;
	if (m->replan_from < 0 || i - 1 < m->replan_from) m->replan_from = i > 0 ? i - 1 : 0;
	return true;
}

/* keep the queue at least SET.lookahead segments deep AND at least the braking distance from
 * the fastest axis speed deep, so a run of 0.01 mm engraving segments still sees far enough */
bool motion_wants_more(const motion_t *m)
{
	if (m->qn >= MQ_LEN - 8) return false;
	if (m->qn < SET.lookahead) return true;
	double vmax = 0, amin = 1e9, jmin = 1e12;
	for (int a = 0; a < NAXES; a++) {
		vmax = fmax(vmax, fmin(SET.ax[a].max_speed, SUSTAIN_SPS / SET.ax[a].steps_per_mm));
		amin = fmin(amin, SET.ax[a].accel); jmin = fmin(jmin, SET.ax[a].jerk);
	}
	/* deep enough for the PLANNER to see a stop from top speed: it plans with the S-curve's
	 * effective acceleration, so size the depth with that (+20 %) */
	double aeff = amin * vmax / (vmax + amin * amin / jmin);
	return m->qdist < 1.2 * vmax * vmax / (2 * aeff);
}

/* Re-plan from the first stale entry. Backward: every segment's entry speed is limited by its
 * corner, its own nominal speed and by having to brake for everything after it (the queue's
 * end counts as a stop). It stops early once a value comes out unchanged - nothing before it
 * can change then. Forward: limited by what acceleration can reach from the speed before. */
static void replan(motion_t *m)
{
	if (m->replan_from < 0 || !m->qn) { m->replan_from = -1; return; }
	double next = 0;
	int stop_at = m->replan_from, i;
	for (i = m->qn - 1; i >= 0; i--) {
		struct plan *p = &m->pl[QI(m, i)];
		if (!p->line) { p->vb = p->ve = 0; next = 0; continue; }
		const struct plan *prev = i ? &m->pl[QI(m, i - 1)] : (m->active && m->op.kind == OP_LINE ? &m->cur : NULL);
		double lim = fmin(p->vj, vrun(m, p));
		if (prev && prev->line) lim = fmin(lim, vrun(m, prev)); else lim = 0;
		double vb = fmin(lim, reach(next, p->aplan, p->len));
		m->replan_iters++;
		if (i < stop_at && fabs(vb - p->vb) < 1e-9) break;
		p->vb = vb;
		next = vb;
	}
	int from = i < 0 ? 0 : i;
	for (int k = from; k < m->qn; k++) {
		struct plan *p = &m->pl[QI(m, k)];
		m->replan_iters++;
		if (!p->line) { p->ve = 0; continue; }
		double v = p->vb;
		if (k == 0) {
			if (m->active && m->op.kind == OP_LINE) v = fmin(v, reach(m->v, m->cur.aplan, m->cur.len - m->s));
			else v = 0;
		} else {
			const struct plan *pv = &m->pl[QI(m, k - 1)];
			v = pv->line ? fmin(v, reach(pv->ve, pv->aplan, pv->len)) : 0;
		}
		p->ve = v;
	}
	m->replan_from = -1;
}

bool motion_idle(const motion_t *m)
{
	if (m->active || m->qn || m->jog_dir || m->home.on || m->hw_busy || m->q_est) return false;
	for (int a = 0; a < NAXES; a++) if (fabs(m->nudge[a]) > 1e-9 || fabs(m->jv[a]) > 1e-12) return false;
	return true;
}

void motion_move_to(motion_t *m, const double t[NAXES], double feed, bool rapid)
{
	gc_op_t op = { .kind = OP_LINE, .rapid = rapid, .feed = feed, .line = -1 };
	memcpy(op.end, t, sizeof op.end);
	motion_push(m, &op);
}

void motion_hold(motion_t *m, bool on) { m->hold = on; }

static void flush_queue(motion_t *m) { m->qn = 0; m->qdist = 0; m->replan_from = -1; }

/* A controlled stop (braking on the ramp past a switch or the E-stop) gets a hard time bound:
 * the deceleration time from the current speed + the jerk ramp + 200 ms. Past it the axes
 * are cut dead (IO-mapping review 2026-10-31): a ramp must never keep an axis going. */
static void begin_brake(motion_t *m)
{
	double v = fabs(m->v), A = 1e9, J = 1e9;
	for (int a = 0; a < NAXES; a++) {
		v = fmax(v, fabs(m->jv[a]));
		A = fmin(A, SET.ax[a].accel); J = fmin(J, SET.ax[a].jerk);
	}
	if (!m->stopping) m->stop_left = v / A + A / J + 0.2;
	m->stopping = true;
}

void motion_stop(motion_t *m, bool immediate)
{
	bool brake = !immediate && !m->dry && SET.graceful_stop;   /* a jog / wheel move brakes on its ramp */
	m->jog_dir = 0; m->home.on = false;
	memset(m->nudge, 0, sizeof m->nudge); memset(m->nudge_in, 0, sizeof m->nudge_in); memset(m->wheel_v, 0, sizeof m->wheel_v);
	if (!immediate && !m->dry && SET.graceful_stop && m->active && m->op.kind == OP_LINE && m->v > 1e-6) {
		/* Controlled stop (user requirement 2026-10-18): brake along the programmed path at the
		 * axes' max deceleration so no step is lost (a lost step means re-homing and re-zeroing
		 * before a resume). The queue stays until the speed is 0 - braking may run on into the
		 * next segments; non-motion ops on the way are skipped. The overshoot is v^2/2a. */
		begin_brake(m); m->hold = false;
		return;
	}
	flush_queue(m);
	if (!m->dry) {
		/* Measured on the unit: when the FPGA can't keep up (40 000 steps/s cap, render stalls)
		 * the plan runs ahead of what was sent, and a decelerating stop still sends that backlog
		 * (5+ mm overrun at 1200 mm/min). So a stop drops every planned-but-unsent step: the
		 * machine stops once the blocks already in the FPGA's queue have played (<= queue_ms). */
		for (int a = 0; a < NAXES; a++) m->pos[a] = (double)m->steps[a] / SET.ax[a].steps_per_mm;
		immediate = true;
	}
	/* Simulate: always stop dead. Found on the unit 2026-10-21: a "braking" simulated move left
	 * active here kept running after the simulation ended and real output was back on - the
	 * next slice put the position back on the simulated path and the FPGA was sent there. */
	if (m->dry) immediate = true;
	if (immediate || !m->active || m->v < 1e-6) { m->active = false; m->v = 0; m->a = 0; m->stopping = false; m->hold = false; }
	else begin_brake(m);
	/* what is already queued in the FPGA (<= queue_ms) would still play; the mode flush is
	 * our candidate for emptying it (to be measured). The counters are then the truth:
	 * steps[] is adopted from them at rest without a position-fault toast. */
	if (immediate && !m->dry && m->q_est > 0) { fpga_block_flush(m->f); m->q_est = 0; m->stop_resync = true; }
	bool coasting = false;
	for (int a = 0; a < NAXES; a++) coasting |= fabs(m->jv[a]) > 1e-12;
	if (brake && coasting) begin_brake(m);                      /* run_simple clears it at rest */
	else { memset(m->jv, 0, sizeof m->jv); memset(m->ja, 0, sizeof m->ja); }
	memcpy(m->plan_end, m->pos, sizeof m->plan_end);
}

void motion_nudge(motion_t *m, int a, double mm) { m->nudge[a] += mm; m->nudge_in[a] += mm; if (mm) m->last_dir[a] = mm > 0 ? 1 : -1; }

static bool next_op(motion_t *m)
{
	while (m->qn) {
		gc_op_t op = m->q[m->qh];
		struct plan p = m->pl[m->qh];
		m->qh = (m->qh + 1) % MQ_LEN; m->qn--;
		if (m->replan_from > 0) m->replan_from--;
		m->line = op.line >= 0 ? op.line : m->line;
		if (m->stopping && op.kind != OP_LINE) continue;    /* braking: only motion matters */
		if (op.kind == OP_LINE) {
			if (!p.line) continue;
			m->qdist -= p.len;
			if (m->qdist < 0) m->qdist = 0;
			memcpy(m->start, m->pos, sizeof m->start);
			m->cur = p; m->op = op; m->s = 0; m->active = true;
			memcpy(m->u, p.u, sizeof m->u); m->len = p.len; m->vmax = p.vnom; m->acc = p.acc;
			if (m->v > p.ve + 1e-9) m->v = p.ve;      /* the corner limit (an allowed instant change) */
			return true;
		}
		if (op.kind == OP_DWELL) { m->op = op; m->dwell_left = op.value; m->active = true; m->v = 0; m->a = 0; return true; }
		/* spindle / coolant / pause / end: hand to the controller, motion has stopped here */
		m->ev = op.kind == OP_SPINDLE ? MEV_SPINDLE : op.kind == OP_COOLANT ? MEV_COOLANT
			: op.kind == OP_PAUSE ? MEV_PAUSE : op.kind == OP_END ? MEV_END : MEV_NONE;
		m->ev_op = op;
		m->v = 0; m->a = 0;
		if (op.kind == OP_SPINDLE && op.ival && SET.spindle_delay > 0) {   /* wait for spin-up */
			m->op = (gc_op_t){ .kind = OP_DWELL, .line = op.line }; m->dwell_left = SET.spindle_delay; m->active = true;
		}
		if (op.kind == OP_PAUSE || op.kind == OP_END) m->hold = op.kind == OP_PAUSE;
		return m->active;
	}
	return false;
}

/* S-curve step: move the acceleration towards `want` by at most jerk*dt, then the speed */
static void scurve(motion_t *m, double want, double dt)
{
	double dj = m->cur.jerk * dt;
	if (m->a < want) m->a = fmin(want, m->a + dj); else m->a = fmax(want, m->a - dj);
	m->v += m->a * dt;
	if (m->v < 0) m->v = 0;
}

static bool jog_coasting(const motion_t *m)
{
	for (int a = 0; a < NAXES; a++) if (fabs(m->jv[a]) > 1e-12) return true;
	return false;
}

static void run_lines(motion_t *m, double dt)
{
	if (m->feed_ov != m->last_ov) { m->last_ov = m->feed_ov; if (m->qn) m->replan_from = 0; }
	replan(m);
	m->applied_pct = -1;
	double budget = dt;
	while (budget > 1e-9) {
		if (!m->active && (m->hold || m->ev != MEV_NONE || !next_op(m))) break;
		if (m->op.kind == OP_DWELL) {
			if (m->hold) break;
			m->dwell_left -= budget; budget = 0;
			if (m->dwell_left <= 0) { budget = -m->dwell_left; m->active = false; }
			continue;
		}
		const struct plan *c = &m->cur;
		bool halt = m->hold || m->stopping;
		double vt = halt ? 0 : vrun(m, c);
		double vexit = m->qn && m->pl[m->qh].line ? m->pl[m->qh].ve : 0;   /* planned entry of the next segment */
		double rem = c->len - m->s;
		double A = c->acc, J = c->jerk;
		double want;
		if (!halt && m->v > vexit && rem - m->v * budget <= brake_dist(m->v, vexit, A, J, m->a))
			want = -fmin(A, sqrt(2 * J * (m->v - vexit)));                              /* brake for what's coming */
		else if (m->v < vt - 1e-6) want = fmin(A, sqrt(2 * J * (vt - m->v)));                      /* rounded top */
		else if (m->v > vt + 1e-6) want = -fmin(A, sqrt(2 * J * (m->v - vt)));                    /* hold / override lowered */
		else want = 0;
		scurve(m, want, budget);
		if (m->v > vt && want >= 0) m->v = vt;
		if (m->v < 1e-4 && (halt || vt < 1e-4)) {
			m->v = 0; m->a = 0;
			if (m->stopping) { m->active = false; m->stopping = false; m->hold = false; flush_queue(m); memcpy(m->plan_end, m->pos, sizeof m->plan_end); }
			break;
		}
		if (m->v < 1e-6) { budget = 0; break; }     /* still ramping the acceleration up from rest */
		if (!c->rapid && c->vnom > 0) m->applied_pct = m->v / c->vnom * 100;
		double ds = m->v * budget;
		if (ds >= rem) {
			budget -= rem / m->v;
			m->s = c->len;
			memcpy(m->pos, m->op.end, sizeof m->pos);
			m->active = false;
			if (m->v > vexit) m->v = vexit;             /* within the corner jump by construction */
			if (m->v < 1e-9) m->a = 0;                   /* at rest: nothing left to ramp */
		} else {
			m->s += ds; budget = 0;
			for (int a = 0; a < NAXES; a++) m->pos[a] = m->start[a] + c->u[a] * m->s;
		}
	}
	if (m->stopping && !m->active && !m->qn && !jog_coasting(m)) { m->stopping = false; m->v = 0; m->a = 0; }   /* braked past the last segment (a braking jog: run_simple ends it) */
}

void motion_jog_cont(motion_t *m, int a, int dir)
{
	if (!dir) { m->jog_dir = 0; return; }
	m->jog_axis = a; m->jog_dir = dir; m->last_dir[a] = dir;
}

/* distance left before axis a reaches its soft/bench limit going in direction dir
 * (huge when no limit is in force) */
static double room(const motion_t *m, int a, int dir)
{
	double lo, hi;
	if (!m->range || !m->range(a, &lo, &hi)) return 1e18;
	double d = dir > 0 ? hi - m->pos[a] : m->pos[a] - lo;
	return d > 0 ? d : 0;
}

/* Speed from which an axis can still stop within d at acceleration A and jerk J: the S-curve
 * stop distance is v^2/2A + v A/2J (the accel ramp costs A/J of time at full speed). */
static double v_stop(double d, double A, double J)
{
	if (d <= 0) return 0;
	if (J <= 0) return sqrt(2 * A * d);               /* no jerk limit: plain ramp */
	double k = A * A / (2 * J);
	return -k + sqrt(k * k + 2 * A * d);
}

#define APPROACH_MIN_MM 10.0
/* Speed cap towards the nearer limit inside the approach zone (user 2026-10-31: "between
 * soft limits and bench limits allow 40 % speed in the limited direction"): the zone is the
 * gap between the bench and the soft limit on that side, at least APPROACH_MIN_MM. Away from
 * the limit the full speed stays - braking distance goes with v², so this is what shrinks the
 * overtravel on a switch. room_mm is the distance left to the binding limit. */
static double approach_vcap(const motion_t *m, int a, int s, double room_mm, double vcap)
{
	double lo, hi;
	if (SET.approach_pct >= 100 || room_mm > 1e17 || !m->range || !m->range(a, &lo, &hi)) return vcap;
	const axis_set_t *x = &SET.ax[a];
	double zone = APPROACH_MIN_MM;
	/* the soft limit binds (bench limits off, or the bench box beyond it): the whole band
	 * between the bench box and the soft limit is the zone. The bench limit binding: 10 mm */
	double lim = s > 0 ? hi : lo, soft = s > 0 ? x->soft_max : x->soft_min;
	if (fabs(lim - soft) < 1e-9) zone = fmax(zone, s > 0 ? soft - x->bench_max : x->bench_min - soft);
	if (room_mm < zone) vcap = fmin(vcap, x->max_speed * SET.approach_pct / 100.0);
	return vcap;
}

/* One axis of a jog or a wheel move, one step of dt: an S-curve towards s * vcap (s = 0:
 * brake to a standstill), never faster than can still stop within dist, and landing
 * EXACTLY on dist. Returns the distance moved (signed). */
static double follow(motion_t *m, int a, int s, double dist, double vcap, double dt)
{
	/* jogs and the wheel use their own jerk (jog.jerk), 0 = acceleration only: LinuxCNC and
	 * grbl jog without a jerk limit too, and the A/J ramp-in is pure latency at the wheel
	 * (user + review 2026-10-31). Programs keep the axis jerk. */
	double A = SET.ax[a].accel, J = SET.jog_jerk, v = m->jv[a], acc = m->ja[a];
	double want = 0;
	/* 0.85 A: braking starts with margin for the acceleration still being ramped down */
	if (s && v * s >= 0) want = s * fmin(vcap, v_stop(dist, J > 0 ? 0.85 * A : A, J));   /* against the motion: brake first */
	double dv = want - v;
	double ades = (dv > 0 ? 1 : -1) * (J > 0 ? fmin(A, sqrt(2 * J * fabs(dv))) : A);
	if (fabs(dv) < 1e-12) ades = 0;
	if (J > 0) { double dj = J * dt; acc = acc < ades ? fmin(ades, acc + dj) : fmax(ades, acc - dj); }
	else acc = ades;
	/* the last stretch: exactly the deceleration that stops on dist (never above A), so a
	 * wheel move lands on its count at walking pace instead of stopping dead */
	if (s && v * s > 0 && dist > 1e-9) {
		double need = v * v / (2 * dist);
		if (need > 0.5 * A && need > fabs(acc) * (acc * s < 0)) acc = -s * fmin(need, A);
	}
	double nv = v + acc * dt;
	if ((dv > 0 && nv > want) || (dv < 0 && nv < want)) { nv = want; acc = 0; }
	/* braking for the landing never runs through zero (without a jerk limit the exact
	 * deceleration can be larger than v/dt: the axis then stepped BACK and oscillated) */
	if (s && v * s > 0 && nv * s < 0) { nv = 0; acc = -v / dt; }
	double step = (v + nv) / 2 * dt;
	if (s && step * s >= dist) { step = s * dist; nv = 0; acc = 0; }   /* land on it */
	if (!s && v * nv <= 0) { nv = 0; acc = 0; }                         /* braked to a standstill */
	m->jv[a] = nv; m->ja[a] = acc;
	return step;
}

static void run_simple(motion_t *m, double dt)
{
	/* continuous jog and pendant (wheel) moves, per axis on its own S-curve (accel + jerk,
	 * user 2026-10-31: starting and stopping dead at 2000 steps/mm lost steps). A wheel move
	 * always covers exactly the distance clicked - the ramp only shapes how; turning back
	 * brakes first. A released jog brakes to a standstill (it runs on a little). Near a
	 * soft/bench limit the speed is capped by the stopping distance, so the axis comes to
	 * rest exactly on the limit. */
	bool any = false;
	for (int a = 0; a < NAXES; a++) {
		double vcap = fmin(SET.ax[a].max_speed, SUSTAIN_SPS / SET.ax[a].steps_per_mm);
		int s = 0; double dist = 0;
		bool jog = m->jog_dir && m->jog_axis == a, wheel = fabs(m->nudge[a]) > 1e-9;
		/* the wheel's own speed, averaged over ~80 ms: a pendant is not a smooth input (hard
		 * detents, a hand that catches), and it decays to 0 within a few slices of the last click */
		m->wheel_v[a] += (m->nudge_in[a] / dt - m->wheel_v[a]) * fmin(1.0, dt / 0.08);
		m->nudge_in[a] = 0;
		if (jog) {
			s = m->jog_dir; dist = room(m, a, s);
			vcap = approach_vcap(m, a, s, dist, fmin(vcap, SET.jog_speed));
			if (dist < 0.0005 && fabs(m->jv[a]) < 1e-9) {        /* arrived at the limit */
				if (dist < 1e17) m->soft_stop = a + 1;
				m->jog_dir = 0; continue;
			}
		} else if (wheel) {
			s = m->nudge[a] > 0 ? 1 : -1;
			double r = room(m, a, s);
			if (r < 0.0005 && fabs(m->jv[a]) < 1e-9) {            /* at the limit: drop the rest */
				m->nudge[a] = 0; if (r < 1e17) m->soft_stop = a + 1; continue;
			}
			vcap = approach_vcap(m, a, s, r, vcap);
			/* velocity follow: never more queued than ~100 ms of travel plus the stop, so the
			 * axis stops with the wheel; the clicks it could not keep up with are dropped */
			if (SET.pendant_follow == 1) {
				double cap = vcap * 0.1 + vcap * vcap / (2 * SET.ax[a].accel);
				if (fabs(m->nudge[a]) > cap) m->nudge[a] = s * cap;
			}
			dist = fmin(fabs(m->nudge[a]), r);
			/* Tracking the wheel: speed = the wheel's own (averaged) speed, feedforward, plus
			 * the distance still queued / pendant.smooth_ms, the correction. The feedforward
			 * keeps up with a hard turn without the lag growing; the correction makes a
			 * single click a gentle move, closes whatever the wheel ran ahead, and settles
			 * the stop exponentially instead of a stop-and-go per click (user 2026-11-01:
			 * "the machine makes interrupted steps, it never sums them up"; "smoother, not
			 * slower: a bigger error, more speed; average the input without delaying the
			 * first response" - the correction acts on the first click, the average follows).
			 * A floor keeps a tiny remainder from crawling: at least a 0.01 mm click's pace. */
			if (SET.pendant_smooth_ms > 0) {
				double tau = SET.pendant_smooth_ms / 1000.0, ff = fmax(m->wheel_v[a] * s, 0);
				vcap = fmin(vcap, fmax(ff + dist / tau, fmax(0.01 / tau, 50.0 / SET.ax[a].steps_per_mm)));
			}
		} else if (fabs(m->jv[a]) < 1e-12) continue;
		double step = follow(m, a, s, dist, vcap, dt);
		m->pos[a] += step;
		if (wheel) { m->nudge[a] -= step; if (fabs(m->nudge[a]) < 1e-9) m->nudge[a] = 0; }
		if (step) m->last_dir[a] = step > 0 ? 1 : -1;
		any |= fabs(m->jv[a]) > 1e-12;
	}
	if (!any && m->stopping && !m->active) m->stopping = false;   /* a braked jog is at rest */
}

/* ---- homing: per axis in the configured order, search -> back off -> set -> retract ---- */
static int home_axis_at(int k)
{
	static const int orders[][NAXES] = { { 2, 0, 1, 3 }, { 2, 0, 1, -1 }, { 2, 1, 0, 3 }, { 0, 1, 2, 3 } };
	int o = CLAMP(SET.home_order, 0, 3);
	return k < NAXES ? orders[o][k] : -1;
}

void motion_home(motion_t *m, int axis)
{
	motion_stop(m, true);
	m->home.on = true; m->home.k = -1; m->home.phase = 99; m->home.only = axis;
}

static void run_home(motion_t *m, double dt)
{
	if (m->home.phase == 99) {                         /* next axis */
		int ax;
		do { m->home.k++; ax = home_axis_at(m->home.k); }
		while (ax >= 0 && (!SET.ax[ax].enabled || !axis_home_port(ax) || (m->home.only >= 0 && ax != m->home.only)));
		if (ax < 0 || m->home.k >= NAXES) { m->home.on = false; m->ev = MEV_HOMED; return; }
		m->home.axis = ax; m->home.phase = 0; m->home.travel = 0;
	}
	/* Every phase moves on the axis' own ramp (follow(): accel + jog.jerk, like a jog)
	 * instead of jumping to its speed (user 2026-11-02: homing was snappy, no acceleration,
	 * and the retract stopped dead). The search brakes ON the switch, the slow back-off
	 * brakes once it releases, and the retract lands on its distance. The reference is the
	 * RELEASE point at the slow speed; whatever the axis still travels while braking is
	 * known, so the counter is preset relative to it and the retract ends exactly at
	 * home_pos - dir * home_backoff. */
	int a = m->home.axis;
	const axis_set_t *s = &SET.ax[a];
	int dir = s->home_dir ? 1 : -1;
	bool sw = m->switch_active && m->switch_active(a);
	double v_search = s->home_search / 60.0, v_locate = s->home_locate / 60.0, step;
	/* a phase that ends hands this tick to the next one: a tick that moved nothing is a
	 * 10 ms stall in the stream (seen as a step in the speed) */
	if (m->home.phase == 0) {                          /* search toward the switch */
		if (sw) m->home.phase = 1;
		else {
			step = follow(m, a, dir, 1e18, v_search, dt);
			m->pos[a] += step; m->home.travel += fabs(step); m->last_dir[a] = dir;
			if (m->home.travel > fabs(s->soft_max - s->soft_min) + 100) { m->home.on = false; m->ev = MEV_HOME_FAIL; }
			return;
		}
	}
	if (m->home.phase == 1) {                          /* on the switch: brake to a stop */
		m->pos[a] += follow(m, a, 0, 0, 0, dt);
		if (fabs(m->jv[a]) > 1e-12) return;
		m->home.phase = 2; m->home.travel = 0;
	}
	if (m->home.phase == 2) {                          /* back off slowly until released: the reference */
		if (sw) {
			step = follow(m, a, -dir, 1e18, v_locate, dt);
			m->pos[a] += step; m->home.travel += fabs(step); m->last_dir[a] = -dir;
			if (m->home.travel > 25) {                 /* switch never releases */ m->home.on = false; m->ev = MEV_HOME_FAIL; }
			return;
		}
		/* the COUNTER at the release, not the commanded position: the command runs a queue
		 * (50 ms) ahead of the motor, which at the slow speed is tens of microns. What is
		 * left is the tick: the switch is sampled every 10 ms and the counters latch at the
		 * heartbeat, so the reference is within home_locate x ~20 ms of the switch edge */
		int32_t cnt[NAXES];
		if (!m->dry) { fpga_positions(m->f, cnt); m->home.ref = (double)cnt[a] / SET.ax[a].steps_per_mm; }
		else m->home.ref = m->pos[a];
		m->home.phase = 3;
	}
	if (m->home.phase == 3) {                          /* brake, then preset the counter */
		if (fabs(m->jv[a]) > 1e-12) { m->pos[a] += follow(m, a, 0, 0, 0, dt); return; }
		/* ... only once the FPGA has played its whole queue */
		if (m->q_est > 0 || m->hw_busy) return;
		motion_set_pos(m, a, s->home_pos + ((double)m->steps[a] / SET.ax[a].steps_per_mm - m->home.ref));   /* the release point = home_pos */
		fpga_preset(m->f, a, (int32_t)m->steps[a]);      /* FPGA counter = machine coordinate */
		m->home.phase = 4;
	}
	if (m->home.phase == 4) {                          /* retract: land on home_pos - dir * back-off */
		double left = (s->home_pos - dir * s->home_backoff - m->pos[a]) * -dir;
		if (left <= 1e-9 && fabs(m->jv[a]) < 1e-12) { m->home.phase = 99; return; }
		step = follow(m, a, -dir, fmax(left, 0), v_search, dt);
		m->pos[a] += step; m->last_dir[a] = -dir;
	}
}

/* ---- blocks (2026-11-01, docs/block-mode.md): one block per 10 ms carrying all axes ---- */
#define BLOCK_DT 0.010

/* the step deltas of this block, gated: a refused (runaway) or blocked axis is rebased.
 * A dry run counts the steps and sends nothing. */
static int emit_block(motion_t *m)
{
	int32_t d[NAXES]; bool any = false;
	for (int a = 0; a < NAXES; a++) {
		double spm = SET.ax[a].steps_per_mm;
		int64_t dd = llround(m->pos[a] * spm) - m->steps[a];
		double top = fmin(SET.ax[a].max_speed * spm, FPGA_MAX_SPS);
		/* a delta no plan can produce: more than a whole second at the axis' top speed (the
		 * legitimate backlog is a few mm). Refuse it rather than send it */
		if ((dd > 0 ? dd : -dd) > (int64_t)top + 100) {
			m->runaway = a + 1;
			motion_stop(m, true);
			for (int k = 0; k < NAXES; k++) m->pos[k] = (double)m->steps[k] / SET.ax[k].steps_per_mm;
			memcpy(m->plan_end, m->pos, sizeof m->plan_end);
			memcpy(m->emit_pos, m->pos, sizeof m->emit_pos);
			m->home.on = false;
			return 0;
		}
		/* level gate: braking (stopping) and homing may run on into a switch, nothing else */
		if (dd && !m->dry && !m->stopping && !m->home.on && m->axis_blocked && m->axis_blocked(a, dd > 0 ? 1 : -1)) {
			m->blocked++;
			m->pos[a] = (double)m->steps[a] / spm;         /* drop it, don't bank it */
			if (!m->qn) m->plan_end[a] = m->pos[a];
			dd = 0;
		}
		d[a] = (int32_t)dd;
		if (dd) { m->last_dir[a] = dd > 0 ? 1 : -1; any = true; }
	}
	memcpy(m->emit_pos, m->pos, sizeof m->emit_pos);
	if (PERF.debug && !m->dry) fprintf(stderr, "%llu emit: d %d %d %d %d active %d qn %d v %.3f jv %.3f %.3f %.3f ev %d\n", (unsigned long long)now_us() / 1000,
		d[0], d[1], d[2], d[3], m->active, m->qn, m->v, m->jv[0], m->jv[1], m->jv[2], (int)m->ev);
	if (!any) return 0;
	if (!m->dry) { fpga_block(m->f, d, BLOCK_DT); m->blocks++; }
	for (int a = 0; a < NAXES; a++) m->steps[a] += d[a];
	return 1;
}

/* The FPGA's queue is the clock: plan in fixed 10 ms blocks until the queue holds
 * queue_ms (or the frame about to be drawn), whatever the wall clock did. With nothing to
 * move, one planner step per wall tick keeps the wheel's averaging and the jog ramps in
 * wall time. */
static void tick_blocks(motion_t *m, double dt)
{
	m->hw_busy = fpga_moving(m->f);
	m->q_est = fpga_block_queue(m->f);
	rescale(m);
	if (m->stopping && (m->stop_left -= dt) < 0) { motion_stop(m, true); m->stop_cut = true; }
	int target = (m->feed_ahead_ms > SET.queue_ms ? m->feed_ahead_ms : SET.queue_ms) / 10;
	if (target < 1) target = 1;
	m->feed_ahead_ms = 0;
	/* The FPGA stopped playing (2026-11-01: a bad word in a block did it): blocks queued, the
	 * counters frozen, nothing reported moving. Pushing on would fill the queue and the block
	 * ioctl then hangs the process - so this is a fault, not something to wait out. */
	if (m->fpga_halted) {                                 /* until the queue reads empty again (a power cycle), no motion at all */
		if (m->q_est == 0 && !m->hw_busy) m->fpga_halted = false;
		else { if (m->active || m->qn || m->jog_dir) motion_stop(m, true); return; }
	}
	if (m->q_est > 0 && !m->hw_busy) { if (++m->stuck_ticks >= 30) { m->fpga_stuck = m->fpga_halted = true; m->stuck_ticks = 0; motion_stop(m, true); return; } }
	else m->stuck_ticks = 0;
	/* the depth is checked BEFORE each block: sending one first "and then seeing" left the
	 * queue one block deeper after every frame top-up, for good (sim trace 2026-11-01) */
	int q0 = m->q_est, sent = 0;
	for (int i = 0; i < 32 && m->q_est < target && m->q_est < FPGA_QUEUE_FULL; i++) {
		if (m->home.on) run_home(m, BLOCK_DT);
		else { run_lines(m, BLOCK_DT); run_simple(m, BLOCK_DT); }
		if (!emit_block(m)) break;                         /* nothing to move: idle, or gated */
		m->q_est++; sent++;
	}
	if (PERF.debug && (sent || q0)) fprintf(stderr, "%llu blocks: queue %d target %d sent %d busy %u\n", (unsigned long long)now_us() / 1000, q0, target, sent, m->hw_busy);
}

void motion_tick(motion_t *m, double dt)
{
	if (dt <= 0) return;                                 /* planned time is ahead of real time: nothing to plan or send */
	if (!m->dry) {
		/* block mode is set once (fpga_init resets it: ctl_apply_axis_config clears fpga_mode) */
		if (m->fpga_mode != 1) { fpga_block_mode(m->f, true); m->fpga_mode = 1; }
		tick_blocks(m, dt);
		return;
	}
	/* simulate (dry run): no FPGA, so no queue to pace by - the wall clock drives the planner */
	m->hw_busy = 0;
	rescale(m);
	if (m->stopping && (m->stop_left -= dt) < 0) { motion_stop(m, true); m->stop_cut = true; }
	if (m->home.on) run_home(m, dt);
	else {
		/* the path in steps of at most 10 ms, however long this tick was: the braking and
		 * S-curve decisions are taken per step, so a late tick (a screen redraw) does not
		 * make the machine brake early */
		for (double left = dt; left > 1e-9; ) {
			double step = left > MOTION_SUBSTEP ? MOTION_SUBSTEP : left;
			run_lines(m, step);
			left -= step;
		}
		run_simple(m, dt);
	}
	emit_block(m);
}
