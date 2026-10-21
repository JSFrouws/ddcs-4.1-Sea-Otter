"""Step 2, called by run_me_after_reboot.bat (after the controller has rebooted into the
temporary app).

It connects to the temporary app over TCP 7777 (sending the one-time token from step 1),
then runs a short sequence of standard Linux commands over that connection to set up the
SSH server (dropbear) so you can log in as root afterwards. It then wires dropbear into the
boot script so it starts on every boot, independent of which main app is running.

Login made deliberately easy (see Readme.md):
  - a fresh SSH key is generated on THIS PC and installed, so `ssh -i <key> root@<ip>`
    needs no password, and
  - a known root password is set as a fallback, so `ssh root@<ip>` + that password works.
Both are intentionally wide open. ADDING PROTECTION AFTERWARDS IS YOUR RESPONSIBILITY:
change or remove the password, replace the key with your own, and don't expose the
controller to an untrusted network. This folder only gives you a way in to configure the
Linux side; it does not secure it for you.
"""
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
STATE = os.path.join(TOP, "connection.txt")
KEY = os.path.join(TOP, "ssh_key")
PASSWORD = "seaotter"   # fallback root password; change it after you are in (see Readme)
PORT = 7777


def read_state():
    d = {}
    for line in open(STATE):
        if "=" in line:
            k, v = line.strip().split("=", 1)
            d[k] = v
    return d["ip"], d["token"]


def make_key():
    """Fresh ed25519 key on this PC (Windows ships ssh-keygen). Returns the public key."""
    if not os.path.exists(KEY):
        subprocess.run(["ssh-keygen", "-t", "ed25519", "-N", "", "-C", "ddcs-connect", "-f", KEY],
                       check=True, capture_output=True)
    return open(KEY + ".pub").read().strip()


class Shell:
    """Line-oriented command channel to the temporary app's /bin/sh on the socket."""

    def __init__(self, ip, token):
        self.s = socket.create_connection((ip, PORT), timeout=10)
        self.s.settimeout(20)
        self.s.sendall(token.encode())   # the 32-character token, as written in connecting.conf
        time.sleep(0.3)

    def run(self, cmd):
        tag = "____done____"
        self.s.sendall((cmd + f'\necho {tag} $?\n').encode())
        out = b""
        while tag.encode() not in out:
            try:
                chunk = self.s.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            out += chunk
        text = out.decode("utf-8", "replace")
        return text.split(tag)[0].strip()

    def close(self):
        try:
            self.s.sendall(b"exit\n")
            self.s.close()
        except OSError:
            pass


def main():
    ip, token = read_state()
    pub = make_key()
    print(f"\n  === Step 2: install SSH on {ip} ===\n")
    print("  Connecting to the temporary app...")
    sh = Shell(ip, token)

    who = sh.run("id -u; uname -s")
    if "0" not in who.split("\n")[0]:
        print(f"  Unexpected response: {who!r}")
        print("  Is the controller rebooted and showing nothing special? Retry, or re-run step 1.")
        sys.exit(1)
    print("  Connected (root shell on the controller).")

    # dropbear from the copy step 1 placed on the disk
    cmds = [
        "cd /ddcsv4",
        "chmod +x /ddcsv4/dropbearmulti",
        "ln -sf dropbearmulti /ddcsv4/dropbear",
        "ln -sf dropbearmulti /ddcsv4/dropbearkey",
        "[ -f /ddcsv4/dropbear_rsa_host_key ] || /ddcsv4/dropbearkey -t rsa -f /ddcsv4/dropbear_rsa_host_key",
        "[ -f /ddcsv4/dropbear_ed25519_host_key ] || /ddcsv4/dropbearkey -t ed25519 -f /ddcsv4/dropbear_ed25519_host_key",
        # dropbear refuses logins unless /root is owned by root and not group/other writable
        "chown 0:0 /root 2>/dev/null; chmod 755 /root",
        "mkdir -p /root/.ssh; chmod 700 /root/.ssh",
    ]
    for c in cmds:
        sh.run(c)

    # install the PC-generated public key, and a fallback password
    sh.run(f"printf '%s\\n' '{pub}' >> /root/.ssh/authorized_keys; chmod 600 /root/.ssh/authorized_keys")
    sh.run(f"echo 'root:{PASSWORD}' | chpasswd -m")

    # start dropbear now, and add it to the boot script so it survives reboots and app swaps
    start = "/ddcsv4/dropbear -p 22 -r /ddcsv4/dropbear_rsa_host_key -r /ddcsv4/dropbear_ed25519_host_key"
    sh.run(f"grep -q dropbear /etc/init.d/rcS || (cp /etc/init.d/rcS /etc/init.d/rcS.before_ssh; echo '{start}' >> /etc/init.d/rcS)")
    sh.run(start)
    running = sh.run("sleep 1; ps | grep -v grep | grep dropbear | head -1")
    sh.close()

    ok = "dropbear" in running
    print("\n  SSH server installed and " + ("started." if ok else "added to boot (not confirmed running)."))
    print("\n  Log in with EITHER:")
    print(f"     ssh -i \"{KEY}\" root@{ip}         (no password)")
    print(f"     ssh root@{ip}                        (password: {PASSWORD})")
    print("\n  >> This login is wide open on purpose. Securing it is YOUR responsibility:")
    print("     change the password (passwd), remove it, or switch to key-only, and keep")
    print("     the controller off untrusted networks.")
    print("\n  Finally, restore the original controller app:")
    print("     run  reset_original_app.bat   then power-cycle the controller.\n")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"\n  FAILED: {e}")
        print("  Make sure the controller was power-cycled after step 1 and has finished booting.\n")
        sys.exit(1)
