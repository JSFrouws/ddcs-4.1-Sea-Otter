# rootfs findings — from the live shell (2026-10-16)

_Gathered via the `recovery_shell` swap, see [shell-access-live-test.md](shell-access-live-test.md).
Read-only exploration; nothing here was changed on the device._

## Confirmed environment

```
id            -> uid=0(root) gid=0
uname -a      -> Linux DDCSV4 2.6.35.3-571-gcca29a0 #362 PREEMPT Tue Dec 21 22:23:08 CST 2021 armv5tejl GNU/Linux
/proc/version -> ...(gcc version 4.4.4 (4.4.4_09.06.2010))... #362 PREEMPT Tue Dec 21 22:23:08 CST 2021
```

Matches our driver's vermagic (`2.6.35.3-571-gcca29a0 preempt mod_unload modversions ARMv5`)
and the GCC version noted in `motiondev.ko`'s `.comment` section exactly.

```
mount -> rootfs / rootfs(rw); /dev/root on / type ext3 (rw); /proc, /sys, /dev/pts
lsmod -> motiondev 4922 0 - Live 0xbf000000   (the vendor driver, already loaded fine)
```

## `/etc/init.d/rcS` — the real boot script (closes the open item in [app-analysis.md](app-analysis.md))

```sh
#!/bin/sh
/bin/mount -t proc none /proc
/bin/mount -t sysfs sysfs /sys
/bin/mount -t devtmpfs devtmpfs /dev
/bin/mkdir /dev/pts
/bin/mount -t devpts devpts /dev/pts
/bin/echo > /dev/mdev.seq
/bin/echo /sbin/mdev > /proc/sys/kernel/hotplug
sbin/mdev -s

/bin/mount -a
echo "7 4 1 7" > /proc/sys/kernel/printk
hostname -F /etc/sysconfig/HOSTNAME

/bin/cp /udisk-sda1/ddcsv4/* /ddcsv4/
/bin/cp /udisk-sda/ddcsv4/* /ddcsv4/
sync

/sbin/insmod /ddcsv4/motiondev.ko
cd /ddcsv4/
./ddcsv4.out &

/usr/local/samba/bin/smbd -D
/usr/local/samba/bin/nmbd -D
```

**This is the vendor's real firmware-update mechanism**, and it's much simpler than
anything in [shell-access-recon.md](shell-access-recon.md)'s static analysis found (that
pass only looked at `ddcsv4.out` itself, which indeed has no update logic - the update
step lives in the *boot script*, outside the app):

- **Every boot unconditionally copies `/udisk-sda1/ddcsv4/*` and `/udisk-sda/ddcsv4/*`
  into `/ddcsv4/`** (silently no-ops if no USB stick / no such folder is present - `cp`
  on a missing source just errors to the console log and continues).
- So the real, intended way to update the app, the driver, or anything else on `SYSDISK`
  is: **put the files in a `ddcsv4/` folder on a USB stick, plug it in before power-on,
  boot once.** No SMB wrestling, no rename-aside dance, no ETXTBSY/STATUS_CANNOT_DELETE -
  the copy happens before `ddcsv4.out` (or the old driver) is even started, so nothing is
  "in use" yet.
- This is the way to go for **deploying the real, CRC-fixed `motiondrv.ko`** (see
  [motiondev-ko.md](motiondev-ko.md)) once it's built - far simpler than any SMB-based
  swap.
- `/sbin/insmod /ddcsv4/motiondev.ko` confirms exactly where and how the driver loads -
  matches every assumption in [motiondev-ko.md](motiondev-ko.md).
- `cd /ddcsv4/; ./ddcsv4.out &` confirms the app launches backgrounded, cwd `/ddcsv4`,
  explaining the `%s/%s` relative-path sprintf pattern found in
  [app-analysis.md](app-analysis.md).
- `smbd`/`nmbd` start **unconditionally**, confirming the earlier reasoning in
  [shell-access-live-test.md](shell-access-live-test.md): Samba never depended on
  `ddcsv4.out` continuing to run, only on `eth0` being configured (which `ddcsv4.out`,
  or our `recovery_shell` replacement, each do on their own startup).

## `/etc/hotplug/add_remove_udisk.sh` — USB insert/remove handling

```sh
if [ $ACTION = "add" ]; then
    mkdir /udisk-$MDEV
    mount /dev/$MDEV /udisk-$MDEV
    echo "/udisk-$MDEV" > /tmp/udiskop
    add=$(ps | grep -e "./ddcsv4.out" | grep -v "grep" | tr -s ' ' | cut -d ' ' -f 2)
    kill -s USR1 $add
else
    umount /udisk-$MDEV
    echo "/udisk-$MDEV" > /tmp/udiskop
    remove=$(ps | grep -e "./ddcsv4.out" | grep -v "grep" | tr -s ' ' | cut -d ' ' -f 2)
    kill -s USR2 $remove
fi
```

Called by `mdev` on USB hotplug. Mounts/unmounts at `/udisk-<device>`, and signals the
running `ddcsv4.out` (`SIGUSR1` on insert, `SIGUSR2` on remove) so the app can rescan
without polling - explains the `"USB stick offline during machining"` string found in
`msg-eng` ([app-analysis.md](app-analysis.md)).

## Open: reading the `preempt_schedule` CRC value

`/proc/kallsyms` confirms the symbol and its CRC-table *address*, but not the CRC
*value* itself:

```
c04c3df0 T preempt_schedule_irq
c04c3e78 T preempt_schedule
c0600768 r __ksymtab_preempt_schedule
c0608b3c r __kcrctab_preempt_schedule
c060db38 r __kstrtab_preempt_schedule
```

Reading the 4 bytes at `c0608b3c` needs `/dev/kmem` (checking availability) or another
way to peek kernel memory from userspace. Only matters if the rebuilt `motiondrv.ko` ends
up actually importing `preempt_schedule` - it may not, see
[motiondev-ko.md](motiondev-ko.md).
