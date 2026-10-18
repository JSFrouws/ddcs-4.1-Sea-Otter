# ddcsv4.out — how it loads the driver and where it runs shell commands

_2026-10-15. Static, read-only analysis of the backed-up `SYSDISK/ddcsv4.out`
(4.9 MB ARM ELF) with [../tools/strfind.py](../tools/strfind.py). Nothing was run on
the unit. This is our own device; the point is to find a way to run `mtrace`/`insmod`
without soldering onto the UART._

## 1. How `motiondev.ko` is loaded — **not by the app**

- `ddcsv4.out` contains **no `insmod`, `modprobe`, `lsmod` or `.ko` string.** It only
  **opens the device node `/dev/motion`** (string at `0x000d3a4c`).
- So the driver is `insmod`ed by a **boot/init script on the root filesystem**, before the
  app starts. That script is **not on SYSDISK** (SYSDISK is the app partition, mounted at
  `/ddcsv4` — see below). We don't have the rootfs yet.
- **To see the boot script we need the rootfs**, which means a shell (section 3) or the UART.

## 2. Filesystem layout (from absolute paths in the binary)

| Mount point | What |
|---|---|
| `/ddcsv4` | **SYSDISK** — the app folder (`ddcsv4.out`, `motiondev.ko`, `setting`, macros) |
| `/share` | **CIFS mount of our PC's share** (the "shared host"), see below |
| `/udisk`, `/udisk-*` | USB stick |
| `/local` | internal/local storage for jobs |
| `/tmp/...` | `marcotmp`, `tmpblock`, `tmpsave`, `udiskop` scratch files |
| `/dev/motion` | the motion driver node |
| `/etc/resolv.conf` | DNS, rewritten for networking |

## 3. `system()` / `popen()` calls — the shell surface

The app is dynamically linked against `system`, `popen`, `__register_atfork`, and shells
out in many places. The commands (with `%s`/`%d` the app fills in):

**Networking** (confirms how eth0 comes up — matches [network.md](network.md)):
```
ifconfig eth0 up
ifconfig eth0 %s netmask %s
route del default gw 0.0.0.0 dev eth0
route add default gw %s dev eth0
udhcpc -i eth0 -q
ifconfig eth0 | grep Bcast | cut -d ':' -f2 | cut -d ' ' -f1
cat /sys/class/net/eth0/carrier
```

**The "shared host" mount** — the unit is a CIFS **client** of our PC:
```
mount -t cifs //%s/share /share -o username=root,password=,iocharset=utf8
umount /share
cat /proc/mounts | grep -e "/share" | cut -d ' ' -f 2
```
- `%s` = the **shared-host IP** from the network menu.
- It logs in as **`username=root, password=` (empty)** and expects a share literally
  named **`share`** on the host. So to feed the unit files via its own "shared host"
  feature, our PC must export `\\192.168.1.65\share` with guest/root empty-password access.
- This also strongly hints the unit's own accounts use **empty passwords** (Samba guest +
  root:"" is the house style of this firmware).

**File manager** — paths built from `/share/%s`, `/local/%s`, `/ddcsv4`, `/udisk` + a
**user-chosen filename**, then run through the shell:
```
/bin/cp %s %s
mv %s %s
mv /tmp/tmpsave %s
rm %s
rm %s/ProbeMap%d.txt
rm .*
df %s | grep -v Used | tr -s ' ' | cut -d ' ' -f 4
sync
```

The app runs as **root** (it mounts filesystems and configures interfaces with no `su`).

## 4. `.rc` files are NOT macros — no shell path there

The `.rc` files (`M6.rc`, `simulate.rc`, `advstart.rc`, …) are **SEGGER emWin
`GUI_Builder` dialog resources** (C source describing the on-screen panels: tool-change,
simulate, etc.), compiled emWin 5.12. They are UI layout, not an executable macro
language, and expose no file or shell access. `.nc` files are plain G-code. So there is
**no "run a shell command from a macro" path**; the shell surface is only section 3.

## 5. Ways to get a shell to run mtrace / swap the driver

In rough order of least to most invasive, to try next (each still to be tested):

1. **Command injection via a filename.** The file manager passes user filenames straight
   into `system("/bin/cp %s %s")`, `rm %s`, etc. Filenames on `CNCDISK` are **writable by
   us over SMB**. A file whose **name** contains shell metacharacters (e.g. a backtick or
   `;` sequence) would execute when the app copies/deletes/measures it. Because the app is
   root, that yields a **root shell / arbitrary command** — enough to `insmod` our driver,
   run `mtrace`, or just read the rootfs and `/proc/kallsyms` (which also solves the
   modversions CRC gap in [motiondev-ko.md](motiondev-ko.md)). **This is the cleanest
   non-hardware route.** Caveats: SMB share may be read-only (untested), and some embedded
   `mount.cifs` filename handling is quirky — needs care. Linux filenames allow almost any
   byte except `/` and NUL.
2. **Command injection via the shared-host IP field.** That value goes into
   `mount -t cifs //%s/share`. If the field isn't strictly validated as numeric, a crafted
   value injects into the mount command. Likely harder (IP fields are often validated), but
   worth a look at the input dialog.
3. **UART console.** The fallback if both injections are dead. Independent of the app, and
   the only option if the network flag (`Disable network functionality`) keeps reverting.

## 6. Next concrete steps (read-only until we decide)

- **Test whether the SMB shares are writable** (try creating a harmless file on `CNCDISK`).
  This gates option 1 and also lets us later drop `mtrace` onto the box.
- Stand up a `share` export on this PC and point the unit's shared-host at it, to watch the
  exact CIFS/credentials handshake and confirm the empty-password assumption.
- Decide, with the user, whether to attempt the filename command-injection to get a shell.
