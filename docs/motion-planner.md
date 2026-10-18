# Motion planner: lookahead, corner speeds, S-curve

_2026-10-18. Code: [../app/src/core/motion.c](../app/src/core/motion.c), tests:
[../app/tests/test_planner.c](../app/tests/test_planner.c)._

## What a "segment" is

One straight line the machine moves along: a G0/G1 block, or one chord of an arc (G2/G3
are cut into chords that deviate at most 0.005 mm from the true circle). A 10 mm circle
is ~100 segments; engraving programs are often thousands of 0.01–0.1 mm segments.

## Lookahead depth

The queue (up to 512 segments) is kept filled to **at least `planner.lookahead` segments
(default 64) AND at least the braking distance from top speed** (at 200 % feed override,
jerk included). Lots of tiny segments therefore don't shorten the view ahead: in the test,
3000 segments of 0.01 mm run at full feed (20/20 mm/s) instead of crawling.

## Corner speed (per axis, not by angle)

At each junction every axis may change speed **instantly** by at most its
**corner jump** (`axis.X.corner_jump`, mm/min, default 300). The corner speed is the highest
speed at which no axis exceeds that:

| Corner | What happens (1200 mm/min feed, defaults) |
|---|---|
| X-only line bending 10° towards Y | Y must jump from 0 to v·sin10° → keeps full speed (20 of 20 mm/s) |
| 90° | X must drop to 0 and Y start → ~3.9 mm/s |
| reversal | X needs 2·v → comes to a stop, as it physically must |

A junction-deviation tolerance (`planner.junction_dev`, default 0.02 mm, 0 = off) adds the
usual centripetal limit; the lower of the two applies.

## Braking for what's coming

Every time segments are added (or the feed override changes) the planner re-plans:

- **backward pass**: each segment's entry speed ≤ its corner speed, its own and the
  previous segment's nominal speed, and the speed from which it can still brake down to
  the next segment's entry within its length (the end of the queue counts as a stop);
- **forward pass**: ≤ what acceleration can reach from the speed before.

It stops early once a value comes out unchanged (nothing before it can change), so the
cost per added segment is small even with a deep queue.

## Acceleration and jerk (S-curve)

Per axis: `axis.X.accel` (mm/s²) and `axis.X.jerk` (mm/s³, default 10 000). Along a segment
the limits are the tightest of the axes involved, scaled by the direction.

- Execution changes the **acceleration** at most by jerk·dt per tick, with rounded tops and
  rounded bottoms (speed eases into the target instead of slamming at full ±A).
- Braking starts when the jerk-aware stopping distance
  `(v² − ve²)/2A + (v + ve)·A/2J` (plus undoing any acceleration still in progress) reaches the
  distance left.
- Planning across many segments uses each segment's **effective** acceleration
  `A·v/(v + A²/J)` (the average of a full S-curve speed change), so chains of tiny segments
  aren't charged a full jerk ramp per segment.

Measured in the tests while moving: acceleration and jerk stay within their limits.
At standstill moments (end of a move, the turning point of a reversal) any leftover
deceleration is dropped; nothing moves at that instant.

## Applied feed on the Run screen

The Feed bar is blue up to the selected override. While cutting, the speed actually applied
right now (`applied % = speed / programmed feed`) is marked, and the gap up to the selected
value is shaded **orange-grey**: you see the machine slowing for a corner or braking for
what's ahead. `GET /api/v1/status` reports it as `overrides.feedApplied`.

## Tuning

| Setting | Effect |
|---|---|
| `axis.X.corner_jump` up | faster through corners, harder on the mechanics |
| `axis.X.jerk` down | softer, smoother starts and stops; longer braking |
| `axis.X.accel` | overall braking / speed-up rate |
| `planner.junction_dev` | 0 = corners limited by the corner jump only |
| `planner.lookahead` | minimum segments ahead (distance-based depth applies too) |

Not verified with motors attached yet: the step counts are exact on the real FPGA, but the
smoothness of the pulse stream between 10 ms bursts can only be judged on a real axis.

## Cost of smoothness (planned time, simulator)

| jerk (mm/s³) | Sign_rosette.nc | Bracket_v3.nc |
|---|---|---|
| 3 000 | 270 s | 71.6 s |
| **10 000 (default)** | 242 s | 69.7 s |
| 30 000 | 233 s | 69.0 s |
| ∞ (plain trapezoid) | 222 s | 68.4 s |

Corner jump 600 instead of 300 saves another ~3 %; 0 (stop at every bend) costs ~18 %. On the
real unit add the burst overhead (stop-go bursts, 40 000 steps/s ceiling): the rosette took 282 s
with the old trapezoid planner and 367 s with this planner at jerk 3000. Measure with
`tools/plan_bench.c` (virtual time, any program, several settings in one run).

## Measured on the real unit (2026-10-18, build local-54, jerk 10 000)

| | old trapezoid planner | this planner, before the render fix | **this planner** |
|---|---|---|---|
| Sign_rosette.nc (incl. 5 s start countdown) | 282 s | 367 s | **258 s** |
| Bracket_v3.nc (incl. countdown + M6 resume) | 86 s | 113 s | **93 s** |
| motion time lost to the 40 ms dt clamp | – | 18 % | **0** |

0 position faults in every run. The "before" column was CPU starvation from ~95 ms screen
renders, fixed in the UI by drawing/copying in pieces with a motion slice in between; the
planner itself costs ~0.13 ms per tick on the ARM926.

## Speed cap and stop overshoot (2026-10-18, after the graceful-stop change)

The real unit overshot stops by 2.0 mm (abrupt) and 3.1–3.5 mm (graceful) at 20 mm/s. Cause:
the plan ran at the FPGA's full 40 000 steps/s, but idle-only bursts with ~2 ms start overhead
can't sustain that, so a backlog of planned-but-unsent steps built up, and a stop still sent it.

- Planned axis speed is capped at **80 % of the FPGA ceiling** (32 000 steps/s = 16 mm/s =
  960 mm/min at 2000 steps/mm), per axis along the path direction. `vrun()` applies feed
  override (up to 300 %) and then this cap, so an override can never plan faster than the
  hardware delivers.
- One burst carries at most **30 ms** of motion (1 200 steps = 0.6 mm); the rest waits for
  the next slice. Steps in flight can't be recalled by a stop, so this bounds the abrupt-stop
  overshoot.
- The lookahead depth is sized with the same effective acceleration the planner uses, so tiny
  segments reach the cap (15.9 of 16 mm/s).

Simulator (now models the 2 ms burst overhead): backlog 0 steps at full speed; abrupt stop
0.00 mm overshoot; graceful stop 0.50 mm (≈ v²/2a 0.43 mm + jerk ramp).

On the unit (2026-10-18, Reset during a 60 mm X jog at the 16 mm/s cap, 3 runs each):

| Stop | Before the fix (20 mm/s) | After |
|---|---|---|
| Abrupt | ~2.0 mm | 0.51, 0.24, 0.28 mm |
| Graceful | 3.1–3.5 mm | 0.88, 0.97, 0.90 mm |

0 position faults, lostMs 0. The real FPGA runs on ~0.3–0.4 mm more than the simulator in both
modes: steps already in flight plus the time until the next tick notices the stop.
