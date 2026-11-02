#!/bin/bash
# fresh.sh [exp-script] [arg]: boot the unit with an UNTOUCHED FPGA (wrapper sleeps instead of
# starting Sea Otter when /ddcsv4/NO_INIT exists; network + SSH are up by then), then run the
# experiment script on the unit and print its log. Without NO_INIT the wrapper execs the real
# binary, so a normal boot is unaffected.
cd "/c/Users/-/Documents/code/ddcs-4.1-Sea-Otter" || exit 1
SSH="ssh -o ConnectTimeout=8 -o BatchMode=yes -i mockup/dropbear/ddcsv4_id_ed25519 root@192.168.1.200"
SCR="/c/Users/-/AppData/Local/Temp/claude/C--Users---Documents-code-webapps-life-os/9a42b403-d84a-4874-ba23-d20152c7f250/scratchpad"
EXP=${1:-/ddcsv4/exp.sh}; ARG=${2:-}
$SSH 'cat > /ddcsv4/seaotter.wrap <<"EOF"
#!/bin/sh
# wrapper: splash -> real binary; NO_INIT flag -> sleep (FPGA untouched, SSH is up); else real binary
case "$1" in --splash) exec /ddcsv4/seaotter.bin "$@";; esac
if [ -f /ddcsv4/NO_INIT ]; then echo "NO_INIT: not starting Sea Otter" >> /tmp/seaotter-boot.log; exec sleep 1000000; fi
exec /ddcsv4/seaotter.bin "$@"
EOF
chmod +x /ddcsv4/seaotter.wrap; cp /ddcsv4/seaotter.wrap /ddcsv4/seaotter; touch /ddcsv4/NO_INIT; sync; echo "wrapper installed, NO_INIT set"; head -c 20 /ddcsv4/seaotter; echo' 2>&1 | grep -v '^\*\*' || exit 1
bash "$SCR/cnccycle.sh" cycle || exit 1
for i in $(seq 1 40); do sleep 5; $SSH 'echo up' 2>/dev/null | grep -q up && break; done
echo "ssh after $((i*5)) s"
$SSH 'tail -3 /tmp/seaotter-boot.log; for p in /proc/[0-9]*; do c=$(tr "\0" " " < $p/cmdline 2>/dev/null); case "$c" in *seaotter*|*blockmove*|*ddcsv4.vendor*) echo "$(basename $p) $c";; esac; done; cd /ddcsv4; sh '"$EXP $ARG"'; cat /tmp/exp*.log | tail -40' 2>&1 | grep -v '^\*\*'
