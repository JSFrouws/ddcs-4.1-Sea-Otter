#!/usr/bin/env python3
"""Read a pendant session out of GET /api/v1/trace: loop holes, click-to-step latency,
lost clicks, and whether a steady turn is one continuous move.

    curl -s 'http://192.168.1.200:8080/api/v1/trace?clear=1' > /dev/null   # before the test
    curl -s 'http://192.168.1.200:8080/api/v1/trace' > trace.csv            # after (not during: ~500 ms)
    python3 tools/wheel_trace.py trace.csv [x|y|z|a]

The trace records a row per motion slice while the pendant is held or an axis moves, so a
gap between rows is either a loop stall or the dead-man released; "stalls" below counts
gaps with held=1 on both sides. The 2026-11-01 sessions are described in
docs/motion-planner.md ("The half a second late pendant")."""
import collections, csv, statistics, sys

path = sys.argv[1]
ax = sys.argv[2] if len(sys.argv) > 2 else "x"
rows = list(csv.DictReader(open(path)))
T = [int(r["t_ms"]) for r in rows]
cmd, fpga = ax + "_cmd", ax + "_fpga"

gaps = [(i, T[i] - T[i - 1]) for i in range(1, len(rows))]
hist = collections.Counter(min(g, 1000) // 10 * 10 for _, g in gaps)
print(f"{len(rows)} rows; row spacing (ms bucket: rows): {sorted(hist.items())}")
stalls = [(T[i - 1], g) for i, g in gaps if g > 100 and rows[i]["held"] == "1" and rows[i - 1]["held"] == "1"]
print(f"loop holes > 100 ms with the wheel held: {len(stalls)}  {stalls[:30]}")

# every wheel change: how long until the commanded position (and the FPGA) moved
lat = []
for i in range(1, len(rows)):
    if rows[i]["mpg"] == rows[i - 1]["mpg"]:
        continue
    d = (int(rows[i]["mpg"]) - int(rows[i - 1]["mpg"]) + 32768) % 65536 - 32768
    t0, c0, f0 = T[i - 1], int(rows[i - 1][cmd]), int(rows[i - 1][fpga])
    moving = i > 1 and rows[i - 1][cmd] != rows[i - 2][cmd]
    tc = tf = None
    for r in rows[i:i + 100]:
        if tc is None and int(r[cmd]) != c0: tc = int(r["t_ms"]) - t0
        if tf is None and int(r[fpga]) != f0: tf = int(r["t_ms"]) - t0
        if tc is not None and tf is not None: break
    lat.append((t0, d, tc, tf, moving))
still = [l for l in lat if not l[4]]
fs = [l[3] for l in still if l[3] is not None]
if fs:
    print(f"wheel changes {len(lat)}, from standstill {len(still)}: FPGA moved after median {statistics.median(fs):.0f} ms, "
          f"p90 {sorted(fs)[int(len(fs) * .9)]}, max {max(fs)}")
lost = [l[:2] for l in still if l[3] is None]
print(f"clicks with no motion within 2 s (x1 clicks are sub-step, so some are expected there): {len(lost)} {lost[:12]}")
print(f"net wheel counts {sum(l[1] for l in lat)}, net {ax} steps {int(rows[-1][cmd]) - int(rows[0][cmd])}"
      f"  (x10: 1 count = 1 step at 400 steps/mm)")

# continuity: runs of rows with the FPGA counter changing
mv = [int(rows[i][fpga]) != int(rows[i - 1][fpga]) for i in range(1, len(rows))]
runs, cur, n = [], mv[0], 0
for m in mv:
    if m == cur: n += 1
    else: runs.append((cur, n)); cur, n = m, 1
runs.append((cur, n))
print("continuous moving stretches >= 1 s:", [f"{n * 20 / 1000:.1f}" for on, n in runs if on and n >= 50], "s")
