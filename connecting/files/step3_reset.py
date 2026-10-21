"""Step 3, called by reset_original_app.bat.

Puts the original controller application back: over the SYSDISK file share it renames
ddcsv4.out.original (the backup step 1 made) back to ddcsv4.out, so the next boot runs the
vendor software again instead of the temporary app. The SSH server stays installed.

This works at any point and leaves the controller exactly as it was, except SSH is now
present. Run it, then power-cycle the controller.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
STATE = os.path.join(TOP, "connection.txt")
SHARE = "SYSDISK"
APP = "ddcsv4.out"
ORIGINAL = "ddcsv4.out.original"


def ip_from_state():
    if os.path.exists(STATE):
        for line in open(STATE):
            if line.startswith("ip="):
                return line.strip().split("=", 1)[1]
    return input("  Controller IP address [192.168.1.200]: ").strip() or "192.168.1.200"


def main():
    from smb.SMBConnection import SMBConnection
    ip = ip_from_state()
    print(f"\n  === Step 3: restore the original app on {ip} ===\n")
    c = SMBConnection("", "", "DDCS-CONNECT", "*SMBSERVER", use_ntlm_v2=False, is_direct_tcp=True)
    if not c.connect(ip, 445, timeout=10):
        sys.exit("  Could not reach the controller's file share.")
    have = {f.filename for f in c.listPath(SHARE, "/")}

    if ORIGINAL not in have:
        print(f"  No backup ({ORIGINAL}) found on the controller.")
        print("  If you still have connecting/backup/ddcsv4.out.*, copy it to the SYSDISK")
        print("  share as ddcsv4.out by hand. Otherwise fetch the vendor firmware and")
        print("  restore ddcsv4.out from it (see Readme.md).")
        sys.exit(1)

    if APP in have:
        c.rename(SHARE, "/" + APP, "/" + APP + ".temporary")  # keep the temp app aside
    c.rename(SHARE, "/" + ORIGINAL, "/" + APP)
    c.close()
    print(f"  Restored: {ORIGINAL} -> {APP}.")
    print("\n  Power-cycle the controller. It will boot the original application again,")
    print("  with SSH still available.\n")


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception as e:
        print(f"\n  FAILED: {e}\n")
        sys.exit(1)
