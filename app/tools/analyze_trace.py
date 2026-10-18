"""Reconcile a PANDORA_TRACE bus trace: bursts sent per axis vs the FPGA position counters.
Usage: analyze_trace.py build/ptrace.txt"""
import sys

sel = 0x0F
bank = {}
bursts = {a: [] for a in range(4)}
cnt_reads = []          # (t, [x,y,z,a]) from consecutive 03,02,05,04,... reads
cur = {}
status = []
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) != 4:
        continue
    t, d, a, v = int(p[0]), p[1], int(p[2], 16), int(p[3], 16)
    if d == "W":
        if a == 0:
            sel = v
        elif sel < 4:
            bank.setdefault(sel, {})[a] = v
            if a == 0x04 and v in (1, 3):
                b = bank[sel]
                n = b.get(0x0A, 0) | b.get(0x0B, 0) << 16
                bursts[sel].append((t, n if v == 3 else -n, b.get(0x12, 0) | b.get(0x13, 0) << 16))
    elif d == "R":
        if 2 <= a <= 9:
            cur[a] = v
            if a == 8:     # last of the set (hi 09 then lo 08)
                vals = []
                for ax in range(4):
                    x = cur.get(3 + 2 * ax, 0) << 16 | cur.get(2 + 2 * ax, 0)
                    vals.append(x - (1 << 32) if x & 0x80000000 else x)
                cnt_reads.append((t, vals))
        elif a == 1:
            status.append((t, v))

first, last = cnt_reads[0], cnt_reads[-1]
print("trace %d ms, %d counter reads" % (last[0] - first[0], len(cnt_reads)))
for ax in range(4):
    s = sum(n for t, n, r in bursts[ax] if first[0] <= t <= last[0])
    print("axis %s: %4d bursts, sent %+8d steps, counter moved %+8d  -> lost %+d" %
          ("XYZA"[ax], len(bursts[ax]), s, last[1][ax] - first[1][ax], s - (last[1][ax] - first[1][ax])))
# where does the counter fall behind? walk the X bursts vs counter over time
ax = int(sys.argv[2]) if len(sys.argv) > 2 else 0
sent = 0; bi = 0; c0 = first[1][ax]; worst = []
B = [b for b in bursts[ax] if b[0] >= first[0]]
for t, vals in cnt_reads:
    while bi < len(B) and B[bi][0] <= t:
        sent += B[bi][1]; bi += 1
    worst.append((t - first[0], sent, vals[ax] - c0))
print("t_ms  sent  counter  backlog (axis %s, every 25th read)" % "XYZA"[ax])
for w in worst[::25]:
    print("%6d %7d %7d %6d" % (w[0], w[1], w[2], w[1] - w[2]))
