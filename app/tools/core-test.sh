#!/bin/sh
# quick core-only build + test (before the UI exists); run inside WSL
cd "$(dirname "$0")/.." && mkdir -p build/host && \
gcc -O1 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
  -DBUILD_ID='"t"' -DBUILD_DATE='"d"' -Isrc -o build/host/tests \
  src/common.c src/core/*.c src/hal/fpga.c src/hal/bus_sim.c src/sim/fpga_sim.c tests/*.c -lm && \
cd build && timeout 300 ./host/tests
