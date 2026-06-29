# US-122MKII macOS Driver

An unofficial, from-scratch audio driver for the **TASCAM US-122MKII** USB audio
interface on **macOS 10.13 High Sierra (Intel)**. TASCAM abandoned this device;
no driver exists for modern macOS. This one is reverse-engineered from the USB
protocol up.

**Status: working.** Audio plays and records cleanly. The driver is stable and
crash-free. The active work is curing a load-induced jitter by moving the
isochronous USB transport to Apple's low-latency IOUSBLib API.

## If you are Claude Code picking this up: read `docs/00-START-HERE.md` first.

It is a five-minute orientation and it links the rest in order. The single most
important thing to absorb before writing code is in `docs/02-history-v1-v2.md`:
this project has an abandoned V2 whose suspected failure cause is the same API
the planned improvement adopts. Understand that tension or repeat it.

## Layout

```
docs/                  read these in numbered order (start at 00)
  00-START-HERE.md       orientation + working constraints
  01-architecture.md     full system layout (the shipping V1)
  02-history-v1-v2.md    the V1/V2 saga, where V2 failed, the core irony
  03-current-state.md    what works now, last session's work, the open problem
  04-roadmap-lowlatency-isoc.md   the staged plan for the work ahead
  05-protocol-facts.md   hardware-proven constants (ground truth)
  06-jitter-research.md  external research that diagnosed the jitter + the fix

src/                   the shipping V1 source
  daemon/                us122d.c + shmring.h  (libusb USB engine + shm contract)
  plugin/                US122.driver/...      (Core Audio HAL plugin)
  menu/                  US122Menu/...         (Swift menu-bar control app)
  plist/                 net.senesh.us122d.plist (launchd config)
  build/                 build_*.sh            (build + install, run on the Mac)

reference/
  v2-abandoned/          the FULL V2 source (study its low-latency iso code)
  linux-reference/       serifpersia's ALSA driver (protocol source of truth)

tools/                 standalone USB probes (descriptor dump, raw playback)
```

## The hardware-loop constraint

The device is on the author's physical Mac. This repo's owner builds and tests
every change there, then feeds logs back. A Claude Code instance working on this
**cannot test IOKit, CoreAudio, libusb, or USB locally**; it compiles only the
portable C core against stubs. The whole method is measure-first, one-change-per-
build. See `docs/00-START-HERE.md` for the full working style.

## Build (on the Mac)

The scripts in `src/build/` build the bundle, daemon, and menu app and print the
sudo install commands. They expect libusb from Homebrew. Full install sequences
are in `docs/03-current-state.md`. Note: after extracting any tarball, the build
scripts need `chmod +x` (the execute bit does not survive tarring).

## License

The Linux reference under `reference/linux-reference/` is GPL (serifpersia/
us144mkii). This driver's own code carries the license in the repo root if/when
published; treat any libusb-derived or reference-derived portions accordingly.
