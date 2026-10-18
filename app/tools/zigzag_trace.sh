#!/bin/sh
# Run zigzag_y.nc on the unit with the FPGA bus trace on, then fetch the trace.
U=http://192.168.1.200:8080/api/v1
D() { /tmp/dssh.sh "$@"; }
cd "$(dirname "$0")/.."
[ -n "$NOUP" ] || /tmp/up.sh app/build/arm/pandora pandora
timeout 30 ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o LogLevel=ERROR -i ../mockup/dropbear/ddcsv4_id_ed25519 root@192.168.1.200 "cat > /local/zigzag_y.nc" < programs/zigzag_y.nc
TR=""; [ -n "$TRACE" ] && TR="PANDORA_TRACE=/tmp/ptrace.txt"
D "killall pandora 2>/dev/null; sleep 1; rm -f /tmp/ptrace.txt; cd /ddcsv4; ($TR nohup ./pandora > /tmp/pandora.log 2>&1 &); sleep 3"
M=$(curl -s $U/status | python -c "import sys,json; m=json.load(sys.stdin)['machine']; print('{\"x\":%.4f,\"y\":%.4f,\"z\":%.4f}' % (m['x']-5, m['y']-5, m['z']-10))")
curl -s -X POST $U/offsets/G54 -d "$M"; echo
curl -s -X POST $U/program/load -d '{"name":"zigzag_y.nc"}'; echo
curl -s -X POST $U/run/start -d '{"mode":"run"}'; echo
i=0; while [ $i -lt 120 ]; do s=$(curl -s $U/status | python -c "import sys,json; print(json.load(sys.stdin)['state'])"); [ "$s" = IDLE ] && break; sleep 0.5; i=$((i+1)); done
sleep 1
curl -s $U/status | python -c "import sys,json; s=json.load(sys.stdin); print('faults', s['posFaults'], 'err', s['posErr'], 'at line*100+state', s['faultLine'], 'waited', s['reversalsWaited'], 'checks', s['posChecks'], 'busErrors', s['busErrors'], 'retries', s['busRetries'], 'errno', s['busErrno'])"
D 'killall pandora; sleep 6; cat /tmp/ptrace.txt' > build/ptrace.txt
wc -l build/ptrace.txt
