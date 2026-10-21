#!/bin/sh
# Assemble the USB-stick folder: build/usb/ddcsv4/ -> copy that ddcsv4 folder to the root
# of a FAT32 stick. Run from app/ after `make arm` and tools/ssh/build-dropbear.sh.
set -e
cd "$(dirname "$0")/.."
OUT=build/usb/ddcsv4
rm -rf build/usb && mkdir -p $OUT
cp install/usb/ddcsv4/ddcsv4.out install/usb/ddcsv4/network.conf install/usb/ddcsv4/ssh-password.txt.example $OUT/
cp build/arm/seaotter $OUT/seaotter
if [ -f tools/ssh/out/dropbearmulti ]; then cp tools/ssh/out/dropbearmulti $OUT/; else echo "note: no dropbearmulti (tools/ssh/build-dropbear.sh) - SSH left out"; fi
sed -i 's/\r$//' $OUT/ddcsv4.out $OUT/network.conf      # the unit's shell needs LF line ends
chmod +x $OUT/ddcsv4.out $OUT/seaotter
ls -l $OUT
