# Shell access — the ddcsv4.out swap (recovery_shell)

_2026-10-15/16. Follow-up to [shell-access-recon.md](shell-access-recon.md): static analysis
found no vendor debug shell, so this uses option 3 from that doc - replacing `ddcsv4.out`
over the vendor's own writable SMB share with a small program that brings up networking and
serves a token-gated shell. Built and functionally tested (listener/token/shell/reconnect,
all working) in [../mockup/recovery-shell/](../mockup/recovery-shell/) before anything
touched the real unit._

> **STATUS as of 2026-10-16, end of session - read this first.**
> This worked end to end on the real hardware. Current state, left deliberately staged
> rather than finished, because the last step needs you to physically power-cycle the
> unit:
> - The real `ddcsv4.out` content **has been restored** on `SYSDISK` (verified by hash +
>   executable bit - see the restore log below), but **this has not taken effect yet**:
>   the unit is still *running* the old `recovery_shell` process in memory (renaming the
>   file on disk doesn't replace an already-running process, see the ETXTBSY section
>   below). **It's running the real app in-name-only until the next boot.**
> - **Persistent SSH is now set up** ([ssh-access.md](ssh-access.md)) and wired into
>   `/etc/init.d/rcS`, so it starts automatically on every future boot too - including
>   the very next one.
> - **So: the next power-cycle brings back the real CNC app *and* gives you SSH
>   (`ssh -i mockup/dropbear/ddcsv4_id_ed25519 root@192.168.1.200`) at the same time.**
>   Nothing else needs to happen first - just power-cycle whenever convenient.
> - After that boot, worth checking: (1) the CNC UI comes up normally, (2) SSH connects,
>   (3) rerun `/ddcsv4/ioctl_probe` to retest the MPG count with the real app running -
>   see [mpg-investigation.md](mpg-investigation.md).
> - Found along the way and documented separately: the real firmware-update mechanism
>   ([rootfs-findings.md](rootfs-findings.md) - a USB stick's `ddcsv4/` folder, copied in
>   every boot) and a complete ioctl-ABI validation against real hardware
>   ([mockup/ioctl-probe/](../mockup/ioctl-probe/) - `FPGA_REG_VERSION` read back
>   `0x0809` from the real FPGA).

## Why this is safe to attempt

- `SYSDISK` write access is the **vendor's own feature** (empty-password root SMB, confirmed
  in [cifs-handshake.md](cifs-handshake.md)), not a bypassed restriction.
- We have a full, **checksummed backup** of the original `ddcsv4.out`
  ([raw/sysdisk-2026-10-15/](raw/sysdisk-2026-10-15/)), and [tools/restore_ddcsv4.py](../tools/restore_ddcsv4.py)
  puts it back with its own verification.
- `ddcsv4.out` has **zero references to smb/samba/smbd** (checked by string search) - Samba
  is an independent system service, always running. The only reason the unit was
  unreachable before was `eth0` being down/unconfigured, which `ddcsv4.out` itself brings up
  via `ifconfig`/`route`. So **SMB access does not depend on our replacement staying
  alive** - as long as our binary brings the interface up once (confirmed working, see
  below), Samba answers regardless of what happens to our process afterward. This was the
  key risk to rule out before touching the real unit.
- `deploy_recovery_shell.py` refuses to run unless the remote file still matches the known
  backup hash, and verifies every write by reading it back before moving on.

## What `recovery_shell` does

[../mockup/recovery-shell/recovery_shell.c](../mockup/recovery-shell/recovery_shell.c):
brings up `eth0` at `192.168.1.200/24` gw `192.168.1.1` (the same static config already set
on the unit) via raw `ioctl()` calls - no dependency on `ifconfig`/busybox being on PATH -
then listens on **TCP 7777**, requires a 32-byte shared token before doing anything, and on
a match `execve`s `/bin/sh` with the socket as stdin/stdout/stderr. Loops to accept again
after each session. Built statically (`-static`) for ARMv5 EABI to sidestep any glibc
version mismatch with the device.

**Tested and fixed before deployment** (see conversation): dropped `/bin/sh`'s `-i` flag
(forced-interactive mode does job-control setup that fails and can kill the shell after one
command without a real tty - the standard socket-shell gotcha), and fixed the test client's
reconnect/exit handling. Verified end-to-end: `id`, `uname -a`, multiple commands in one
session, clean `exit`, and the listener accepting a fresh connection afterward - all working
on a native x86_64 build, and the binary itself confirmed running without crashing under
`qemu-arm` ARM emulation.

## Mechanical gotcha #1: this is not a hot-swap

Overwriting the file on disk does **not** replace the already-running `ddcsv4.out` process
in memory - Linux keeps executing the image it already loaded. The swap only takes effect
on the **next exec**, i.e. after a **power-cycle**. The live unit keeps running normally
right up until you power-cycle it.

## Mechanical gotcha #2: ETXTBSY, and the executable-bit question it reopens

The first live attempt (truncate-in-place, via `storeFile` on the existing path) failed
with `STATUS_ACCESS_DENIED`. This is **ETXTBSY**: Linux refuses to open a *currently
executing* file for writing - staging a new filename worked fine for the same reason it
failed on `ddcsv4.out` itself.

First fix attempt: **delete (unlink) the live `ddcsv4.out`, then create a fresh file at
that path.** Deleting a running executable is always allowed by the Linux kernel and
doesn't disturb the running process - but this specific server returned
`STATUS_CANNOT_DELETE` instead: **Samba itself** (not the kernel) refuses the delete, via
its "kernel share modes" feature, which detects the file is open locally and blocks SMB
deletes of it as a safety measure.

**Working fix: rename it aside instead of deleting it.** Confirmed first with a
non-destructive round-trip test ([../tools/test_rename_live.py](../tools/test_rename_live.py):
rename the live file away, confirm, rename it straight back, verify the hash is still
exactly the known-good one) before ever using it for the real swap. Renaming doesn't
require the file to be closed, so it isn't blocked by either ETXTBSY or Samba's
share-mode check, and the running process is completely unaffected (same name-vs-inode
reasoning as the delete approach - it just works, where delete didn't). Either way the new
`ddcsv4.out` is a **new inode**, created fresh via SMB - which reopens the question
[smb-shares.md](smb-shares.md) originally sidestepped: does it come out executable?

- **pysmb has no CIFS Unix Extensions support** (chmod), even though the server
  advertises `CAP_UNIX`. Added [../tools/smb_unix_ext.py](../tools/smb_unix_ext.py): a
  small hand-rolled SMB1 client for exactly the one operation pysmb lacks
  (`SMB_SET/QUERY_FILE_UNIX_BASIC` via `TRANS2_SET/QUERY_PATH_INFORMATION`), built on top
  of **pysmb's own proven `SMBMessage`/request-response classes** for the wire format
  (only the NTLMv1-empty-password crypto and the raw NEGOTIATE/SESSION_SETUP/TREE_CONNECT
  framing are hand-written, reusing logic already verified in `ntlm_verify.py` and
  `smb_logserver.py`).
- **Self-tested against a throwaway file before touching anything real:** create via
  pysmb, chmod via the new tool, query back to confirm - `SELFTEST PASSED`
  (`.venv/Scripts/python -I tools/smb_unix_ext.py selftest CNCDISK`).
- Useful side finding from the self-test: **Samba's default create mode here is `0744`**
  (owner read/write/execute) - a fresh file is *already* executable by default. The
  explicit `chmod 0755` is done anyway for certainty rather than relying on that default.
- `deploy_recovery_shell.py` and `restore_ddcsv4.py` now both: **rename** the live file
  aside (to `ddcsv4.out.was_running` / `ddcsv4.out.recovery_shell_was_running`
  respectively - left in place afterward, harmless, can be tidied up later), create the
  replacement, `chmod 0755` it explicitly, and verify both content hash and the
  executable bit before declaring success - refusing (exit nonzero, clear message) if
  either check fails, rather than leaving you to discover it after a power-cycle.

## Live procedure

1. `.venv/Scripts/python -I tools/deploy_recovery_shell.py` - verifies the remote file
   against the backup, stages the new binary, verifies the staged copy, then overwrites
   `ddcsv4.out` in place and verifies that too.
2. **Power-cycle the unit.** The CNC UI will not come up this boot - that's expected. The
   screen may go blank/stay on the boot splash; this is temporary and reversible.
3. From the dev PC: `python3 mockup/recovery-shell/recovery_connect.py 192.168.1.200 7777`
   (the token is embedded in both `recovery_shell.c` and `recovery_connect.py` - keep them
   in sync if either is regenerated).
4. Confirm with `id`, `uname -a`, and check `/proc/kallsyms` for `__crc_preempt_schedule`
   (closes the last open item in [motiondev-ko.md](motiondev-ko.md)), grab the real
   `motiondev.ko` loader script from the rootfs (closes the open item in
   [app-analysis.md](app-analysis.md)), etc.
5. When done: `exit` the shell (or just disconnect - the listener keeps accepting).
6. `.venv/Scripts/python -I tools/restore_ddcsv4.py` - writes the verified original back
   and confirms the hash.
7. **Power-cycle again** to return to normal operation.

## If something goes wrong

- **Swap script refuses to run:** the remote file no longer matches the backup hash - stop
  and investigate before forcing anything.
- **After power-cycle, nothing answers:** give it the usual boot time, then re-run the
  network check (`ping 192.168.1.200`, `tools/smb_logserver.py`-style checks). If SMB comes
  back but the listener doesn't, the binary may have crashed after bringing up networking -
  SMB write access still works (independent of our process, per above), so
  `restore_ddcsv4.py` + power-cycle gets you back regardless.
- **Worst case, nothing comes back at all:** UART is still the hardware fallback, unaffected
  by any of this (same physical board, same recovery path as before).
