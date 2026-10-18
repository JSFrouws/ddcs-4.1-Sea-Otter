# Network access

## Plan

1. Connect the unit to the same router/switch as the PC (LAN `192.168.1.0/24`, gateway `192.168.1.1`).
2. In the stock app, set the IP to DHCP, or set it statically:
   - IP `192.168.1.200` (checked free on 2026-10-15, no ping reply)
   - Mask `255.255.255.0`, gateway `192.168.1.1`
3. From the PC: ping, then scan common ports (21 FTP, 22 SSH, 23 Telnet, 80 HTTP,
   139/445 SMB, plus a full 1–65535 sweep). Save the output to `raw/`.
4. Try whatever answers. Default logins to try on this kind of embedded i.MX28
   Linux: `root` with no password, `root`/`root`.

Open question: does the network menu make the unit a **server** (something we can log
into), or only a **client** that mounts a share on the PC for G-code files? If it's a
client, the share mount is still useful: it shows which protocol the unit uses.

## Unit's network menu (seen 2026-10-15)

The menu has three fields: **local IP**, **router IP** and **shared host IP**.
The "shared host" field means the unit is a **client** that mounts a share on a
PC (most likely SMB/CIFS) to load files from.

Values entered:

| Field | Value |
|---|---|
| Local IP | `192.168.1.200` |
| Router IP | `192.168.1.1` |
| Shared host IP | `192.168.1.65` (the dev PC; gets its address from DHCP, so it may change) |

## Results

- 2026-10-15: ping to `192.168.1.200` before the settings were applied: no reply.
- 2026-10-15: the menu also has **"Disable network functionality"**, default **YES**; set to
  **NO**. There is no save button, but the values survive a reboot.
- 2026-10-15, after the reboot: still **no ARP reply** at `.200` ("Destination host
  unreachable" comes from the PC itself). A sweep of the whole /24 found no new host and
  no Freescale MAC: see [raw/2026-10-15_lan-sweep.txt](raw/2026-10-15_lan-sweep.txt).
  The unit isn't on the wire yet. Either the link is down, or the app only brings
  up `eth0` when the network is actually used (e.g. browsing the shared folder).
- 2026-10-15, later: **the "Disable network functionality" setting had reverted to YES.**
  After it was set to NO again, the unit answers ping. It **doesn't seem to stick**: check it
  after every reboot.

## What the unit exposes (2026-10-15)

Raw output: [raw/2026-10-15_ddcs-scan.txt](raw/2026-10-15_ddcs-scan.txt).

| Item | Value |
|---|---|
| IP | `192.168.1.200` (static, from the menu) |
| MAC | `00:04:00:00:00:00`: placeholder, never programmed. Two of these units on one LAN would clash |
| Ping TTL | 64 (Linux) |
| Open TCP ports | **139, 445 only**, out of all 65535. **No SSH, Telnet, FTP or HTTP** |
| NetBIOS name service (UDP 137) | no answer to `nbtstat -A` |
| SMB dialects | **SMB1 only** (`NT LM 0.12`). SMB2 negotiate: connection closed |
| SMB security | `security_mode=0x02`: **share-level**, encrypted passwords, no signing. Typical of old Samba 3.0.x |
| Workgroup | `WORKGROUP` |

So the unit runs a **Samba server** as well as having the "shared host" client setting.
The Windows 11 SMB client doesn't speak SMB1 (`net view` gives error 53), so browsing it
needs an SMB1-capable client, e.g. Python `pysmb` or `impacket`.

The probe tool is [../tools/smb_probe.py](../tools/smb_probe.py). It only negotiates and never logs in.

Guest login works with `pysmb`, installed in the project `.venv`. The shares are
`SYSDISK` (the stock app folder) and `CNCDISK` (G-code): see [smb-shares.md](smb-shares.md).

## Our PC as the unit's "shared host" (capturing the client handshake)

Goal: make the unit mount a `share` from this PC so we can see exactly what it sends as a
CIFS **client**. From the binary ([app-analysis.md](app-analysis.md)) it runs:
`mount -t cifs //<host>/share /share -o username=root,password=,iocharset=utf8`
— so it offers account **root** with an **empty password**, SMB1, to a share named **share**.

### Setup state (2026-10-15)

| Thing | State |
|---|---|
| This PC | not admin; port **445 is held by the Windows kernel** (System PID 4 / `LanmanServer`) |
| impacket | **blocked**: Windows Defender quarantines it as a hacktool. Removed it |
| Our server | wrote a stdlib-only SMB1 handshake logger: [../tools/smb_logserver.py](../tools/smb_logserver.py). No third-party deps (Defender-safe), changes no Windows account policy — it only answers SMB1 negotiate with a password challenge, logs the account/password fields, then refuses the tree-connect |
| Port 445 | not in any reserved range, so a non-admin process can bind it **once the Windows Server service is stopped** |
| Firewall | Ethernet is **Private**; the existing File-and-Printer-Sharing inbound rule should already permit 445 |

### Remaining step needs admin (reversible) — run in an **elevated** PowerShell

```powershell
Stop-Service LanmanServer -Force    # frees TCP 445 (also pauses Windows file sharing)
```
Then I run `tools/smb_logserver.py` (non-admin), the unit's shared-host is pointed at this PC
(`192.168.1.65`), and triggering a share browse on the unit produces the capture in
`docs/raw/<date>_cifs-handshake.txt`. Afterwards, restore sharing:
```powershell
Start-Service LanmanServer
```

### Results (2026-10-15)

Captured. The unit mounts as **root with an empty password**, **SMB1 / NT LM 0.12 / NTLMv1**,
and its NativeOS string confirms the kernel `2.6.35.3-571-gcca29a0`. Full analysis and the
cryptographic empty-password proof: [cifs-handshake.md](cifs-handshake.md).

Freeing port 445 needed more than stopping the Server service: the `srv2` and `srvnet`
**kernel drivers** also had to be stopped (elevated). The Windows SMB stack auto-restarted
afterward, so normal file sharing is back with no manual step.
