#!/bin/sh
# Cross-build the on-device experiment tools (run inside WSL)
cd "$(dirname "$0")" && mkdir -p ../build/arm && \
arm-linux-gnueabi-gcc -static -O2 -Wall -o ../build/arm/fpgaprobe fpgaprobe.c && \
arm-linux-gnueabi-gcc -shared -fPIC -Os -fno-stack-protector -U_FORTIFY_SOURCE -U_TIME_BITS -D_TIME_BITS=32 -o ../build/arm/iotrace2.so iotrace2.c && \
ls -l ../build/arm/fpgaprobe ../build/arm/iotrace2.so
