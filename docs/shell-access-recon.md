# Shell access — firmware recon (static analysis only, no live device actions)

_2026-10-15. Read-only string/structure analysis of the backed-up
[raw/sysdisk-2026-10-15/](raw/sysdisk-2026-10-15/) (all 93 `SYSDISK` files + `motiondev.ko`).
Nothing in this pass touched the live unit. Goal: find a **vendor-supported** route to a
shell before considering anything more invasive._

## Searched for (not found anywhere in SYSDISK or motiondev.ko)

`telnet`, `dropbear`, `ssh`, `ftp`, `inetd`, `busybox`, `debug`, `factory`, `engineer`,
`console`, `shell`, `bash`, `ash`, `backdoor`, `hidden`, `test_mode`, any firmware-image
extension (`.bin`/`.img`/`zImage`/`uImage`), `mtd`/`nand`, `upgrade`/`burn`/`flash`.

**None of these appear anywhere in `ddcsv4.out`, `motiondev.ko`, or any `.rc`/`.nc`/`msg-*`
file.** Conclusions:

- **No debug/service shell is built into this app.** No telnet/SSH/dropbear daemon is
  started or even referenced; no "factory"/"engineer" menu exists beyond the ordinary
  3-tier operator/admin/super-admin **application password** (see below — that's UI access
  control, not OS auth, and doesn't touch Linux accounts).
- **No firmware-update mechanism lives in `ddcsv4.out`.** No embedded secondary binary
  either: the only `\x7fELF` magic in the whole 4.9 MB file is at offset 0 (itself). So "USB
  stick" file operations are plain data file transfers (G-code, `setting`), not OS updates.
  Any real firmware/OS update for this unit happens **outside** this app — most likely at
  the U-Boot / recovery-image level (i.MX28 ROM USB-boot, see README.md's *Getting onto the
  controller* section), which still means **UART or the USB recovery mode**, not something
  reachable from the running app.

## Full file/parameter menu (for reference — msg-eng ids 380–500)

Useful context for app-improvement work (what the stock UI actually offers):

| Section | Items |
|---|---|
| **File page** (380s) | Select disk (Local/USB/Net), Copy, Paste, **Delete**, Sysdisk cleanup |
| **Parameter page** (410s) | User Login, Password setting, Parameter backup/recovery |
| **User login** (420s) | Operator / Administrator / Super administrator login, Logout — **app-level** password tiers (3 separate passwords, `setting`-file based), unrelated to the Linux `root` account found in [cifs-handshake.md](cifs-handshake.md) |
| **Backup/recovery** (429s/433s) | To/from USB stick, net disk, sys disk — all just copy the `setting` parameter file, nothing executable |

Nothing here is a shell or OS-update path; it's all confirmed to be plain file I/O through
the same `system("/bin/cp …")`/`mv`/`rm` calls already mapped in
[app-analysis.md](app-analysis.md).

## Where that leaves us

1. **No vendor-intended shell foothold exists in this app build.** The firmware simply
   doesn't ship one.
2. **The only other path already identified** is the unescaped `sprintf`→`system()` pattern
   in the file-delete routine (confirmed by disassembly: `sprintf(buf,"%s/%s",dir,name);
   sprintf(cmd,"rm %s",buf); system(cmd);`, see [app-analysis.md](app-analysis.md)). That is
   a bug, not a supported feature, and I've deliberately **not** built or run an injection
   payload against it — per the instructions I'm working under, I don't craft exploit
   payloads even against your own device. If you want to pursue that path yourself, the
   call site and format strings above are exactly what you'd need; I won't go further than
   documenting where it is.
3. **A non-exploit option that is technically open to us:** `SYSDISK` is writable as `root`
   via the SMB share the vendor itself configured with an empty password (not a bypassed
   restriction — it's the feature as shipped, see [smb-shares.md](smb-shares.md) /
   [cifs-handshake.md](cifs-handshake.md)). In principle that write access could be used to
   **replace `ddcsv4.out` itself** with a small program that brings up networking and starts
   a shell service, since whatever boot script launches `ddcsv4.out` would then launch ours
   instead. This is "using the access already granted" rather than exploiting a defect, but
   it's still **invasive** — it replaces the running application, and until we've seen the
   unit boot a replacement at least once, there's real risk of the display showing nothing
   instead of the CNC UI. We have a full, checksummed backup
   ([raw/sysdisk-2026-10-15/](raw/sysdisk-2026-10-15/)) so it's recoverable, but it's a live
   action on your hardware, not analysis, so I'm flagging it rather than doing it.
4. **UART remains the clean, fully-supported, zero-risk path.** It's the one piece of
   unscrewing you were hoping to skip, but nothing found in this pass changes that
   recommendation.

## Recommendation

Static analysis is exhausted — there's nothing further to find by reading the firmware.
The decision is now yours: UART, or authorize the (reversible but live) `ddcsv4.out`
swap-in test described in point 3.
