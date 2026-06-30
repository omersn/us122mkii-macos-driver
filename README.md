# TASCAM US-122MKII — Unofficial macOS Driver

An unofficial, from-scratch audio driver that brings the **TASCAM US-122MKII** USB
audio interface back to life on modern macOS. TASCAM discontinued this device and
never released a driver for current versions of macOS, so on a recent Mac it is
otherwise just a paperweight. This project makes it work again, reverse-engineered
from the USB protocol up.

**Status:** working and stable. As of **V3**, the transport uses Apple's
low-latency isochronous USB API, which **cured the load-induced jitter** of
earlier versions: audio now stays glitch-free even under heavy system load. Plays
and records cleanly, crash-free. Built and tested on **macOS 13.7.8 Ventura
(Intel)**.

---

> [!IMPORTANT]
> **Not every US-122MKII will work with this driver.** The US-122MKII shipped in
> more than one hardware revision. Some units boot straight into a ready state and
> work with this driver as-is. Other, **FPGA-based revisions come up "cold" and
> require the host driver to upload firmware to the device every time it is
> plugged in**, before the audio interface even appears to the computer.
>
> **This driver does not include that firmware upload.** It only supports units
> that already present themselves as a ready audio device on their own. If your
> unit is a firmware-upload revision, it will not work with this driver (yet).

### How to tell whether your unit is supported

Plug the interface into your Mac and run this in Terminal:

```bash
system_profiler SPUSBDataType | grep -B2 -A8 -i tascam
```

- **Supported:** the device appears with **Vendor ID `0x0644`** and
  **Product ID `0x8021`**. That means its firmware is already resident and the
  device is ready, which is all this driver needs.
- **Not supported (needs firmware):** the device shows a different product ID,
  appears under a different vendor (the bare USB controller), shows up only for a
  moment, or does not appear at all. Those are the cold, firmware-upload revisions
  this driver cannot initialize.

## Why this project exists

TASCAM abandoned the US-122MKII years ago. There is no official driver for any
current macOS release, and the device uses a vendor-specific USB class, so macOS
has no built-in support to fall back on. The hardware itself is still perfectly
good. This driver makes a discontinued, otherwise-unusable interface useful again.

## How it works

The driver is three small cooperating pieces:

- **`us122d`** — a background daemon that owns the USB device and runs the
  real-time audio transfer engine.
- **US122.driver** — a Core Audio plugin that presents the interface to macOS, so
  it shows up in Sound settings and in any audio application.
- **US122Menu** — a menu-bar app to switch the driver on and off, choose the
  buffer size, and see device status at a glance.

The pieces talk to each other through shared memory. The full design is in
[`docs/01-architecture.md`](docs/01-architecture.md).

## Status and known limitations

- **Works:** clean full-duplex 24-bit audio in and out, survives plug/unplug, no
  crashes.
- **Load-induced jitter: fixed in V3.** Earlier versions could skip under heavy
  system load (a USB delivery-timing problem, not a buffer shortage). V3's
  low-latency transport updates the USB frame list at primary interrupt time, so
  it holds steady under load: worst-case completion gaps dropped from ~100-400 ms
  to a few milliseconds, measured under the same load that used to skip. See
  [`docs/07-stage0-findings-and-stage1-plan.md`](docs/07-stage0-findings-and-stage1-plan.md).
- **Audio only:** the device's MIDI ports are not handled by this driver.
- **Tested on:** macOS 13.7.8 Ventura on Intel. Other macOS versions and Apple
  Silicon Macs are untested.

## Building and installing

**Easiest, one command** (clone the repo, then):

```bash
./install.sh
```

The guided installer checks your Mac and device, builds the daemon + plugin +
menu app, installs them (asking once for your password), and verifies the
low-latency driver is live. Remove it any time with `./install.sh --uninstall`.
V3 needs only the Xcode Command Line Tools, **no Homebrew or libusb**.

For the manual step-by-step (or to understand what the installer does), see
**[INSTALL.md](INSTALL.md)**.

## Repository layout

```
docs/        design and protocol documentation (start at docs/00-START-HERE.md)
src/         the driver: daemon, Core Audio plugin, menu app, build scripts
reference/   the Linux ALSA reference driver (protocol source of truth)
             plus an abandoned single-process prototype kept for study
tools/       small standalone USB probes (descriptor dump, raw tone/wav playback)
```

## Credits and license

The USB protocol was cross-checked against **serifpersia's Linux ALSA driver**
(included under [`reference/linux-reference/`](reference/linux-reference/)), which
is GPL-2.0. Thanks to that project for documenting the hardware.

Because this repository bundles and derives from GPL-2.0 reference code, the
project's own license is being finalized as a GPL-compatible license. Until that
is committed, treat the reference material as GPL-2.0.

## Disclaimer

This is an **unofficial** project and is not affiliated with, authorized by, or
endorsed by TASCAM or TEAC. The US-122MKII is a discontinued product. This driver
is reverse-engineered and provided as-is, with no warranty. Use it at your own
risk.

---

*Working on the code? Start with [`docs/00-START-HERE.md`](docs/00-START-HERE.md)
for a full orientation, and see [`CLAUDE.md`](CLAUDE.md) for the engineering
ground rules.*
