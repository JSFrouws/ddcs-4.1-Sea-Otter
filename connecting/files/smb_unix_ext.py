"""CIFS Unix Extensions SET/QUERY_PATH_INFORMATION (SMB_*_FILE_UNIX_BASIC), which pysmb
does not implement even though the DDCS's Samba server advertises CAP_UNIX (confirmed in
docs/network.md). Needed because overwriting a file over plain SMB1 either:
  - fails with ETXTBSY if the target is currently executing (can't open-for-write), or
  - succeeds via delete+recreate, but the new inode gets Samba's default create
    permissions, which may not include the executable bit.
This sets the exact unix mode bits via the CIFS Unix extensions SET_PATH_INFORMATION
request, independent of Samba's create-mask config.

This is a small, self-contained SMB1 client (NEGOTIATE/SESSION_SETUP/TREE_CONNECT done by
hand - same NTLMv1-empty-password logic already verified in tools/ntlm_verify.py and the
same SMB1 framing already verified in tools/smb_logserver.py) so it never has to interleave
raw packets with pysmb's own request/response state machine.

Usage as a library: see chmod_path() / stat_unix() below.
Usage as a script (self-test against a throwaway file - does not touch anything else):
  .venv/Scripts/python -I tools/smb_unix_ext.py selftest <share> [host]
"""
import socket
import struct
import sys

from Crypto.Cipher import DES
from Crypto.Hash import MD4

SMB_SET_FILE_UNIX_BASIC = 0x0200
SMB_QUERY_FILE_UNIX_BASIC = 0x0200
TRANS2_SET_PATH_INFORMATION = 0x0006
TRANS2_QUERY_PATH_INFORMATION = 0x0005
NEG_ONE_64 = 0xFFFFFFFFFFFFFFFF
NEG_ONE_32 = 0xFFFFFFFF


def _des_key(k7: bytes) -> bytes:
    b = int.from_bytes(k7, "big")
    return bytes(((b >> (49 - 7 * i)) & 0x7F) << 1 for i in range(8))


def _ntlmv1_response(hash16: bytes, challenge: bytes) -> bytes:
    key21 = hash16 + b"\x00" * 5
    out = b""
    for i in range(0, 21, 7):
        out += DES.new(_des_key(key21[i:i + 7]), DES.MODE_ECB).encrypt(challenge)
    return out


def _nb(payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + payload


def _recv_nbt(sock) -> bytes:
    hdr = b""
    while len(hdr) < 4:
        c = sock.recv(4 - len(hdr))
        if not c:
            raise ConnectionError("connection closed")
        hdr += c
    length = struct.unpack(">I", hdr)[0] & 0xFFFFFF
    buf = b""
    while len(buf) < length:
        c = sock.recv(length - len(buf))
        if not c:
            raise ConnectionError("connection closed mid-message")
        buf += c
    return buf


def _smb_header(cmd, flags2, tid, pid, uid, mid, flags=0x18):
    return (b"\xffSMB" + bytes([cmd]) + b"\x00" * 4 +
            bytes([flags]) + struct.pack("<H", flags2) + b"\x00" * 12 +
            struct.pack("<HHHH", tid, pid & 0xFFFF, uid, mid))


class RawSMB1:
    """Minimal synchronous SMB1 client: negotiate, guest session setup, tree connect,
    and the one TRANS2 operation this tool exists for. No concurrency, no retries -
    intentionally small and auditable rather than general-purpose."""

    def __init__(self, host, port=445, timeout=10):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.pid = 1234
        self.mid = 1
        self.uid = 0
        self.tid = 0

    def _next_mid(self):
        self.mid += 1
        return self.mid

    def negotiate(self):
        # The server's dialect negotiation needs the full historical progression to land
        # on NT LM 0.12 - a bare single-dialect offer gets DialectIndex=0xFFFF (rejected).
        # Same list proven working in tools/smb_probe.py against this exact server.
        dialects = [b"PC NETWORK PROGRAM 1.0", b"LANMAN1.0", b"Windows for Workgroups 3.1a",
                    b"LM1.2X002", b"LANMAN2.1", b"NT LM 0.12", b"SMB 2.002", b"SMB 2.???"]
        data = b"".join(b"\x02" + d + b"\x00" for d in dialects)
        body = (self._smb0(0x72) + b"\x00" + struct.pack("<H", len(data)) + data)
        self.sock.sendall(_nb(body))
        r = _recv_nbt(self.sock)
        assert r[:4] == b"\xffSMB" and r[4] == 0x72, "bad negotiate reply"
        # Fixed offsets for the NT LM 0.12 negotiate response (wordcount=17), proven
        # against this exact server in tools/smb_probe.py: DialectIndex(2) SecurityMode(1)
        # MaxMpxCount(2) MaxNumberVcs(2) MaxBufferSize(4) MaxRawSize(4) SessionKey(4)
        # Capabilities(4) SystemTimeLow(4) SystemTimeHigh(4) ServerTimeZone(2)
        # ChallengeLength(1), starting at byte 33.
        key_len = r[66]
        self.challenge = r[69:69 + key_len]
        return self.challenge

    def _smb0(self, cmd):
        return _smb_header(cmd, 0xC801, 0, self.pid, self.uid, self._next_mid()) + b"\x00"

    def session_setup_guest(self):
        challenge = self.negotiate()
        nt_hash = MD4.new(b"").digest()
        lm_hash = bytes.fromhex("aad3b435b51404ee" * 2)
        nt_resp = _ntlmv1_response(nt_hash, challenge)
        lm_resp = _ntlmv1_response(lm_hash, challenge)
        account, domain, os_, lanman = b"\x00", b"WORKGROUP\x00", b"SeaOtter\x00", b"SeaOtter\x00"
        words = struct.pack("<BBHHHHIHH", 0xFF, 0, 0, 0xFFFF, 2, 0, 0,
                             len(lm_resp), len(nt_resp)) + b"\x00\x00\x00\x00" + struct.pack("<I", 0x00000050)
        data = lm_resp + nt_resp + account + domain + os_ + lanman
        hdr = _smb_header(0x73, 0xC801, 0, self.pid, 0, self._next_mid())
        body = hdr + bytes([13]) + words + struct.pack("<H", len(data)) + data
        self.sock.sendall(_nb(body))
        r = _recv_nbt(self.sock)
        assert r[4] == 0x73, "bad session setup reply"
        status, = struct.unpack_from("<I", r, 5)
        if status not in (0, 0xC0000016):  # 0 = OK; some servers return more info needed
            raise PermissionError(f"session setup failed: NTSTATUS 0x{status:08x}")
        self.uid, = struct.unpack_from("<H", r, 32)
        return self.uid

    def tree_connect(self, share, host):
        # Mirrors pysmb's ComTreeConnectAndxRequest byte-for-byte (smb/smb_structs.py) -
        # in particular PasswordLength is the password field's own length (1, for the
        # single empty-string null terminator), NOT the whole data block's length, and
        # Flags must be 0x08, not 0.
        password = b"\x00"  # '' + '\0', UTF-8 encoded
        password_len = len(password)
        padding = b"\x00" if password_len % 2 == 0 else b""
        path = f"\\\\{host}\\{share}".encode("utf-16-le")
        service = b"?????\x00"
        data = password + padding + path + b"\x00\x00" + service
        words = struct.pack("<BBHHH", 0xFF, 0, 0, 0x08, password_len)
        hdr = _smb_header(0x75, 0xC801, 0, self.pid, self.uid, self._next_mid())
        body = hdr + bytes([4]) + words + struct.pack("<H", len(data)) + data
        self.sock.sendall(_nb(body))
        r = _recv_nbt(self.sock)
        assert r[4] == 0x75, "bad tree connect reply"
        status, = struct.unpack_from("<I", r, 5)
        if status != 0:
            raise PermissionError(f"tree connect failed: NTSTATUS 0x{status:08x}")
        self.tid, = struct.unpack_from("<H", r, 24)
        return self.tid

    def _trans2(self, subcommand, params: bytes, data: bytes = b"") -> bytes:
        # Built and parsed with pysmb's own (proven) SMBMessage/ComTransaction2Request
        # wire-format code rather than a hand-rolled TRANS2 envelope - offset/padding
        # arithmetic for TRANS2 is easy to get subtly wrong, and this is the one piece
        # pysmb's high-level API doesn't expose (it has no Unix-extensions support), not
        # a reason to reimplement the whole envelope by hand too.
        from smb.smb_structs import ComTransaction2Request, ComTransaction2Response, SMBMessage

        req = ComTransaction2Request(
            max_params_count=1024, max_data_count=4096, max_setup_count=0,
            params_bytes=params, data_bytes=data,
            setup_bytes=struct.pack("<H", subcommand))
        m = SMBMessage(req)
        m.flags, m.flags2 = 0x18, 0xC801
        m.tid, m.pid, m.uid, m.mid = self.tid, self.pid, self.uid, self._next_mid()
        self.sock.sendall(_nb(m.encode()))

        resp = SMBMessage()
        resp.decode(_recv_nbt(self.sock))
        if resp.status.hasError:
            raise OSError(f"TRANS2 subcommand 0x{subcommand:04x} failed: "
                           f"NTSTATUS 0x{resp.status.internal_value:08x}")
        assert isinstance(resp.payload, ComTransaction2Response)
        return resp.payload.data_bytes

    def close(self):
        self.sock.close()


def _path_params(path: str, info_level: int) -> bytes:
    p = path.replace("/", "\\")
    if not p.startswith("\\"):
        p = "\\" + p
    return struct.pack("<H", info_level) + b"\x00\x00\x00\x00" + p.encode("utf-16-le") + b"\x00\x00"


def chmod_path(conn: RawSMB1, path: str, mode: int):
    """Set unix permission bits on `path`, leaving everything else (size/times/uid/gid)
    untouched via the CIFS Unix extensions -1 ("don't change") sentinel."""
    data = struct.pack("<QQQQQQQIQQQQQ",
                        NEG_ONE_64, NEG_ONE_64, NEG_ONE_64, NEG_ONE_64, NEG_ONE_64,  # size/times
                        NEG_ONE_64, NEG_ONE_64,                                     # uid, gid
                        NEG_ONE_32,                                                 # type
                        NEG_ONE_64, NEG_ONE_64, NEG_ONE_64,                         # dev major/minor, uniqueid
                        mode,                                                       # permissions - the only real value
                        NEG_ONE_64)                                                 # nlinks
    conn._trans2(TRANS2_SET_PATH_INFORMATION, _path_params(path, SMB_SET_FILE_UNIX_BASIC), data)


def stat_unix(conn: RawSMB1, path: str) -> dict:
    data = conn._trans2(TRANS2_QUERY_PATH_INFORMATION, _path_params(path, SMB_QUERY_FILE_UNIX_BASIC))
    fields = struct.unpack_from("<QQQQQQQIQQQQQ", data, 0)
    names = ["EndOfFile", "NumOfBytes", "ctime", "atime", "mtime", "Uid", "Gid",
             "Type", "DevMajor", "DevMinor", "UniqueId", "Permissions", "Nlinks"]
    return dict(zip(names, fields))


def _selftest(share, host):
    print(f"connecting to {host} ...")
    conn = RawSMB1(host)
    conn.session_setup_guest()
    conn.tree_connect(share, host)
    print(f"authenticated, tid={conn.tid}")

    import io
    from smb.SMBConnection import SMBConnection
    path = "_unixext_selftest.tmp"
    pc = SMBConnection("", "", "PANDORA-PC", "*SMBSERVER", use_ntlm_v2=False, is_direct_tcp=True)
    pc.connect(host, 445, timeout=10)
    pc.storeFile(share, "/" + path, io.BytesIO(b"hello\n"))
    print(f"created throwaway file /{path} via pysmb")

    before = stat_unix(conn, path)
    print(f"permissions before chmod: 0o{before['Permissions'] & 0o7777:o}")

    chmod_path(conn, path, 0o750)
    after = stat_unix(conn, path)
    print(f"permissions after chmod(0o750): 0o{after['Permissions'] & 0o7777:o}")

    ok = (after["Permissions"] & 0o7777) == 0o750
    print("SELFTEST " + ("PASSED" if ok else "FAILED"))

    pc.deleteFiles(share, "/" + path)
    pc.close()
    conn.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "selftest":
        _selftest(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "192.168.1.200")
    else:
        print(__doc__)
