#!/bin/sh
# Rebuild the binaries in connecting/ (WSL / Linux with gcc-arm-linux-gnueabi).
#   connecting/DDCS4.1_temporary_app   <- temporary_shell.c
#   connecting/files/dropbearmulti     <- app/tools/ssh/build-dropbear.sh
set -e
cd "$(dirname "$0")"
arm-linux-gnueabi-gcc -static -Os -Wall -o ../../DDCS4.1_temporary_app temporary_shell.c
arm-linux-gnueabi-strip ../../DDCS4.1_temporary_app
sh ../../../app/tools/ssh/build-dropbear.sh
cp ../../../app/tools/ssh/out/dropbearmulti ../dropbearmulti
ls -l ../../DDCS4.1_temporary_app ../dropbearmulti
