# Installing Sea Otter from a USB stick

The simplest way to put Sea Otter (and SSH) on a DDCS V4.1. No network, no PC tools on the
controller, no opening the case. Uses the vendor's own documented update procedure. **Tested
on the unit 2026-10-18** (boot into Sea Otter, boot into the vendor app with SSH, switching
between them), by copying the package into place over the network; a boot from a physical
stick has not been tried yet.

## How it works

The controller's boot script (`/etc/init.d/rcS`, see [rootfs-findings.md](rootfs-findings.md))
does this on **every** boot, before any app starts:

```sh
/bin/cp /udisk-sda1/ddcsv4/* /ddcsv4/
/bin/cp /udisk-sda/ddcsv4/* /ddcsv4/
...
cd /ddcsv4/
./ddcsv4.out &
```

So every file in a `ddcsv4` folder on a USB stick is copied over the app folder, and then
`ddcsv4.out` is started.

**This is the vendor's own, documented update procedure.** The *DDCS V4.1 Users Manual V1*,
section 3.4.3 *Software Update* (pages 70–71), says: put a folder named **`ddcsv4`** (stressed:
"not INSTALL") in the root of an empty FAT32 USB stick, switch the controller off for 10 s,
insert the stick, switch on; the screen is blocked for about 30 s while it updates; then
remove the folder, or the controller updates again on every start. Its file table calls
`ddcsv4.out` the "System program file". The vendor's update package is exactly that folder:
the 2025-04-04 release (`ddcsv4(2025-04-04).rar`) contains one `ddcsv4/` folder with
`ddcsv4.out`, the `motiondev.ko` driver, the macros (`*.rc`, `*.nc`), the language and message
files and `logo.bmp` — no installer, script or readme ([listing](raw/2026-10-22_vendor-package-2025-04-04-listing.txt)).
The package does **not** contain the `setting` file, so a vendor update keeps the machine's
parameters.

Sea Otter's package uses the same mechanism and the same folder name. The only file whose
name is fixed is `ddcsv4.out` — the one file the boot script starts. Everything else in the
folder can be named freely. Nothing outside `/ddcsv4` (the boot script itself, the driver
loading, Samba) is changed by either install. The install package's `ddcsv4.out` is a **start script**, not the
vendor program. At boot it:

1. sets the root password if you put one on the stick (`ssh-password.txt`, then deletes it),
2. starts the SSH server (`dropbear`, password and key login, port 22),
3. starts the vendor app instead, if you asked for that (`USE_VENDOR`, see below), or
4. sets up the network from `network.conf` (the vendor app used to do this),
5. checks the FPGA driver the boot script just loaded (`motiondev.ko`) against the one Sea
   Otter was tested with, and
6. runs Sea Otter, which shows `logo.bmp` while it starts. If Sea Otter exits it restarts
   it; after 5 exits in a row it falls back to the vendor app, if `ddcsv4.vendor` is there
   (tested on the unit).

**Does the API work after a USB install?** Yes. Sea Otter serves the panel and the API on
port 8080 as soon as it runs, and the start script brings the network up first. SSH is
only needed for development; for normal use the USB stick is enough.

## What you need

- A USB stick formatted **FAT32**.
- The install package `seaotter-<version>.zip` from the
  [Releases](https://github.com/JSFrouws/ddcs-4.1-Sea-Otter/releases) page, or built with
  `app/install/make-usb.sh` (below). It has the **same layout as the vendor's update package**
  (`ddcsv4(2025-04-04).rar` → `ddcsv4(2025-04-04)/ddcsv4/…`):

```
seaotter-v0.2.0.zip      (v0.2.0 itself is called seaotter.v0.2.0.zip on GitHub)
└── seaotter(v0.2.0)/
    └── ddcsv4/             <- this folder goes to the root of the USB stick
        ├── ddcsv4.out      the start script (takes the place of the vendor's ddcsv4.out)
        ├── seaotter        the Sea Otter program (static ARM binary)
        ├── motiondev.ko    the FPGA driver Sea Otter was tested with (see below)
        ├── logo.bmp        boot logo, 1024 x 600 24-bit BMP (replaceable, see boot-logo.md)
        ├── macro-*.nc      the default macros (Safe Z, Park, ...; see user-manual.md, Macros)
        ├── dropbearmulti   SSH server with password login (ssh-setup.md)
        ├── network.conf    IP address settings: edit before installing
        └── ssh-password.txt.example   rename to ssh-password.txt to set a root password
```

**Why the driver is in the package.** The controller's boot script loads `/ddcsv4/motiondev.ko`
(the vendor's FPGA driver) before it starts `ddcsv4.out`, and it copies the stick first — so
the driver on the stick is the one that gets loaded. Sea Otter talks to the FPGA through that
driver and has only been tested with one version of it: the one in the vendor's 2025-04-04
release (MD5 `136136bd4404b8a82114ba051c7023d3`). Shipping exactly that file means a later
vendor release with a different driver can never leave Sea Otter running on a driver it was
not tested with. The start script also checks it on every boot; if it differs, Sea Otter
shows a permanent red **UNTESTED FPGA DRIVER** banner and reports it as `driverWarning` in the
API status. (The packaging script refuses to build a package with any other `motiondev.ko`.)
Going back to the vendor software puts the vendor's own driver back, from its package.

- **The vendor's original software**, to be able to go back: download it from the maker
  (see [Restoring the original software](#restoring-the-original-software)) or copy it off
  the controller first (`\\<controller-ip>\SYSDISK\ddcsv4.out`). **The install overwrites
  `ddcsv4.out` on the controller.** Putting the original in the package as `ddcsv4.vendor`
  keeps a copy on the controller, so you can switch back without a PC.

## Installing

1. Unzip the package and copy the inner **`ddcsv4`** folder to the root of an empty FAT32
   stick, so you have `E:\ddcsv4\seaotter` etc. (not `E:\seaotter(v…)\ddcsv4\…`).
2. Edit `ddcsv4\network.conf` on the stick: the IP address, mask and gateway for your
   network (or `MODE=dhcp`). Sea Otter doesn't read the vendor app's network menu.
3. Recommended: copy the vendor's original `ddcsv4.out` into the same folder **renamed to
   `ddcsv4.vendor`**.
4. Optional, for SSH: rename `ssh-password.txt.example` to `ssh-password.txt` and replace the
   text with your password (one line).
5. Switch the controller off for 10 s, plug the stick in, switch on. The boot logo shows,
   and after ~15 s Sea Otter's screen appears.
6. **Take the stick out**, or at least delete `ssh-password.txt` from it: the stick is copied
   again on every boot while it's in.
7. Check from a PC: `http://<the IP from network.conf>:8080/` shows the panel in the browser.

Files that stay on the controller in `/ddcsv4`: `seaotter.cfg` (settings),
`seaotter-work.cfg` (work offsets, tools). Programs go to `/local` (the CNCDISK share) or
come from a USB stick.

## Updating Sea Otter

Install the new package the same way (its `ddcsv4` folder on a stick, boot, remove the stick).
Settings and offsets are kept: they live in `seaotter.cfg` / `seaotter-work.cfg`, which the
package doesn't contain. A single new file (e.g. just `seaotter`, or your own `logo.bmp`) in
`ddcsv4\` on a stick works too.

## Restoring the original software

Use the vendor's own update package and procedure — the same USB route, in reverse:

1. Download the newest DDCS V4.1 software from the maker, Digital Dream:
   **[www.ddcnc.com › Downloads › Software & firmware](http://www.ddcnc.com/?m=home&c=Lists&a=index&tid=106&lang=en)**. The newest V4.1 entry
   when this was written: **[DDCS V4.1 (2025-04-04) software](http://www.ddcnc.com/?m=home&c=View&a=index&aid=229&lang=en)** (uploaded 2025-04-09),
   file `ddcsv4(2025-04-04).rar`. *Use the `http://www.ddcnc.com` address: in October 2026 the
   `https://` site had an expired certificate and showed a hosting notice.* The V4.1 user
   manual is under [Downloads › Manuals](http://www.ddcnc.com/?m=home&c=Lists&a=index&tid=101&lang=en).
2. Unpack the `.rar` (7-Zip, WinRAR). Inside is a folder `ddcsv4(2025-04-04)` containing a
   folder **`ddcsv4`**.
3. Copy that inner **`ddcsv4`** folder to the root of an **empty FAT32** USB stick, so you have
   `E:\ddcsv4\ddcsv4.out`, `E:\ddcsv4\motiondev.ko`, … (not `E:\ddcsv4(2025-04-04)\…`).
4. Switch the controller off, wait 10 s, insert the stick, switch on. The screen is blocked for
   about 30 s, then the vendor software starts.
5. **Remove the stick** (or delete the folder from it), otherwise it updates again on every
   start.

Its `ddcsv4.out` overwrites Sea Otter's start script, so the controller is back to stock. Sea
Otter's own files (`seaotter`, `seaotter.cfg`, `network.conf`, `dropbearmulti`, …) stay in
`/ddcsv4` but nothing starts them any more; the vendor's machine parameters (`setting`) were
never touched. If SSH was installed with the `connecting/` tools it keeps starting from the
boot script; see [connecting/Readme.md](../connecting/Readme.md) to remove it.

To check you have the same release that was tested here (downloaded 2026-10-22): the
`ddcsv4.out` inside is 4 938 156 bytes, MD5 `237704ed235403b140d74cfcd616e10a`, and
`motiondev.ko` MD5 `136136bd4404b8a82114ba051c7023d3` — identical to the software found on the
test controller. The `.rar` was SHA-256
`6741d1b537dcaf41640b9d7840597ec4e41d139a4c4f137c0611c22168ea0321` (a re-upload by the maker
would change this).

### Switching without a PC

If you put the original `ddcsv4.out` in the Sea Otter package as `ddcsv4.vendor`, it is already
on the controller: put an empty file named `USE_VENDOR` in `ddcsv4\` on a stick and boot. The
vendor app runs and SSH still starts. To go back to Sea Otter, delete `/ddcsv4/USE_VENDOR` over
SSH or the `SYSDISK` share and reboot (a stick can only add files, not remove them).

## SSH only, keeping the vendor app

Use the package with `ddcsv4.vendor` (the vendor's original), an empty `USE_VENDOR` file and
`ssh-password.txt`. The controller keeps running the vendor software, and you can log in
with `ssh root@<controller-ip>` (port 22). The vendor app sets up the network itself from
its own menu in that case.

## Building the package

In WSL / Linux, from `app/`:

```sh
make arm                              # build/arm/seaotter
sh tools/ssh/build-dropbear.sh        # tools/ssh/out/dropbearmulti (once)
sh install/make-usb.sh v0.2.0         # -> build/seaotter-v0.2.0.zip (+ .sha256)
```

The package sources are in `app/install/usb/ddcsv4/` (start script, `network.conf`,
`logo.bmp`, `motiondev.ko`). GitHub Actions runs the same script on every push and attaches
the zip to a Release for tags `v*`.

## If something goes wrong

- **No network / no panel in the browser:** check `network.conf`. The start script logs to
  `/tmp/seaotter-boot.log` and Sea Otter to `/tmp/seaotter.log` (readable over SSH).
- **Red "UNTESTED FPGA DRIVER" banner:** `/ddcsv4/motiondev.ko` is not the driver Sea Otter
  was tested with (e.g. a vendor update ran in between). Install the Sea Otter package again;
  it brings the tested driver.
- **Black screen:** after 5 failed starts the start script runs `ddcsv4.vendor` if it's
  there. Otherwise restore the vendor software from a stick (see
  [Restoring the original software](#restoring-the-original-software)).
- The USB copy only works with the stick in **before** power-on.
