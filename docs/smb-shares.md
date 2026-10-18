# SMB shares on the unit

_Found 2026-10-15 with [../tools/smb_browse.py](../tools/smb_browse.py) (pysmb, guest login, listing only).
Full listing: [raw/2026-10-15_smb-shares.txt](raw/2026-10-15_smb-shares.txt)._

How to connect: SMB1 over TCP 445, **guest** (empty user and password), remote name `*SMBSERVER`.
Server comment: `IPC Service (Arm Linux Samba Server)`.

| Share | What it is |
|---|---|
| `SYSDISK` | **The stock application folder**, flat with no subdirectories. 97 files |
| `CNCDISK` | User G-code storage. Currently only `CampusBalk.nc` (183852 B) |
| `IPC$` | standard |

## SYSDISK contents (grouped)

| Group | Files | Notes |
|---|---|---|
| Application | `ddcsv4.out` (4938156 B) | the main program |
| Driver | `motiondev.ko` (14231 B) | the stock motion driver, loaded from here at boot (probably). This is the file our `motiondrv.ko` would replace |
| Settings | `setting`, `setting-bk` (12000 B each), `uservar` (3200), `coord1` (432) | binary parameter tables; `setting-bk` is the backup copy |
| UI languages | `eng`, `chs`, `rus`, `jp`, `custom` + `msg-eng/chs/rus/custom` | string tables |
| Boot logo | `logo.bmp` (1843256 B) | probably 800x600 24-bit (1843200 + 54 header) |
| Macros (`.rc`) | `adjush`, `advstart*`, `array`, `break*`, `center`, `M6`, `macroMill*`, `probe-*`, `selcoord`, `simulate`, `template` | the macro scripts behind the menu functions |
| Fixed G-code (`.nc`) | `home_0..4`, `home_ref1..4`, `zero*`, `gotoz*`, `safez`, `M3/M4/M5`, `slib*.nc`, `pause`, `end`, … | short fixed programs called by the app |
| Job state | `.CampusBalk*.nc.env/.pos/.msg`, `.break0-0`, `.file` | resume/breakpoint state per job |

## Implications

- **This is a file transfer path onto the box with no UART needed**, if the share is
  writable. Not tested yet: writing hasn't been tried, on purpose.
- It's also a way to **back up** the whole stock app folder before changing anything.
- No shell yet: SMB only gives files. To *run* `mtrace` or `insmod` we still need a way to
  execute something. Options to investigate: the boot script (where it loads `motiondev.ko`
  from), a macro/`.rc` feature that can call the shell, or the UART.

## Backup

2026-10-15: all 93 files of `SYSDISK` downloaded read-only to
[raw/sysdisk-2026-10-15/](raw/sysdisk-2026-10-15/) (7.3 MB) with
[../tools/smb_backup.py](../tools/smb_backup.py). Every size matches the listing, and the
checksums are in `SHA256SUMS`. Watch out for `simulate.RC` and `simulate.rc`: they're two
files on the unit but one name on Windows. Both were fetched into memory and are
byte-identical (sha256 `72de446d…0388`), so the single local copy stands for both.

## Write test (2026-10-15)

Both shares are **fully read-write** over guest SMB1. [../tools/smb_writetest.py](../tools/smb_writetest.py)
created one throwaway file, listed it, read it back (bytes matched) and deleted it on each —
all OK, nothing left behind:

| Share | write | read-back | delete |
|---|---|---|---|
| `CNCDISK` | OK | matches | OK |
| `SYSDISK` | OK | matches | OK |

**Implications:**
- `SYSDISK` is the app partition (mounts at `/ddcsv4`), so `ddcsv4.out`, **`motiondev.ko`**,
  `setting`, and the macros can all be **replaced over the network** — no UART needed to
  deploy our driver or `mtrace`. Back up first (done: [raw/sysdisk-2026-10-15/](raw/sysdisk-2026-10-15/)).
- Writability also means we can drop files whose **names** we control onto the box, which is
  the prerequisite for the filename→`system()` path noted in [app-analysis.md](app-analysis.md).
- Caution: a bad `motiondev.ko` or `ddcsv4.out` could stop the app from booting, and network
  access depends on the app, so **don't overwrite either until there's a recovery path**
  (UART console, or confidence we can restore the file blind over SMB).
