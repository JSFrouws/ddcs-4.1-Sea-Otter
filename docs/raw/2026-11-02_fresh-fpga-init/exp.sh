#!/bin/sh
cd /ddcsv4; exec > /tmp/exp.log 2>&1
echo "untouched FPGA: $(./motionw r0x00 r0x36 r0x41 | tr "\n" " ")"
echo "=== raw vendor init WITH reads, timed, then +400 5 ms"; INIT_FILE=/ddcsv4/${1:-init_rawr.txt} BLK_MS=5 IDLE_MS=1200 NOCTRL=1 ./blockmove 1 400 2000 10000 32 2>&1 | grep -E "replayed|0x36 =|idle .* done|position final|giving up"
echo "=== -400, 10 ms blocks"; BLK_MS=10 IDLE_MS=1200 NOCTRL=1 ./blockmove 1 -400 2000 10000 32 2>&1 | grep -E "0x36 =|idle .* done|position final|giving up"
echo "=== done"
