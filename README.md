# Sea Otter — open controller software for the DDCS V4.1

Sea Otter is a complete, from-scratch replacement for the software on the **DDCS V4.1**
CNC motion controller (the Digital Dream 4-axis stand-alone controller; Freescale i.MX28 /
ARM926, 1024×600 LCD, 18-key panel, MPG pendant). It does everything the original does and
more, and installs from a **USB stick** in one step — swap it in, swap the original back
whenever you like.

It is one small C program (a static ARMv5 binary, ~1.4 MB) that provides:

- **G-code**: G0–G3 in all planes, drilling cycles (G81/G82/G83/G73/G85/G89), work offsets
  G54–G59, multi-part jobs, tool changes, a full parse-and-check on load.
- **Motion**: lookahead planning with S-curve acceleration, per-axis limits, a controlled
  (no-lost-steps) stop on Reset / E-stop / limit.
- **Setup & safety**: homing, touch-off, soft and hard limits, feed/spindle overrides to
  300 %, TryCut and Simulate dry runs, resume-from-line.
- **The panel**: the whole UI on the LCD with the 18 keys, LEDs and the optional MPG
  pendant, plus a toolpath view with a CPU-saver mode.
- **Network**: a browser panel and a full HTTP API on port 8080.

The same binary also runs on a PC against a built-in FPGA **simulator**, with the panel in a
browser — so you can try the whole system, and develop for it, without the hardware.

> [!WARNING]
> **Not yet tested on a machine with drives connected.** Sea Otter has been run on a real
> DDCS V4.1 controller — step-exact on its FPGA with real CAM programs up to 64 000 lines,
> 0 lost steps, limit inputs and controlled stops exercised through the real input path —
> but **never with stepper/servo drives, motors or real limit/home switches attached.**
> Do the first tests with the motors unpowered or free to move, keep a hand on the E-stop,
> and see the disclaimer at the end. Details: [docs/fpga-motion.md](docs/fpga-motion.md).

## Install it (USB stick — nothing else needed)

The controller copies files from a USB stick into place at boot and runs whatever is named
`ddcsv4.out`. Sea Otter uses exactly that path, so installing is just:

1. Download the USB package from the
   [Releases](https://github.com/JSFrouws/ddcs-4.1-Sea-Otter/releases) page (built and tested
   by GitHub Actions from this repository, with a SHA-256 checksum), or build it yourself.
   It is laid out like the vendor's own update package: unzip it and copy the inner
   **`ddcsv4`** folder to the root of an empty FAT32 stick.
2. Set the controller's IP address in `network.conf` on the stick.
3. Power off, plug in, power on. After ~15 s Sea Otter is running; the browser panel and the
   API answer at `http://<that IP>:8080/`.
4. Remove the stick.

No cabling into the board, no shell, no extra tools — a USB stick is the whole install, and
the network/API come up on their own. This is the controller's own update mechanism: the
vendor's V4.1 manual (section 3.4.3) updates the software the same way, with a folder named
`ddcsv4` on a FAT32 stick, and the only file whose name is fixed is `ddcsv4.out`.

**→ [docs/usb-install.md](docs/usb-install.md)**

## Going back to the original software

Same route, with the vendor's package:

1. Download the newest DDCS V4.1 software from Digital Dream:
   **[www.ddcnc.com › Downloads › Software & firmware](http://www.ddcnc.com/?m=home&c=Lists&a=index&tid=106&lang=en)** — the newest V4.1 release at
   the time of writing is [DDCS V4.1 (2025-04-04) software](http://www.ddcnc.com/?m=home&c=View&a=index&aid=229&lang=en), file
   `ddcsv4(2025-04-04).rar`. (Use `http://www.ddcnc.com`; the `https://` address had an
   expired certificate in October 2026.)
2. Unpack it and copy the inner **`ddcsv4`** folder to the root of an empty FAT32 stick.
3. Controller off for 10 s, insert the stick, power on, wait ~30 s.
4. Remove the stick.

The vendor's `ddcsv4.out` replaces Sea Otter's start script and the controller runs the
original software again; its machine parameters are never touched by either install. Details
and checksums: [docs/usb-install.md › Restoring the original software](docs/usb-install.md#restoring-the-original-software).

## Read this first

| You want to… | Go to |
|---|---|
| **Use the machine** | [docs/user-manual.md](docs/user-manual.md) · [PDF](docs/user-manual.pdf) |
| **Install / update / revert** | [docs/usb-install.md](docs/usb-install.md) |
| **Drive it over the network** | [docs/api-reference.md](docs/api-reference.md) |
| **Get a Linux login on the controller** (development) | [connecting/](connecting/Readme.md) |
| **Understand how it works** | [docs/app-architecture.md](docs/app-architecture.md), [docs/motion-planner.md](docs/motion-planner.md), [docs/fpga-motion.md](docs/fpga-motion.md) |
| **See everything learned about the hardware** | [docs/README.md](docs/README.md) |

## Build it

In WSL or Linux, with `gcc`, `make`, `python3`, and for the controller
`gcc-arm-linux-gnueabi` (+ `qemu-user` to run the ARM tests):

```sh
cd app
make test                             # ~900 checks (G-code, motion vs simulated FPGA, UI, API)
make arm                              # build/arm/seaotter   (the controller binary)
make host                             # build/host/seaotter  (PC + simulator)
sh tools/ssh/build-dropbear.sh        # the SSH server for the USB package (once)
sh install/make-usb.sh                # assemble the USB stick folder -> build/usb/ddcsv4/
```

Run it on a PC (no hardware):

```sh
./build/host/seaotter --programs programs --data build/run --port 8090
# then open http://localhost:8090/
```

More in [app/README.md](app/README.md). Every push runs the same steps on GitHub Actions
([.github/workflows/build.yml](.github/workflows/build.yml)): native and ARM tests, the
binary and the USB package (kept as a workflow artifact). Pushing a tag `v*` publishes that
package as a Release.

## Repository layout

```
app/          the controller software (C), its tests, the web panel, and the USB installer
docs/         user manual, API reference, install guide, and the hardware reverse-engineering notes
connecting/   one-time helper to get a root SSH login on the controller (development only)
```

## How Sea Otter was built

The DDCS V4.1 is a closed device: Sea Otter was written from scratch by observing the
hardware directly — decoding the FPGA's motion registers, the TM1638 keypad, the MPG
pendant and the I/O by watching what the original firmware does and measuring the real
controller. Those findings, and the methods, are written up in [docs/](docs/README.md).
None of the vendor's code is used or included here.

The idea of an open replacement for these Digital Dream controllers is not new — the
[Pandora-CNC](https://github.com/Pandora-CNC) project did pioneering work for the older
**DDCSV1.1** (a different SoC), and was an inspiration for taking on the V4.1. Sea Otter is
an independent system for the V4.1, not a port of it.

## License

Sea Otter is released under the **MIT License** — free to use, copy, modify and
distribute, including commercially, as long as the copyright notice is kept. See
[LICENSE](LICENSE).

Included third-party parts keep their own licenses: dropbear (SSH server, MIT-style),
the Barlow, JetBrains Mono and DejaVu fonts (SIL OFL / DejaVu license, see
`app/tools/fonts/`), and `motiondev.ko`, Digital Dream's FPGA driver from their public
DDCS V4.1 2025-04-04 software release, included unmodified so Sea Otter always runs on the
driver it was tested with. The module declares itself GPL; its source has not been published
by its maker, so it is not available from this project either.

## Disclaimer — no liability

A CNC machine can cause serious injury and damage. This software controls motion,
spindles and outputs and **is provided "as is", without warranty of any kind**. It is
experimental, not certified for any safety function, and has not been tested on a machine
with drives connected (see the notice at the top). You use it entirely at your own risk:
the authors and contributors accept **no liability** for any injury, damage, loss of data,
broken tools, scrapped work or any other consequence of using it, or of installing it on
your controller. Keep a working, hard-wired E-stop that cuts the drives independently of
this software, and keep the original firmware so you can always go back.

This project is not affiliated with or endorsed by the maker of the DDCS controllers.
