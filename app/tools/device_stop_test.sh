#!/bin/sh
# Reset during a 60 mm jog: distance travelled after the stop command, counters vs commanded.
U=${1:-http://192.168.1.200:8080}/api/v1
j() { python -c "import sys,json; s=json.load(sys.stdin); print(s['steps']['x'], s['fpga']['x'], s['posFaults'], s['state'])"; }
for feed in 1200 3000; do
  curl -s -X PUT $U/settings -d "{\"jog.feed\":$feed}" >/dev/null
  curl -s -X POST $U/jog -d '{"axis":"X","dist":60}' >/dev/null
  sleep 1.5
  af=$(curl -s -X POST $U/machine/stop | python -c "import sys,json; print(json.load(sys.stdin)['fpgaAtStop']['x'])")
  sleep 2
  b=$(curl -s $U/status | j)
  set -- $b
  python -c "print('feed $feed mm/min: travel after stop %.2f mm | final steps=$1 fpga=$2 faults=$3 $4' % (($2 - $af) / 2000))"
done
curl -s -X PUT $U/settings -d '{"jog.feed":1200}' >/dev/null
