# 02 - History: V1, V2, and the irony you must understand

This is the most important document in the package. It explains where V2 failed,
in detail, and why that failure is in direct tension with the work ahead. If you
internalize one thing, make it the section "The irony, stated plainly."

## Timeline

- **Build phase (V1):** reverse-engineered the USB protocol from scratch, built
  the daemon + plugin + shm architecture, fought a long clock-drift and
  resampler battle, reached clean working audio. Hardware-proven.
- **V2 attempt:** a ground-up rearchitecture to delete V1's hardest part (the
  two-clock resampler). Got 95% working, then hit an unresolved distortion wall.
  Abandoned, parked as reference.
- **V1 hardening:** returned to the working V1 and made it robust: hotplug,
  disconnect/reconnect recovery, disappear-from-list, the menu app.
- **V1 robustness tuning (the most recent session):** chased "skips under load,"
  fixed a real crash, then diagnosed the residual as a structural transport
  limitation, and researched the fix. That fix is the work ahead.

## What V1 is, and what hurt about it

V1 works (clean audio, robust). Its pain is structural, all of it flowing from
the two-process split: the USB engine (daemon) and the Core Audio plugin run in
separate processes with **two independent clocks**, bridged by shared memory.
That split forced:
- a resampler to reconcile the device clock against the coreaudiod clock,
- a cross-process realtime-priority fight,
- a deep buffer as a shock absorber.

V1 chose the split for one defensible reason: fear that the **coreaudiod sandbox
would forbid raw USB access** from a HAL plugin. Putting USB in a root daemon
sidestepped that fear. (Hold that thought; V2 disproved the fear.)

## What V2 was

A **single-process** AudioServerPlugIn that owned the USB device directly via
IOKit (`IOUSBLib`) and ran the isochronous engine on a realtime thread **inside
the plugin**. One process, one clock (the device), no shared memory, no
resampler, no daemon. It carried the protocol facts over from V1 verbatim
(those are hardware truth and do not change).

The full V2 source is in `reference/v2-abandoned/`. It is worth reading. It got
remarkably far.

## What V2 proved (the good news that outlived it)

**The coreaudiod sandbox fear was unfounded.** V2's first hardware test showed
the plugin loading inside `coreaudiod`, opening the USB device through IOKit,
and starting isochronous streaming. The log showed `device present` and
`streaming at 48000 Hz` from the coreaudiod driver-host process. So a HAL plugin
on macOS 13.7.8 Ventura **can** open a raw USB device. That single fact reshapes the
option space for any future rearchitecture: the two-process split is not
actually required by the sandbox.

## The V2 bring-up bugs that WERE solved (in order)

Each was diagnosed by instrumenting, reproducing, and fixing one thing:

1. **Digital noise.** Cause: the IOKit low-latency iso buffer layout. V2 first
   wrote packets tightly packed (what libusb needed in V1). IOKit low-latency
   iso reads each packet from a **fixed slot** at `index x maxPacketSize`, not
   contiguously. Fix: fixed-stride layout. Noise became recognizable audio.
2. **Extreme (~2.7s) delay.** Cause: V1's giant 131072-frame ring was carried
   into V2, where a single-clock design needs almost no buffer. Fix: shrink the
   ring to ~8192 frames. Delay collapsed.
3. **Choppiness / duplex divergence.** Cause: a per-transfer "re-anchor" that
   jumped each transfer's frame number forward independently, scrambling packet
   order on the duplex-coupled device. Fix: a single monotonic frame counter per
   direction, scheduling transfers in strict sequence. Ordering held
   (`reanchor=0`), and capture/playback callback counts locked in lockstep.
4. **Split clock (input 88.2k while output 48k).** Cause: stream formats were
   not settable, so Core Audio defaulted the input stream to 88.2k independently.
   Fix: collapse both stream rates onto one shared device rate. Audio MIDI Setup
   then showed input and output moving together.

After all four, the metrics were pristine: `reanchor=0`, duplex synced,
`underrun=0`, ring fill steady, capture delivering at the device rate. A value
dump proved the **bytes leaving the driver were perfect full-precision 24-bit.**

## The V2 bug that was NOT solved (why V2 was abandoned)

A **rate-dependent "4-bit bit-crusher" distortion.** The original audio was
fully recognizable underneath, but gritty, as if quantized to ~4 bits. Key
properties, all measured:
- The bytes leaving the driver were proven correct (the value dump).
- The rate-set control message matched the Linux reference exactly.
- Timing, ordering, and duplex sync were all clean.
- The distortion **scaled smoothly with sample rate** and was **closest to
  clean at 96kHz.**
- The device's measured capture rate read ~47,500 fps against a requested 48000.
  This was concluded to be a **measurement artifact**, not a real 1% clock
  error, because (a) a true 1% error would have wrecked V1 too and it did not,
  and (b) the playback ring fill held rock-steady with zero underruns, which a
  real overfeed could not produce.

With the data proven correct and the clock proven fine, the diagnosis landed on
the **one structural thing V2 did that both V1 and the Linux reference never
did: IOKit low-latency isochronous transport** (`LowLatencyReadIsochPipeAsync` /
`LowLatencyWriteIsochPipeAsync`). V1's libusb uses the **standard**
`ReadIsochPipeAsync` / `WriteIsochPipeAsync`. The Linux reference uses plain
`usb_submit_urb`. Both clock this device cleanly. V2's low-latency path was the
lone remaining suspect.

The "best at 96k, scales with rate" signature fit a **per-slot edge artifact in
the low-latency buffer layout**: at higher rates more frames pack into each
fixed-stride slot, so any per-slot edge error averages down. That points at the
buffer layout (the stride/offset model), not at low-latency delivery as such.

**V2 stopped at a decision point**, with two untried options on the table:
- **Option 1 (primary suspect):** switch V2 from low-latency iso to **standard**
  `WriteIsochPipeAsync` / `ReadIsochPipeAsync` (what V1 and the reference use),
  keeping every proven piece. Never tried.
- **Option 3 (cheaper bet):** re-examine the low-latency buffer layout directly;
  the controller may read each packet's data from an offset recorded in the
  frame list rather than the assumed fixed stride. The container had no macOS
  SDK headers to confirm the struct, so this was never resolved.

V2 was then parked in favor of continuing with the working V1.

## The irony, stated plainly (READ THIS TWICE)

The most recent session diagnosed V1's load-induced jitter and concluded the fix
is to **adopt Apple's low-latency isochronous API** (the V1 transport currently
rides stock libusb, which uses standard iso, and that standard delivery path is
what stalls under load). The research for this is in `docs/06-jitter-research.md`.

So the two facts in tension are:

- **V2 suspected low-latency iso as the CAUSE of its bit-crush distortion.**
- **V1's planned fix is to ADOPT low-latency iso to cure jitter.**

These point in opposite directions on the exact same API. A fresh instance that
does not know the V2 history will adopt low-latency iso, hit the same bit-crush,
and not understand why. Do not be that instance.

## How to resolve the tension (this is your guidance for the work ahead)

The contradiction is resolvable, and the resolution shapes Stage 1 of the
roadmap:

1. **V2 never PROVED low-latency iso was the cause.** It was the last suspect by
   elimination, and the session ended before testing the standard-iso swap
   (Option 1). "Low-latency is guilty" is a strong hypothesis, not a verdict.

2. **The leading sub-theory inside V2 was the BUFFER LAYOUT, not low-latency
   delivery itself.** The "best at 96k / per-slot edge artifact" reasoning points
   at how V2 laid bytes into the low-latency buffer (fixed-stride vs the
   frame-list offset model), which is a fixable implementation bug, not an
   inherent property of the API. V2 assumed fixed-stride. If that assumption was
   wrong, that single mistake would explain BOTH why V2 bit-crushed AND exactly
   what to get right in V1.

3. **Therefore the buffer layout is the highest-risk, highest-attention part of
   the V1 low-latency rewrite.** Before writing the streaming path:
   - Read V2's actual low-latency iso code in
     `reference/v2-abandoned/V2-US122.driver/Contents/MacOS/V2-usbengine-iokit.inc`
     and `V2-usbengine.c`. It is a real, mostly-working implementation. Learn
     from its working parts (open, claim, rate-set, duplex frame scheduling) and
     treat its buffer layout as the suspected-broken part.
   - On the Mac (where the SDK headers exist, unlike V2's container), read the
     IOUSBLib headers for `IOUSBLowLatencyIsocFrame` and `LowLatencyCreateBuffer`
     and confirm the EXACT data-offset model: does the controller read packet i's
     data at a fixed `i x maxPacketSize` stride, or at an offset it computes /
     that the frame list records? This is the question V2 could not answer from
     Linux. Answer it first.
   - Validate the capture path (read-only) before playback, with the value-dump
     instrumentation V2 already pioneered, so you confirm bytes in == bytes out
     before trusting the layout.

4. **Keep the standard-iso swap as a fallback hypothesis.** If a correct
   low-latency buffer layout still bit-crushes on hardware, then low-latency
   delivery really is the problem on this device, and the answer is that V1's
   jitter is the user-space floor (low-latency cannot be used cleanly here). That
   would be a real, if disappointing, finding. But test a correct layout first.

## Why not just finish V2 instead of improving V1?

A fair question, and worth weighing honestly rather than assuming V1:
- **For V2:** it proved coreaudiod-USB works, it deletes the resampler and the
  two-clock complexity, and its only blocker is one unresolved (and possibly
  simple) buffer-layout bug. If that bug is the fixed-stride assumption, V2 could
  be both simpler AND the natural home for low-latency iso.
- **For V1:** it ships today, it is robust, and the user has been hardening it
  for multiple sessions. The low-latency work targets a specific, well-understood
  jitter cause.
- **The shared truth:** both paths converge on the same unsolved technical
  question, "what is the correct low-latency iso buffer layout on this device."
  Whichever architecture you put it in, that question must be answered. The
  roadmap (`docs/04`) plans it inside V1 because that is the shipping line, but
  the V2 code is the closest existing implementation to study, and if the layout
  question resolves cleanly, reviving V2 is a legitimate alternative the user may
  want to weigh. Surface that choice; do not silently assume V1.

## Bundle identities (so the two can coexist on the Mac)

- V1 plugin: `net.senesh.us122.driver`, device shows as "US-122MKII".
- V2 plugin: `net.senesh.us122.driver.v2`, device shows as "US-122MKII (V2)".
- Distinct names were deliberate so both can be installed side by side for
  A/B comparison.
