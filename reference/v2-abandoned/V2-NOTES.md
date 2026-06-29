# V2-NOTES.md — TASCAM US-122MKII driver, V2 architecture

## What V2 is

A single-process AudioServerPlugIn that owns the USB device directly via IOKit
and runs the isochronous engine on a dedicated realtime thread inside the
plugin. One process, one clock (the device), no shared memory, no resampler,
no helper daemon.

This is a ground-up rearchitecture of V1. V1 worked (clean audio, load-tolerant
after much tuning) but its whole hard middle — clock drift, the resampler, the
cross-process priority fight — came from splitting the USB engine (daemon) from
the Core Audio plugin and bridging them over shared memory with two independent
clocks. V2 deletes that split. The protocol facts are carried over verbatim
from V1, which were proven on hardware.

## The one big risk, stated plainly

**V2 does raw IOKit USB access from inside `coreaudiod`.** `coreaudiod` is a
sandboxed system daemon. It is genuinely uncertain whether macOS (High Sierra,
10.13) will allow a HAL plugin loaded into `coreaudiod` to open a USB device
via `IOUSBLib`. Three possible outcomes on first hardware test:

1. **It works.** The plugin opens the device, streams, and you have the V2
   benefit (one clock, no resampler, simpler, likely more stable). 

2. **USB open is denied** (e.g. `USBDeviceOpen` returns an access/permission
   error). The engine logs the failure and the device reports not-alive. If
   this happens, V2-in-coreaudiod is blocked by the sandbox, and the fallback
   is a hybrid: keep V1's separate-process USB engine (which CAN open the
   device, proven) but feed V2's cleaner single-clock design by having the
   plugin still own timing. That is a real fork we would design if we hit it.

3. **It works but is unstable** under the sandbox (e.g. the runloop source
   doesn't get serviced reliably inside coreaudiod). Diagnosable from the logs.

I want this risk visible up front because it is the thing most likely to make
or break V2, and it cannot be tested anywhere but on your Mac. V1 sidestepped it
entirely by doing USB in a normal root process (the daemon). That was, in
hindsight, V1's one quietly correct instinct.

## What is proven vs. what needs hardware validation

**Proven (carried from V1, unchanged):**
- All protocol facts: VID/PID 0x0644/0x8021, interface 1 / alt 1, EP 0x02 OUT
  and 0x81 IN, 24-bit high-3-of-32 packing, 6 bytes/frame, the rate-set control
  request bytes, the 8000-microframe pacing, duplex coupling.
- The Core Audio object model (device enumeration, streams, formats, rate
  setting) — it appeared and worked in V1.
- The realtime thread policy and the deep-buffer approach.
- The in-process ring data path (unit-tested in this build: `v2_ring_test`).

**Needs on-hardware validation (marked `[VALIDATE]` in the code):**
- That `coreaudiod` may open the device at all (the big risk above).
- The IOKit isochronous transfer path: `LowLatencyCreateBuffer`,
  `LowLatency{Read,Write}IsochPipeAsync`, the frame-list `frReqCount`/
  `frActCount` semantics, and scheduling on future bus frame numbers. This is
  the IOKit equivalent of V1's libusb iso path; the shape follows Apple's USB
  sample code but the exact frame timing must be confirmed by ear and log.
- The pipe-index resolution (`find_pipe`): IOKit numbers pipes 1..N in
  descriptor order; we map EP 0x02/0x81 by direction+number. Confirm against
  the real descriptors (V1's `us122_descdump` output is the reference).
- The rate-set `DeviceRequest` recipient. V1 sent it via libusb control on the
  device handle; V2 uses `(*dev)->DeviceRequest`. If rate setting fails, this is
  the first place to look.
- Tight iso packing: V1 discovered packets must be laid contiguously with only
  lengths varying. V2's `fill_playback_xfer` does the same (contiguous bytes,
  per-packet `frReqCount`). Confirm no "grit/no fundamental" artifact.

## Build and install

    ./V2-build.sh
    sudo cp -R V2-US122.driver /Library/Audio/Plug-Ins/HAL/
    sudo killall coreaudiod

The device appears as **US-122MKII (V2)** (distinct name so it can coexist with
the V1 install during comparison). Select it and play audio.

## What to watch in the logs

    log show --last 2m --predicate 'eventMessage CONTAINS "V2 US122"'
    log show --last 2m --predicate 'eventMessage CONTAINS "V2 us122 engine"'

Key lines:
- `device present` / `device absent` — presence tracking (drives the device
  appearing/disappearing in the list on plug/unplug).
- `streaming at 48000 Hz` — the engine opened the device and started iso.
- If you see no `streaming` line and the device shows not-alive, that is most
  likely the coreaudiod USB-access denial (outcome 2 above).

## Disconnect-from-list feature (the thing we planned for today)

Implemented in V2 from the start: `kAudioDevicePropertyDeviceIsAlive` reflects
the engine's live presence, and `on_device_state` posts a property-changed
notification so the device disappears when unplugged and reappears when
replugged. The engine polls the bus for presence even when not streaming, so
the device shows in the list whenever the hardware is connected.

## If V2 hits the coreaudiod USB wall

The clean fallback keeps V1's proven separate-process USB engine but is not a
step backward in design: we would keep the deep buffer and realtime thread, and
the only thing we lose is the single-clock simplicity. Practically, V1 at step
4q is already a working driver; V2 is the "nicer architecture if the sandbox
allows it" version. Either way you have something that works.

## Files

- `V2-US122.driver/Contents/MacOS/V2-US122.c` — Core Audio plugin object model
- `V2-US122.driver/Contents/MacOS/V2-usbengine.h` — engine interface
- `V2-US122.driver/Contents/MacOS/V2-usbengine.c` — engine + ring + RT thread
- `V2-US122.driver/Contents/MacOS/V2-usbengine-iokit.inc` — IOKit open/iso plumbing
- `V2-US122.driver/Contents/Info.plist` — bundle metadata (distinct from V1)
- `V2-build.sh` — build + install instructions
