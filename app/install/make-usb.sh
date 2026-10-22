#!/bin/sh
# Assemble the USB update package, in the same layout as the vendor's own update package
# (ddcsv4(2025-04-04).rar -> ddcsv4(2025-04-04)/ddcsv4/...):
#
#   build/seaotter-<VERSION>.zip     (no parentheses in the file name: GitHub renames them)
#     seaotter(<VERSION>)/ddcsv4/   <- copy THIS folder to the root of an empty FAT32 stick
#         ddcsv4.out      start script (the file the controller's boot script runs)
#         seaotter        the app
#         motiondev.ko    the FPGA driver Sea Otter was tested with (vendor 2025-04-04)
#         logo.bmp        boot logo, 1024 x 600 24-bit BMP
#         dropbearmulti   SSH server (if built)
#         network.conf, ssh-password.txt.example
#
# Run after `make arm` (and tools/ssh/build-dropbear.sh for SSH).
#   sh install/make-usb.sh [VERSION]      VERSION defaults to $PIPELINE_ID, else "local"
set -e
cd "$(dirname "$0")/.."
VER=${1:-${PIPELINE_ID:-local}}
NAME="seaotter($VER)"                # the folder inside, like the vendor's ddcsv4(2025-04-04)
ZIP="seaotter-$VER.zip"
SRC=install/usb/ddcsv4
OUT="build/usb/$NAME/ddcsv4"
KO_TESTED=136136bd4404b8a82114ba051c7023d3

rm -rf build/usb && mkdir -p "$OUT"
cp "$SRC/ddcsv4.out" "$SRC/network.conf" "$SRC/ssh-password.txt.example" "$SRC/logo.bmp" "$SRC/motiondev.ko" "$OUT/"
cp build/arm/seaotter "$OUT/seaotter"
if [ -f tools/ssh/out/dropbearmulti ]; then cp tools/ssh/out/dropbearmulti "$OUT/"; else echo "note: no dropbearmulti (tools/ssh/build-dropbear.sh) - SSH left out"; fi
sed -i 's/\r$//' "$OUT/ddcsv4.out" "$OUT/network.conf"      # the unit's shell needs LF line ends
chmod +x "$OUT/ddcsv4.out" "$OUT/seaotter"

# the driver must be exactly the tested one
KO=$(md5sum "$OUT/motiondev.ko" | cut -d ' ' -f 1)
[ "$KO" = "$KO_TESTED" ] || { echo "motiondev.ko md5 $KO is not the tested $KO_TESTED" >&2; exit 1; }
# the logo must be in the controller's format
python3 - "$OUT/logo.bmp" <<'EOF'
import struct, sys
d = open(sys.argv[1], "rb").read(54)
w, h = struct.unpack_from("<ii", d, 18)
ok = d[:2] == b"BM" and w == 1024 and abs(h) == 600 and struct.unpack_from("<H", d, 28)[0] == 24 \
    and struct.unpack_from("<I", d, 30)[0] == 0
sys.exit(0 if ok else "logo.bmp must be a 1024 x 600 24-bit uncompressed BMP")
EOF

# the zip (python: no zip tool needed; keeps the executable bits for Linux unzippers)
python3 - "build/usb" "$NAME" "build/$ZIP" <<'EOF'
import os, sys, zipfile
root, name, out = sys.argv[1:]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(os.path.join(root, name)):
        rel = os.path.relpath(d, root).replace(os.sep, "/")
        zd = zipfile.ZipInfo(rel + "/")                         # folder entries, like the vendor archive
        zd.external_attr = (0o40755 << 16) | 0x10
        z.writestr(zd, b"")
        for f in sorted(files):
            p = os.path.join(d, f)
            zi = zipfile.ZipInfo.from_file(p, rel + "/" + f)
            zi.external_attr = (0o755 if f in ("ddcsv4.out", "seaotter", "dropbearmulti") else 0o644) << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, open(p, "rb").read())
EOF
( cd build && sha256sum "$ZIP" > "$ZIP.sha256" )
ls -l "$OUT"
echo "package: build/$ZIP"
cat "build/$ZIP.sha256"
