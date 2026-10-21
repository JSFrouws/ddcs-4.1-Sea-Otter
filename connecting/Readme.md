# connecting/ — get a Linux login on the DDCS V4.1

This folder gets you a **root login (SSH) on the DDCS V4.1 controller**, so you can
configure the Linux side of the device. You need it once, for development or for setting
things up by hand. **For just running the Sea Otter software you do not need any of this** —
use the USB stick (see [../docs/usb-install.md](../docs/usb-install.md)); that needs no
shell access at all.

The controller ships with no SSH server and no login you know the password to. These scripts
install one, and make the first login deliberately easy. **Securing it afterwards is your
job** (see *Security* below).

> Windows PC, controller on the same network. Everything runs from this folder by
> double-clicking the `.bat` files. They set up a small local Python environment the first
> time (needs Python 3 from python.org, "Add to PATH" ticked).

## What it does, and why it works

The controller starts whatever file is named `ddcsv4.out` at boot, and it copies files from
its own network file share (and from a USB stick) into place. That is the same mechanism the
vendor uses for updates. These scripts use it to:

1. **swap in a small temporary app** in place of `ddcsv4.out`. After a reboot the controller
   runs it instead of its normal software. It does one thing: bring up the network and offer
   a one-time, token-protected shell on the network.
2. **use that shell once** to install the SSH server (dropbear) and set up a root login, and
   wire SSH into the boot script so it stays after reboots.
3. **put the original app back**, so the controller returns to normal — now with SSH present.

Nothing is deleted: the original `ddcsv4.out` is renamed aside (and also copied into
`connecting/backup/` on your PC) and restored in step 3.

## Files here

| File | What it is |
|---|---|
| `run_me_first.bat` | Step 1: configure the network, back up the original app, load the temporary app |
| `DDCS4.1_temporary_app` | The temporary app itself (a small static ARM binary; its source is in `files/src/temporary_shell.c`) |
| `run_me_after_reboot.bat` | Step 2: install SSH and set up the root login |
| `reset_original_app.bat` | Step 3: put the original controller app back |
| `Readme.md` | This file |
| `files/` | The scripts and the SSH-server binary the `.bat` files use |

## Steps

**1. `run_me_first.bat`**
Asks for the controller's IP settings and talks you through setting them on the controller
and wiring it to your PC (via a router, or a direct cable). It tests the connection, and
when it answers, backs up the original app and puts the temporary app in its place. Then it
tells you to reboot.

**2. Power-cycle the controller** and wait about 20 seconds for it to boot.

**3. `run_me_after_reboot.bat`**
Connects to the temporary app, installs the SSH server, and sets up the root login. It
prints the two ways to log in (see below).

**4. `reset_original_app.bat`**
Restores the original controller app. Power-cycle the controller once more; it now runs its
normal software again, with SSH available.

After this you can log in any time:

```
ssh -i connecting\ssh_key root@<controller-ip>      (no password; the key is generated on your PC)
ssh root@<controller-ip>                            (password: seaotter)
```

## Security — your responsibility

The login is left **wide open on purpose**, so you are certain to get in: a known password
(`seaotter`) and a generated key with no passphrase. This is fine on a private workshop
network for setup, and **not** fine on anything exposed.

Once you are in, secure it yourself. For example:

```sh
passwd                 # set a real root password (use: passwd -a m)
# or remove password login and keep only your own key:
#   put your public key in /root/.ssh/authorized_keys and remove the generated one
```

Do not expose the controller (port 22, or the Sea Otter API on port 8080) to the internet.
This folder only gives you a door in; it does not lock it behind you.

## Going back to completely stock

`reset_original_app.bat` already restores the original app. To also remove SSH, log in and:

```sh
cp /etc/init.d/rcS.before_ssh /etc/init.d/rcS     # the backup step 2 made
rm -f /ddcsv4/dropbear* /ddcsv4/connecting.conf
```

then reboot. (If you no longer have a login, use the USB firmware-update path to replace the
whole `/ddcsv4` contents.)

## If something goes wrong

- **Step 1 can't reach the controller:** re-check the IP you set on the controller, the
  cable/router, and — for a direct cable — that the PC has a fixed address in the same range.
- **Step 2 can't connect:** the controller must have been power-cycled after step 1 and
  finished booting. The temporary app listens on TCP port 7777.
- **The controller won't boot normally again:** run `reset_original_app.bat`. If you have no
  backup of the original app, fetch the vendor firmware and restore `ddcsv4.out` from it over
  the file share, or via the USB firmware-update path.

## Rebuilding the binaries

`files/src/build.sh` (WSL/Linux with `gcc-arm-linux-gnueabi`) rebuilds
`DDCS4.1_temporary_app` and the SSH-server binary.

## Credits

The bring-up approach — swapping the boot app over the device's own file share to get a
first shell, then installing dropbear — follows the groundwork in
[../docs/shell-access-live-test.md](../docs/shell-access-live-test.md) and
[../docs/ssh-setup.md](../docs/ssh-setup.md).
