# CIFS client handshake — what the unit sends as "shared host" client

_2026-10-15. We stood up a stdlib SMB1 server on this PC
([../tools/smb_logserver.py](../tools/smb_logserver.py)), pointed the unit's **shared host**
at `192.168.1.65`, and selected **NetDisk** on the unit to trigger its
`mount -t cifs //192.168.1.65/share`. Raw capture + verification:
[raw/2026-10-15_cifs-handshake.txt](raw/2026-10-15_cifs-handshake.txt)._

## What the unit sends

| Field | Value |
|---|---|
| Transport | TCP **445** (direct SMB, not NetBIOS/139) |
| Dialects offered | `LM1.2X002`, `LANMAN2.1`, **`NT LM 0.12`**, `POSIX 2` (CIFS Unix extensions) |
| SMB version | **SMB1 only**, non-extended security (no NTLMSSP/SPNEGO) |
| Account | **`root`** |
| Auth | **NTLMv1** challenge/response (24-byte LM + 24-byte NT fields) |
| Password | **EMPTY — cryptographically confirmed** (see below) |
| NativeOS | **`Linux version 2.6.35.3-571-gcca29a0`** |
| NativeLanMan | `CIFS VFS Client for Linux` |
| Tree connect | `\\192.168.1.65\share` |
| Retry | the client reconnects every ~10 s while NetDisk is selected and the mount fails |

## Empty password — the proof

A 24-byte NTLM response does **not** mean the password is non-empty: `mount.cifs` with
`sec=ntlm` always sends a challenge/response, even for an empty password. To actually prove
it, [../tools/ntlm_verify.py](../tools/ntlm_verify.py) computes the NTLMv1 response an **empty**
password would produce for our challenge `1122334455667788`:

```
NT hash('')       = 31d6cfe0d16ae931b73c59d7e0c089c0
empty NT response = eefabc742621a883aec4b24e0f7fbf05e17dc2880abe07cc
```

That exact 24-byte sequence is present in the captured session-setup bytes
(`007400|eefabc742621a883aec4b24e0f7fbf05e17dc2880abe07cc|eefabc...`). So the unit
authenticates as **root with an empty password**, confirming the hardcoded mount option
`username=root,password=` found in the binary ([app-analysis.md](app-analysis.md)). (The
unit put the NT response in both the LM and NT slots; the real LM-of-empty response is not
used. The capture tool's byte offsets were ~3 bytes misaligned, which is why the raw dump
shows a `007400` prefix and a duplicated tail — the response bytes themselves are intact and
match.)

## Why this matters

- **Independent kernel confirmation.** The client's NativeOS string is
  `2.6.35.3-571-gcca29a0` — the exact kernel our driver's vermagic targets
  ([motiondev-ko.md](motiondev-ko.md)). Two independent sources now agree.
- **To serve files to the unit from a host**, the SMB server must: speak **SMB1**, accept
  **`NT LM 0.12`**, and accept **user `root` with an empty password via NTLMv1** (or map that
  to guest). A Linux/Samba host does this trivially
  (`server min protocol = NT1`, `ntlm auth = ntlmv1-permitted`, and either a passwordless
  `root` or `map to guest = Bad User`). Windows 10/11 is painful for this (SMB1 server is
  deprecated, blank-password network logon is blocked by policy), which is why our capture
  used a custom logger rather than a real Windows share.

## Doing this again on Windows (notes)

- Port 445 is held by the Windows kernel SMB stack. Freeing it needs, in an **elevated**
  shell: `Stop-Service LanmanServer -Force` **and** `Stop-Service srv2 -Force; Stop-Service
  srvnet -Force` (stopping just the service is not enough; the `srv2`/`srvnet` kernel drivers
  keep the port). The stack **auto-restarts** (trigger-started), restoring normal sharing —
  no manual `Start-Service` was needed afterward here.
- Windows Defender quarantines **impacket**; use the stdlib logger instead.
- `pycryptodome` (for the NTLM check) installs fine and is not flagged.
