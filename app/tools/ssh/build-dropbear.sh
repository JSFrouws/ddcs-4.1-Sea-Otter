#!/bin/sh
# Build a static ARM dropbear WITH password login for the DDCS V4.1 (run in WSL / Linux).
# Needs: gcc-arm-linux-gnueabi, make, curl, bzip2. Output: ./out/dropbearmulti
# The unit's libc has no usable crypt() for a static build, so md5crypt.c (next to this
# script) supplies one for "$1$" hashes - set the password with `chpasswd -m` on the unit.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
V=2024.86
W=${WORK:-/tmp/dropbear-pw}
mkdir -p "$W" "$HERE/out"
cd "$W"
[ -f dropbear-$V.tar.bz2 ] || curl -fsSL https://matt.ucc.asn.au/dropbear/releases/dropbear-$V.tar.bz2 -o dropbear-$V.tar.bz2
rm -rf dropbear-$V && tar xjf dropbear-$V.tar.bz2 && cd dropbear-$V
./configure --host=arm-linux-gnueabi --disable-zlib --disable-utmp --disable-utmpx \
	--disable-wtmp --disable-wtmpx --disable-lastlog --disable-syslog \
	CC=arm-linux-gnueabi-gcc CFLAGS="-Os -fno-PIE -fno-stack-protector" LDFLAGS="-static -no-pie" >/dev/null
echo '#define DROPBEAR_SVR_PASSWORD_AUTH 1' > localoptions.h
sed -i 's|^/\* #undef HAVE_CRYPT \*/|#define HAVE_CRYPT 1|' config.h
sed -i 's/^LDFLAGS=.*/LDFLAGS=-static/' Makefile        # configure bakes -pie in; it must go
arm-linux-gnueabi-gcc -Os -fno-PIE -c "$HERE/md5crypt.c" -o md5crypt.o
make -j4 PROGRAMS='dropbear dropbearkey' MULTI=1 LIBS="$PWD/md5crypt.o" >/dev/null
cp dropbearmulti "$HERE/out/dropbearmulti"
arm-linux-gnueabi-strip "$HERE/out/dropbearmulti"
ls -l "$HERE/out/dropbearmulti"
