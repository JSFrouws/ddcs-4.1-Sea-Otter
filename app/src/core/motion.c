#include <math.h>
#include <string.h>
#include "motion.h"
#include "settings.h"

/* What idle-only bursts can sustain per axis: each burst costs ~2 ms of start overhead and the
 * next one may only go in once the axis is idle again, so a plan at the full 40 000 steps/s
 * builds a backlog of unsent steps - which a stop then still has to send (measured: 2-3.5 mm
 * stop overshoot at 20 mm/s). Planning at 80 % keeps the backlog near zero. */
#define FPGA_SUSTAIN_SPS (FPGA_MAX_SPS * 0.8)
/* steps in one burst, at most: whatever is in flight can't be recalled by a stop */
#define BURST_MAX_S 0.030
/* longest kinematic step inside one tick (see motion_tick) */
#define MOTION_SUBSTEP 0.010

int rapid_override(int feed_ov) { return feed_ov < 100 ? feed_ov : 100; }

void motion_init(motion_t *m, fpga_t *f, const double mpos[NAXES])
{
	memset(m, 0, sizeof *m);
	m->f = f;
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
 * emit() sends pos * steps/mm - steps, so keeping pos across a new scale turns the difference
 * into one huge burst stream with no ramp and no limits (2026-10-31, X ran +336.6 mm). The
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
		double cap = fmin(SET.ax[a].max_vel / 60.0, FPGA_SUSTAIN_SPS / SET.ax[a].steps_per_mm);   /* FPGA limit */
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
		if (du > 1e-9) v = fmin(v, SET.ax[a].corner_jump / 60.0 / du);
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
		vmax = fmax(vmax, fmin(SET.ax[a].max_vel / 60.0, FPGA_SUSTAIN_SPS / SET.ax[a].steps_per_mm));
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
	if (m->active || m->qn || m->jog_dir || m->home.on || m->hw_busy) return false;
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
	memset(m->nudge, 0, sizeof m->nudge);
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
		 * machine stops at the end of the burst already in flight (one slice, ~0.2 mm). */
		for (int a = 0; a < NAXES; a++) m->pos[a] = (double)m->steps[a] / SET.ax[a].steps_per_mm;
		immediate = true;
	}
	/* Simulate: always stop dead. Found on the unit 2026-10-21: a "braking" simulated move left
	 * active here kept running after the simulation ended and real output was back on - the
	 * next slice put the position back on the simulated path and the FPGA was sent there. */
	if (m->dry) immediate = true;
	if (immediate || !m->active || m->v < 1e-6) { m->active = false; m->v = 0; m->a = 0; m->stopping = false; m->hold = false; }
	else begin_brake(m);
	bool coasting = false;
	for (int a = 0; a < NAXES; a++) coasting |= fabs(m->jv[a]) > 1e-12;
	if (brake && coasting) begin_brake(m);                      /* run_simple clears it at rest */
	else { memset(m->jv, 0, sizeof m->jv); memset(m->ja, 0, sizeof m->ja); }
	memcpy(m->plan_end, m->pos, sizeof m->plan_end);
}

void motion_nudge(motion_t *m, int a, double mm) { m->nudge[a] += mm; if (mm) m->last_dir[a] = mm > 0 ? 1 : -1; }

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

/* One burst per axis per slice, and ONLY into an idle axis. Measured on the unit: a burst sent
 * while the axis is still active is unreliable - an opposite one is ignored, and appending even
 * in the same direction loses steps at certain timings (docs/fpga-motion.md). A burst into an
 * idle axis has been step-exact in every test. So a busy axis just keeps its steps for the next
 * slice. The rate is chosen so each burst (incl. the FPGA's ~2 ms start overhead) ends inside
 * its slice and the axis is idle again when the next one is due. */
static int emit(motion_t *m, double dt)
{
	int bursts = 0;
	uint8_t moving = m->hw_busy;
	for (int a = 0; a < NAXES; a++) {
		int64_t d = llround(m->pos[a] * SET.ax[a].steps_per_mm) - m->steps[a];
		if (!d) continue;
		/* a delta no plan can produce: more than a whole second at the axis' top speed (the
		 * legitimate backlog is a few mm). Refuse it rather than send it */
		double top = fmin(SET.ax[a].max_vel / 60.0 * SET.ax[a].steps_per_mm, FPGA_MAX_SPS);
		if ((d > 0 ? d : -d) > (int64_t)top + 100) {
			m->runaway = a + 1;
			motion_stop(m, true);
			for (int k = 0; k < NAXES; k++) m->pos[k] = (double)m->steps[k] / SET.ax[k].steps_per_mm;
			memcpy(m->plan_end, m->pos, sizeof m->plan_end);
			memcpy(m->emit_pos, m->pos, sizeof m->emit_pos);
			m->home.on = false;
			return bursts;
		}
		/* level gate: braking (stopping) and homing may run on into a switch, nothing else */
		if (!m->dry && !m->stopping && !m->home.on && m->burst_blocked && m->burst_blocked(a, d > 0 ? 1 : -1)) {
			m->blocked++;
			m->pos[a] = (double)m->steps[a] / SET.ax[a].steps_per_mm;   /* drop it, don't bank it */
			if (!m->qn) m->plan_end[a] = m->pos[a];
			continue;
		}
		int64_t dmax = (int64_t)(FPGA_MAX_SPS * BURST_MAX_S);     /* the rest waits for the next slice */
		if (d > dmax) d = dmax; else if (d < -dmax) d = -dmax;
		int dir = d > 0 ? 1 : -1;
		m->last_dir[a] = dir;
		if (!m->dry) {
			if (moving & (1u << a)) { m->deferred++; continue; }
		}
		m->burst_dir[a] = dir;
		/* Pacing (2026-10-31, measured with a motor and the trace): the rate follows the PLANNED
		 * speed (+15 %), not "these steps within this slice". Slices come every 10-40 ms; sized
		 * to the slice, a burst still running when the next slice came made it skip and send
		 * double next time - 2x speed spikes and stops, the jerk felt on the machine. At the
		 * planned speed a skipped slice only makes the next burst longer, never faster; the
		 * 15 % drains the backlog. Without a planned speed (the last steps of a move, a stop)
		 * the old rule: fit burst + ~2 ms overhead into 85 % of the slice. */
		double spm = SET.ax[a].steps_per_mm, ad = (double)(d > 0 ? d : -d);
		double planned = fabs(m->pos[a] - m->emit_pos[a]) * spm / (dt > 1e-4 ? dt : 1e-4);
		double win = dt * 0.85 - 0.002;
		double r = ad / (win > 0.002 ? win : 0.002);
		/* not while homing: there the counter is preset mid-search, so every burst must be done
		 * by the next slice (a longer burst still stepping at the preset ran the sim 20 steps off) */
		if (planned > 100 && !m->home.on) r = fmax(planned * 1.15 + 50, ad / 0.120);   /* backlog gone within ~120 ms */
		/* never faster than the axis' own top speed + 15 % (review 2026-10-31): a backlog after a
		 * stall drains at most that fast, or it is the same spike again - a stepper stalls on it */
		r = fmin(r, fmax(top * 1.15, planned * 1.15 + 50));   /* top: the guard's max_vel rate above */
		uint32_t rate = r > FPGA_MAX_SPS ? FPGA_MAX_SPS : (uint32_t)r + 1;
		m->rate[a] = rate;
		while (d) {                                   /* bursts are 32-bit */
			int32_t n = d > 0x7FFFFFFF ? 0x7FFFFFFF : d < -0x7FFFFFFF ? -0x7FFFFFFF : (int32_t)d;
			if (!m->dry) fpga_burst(m->f, a, n, rate);
			m->steps[a] += n; d -= n; bursts++;
		}
	}
	memcpy(m->emit_pos, m->pos, sizeof m->emit_pos);
	return bursts;
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
	double k = A * A / (2 * J);
	return d > 0 ? -k + sqrt(k * k + 2 * A * d) : 0;
}

/* One axis of a jog or a wheel move, one step of dt: an S-curve towards s * vcap (s = 0:
 * brake to a standstill), never faster than can still stop within dist, and landing
 * EXACTLY on dist. Returns the distance moved (signed). */
static double follow(motion_t *m, int a, int s, double dist, double vcap, double dt)
{
	double A = SET.ax[a].accel, J = SET.ax[a].jerk, v = m->jv[a], acc = m->ja[a];
	double want = 0;
	/* 0.85 A: braking starts with margin for the acceleration still being ramped down */
	if (s && v * s >= 0) want = s * fmin(vcap, v_stop(dist, 0.85 * A, J));   /* against the motion: brake first */
	double dv = want - v;
	double ades = (dv > 0 ? 1 : -1) * fmin(A, sqrt(2 * J * fabs(dv)));
	if (fabs(dv) < 1e-12) ades = 0;
	double dj = J * dt;
	acc = acc < ades ? fmin(ades, acc + dj) : fmax(ades, acc - dj);
	/* the last stretch: exactly the deceleration that stops on dist (never above A), so a
	 * wheel move lands on its count at walking pace instead of stopping dead */
	if (s && v * s > 0 && dist > 1e-9) {
		double need = v * v / (2 * dist);
		if (need > 0.5 * A && need > fabs(acc) * (acc * s < 0)) acc = -s * fmin(need, A);
	}
	double nv = v + acc * dt;
	if ((dv > 0 && nv > want) || (dv < 0 && nv < want)) { nv = want; acc = 0; }
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
		double vcap = fmin(SET.ax[a].max_vel / 60.0, FPGA_SUSTAIN_SPS / SET.ax[a].steps_per_mm);
		int s = 0; double dist = 0;
		bool jog = m->jog_dir && m->jog_axis == a, wheel = fabs(m->nudge[a]) > 1e-9;
		if (jog) {
			s = m->jog_dir; dist = room(m, a, s);
			vcap = fmin(vcap, SET.jog_feed / 60.0);
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
			dist = fmin(fabs(m->nudge[a]), r);
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

void motion_home(motion_t *m)
{
	motion_stop(m, true);
	m->home.on = true; m->home.k = -1; m->home.phase = 99;
}

static void run_home(motion_t *m, double dt)
{
	if (m->home.phase == 99) {                         /* next axis */
		int ax;
		do { m->home.k++; ax = home_axis_at(m->home.k); } while (ax >= 0 && (!SET.ax[ax].enabled || !SET.ax[ax].home_port));
		if (ax < 0 || m->home.k >= NAXES) { m->home.on = false; m->ev = MEV_HOMED; return; }
		m->home.axis = ax; m->home.phase = 0; m->home.travel = 0;
	}
	int a = m->home.axis;
	const axis_set_t *s = &SET.ax[a];
	int dir = s->home_dir ? 1 : -1;
	bool sw = m->switch_active && m->switch_active(a);
	double d;
	switch (m->home.phase) {
	case 0:                                            /* search toward the switch */
		if (sw) { m->home.phase = 1; m->home.travel = 0; break; }
		d = s->home_search / 60.0 * dt;
		m->pos[a] += dir * d; m->home.travel += d; m->last_dir[a] = dir;
		if (m->home.travel > fabs(s->soft_max - s->soft_min) + 100) { m->home.on = false; m->ev = MEV_HOME_FAIL; }
		break;
	case 1:                                            /* back off slowly until released */
		if (!sw) { m->home.phase = 2; break; }
		d = s->home_locate / 60.0 * dt;
		m->pos[a] -= dir * d; m->last_dir[a] = -dir; m->home.travel += d;
		if (m->home.travel > 25) {                     /* switch never releases */ m->home.on = false; m->ev = MEV_HOME_FAIL; }
		break;
	case 2:                                            /* this is the reference */
		motion_set_pos(m, a, s->home_pos);
		fpga_preset(m->f, a, (int32_t)m->steps[a]);      /* FPGA counter = machine coordinate */
		m->home.travel = 0; m->home.phase = 3;
		break;
	case 3:                                            /* retract the back-off distance */
		d = fmin(s->home_search / 60.0 * dt, s->home_backoff - m->home.travel);
		m->pos[a] -= dir * d; m->home.travel += d;
		if (m->home.travel >= s->home_backoff - 1e-9) m->home.phase = 99;
		break;
	}
}

void motion_tick(motion_t *m, double dt)
{
	m->hw_busy = m->dry ? 0 : fpga_moving(m->f);
	rescale(m);
	if (m->stopping && (m->stop_left -= dt) < 0) { motion_stop(m, true); m->stop_cut = true; }
	if (m->home.on) run_home(m, dt);
	else {
		/* the path in steps of at most 10 ms, however long this tick was: the braking and
		 * S-curve decisions are taken per step, so a late tick (a screen redraw) no longer
		 * makes the machine brake early - measured: a 32 ms tick every 18 made a 1200-segment
		 * engraving 2.4 % slower than at steady 10 ms ticks. Still one burst per tick. */
		for (double left = dt; left > 1e-9; ) {
			double step = left > MOTION_SUBSTEP ? MOTION_SUBSTEP : left;
			run_lines(m, step);
			left -= step;
		}
		run_simple(m, dt);
	}
	if (emit(m, dt) && !m->dry) {
		fpga_status_sweep(m->f);      /* as the vendor app does after its bursts */
		m->hw_busy = fpga_moving(m->f);
	}
}
