# 04 - Roadmap: the low-latency isochronous rewrite

> **Update 2026-06-29 (see `docs/07`):** Decision made: **Path A**. **Stage 0 is
> resolved** from the local SDK headers + V1 as reference (no frame-list offset
> field; low-latency data layout == standard; correct layout = V1's contiguous
> write / fixed-stride read). The current Stage 1 plan lives in
> `docs/07-stage0-findings-and-stage1-plan.md`; use it instead of the Stage 0/1
> text below where they differ.

The goal: cure V1's load-induced jitter by moving the isochronous transport from
stock libusb (standard iso, run-loop delivery, stalls under load) to Apple's
**low-latency IOUSBLib isoc API** (`LowLatencyCreateBuffer`,
`LowLatencyReadIsochPipeAsync`, `LowLatencyWriteIsochPipeAsync`), whose frame
list updates at primary interrupt time and does not depend on run-loop delivery.

Read `docs/02-history-v1-v2.md` first. V2 used this exact API and hit an
unresolved bit-crush; the buffer layout is the suspected cause and is the
highest-risk part of this work.

## The decision the user should make first (do not skip)

Two architectures can host the low-latency isoc engine:

- **Path A: put it in V1.** Replace the daemon's libusb streaming path with a
  native low-latency IOUSBLib engine. Keep the daemon-as-root structure (which
  sidesteps the coreaudiod sandbox question) and the shm bridge to the plugin.
  The two-clock resampler stays. This is the conservative path: it touches only
  the transport of a shipping driver.

- **Path B: revive V2.** V2 already IS a native IOKit single-process engine with
  low-latency isoc; its only blocker is the buffer-layout bug. If that bug is the
  fixed-stride assumption (a real possibility), reviving V2 gets you the simpler
  one-clock architecture AND the jitter fix together. V2 also proved coreaudiod
  can open USB, so the sandbox fear that justified V1's split is gone.

Both converge on the same unsolved question: **the correct low-latency iso
buffer layout on this device.** Path A answers it in fresh daemon code; Path B
answers it in V2's existing code. The roadmap below is written for Path A
because it protects the shipping line, but the user may reasonably prefer B.
**Surface this choice explicitly before starting. Do not silently assume A.**

The stages below assume Path A. If the user picks B, the same stages apply but
the code lives in `reference/v2-abandoned/` (promoted out of reference), and you
skip the shm/resampler parts because V2 has one clock.

## Stage 0: answer the buffer-layout question (BLOCKING, do this first)

Before any streaming code, resolve the question V2 could not answer from a Linux
container: **how does the low-latency controller read packet data from the
buffer?**

- On the Mac, read the IOUSBLib SDK headers for `IOUSBLowLatencyIsocFrame`,
  `LowLatencyCreateBuffer`, `LowLatencyCreateFrameList` (names per the SDK), and
  the `LowLatency{Read,Write}IsochPipeAsync` signatures.
- Determine definitively: is packet i's data read at a fixed `i x maxPacketSize`
  stride, or at an offset the controller computes / the frame list records? V2
  assumed fixed-stride. Confirm or refute.
- Study V2's implementation in
  `reference/v2-abandoned/V2-US122.driver/Contents/MacOS/V2-usbengine-iokit.inc`
  and `V2-usbengine.c`. It is mostly-working code: open, claim, set alt, rate-set,
  duplex frame scheduling all functioned. Its buffer fill (`fill_playback_xfer`)
  is the suspected-broken part.
- Cross-check against Apple's USB sample code and any reputable example of
  `LowLatencyReadIsochPipeAsync` (note: cgminer's `darwin_usb.c` and libusb's own
  `darwin_usb.c` show the STANDARD calls, useful for contrast but not the
  low-latency layout).

Deliverable of Stage 0: a written statement of the exact buffer/frame-list
layout the API requires, with the source (header line or sample) that proves it.
Everything downstream depends on getting this right.

Estimate: 1-3 Claude-cycles (mostly reading; some may be the user pasting header
excerpts you cannot see).

## Stage 1: low-latency CAPTURE path (read-only, easiest to verify)

Build a libusb-independent capture engine: `LowLatencyCreateBuffer` for the data
and frame-list buffers, `LowLatencyReadIsochPipeAsync` on EP 0x81, frame list
with a chosen `updateFrequency`, on a dedicated CFRunLoop thread promoted to
realtime. Keep libusb only for enumerate/open/claim/set-alt if convenient, or go
fully native.

Prove capture reaches the ring with correct data, glitch-free under a window-move
load. Reuse V2's value-dump instrumentation: log the first few frames at each
stage so you confirm bytes-in == bytes-out before trusting anything.

Validation gate: capture is clean under load AND the bytes match. Only then
proceed.

Estimate: 4-7 cycles. This is the hard part. First compile-and-run will not be
clean; expect 2-3 cycles just to get correct capture. The buffer layout from
Stage 0 is the make-or-break.

## Stage 2: low-latency PLAYBACK path

`LowLatencyWriteIsochPipeAsync` on EP 0x02, contiguous packed buffer per the
Stage 0 layout, frame scheduling on future bus frame numbers (watch for
`kIOReturnIsoTooOld`, which V2's history flagged as the likely snag, the lead
frame offset must be right). On the duplex-coupled device, share the bus frame
counter with capture (V2's single-monotonic-counter approach is the proven
pattern; copy it).

Validation gate: playback is clean under load and does NOT bit-crush. This is
where V2 died. If a CORRECT Stage 0 layout still bit-crushes here, that is the
decisive finding that low-latency iso cannot be used cleanly on this device, and
the jitter is the user-space floor. Test a correct layout before concluding that.

Estimate: 3-5 cycles. The `kIOReturnIsoTooOld` / lead-frame tuning is the likely
multi-cycle snag.

## Stage 3: integrate and replace the libusb streaming loop

Splice the native engine into the daemon lifecycle (acquire / stream / teardown,
the plist, the menu app, the shm contract, the buffer control all stay).
Re-solve teardown for the new buffer model (the current stranded-transfer logic
is libusb-specific and does not apply; low-latency buffers free differently).

Estimate: 2-4 cycles.

## Stage 4: tune and verify under the real triggers

Tune `updateFrequency`, packets-per-transfer, and the number of buffers. Measure
`gap_max` and `pb_cb/s` under window-move and Spotlight load using the existing
instrumentation. Before, `pb_cb/s` fell to ~100 and `gap_max` hit ~400ms; the
target is `pb_cb/s` holding near 500 and `gap_max` staying low under the same
load.

Estimate: 2-4 cycles.

## Total estimate

Roughly **11-20 Claude-cycles, realistically ~15.** Stage 1 dominates and is
where it can balloon if the buffer layout fights back. Most of the wall-clock is
the user's hardware test loops, since you cannot test IOKit here.

## Honest risk register

- **Buffer layout (highest).** V2's unsolved wall. Stage 0 exists to retire this
  risk before writing streaming code. If Stage 0 is gotten wrong, Stages 1-2
  will bit-crush exactly as V2 did and you will burn cycles not knowing why.
- **You cannot compile IOKit/CoreAudio here.** Every low-latency line is
  unverified until the user builds it. Structure the native code in isolated
  files so a typo costs a targeted fix, not a full re-architecture round. Expect
  more "fix the compile error I pasted" cycles than usual early on.
- **`kIOReturnIsoTooOld` / frame scheduling.** The lead-frame offset for
  scheduling transfers on future bus frames is fiddly and device-specific. V2
  hit it. Budget a cycle or two in Stage 2.
- **The standard-iso fallback.** If a correct low-latency layout still bit-crushes,
  the fallback conclusion is that low-latency cannot be used cleanly on this
  device and V1's jitter is the floor. This is a legitimate (if unwanted)
  outcome. Do not thrash on it; report it cleanly with the evidence.

## Why git + Claude Code matters for this specifically

This is ~15 iterations of IOKit code you cannot compile locally, across multiple
files, with the user testing each on the Mac. Version control and fast local
builds on the Mac (real SDK headers, real device) are exactly what this phase
needs, which is why the user is moving here. Use branches per stage; keep each
stage's working state committed before tuning the next.
