# Persistent SSH access (dropbear)

_2026-10-16. Set up via the live `recovery_shell` session
([shell-access-live-test.md](shell-access-live-test.md)). Standard key-based SSH, no more
need for the custom token listener for day-to-day work._

## What's in place

- **Static ARM dropbear**, built from source (`dropbear-2024.86`) in WSL:
  `arm-linux-gnueabi-gcc`, `--host=arm-linux-gnueabi`, password auth disabled at compile
  time (`DROPBEAR_SVR_PASSWORD_AUTH 0` - this libc build has no `crypt()`), linked fully
  static with `-no-pie` (dropbear's configure bakes `-Wl,-pie` into the Makefile's
  `LDFLAGS` as a hardening default, which bypasses `-no-pie`/`-fno-PIE` driver flags
  since it's a raw linker flag - had to `sed` the generated `Makefile`'s `LDFLAGS=` line
  directly). Build notes kept here in case it needs redoing:
  ```sh
  curl -fsSL https://matt.ucc.asn.au/dropbear/releases/dropbear-2024.86.tar.bz2 -o dropbear.tar.bz2
  tar xjf dropbear.tar.bz2 && cd dropbear-2024.86
  ./configure --host=arm-linux-gnueabi --disable-zlib --disable-utmp --disable-utmpx \
      --disable-wtmp --disable-wtmpx --disable-lastlog --disable-syslog \
      CC=arm-linux-gnueabi-gcc CFLAGS="-Os -fno-PIE -fno-stack-protector" \
      LDFLAGS="-static -no-pie"
  echo '#define DROPBEAR_SVR_PASSWORD_AUTH 0' >> localoptions.h
  sed -i 's/^LDFLAGS=.*/LDFLAGS=-static/' Makefile   # see note above - required
  make -j4 PROGRAMS='dropbear dropbearkey' MULTI=1
  ```
- **Deployed to `/ddcsv4/`** on `SYSDISK` as `dropbearmulti`, with `dropbear` and
  `dropbearkey` as symlinks to it (multi-call dispatch on `argv[0]`, same idea as
  busybox). Host keys (`dropbear_rsa_host_key`, `dropbear_ed25519_host_key`) generated
  on-device with `dropbearkey`.
- **Public-key auth only.** Our key pair is in
  [../mockup/dropbear/](../mockup/dropbear/) (`ddcsv4_id_ed25519` /
  `ddcsv4_id_ed25519.pub` - **the private key is a plain local file, not committed to any
  remote/shared location**). Installed as `/root/.ssh/authorized_keys` on the device.
- **Added to `/etc/init.d/rcS`** (after the `smbd`/`nmbd` lines; original backed up on the
  device as `/etc/init.d/rcS.orig_backup_2026-10-16`):
  ```sh
  # Added by Pandora-DDCSV4 recovery (2026-10-16): persistent SSH via dropbear.
  # Public-key auth only, see docs/shell-access-live-test.md in the project.
  /ddcsv4/dropbear -p 22 -r /ddcsv4/dropbear_rsa_host_key -r /ddcsv4/dropbear_ed25519_host_key
  ```
  So it starts on **every future boot**, independent of which `ddcsv4.out` is in place.

## A fix that was needed: `/root` ownership

Dropbear refused all logins at first with `/root must be owned by user or root, and not
writable by group or others` - `/root` was owned by uid 1000 (not root) with group-write
set. Fixed with `chown 0:0 /root && chmod 755 /root` (note: `chown root:root` by *name*
failed with `unknown user/group root:root` even though `/etc/passwd` has a `root` entry -
this BusyBox's `chown` applet apparently only accepts numeric IDs; a minor but notable
quirk of this build). This fix is now permanent on the device (it's a one-time filesystem
change, not something `rcS` needs to redo).

## How to connect (once the real app is running again)

```sh
ssh -i mockup/dropbear/ddcsv4_id_ed25519 root@192.168.1.200
```

## Gotcha hit while building the deployment pipeline: SSH remote-command + `$(())`

When passing a remote command to `ssh host '...'` that contains `$((...))` arithmetic,
embedding it inside an **already-double-quoted** outer shell argument causes the OUTER
shell to swallow or mis-escape the `$`. Always wrap the whole remote command in **single
quotes** at the point it's handed to `ssh`, e.g.:
```sh
ssh ... host 'dd if=/dev/kmem bs=4 skip=$((0xADDR/4)) count=1 | od -x'
```
Got bitten by this twice in this session (once building a heredoc that left a literal
`EOF` line in a remote file - fixed by using plain `echo >>` instead of heredocs over
this path).
