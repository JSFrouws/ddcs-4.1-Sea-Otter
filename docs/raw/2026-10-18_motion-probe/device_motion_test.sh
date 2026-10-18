#!/bin/sh
# Motion reliability campaign against pandora running on the unit (real FPGA, no motors needed).
# After each step: FPGA position counters vs commanded steps, faults, reversals held back.
# Usage: tools/device_motion_test.sh [http://192.168.1.200:8080]
U=${1:-http://192.168.1.200:8080}/api/v1
post() { curl -s -m 10 -X POST "$U/$1" -d "$2"; echo; }
st() { curl -s -m 10 "$U/status"; }
f() { st | python -c "import sys,json; s=json.load(sys.stdin); print('   state=%s fpga=%s steps=%s faults=%d checks=%d waited=%d bursts=%d line=%d' % (s['state'], [s['fpga'][k] for k in 'xyza'], [s['steps'][k] for k in 'xyza'], s['posFaults'], s['posChecks'], s['reversalsWaited'], s['bursts'], s['program']['line']))"; }
idle() { i=0; while [ $i -lt ${1:-600} ]; do s=$(st | python -c "import sys,json; print(json.load(sys.stdin)['state'])"); [ "$s" = IDLE ] && return; sleep 0.5; i=$((i+1)); done; echo "   (timeout)"; }
t0() { T0=$(date +%s.%N); }
dt() { python -c "print('   took %.2f s' % ($(date +%s.%N) - $T0))"; }

echo "== 1. jog X +10 mm (keypad-style step jog, ramped)"; t0; post jog '{"axis":"X","dist":10}'; sleep 0.3; idle; dt; sleep 0.3; f
echo "== 2. 20 alternating 1 mm jogs on Y, sent without waiting (reversals while stepping)"
for i in 1 2 3 4 5 6 7 8 9 10; do post jog '{"axis":"Y","dist":1}' >/dev/null; post jog '{"axis":"Y","dist":-1}' >/dev/null; done; sleep 1; idle; sleep 0.3; f
echo "== 3. G54 around the current position, load Bracket_v3.nc"
M=$(st | python -c "import sys,json; m=json.load(sys.stdin)['machine']; print('{\"x\":%.4f,\"y\":%.4f,\"z\":%.4f}' % (m['x']-60, m['y']-35, m['z']-20))")
post offsets/G54 "$M"; post program/load '{"name":"Bracket_v3.nc"}'
echo "== 4. run it (pauses at M6: resume once)"; t0; post run/start '{"mode":"run"}'; sleep 2; post run/start '{}'; idle 1200; dt; sleep 0.3; f
echo "== 5. Sign_rosette.nc: 1200 short segments, constant direction changes"; post program/load '{"name":"Sign_rosette.nc"}'; t0; post run/start '{"mode":"run"}'; idle 2400; dt; sleep 0.3; f
echo "== 6. pause mid-run, resume, then Reset mid-run"; post run/start '{"mode":"run"}'; sleep 4; post run/pause; sleep 2; f; post run/start '{}'; sleep 3; post run/stop; sleep 1; idle; sleep 0.3; f
echo "== 7. pendant-style nudges: 200 x 0.001 mm on Z"; for i in $(seq 1 40); do post jog '{"axis":"Z","dist":0.005}' >/dev/null; done; sleep 1; idle; sleep 0.3; f
