# SSH into a DDCS V4.1 with a user name and password

How to get a root shell over the network on a stock DDCS V4.1, with a normal password
login (`ssh root@<ip>`, type the password). Key pairs work too, but aren't needed on a
local network: add them yourself if you want them (end of this page).

**Tested 2026-10-18** on the desk unit: password login on port 22, both with Pandora and
with the vendor app running.

## Why it takes a custom SSH server

The controller has no SSH server, and it has no compiler. BusyBox on it has `passwd` /
`chpasswd`, and `/etc/shadow` holds a vendor-set root password (an old DES hash; nobody
knows the password). So the job is:

1. get an SSH server binary onto the controller that runs on its old Linux (2.6.35, ARMv5),
2. start it at boot, and
3. give `root` a password you know.

The server is **dropbear 2024.86**, built static for ARMv5 with password login enabled.
The cross toolchain has no `crypt()` for a static build, so it brings its own
([../app/tools/ssh/md5crypt.c](../app/tools/ssh/md5crypt.c), checked against
`openssl passwd -1`). It verifies `$1$` (MD5-crypt) passwords, the format the controller's
`chpasswd -m` writes. The old vendor DES hash never matches, so set a password first.

## The easy way: a USB stick (recommended)

Everything above is done by the USB install package: [usb-install.md](usb-install.md).

- **With Pandora:** install as described there, with `ssh-password.txt` holding your
  password.
- **Keeping the vendor app:** the same package plus `ddcsv4.vendor` (the vendor's original
  `ddcsv4.out`) and an empty `USE_VENDOR` file. See *SSH only* in usb-install.md.

Then:

```sh
ssh root@192.168.1.200
```

(Use the controller's IP. Windows 10/11 has `ssh` built in: PowerShell or cmd.)
The first connection asks you to confirm the host key: answer `yes`.

To change the password later, log in and run **`passwd -a m`** (or
`echo 'root:newpassword' | chpasswd -m`). The `m` matters: it makes an MD5 hash, the only
kind this dropbear can check. A plain `passwd` may write an old DES hash, and then password
login stops working until you set it again with `-a m` (log in with a key, or use the USB
stick's `ssh-password.txt`).

## By hand, over the network (how it was first done)

For a controller you can already reach some other way: SMB write access to `SYSDISK`, or
an existing shell. The first unit was opened up through a temporary `ddcsv4.out` swap
([shell-access-live-test.md](shell-access-live-test.md)). The USB route above does the same
without that risk. The manual steps, once you have a shell:

```sh
# 1. copy dropbearmulti to /ddcsv4/ (SMB: \\<ip>\SYSDISK, or a USB stick), then:
cd /ddcsv4
chmod +x dropbearmulti
ln -sf dropbearmulti dropbear
ln -sf dropbearmulti dropbearkey
./dropbearkey -t rsa -f dropbear_rsa_host_key
./dropbearkey -t ed25519 -f dropbear_ed25519_host_key

# 2. dropbear refuses logins when /root isn't owned by root (it isn't, on this firmware).
#    BusyBox chown only takes numbers here:
chown 0:0 /root && chmod 755 /root

# 3. a password dropbear can check (MD5-crypt):
echo 'root:your-password' | chpasswd -m

# 4. start it now, and at every boot:
./dropbear -p 22 -r /ddcsv4/dropbear_rsa_host_key -r /ddcsv4/dropbear_ed25519_host_key
cp /etc/init.d/rcS /etc/init.d/rcS.orig
echo '/ddcsv4/dropbear -p 22 -r /ddcsv4/dropbear_rsa_host_key -r /ddcsv4/dropbear_ed25519_host_key' >> /etc/init.d/rcS
```

Use `echo >>`, not a heredoc, when you type this over a remote command line (a heredoc
left a stray `EOF` line in rcS once).

## Building dropbearmulti yourself

In WSL / Linux with `gcc-arm-linux-gnueabi`:

```sh
sh app/tools/ssh/build-dropbear.sh      # -> app/tools/ssh/out/dropbearmulti (~1 MB, static)
```

The script downloads dropbear 2024.86, enables password login, links `md5crypt.c` in as
`crypt()`, and removes the `-pie` that dropbear's configure forces into the link line
(required for this controller, see [ssh-access.md](ssh-access.md)). Background notes: [ssh-access.md](ssh-access.md).

## Copying files over SSH

The controller has no `scp` / `sftp`. Copy with `cat`:

```sh
ssh root@192.168.1.200 "cat > /ddcsv4/pandora.new" < pandora
ssh root@192.168.1.200 "chmod +x /ddcsv4/pandora.new && mv /ddcsv4/pandora.new /ddcsv4/pandora"
```

A running program can't be overwritten (`Text file busy`): copy to a new name, then `mv`,
or stop the program first (`killall pandora`).

## Optional: key login

```sh
ssh-keygen -t ed25519 -f ~/.ssh/ddcs                    # on the PC
ssh root@<ip> "mkdir -p /root/.ssh && cat >> /root/.ssh/authorized_keys" < ~/.ssh/ddcs.pub
ssh -i ~/.ssh/ddcs root@<ip>
```

## Security

This is a **root login on your local network**. It's meant for a workshop LAN, not the
internet. Choose a real password, don't forward port 22 on your router, and remember that
the Pandora API on port 8080 has no password at all (it can be switched off on the panel).
