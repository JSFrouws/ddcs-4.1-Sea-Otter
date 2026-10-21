"""Step 1, called by run_me_first.bat.

Configure the network, confirm the controller answers, then over its own SYSDISK file
share: back up the vendor app (ddcsv4.out -> ddcsv4.out.original), write a small config
file, and put the temporary app in place of ddcsv4.out. Nothing is deleted - the vendor
app is only renamed, and step3 (reset_original_app.bat) puts it back.

The controller runs whatever is named ddcsv4.out at boot; this is the vendor's own
firmware-update path (a USB/SMB file swap), used here to load the temporary app.
"""
import hashlib
import io
import os
import secrets
import socket
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
sys.path.insert(0, HERE)

SHARE = "SYSDISK"
APP = "ddcsv4.out"
ORIGINAL = "ddcsv4.out.original"
STAGING = "ddcsv4.out.staging"
TEMP_APP = os.path.join(TOP, "DDCS4.1_temporary_app")
STATE = os.path.join(TOP, "connection.txt")


def sha(b):
    return hashlib.sha256(b).hexdigest()


def ping(ip):
    r = subprocess.run(["ping", "-n", "1", "-w", "1500", ip], capture_output=True, text=True)
    return r.returncode == 0 and "TTL=" in r.stdout.upper()


def connect(ip):
    from smb.SMBConnection import SMBConnection
    c = SMBConnection("", "", "DDCS-CONNECT", "*SMBSERVER", use_ntlm_v2=False, is_direct_tcp=True)
    if not c.connect(ip, 445, timeout=10):
        raise OSError("file share did not accept the connection")
    return c


def read(c, name):
    b = io.BytesIO()
    c.retrieveFile(SHARE, "/" + name, b)
    return b.getvalue()


def write(c, name, data):
    c.storeFile(SHARE, "/" + name, io.BytesIO(data))
    if sha(read(c, name)) != sha(data):
        raise OSError(f"{name} did not read back correctly after writing")


def write_into_place(c, name, data):
    """Write even if `name` is a running binary: upload to a side name, move any existing
    file aside, then rename the new one into place (a running process keeps the old inode)."""
    c.storeFile(SHARE, "/" + name + ".new", io.BytesIO(data))
    if sha(read(c, name + ".new")) != sha(data):
        raise OSError(f"{name}: staged copy did not read back correctly")
    have = {f.filename for f in c.listPath(SHARE, "/")}
    if name in have:
        if name + ".old" in have:
            c.deleteFiles(SHARE, "/" + name + ".old")
        c.rename(SHARE, "/" + name, "/" + name + ".old")
    c.rename(SHARE, "/" + name + ".new", "/" + name)


def names(c):
    return {f.filename for f in c.listPath(SHARE, "/")}


def chmod_x(ip, name):
    """Set 0755 via the CIFS Unix extensions (pysmb can't). Returns the mode, or None on
    failure - Samba's default create mask usually keeps the x bit anyway."""
    try:
        from smb_unix_ext import RawSMB1, chmod_path, stat_unix
        u = RawSMB1(ip)
        u.session_setup_guest()
        u.tree_connect(SHARE, ip)
        chmod_path(u, name, 0o755)
        mode = stat_unix(u, name)["Permissions"] & 0o777
        u.close()
        return mode
    except Exception as e:
        print(f"  (could not set permissions on {name}: {e})")
        return None


def main():
    print("\n  === Step 1: connect and load the temporary app ===\n")
    print("  On the DDCS V4.1 itself, in its network settings menu:")
    print("    - Disable network functionality  -> No")
    print("    - Obtain IP address automatically -> No")
    print("    - Local IP address   -> a free address, e.g. 192.168.1.200")
    print("    - Net mask           -> 255.255.255.0")
    print("    - Router IP address  -> your router, e.g. 192.168.1.1")
    print("  Save and restart the controller once so the address takes effect.\n")
    print("  Connect the controller to the same router/switch as this PC (or straight to")
    print("  the PC with a cable; then give the PC a fixed address in the same range).\n")

    ip = input("  IP address you set on the controller [192.168.1.200]: ").strip() or "192.168.1.200"
    mask = input("  Net mask [255.255.255.0]: ").strip() or "255.255.255.0"
    gw = input("  Router IP (blank if the PC is wired straight to it) [192.168.1.1]: ").strip()

    while True:
        input("\n  Press Enter to test the connection...")
        if ping(ip):
            print(f"  OK - {ip} answers.")
            break
        print(f"\n  No answer from {ip}.")
        print("    - is the controller on, booted, and cabled to the same network?")
        print("    - does the address above match what you set on the controller?")
        print("    - direct cable: is this PC set to a fixed address in the same range?")

    print("\n  Opening the controller's file share...")
    c = connect(ip)

    token = secrets.token_hex(16)  # 32 hex chars; the temporary app wants this before a shell
    conf = f"IP={ip}\nMASK={mask}\nGW={gw}\nTOKEN={token}\n"
    write(c, "connecting.conf", conf.encode())

    # the SSH server binary, so step2 can install it from the controller's own disk
    dbm = open(os.path.join(HERE, "dropbearmulti"), "rb").read()
    write_into_place(c, "dropbearmulti", dbm)
    chmod_x(ip, "dropbearmulti")
    print(f"  Copied the SSH server onto the controller ({len(dbm)} bytes).")

    # back up the current app, then put the temporary app in its place - done LAST, so an
    # earlier failure never leaves the controller without an app to boot
    have = names(c)
    if ORIGINAL in have:
        print(f"  The original app is already backed up as {ORIGINAL} (an earlier run).")
    elif APP in have:
        data = read(c, APP)
        os.makedirs(os.path.join(TOP, "backup"), exist_ok=True)
        open(os.path.join(TOP, "backup", f"{APP}.{sha(data)[:8]}"), "wb").write(data)
        c.rename(SHARE, "/" + APP, "/" + ORIGINAL)
        print(f"  Backed up the original app: {APP} -> {ORIGINAL} (and to connecting/backup/).")
    else:
        print(f"  Note: no {APP} found on the share - continuing.")

    payload = open(TEMP_APP, "rb").read()
    write(c, APP, payload)
    print(f"  Placed the temporary app as {APP} ({len(payload)} bytes).")
    mode = chmod_x(ip, APP)
    if mode is not None:
        print(f"  Permissions on {APP}: 0o{mode:o}" + ("" if mode & 0o111 else "  (no x bit - see Readme)"))

    c.close()
    open(STATE, "w").write(f"ip={ip}\ntoken={token}\n")
    print("\n  Done. Now:")
    print("    1. Restart (power-cycle) the controller.")
    print("    2. Wait ~20 seconds for it to boot.")
    print("    3. Run  run_me_after_reboot.bat\n")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"\n  FAILED: {e}\n")
        sys.exit(1)
