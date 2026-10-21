# Installing Sea Otter from a USB stick

The simplest way to put Sea Otter (and SSH) on a DDCS V4.1. No network, no PC tools on the
controller, no opening the case. **Tested on the unit 2026-10-18** (boot into Sea Otter,
boot into the vendor app with SSH, switching between them).

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
`ddcsv4.out` is started. The install package's `ddcsv4.out` is a **start script**, not the
vendor program. At boot it:

1. sets the root password if you put one on the stick (`ssh-password.txt`, then deletes it),
2. starts the SSH server (`dropbear`, password and key login, port 22),
3. starts the vendor app instead, if you asked for that (`USE_VENDOR`, see below), or
4. sets up the network from `network.conf` (the vendor app used to do this), and
5. runs Sea Otter. If Sea Otter exits it restarts it; after 5 failed starts it falls back to
   the vendor app, if `ddcsv4.vendor` is there.

**Does the API work after a USB install?** Yes. Sea Otter serves the panel and the API on
port 8080 as soon as it runs, and the start script brings the network up first. SSH is
only needed for development; for normal use the USB stick is enough.

## What you need

- A USB stick formatted **FAT32**.
- The install package: the `ddcsv4` folder built with `app/install/make-usb.sh` (below), or
  a released copy. It contains:

| File | What |
|---|---|
| `ddcsv4.out` | The start script (replaces the vendor's `ddcsv4.out` on the controller) |
| `seaotter` | The Sea Otter program (static ARM binary) |
| `dropbearmulti` | SSH server with password login ([ssh-setup.md](ssh-setup.md)) |
| `network.conf` | IP address settings: **edit before installing** |
| `ssh-password.txt.example` | Rename to `ssh-password.txt` and put your root password in it to set one |

- **The vendor's original `ddcsv4.out`**, from the vendor's firmware package or a copy taken
  earlier (`SYSDISK` share, `\\<controller-ip>\SYSDISK\ddcsv4.out`). **Keep it safe: the
  install overwrites it on the controller.** Putting it in the package as `ddcsv4.vendor`
  keeps a copy on the controller, so you can switch back without a PC.

## Installing

1. Copy the `ddcsv4` folder to the root of the stick, so you have `E:\ddcsv4\seaotter` etc.
2. Edit `ddcsv4\network.conf` on the stick: the IP address, mask and gateway for your
   network (or `MODE=dhcp`). Sea Otter doesn't read the vendor app's network menu.
3. Recommended: copy the vendor's original `ddcsv4.out` into the same folder **renamed to
   `ddcsv4.vendor`**.
4. Optional, for SSH: rename `ssh-password.txt.example` to `ssh-password.txt` and replace the
   text with your password (one line).
5. Switch the controller off, plug the stick in, switch on. After ~15 s Sea Otter's screen
   appears.
6. **Take the stick out**, or at least delete `ssh-password.txt` from it: the stick is copied
   again on every boot while it's in.
7. Check from a PC: `http://<the IP from network.conf>:8080/` shows the panel in the browser.

Files that stay on the controller in `/ddcsv4`: `seaotter.cfg` (settings),
`seaotter-work.cfg` (work offsets, tools). Programs go to `/local` (the CNCDISK share) or
come from a USB stick.

## Updating Sea Otter

Put the new `seaotter` file (alone is fine) in `ddcsv4\` on a stick, boot with it, take it
out. Settings and offsets are kept.

## Back to the vendor app

Either:
- **Switch:** put an empty file named `USE_VENDOR` in `ddcsv4\` on the stick (and
  `ddcsv4.vendor` if it isn't on the controller yet), boot. The vendor app runs, and SSH
  still starts. To go back to Sea Otter, delete `/ddcsv4/USE_VENDOR` over SSH or the
  `SYSDISK` share and reboot. A stick can only add files, not remove them.
- **Uninstall:** put the vendor's original `ddcsv4.out` in `ddcsv4\` on the stick and boot
  once. That overwrites the start script and the controller is back to stock (Sea Otter's
  files stay in `/ddcsv4` but are never started).

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
sh install/make-usb.sh                # -> build/usb/ddcsv4/
```

## If something goes wrong

- **No network / no panel in the browser:** check `network.conf`. The start script logs to
  `/tmp/seaotter-boot.log` and Sea Otter to `/tmp/seaotter.log` (readable over SSH).
- **Black screen:** after 5 failed starts the start script runs `ddcsv4.vendor` if it's
  there. Otherwise put the vendor's original `ddcsv4.out` on a stick and boot (see
  *Uninstall*).
- The USB copy only works with the stick in **before** power-on.
