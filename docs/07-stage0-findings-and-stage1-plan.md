# 07 - Stage 0 findings, the V2 correction, and the Stage 1 plan

Authoritative current understanding (2026-06-29). Where this conflicts with
docs/02, 04, or 05, this document wins; those carry an older narrative.

## Status

- **Target OS: macOS 13.7.8 Ventura (Intel).** All prior testing was on Ventura;
  the "10.13 High Sierra" in older docs was a labeling error, now corrected.
- **The device tests directly on the dev Mac now.** Claude Code runs locally with
  the US-122MKII attached. V1 (build6, shm v6) is installed and was confirmed
  streaming live on Ventura. The load-induced jitter reproduced live: `gap_max`
  ~115-119 ms, `pb_cb/s` collapsing to ~313, `underrun=0` throughout.
- **Architecture decision: Path A** - low-latency engine inside V1, keep the
  daemon + shm + resampler. Not reviving V2.

## Stage 1 RESULT: VALIDATED on hardware (2026-06-30)

The low-latency PLAYBACK engine works. Standalone tester
`tools/us122_lowlat_tone.c` (native IOKit `LowLatencyWriteIsochPipeAsync`,
contiguous layout, fresh sine), first hardware runs:

- **Audio: clean.** Steady 440 Hz sine, no bit-crush, at 48 kHz. V2's wall is
  gone; the CONTIGUOUS playback layout (vs V2's fixed-stride `i*MAX_PACKET_SIZE`)
  was the fix. User-confirmed by ear, "100% clean, steady."
- **Jitter: cured.** 30 s under heavy load (the same window-move / Spotlight
  stress that crushed V1): `gap_max` stayed **1.2-3.6 ms**, `late(>20ms)=0`,
  `pb_cb` steady ~1000/s, `reanchor=0`, `pb_err=0`. V1 under the same load
  collapsed to `pb_cb`~313 and `gap_max` 115-419 ms. ~100x better worst-case;
  user confirmed playback was untouched by the stress.
- Device runs ~48000 fps steady; nominal pacing is correct (no drift glitch in
  30 s). The earlier "~47409" was a startup-ramp artifact in a cumulative average.
- IOKit bring-up worked first try: `set_rate` ok, pipes pb=2 / cap=1 (mps=78),
  32+32 transfers submitted, **no `kIOReturnIsoTooOld`** (lead=50 frames).
  `SetPipePolicy` + `ResetPipe` applied (V2's other suspects, controlled for).

Deferred (cleanup if all else holds): the rate gate (44.1k/96k) to confirm no
rate-scaled artifact at other rates.

## Stage 3 PROGRESS: native low-latency daemon (2026-06-30)

`src/daemon/us122d_ll.c` - a drop-in alternative to the libusb `us122d`. Same shm
contract (shmring.h), so the existing plugin + menu app work unchanged; only the
transport is native low-latency IOKit. Reuses the validated tester engine, lifts
V1's interpolating resampler (playback) and capture-to-ring logic. V1 left intact
for A/B. Capture (Stage 2) is included in the same daemon.

Validated on hardware so far (headless self-test, `US122_LL_FORCE=1`):
- Compiles clean against real frameworks + shmring.h.
- Acquires the device via IOKit, submits 32+32 transfers (`first_err=0x0`),
  streams with **gap_max 1.3-2.0 ms, pb_err=0, reanchor=0** (jitter cure intact),
  resampler runs (silence + underrun counts on an empty ring, expected), capture
  discards cheaply with no consumer. Clean teardown + exit, no crash.
- Full presence path works: with device_present=1 / user_hide=0, the plugin
  advertised **US-122MKII** in CoreAudio.

END-TO-END VALIDATED (2026-06-30): real music played through CoreAudio -> plugin
-> ring -> us122d_ll -> device, user-confirmed "zero drops, very little latency."
~100 s log: gap_max 1.2-3.7 ms (one 6.1 ms blip), pb_err=0, reanchor=0, and the
resampler held pb_ring at the ~512 ms buffer_ms setpoint (underrun froze the
instant audio arrived). The jitter-cure goal of the whole project is achieved in
a working daemon. Test recipe (no sudo needed in FORCE mode; keep the menu app
closed so the libusb daemon stays dormant):
```
sudo launchctl unload /Library/LaunchDaemons/net.senesh.us122d.plist
sudo killall us122d 2>/dev/null
clang src/daemon/us122d_ll.c -o /tmp/us122d_ll -Isrc/daemon \
    -framework IOKit -framework CoreFoundation -lpthread -Wall -O2
US122_LL_FORCE=1 /tmp/us122d_ll        # device shows as US-122MKII; set as output, play, LISTEN
# restore: Ctrl-C, then sudo launchctl load <plist>; open US122Menu.app
```
CAPTURE (Stage 2) VALIDATED 2026-06-30: with V3 running, the device appears as a
CoreAudio input and a recording returns real signal (max_volume -53.7 dB, not
digital silence) through device -> V3 low-latency capture -> ring -> plugin.

V3 PACKAGED: us122d_ll branded V3, per-0.5s log gated behind US122_DEBUG, restarts
session on plugin rate change, device-loss recovery via the acquire loop.
`src/build/build_us122d_v3.sh` builds daemon+plugin (no libusb). `INSTALL.md` is
the end-user guide. V3 installs over V1 (binary -> /usr/local/bin/us122d, existing
plist/plugin/menu unchanged).

REMAINING (needs sudo + hardware): install V3 as the real LaunchDaemon and test
the gated (non-FORCE) path via the menu app; hotplug under V3 (unplug/replug);
multi-minute soak; other sample rates (44.1k/88.2k/96k).

## Stage 0: the buffer-layout question is ANSWERED

From the local SDK headers (`IOKit/usb/USB.h`, `IOUSBLib.h`) plus V1 as a
proven-correct, on-hardware reference:

1. **No per-packet data-offset field exists** in either isoc frame struct.
   `IOUSBIsocFrame` = `{frStatus, frReqCount, frActCount}` (8 bytes);
   `IOUSBLowLatencyIsocFrame` adds `AbsoluteTime frTimeStamp` (16 bytes). The
   roadmap's "maybe the offset is recorded in the frame list" theory is therefore
   impossible. Packet position is implicit, governed by `frReqCount`.
2. **Low-latency data layout is identical to standard isoc.** `IOUSBLib.h`: the
   low-latency calls are "analogous to ReadIsochPipeAsync/WriteIsochPipeAsync;
   they differ in that the frame list data is updated at primary interrupt time."
   Differences are only: buffers from `LowLatencyCreateBuffer` (read/write/
   framelist types), 16-byte frame structs, `updateFrequency` 0-8 ms.
3. **The correct layout = V1's libusb layout** (proven clean on this device):
   - **Playback (write): CONTIGUOUS**, packed by the variable per-packet byte
     count. us122d.c sets `iso_packet_desc[i].length = bytes` and packs at a
     running offset (`running += bytes`). Not fixed stride.
   - **Capture (read): fixed stride `i * 78`**; set `frReqCount = 78` for every
     packet and gather `actual_length` valid bytes from each slot. (When reqcount
     is constant, contiguous == fixed-stride, so capture cannot distinguish the
     two models; playback is where layout matters.)

## V2 correction (supersedes docs/02's "irony")

- **The V2 source in `reference/v2-abandoned/` uses STANDARD isoch**
  (`WriteIsochPipeAsync`/`ReadIsochPipeAsync`, 8-byte `IOUSBIsocFrame`), NOT the
  low-latency API docs/02 describes. Only stale comments mention low-latency. The
  standard-iso "Option 1" was coded but, per history, never hardware-tested.
- **V2 ran on the real device and did NOT skip or break.** It sounded bit-crushed
  (~4-bit, recognizable underneath, scaled smoothly with sample rate, cleanest at
  96 kHz). So the **low-latency transport timing was clean on this hardware** -
  positive evidence that low-latency iso cures the jitter. The failure was DATA,
  not timing.
- **Root cause of the bit-crush is OPEN.** The fixed-stride playback layout in
  V2's `fill_playback_xfer` is one candidate, but it does not fit a uniform,
  recognizable bit-crush (a gross stride mismatch would skip or garble, not gently
  quantize). The "per-packet-edge, scales-with-rate, best at 96k" signature points
  at a subtle per-packet-boundary corruption that has not been identified. Do not
  assert the cause.

## Why Path A is safe regardless of V2's unsolved bug

Path A ports **V1's proven layout** into low-latency calls; it does not reuse
V2's code. The headers confirm the low-latency data layout equals the standard
layout V1 already uses cleanly. Replicate V1's exact contiguous-write /
fixed-stride-read layout and we sidestep whatever V2 did wrong.

## Stage 1 plan: low-latency PLAYBACK engine (decided 2026-06-30)

Playback first, by the user's call: it is the goal, it is where V2's bit-crush
lived, and "play a tone and listen" is a faster, more decisive verdict than a
capture value-dump. Tradeoff accepted: this couples the new plumbing, the
layout-critical contiguous write, and future-frame scheduling in the first test,
so instrument well (below) to keep a failure diagnosable.

1. **Isolate the IOKit code** in a new file (e.g. `src/daemon/lowlat_iso.c/.h`)
   so a compile error is a targeted fix, not an architecture round.
2. **Acquire** via the proven protocol facts (vid 0644 / pid 8021, iface 1, alt 1,
   **EP 0x02 OUT**, rate-set control transfer). Keep libusb for enumerate/open/
   claim at first if convenient, or go native IOUSBLib.
3. **Allocate** with `LowLatencyCreateBuffer`: a data buffer
   (`FRAMES_PER_XFER * 78`, **kUSBLowLatencyWriteBuffer**) and a frame-list buffer
   (`FRAMES_PER_XFER * sizeof(IOUSBLowLatencyIsocFrame)` = *16* per frame,
   kUSBLowLatencyFrameListBuffer). Queue several transfers (start with 16).
4. **Fill = the layout-critical step. Copy V1's `us122d.c` exactly:** per packet,
   `g_accum += rate; frames = g_accum/8000; bytes = frames*6`; pack **contiguously**
   (`running += bytes`), and set `frameList[i].frReqCount = bytes`. NOT fixed
   stride. This is the one thing V2 got wrong; getting it identical to V1 is the
   whole game.
5. **Submit** `LowLatencyWriteIsochPipeAsync` on EP 0x02, `updateFrequency=1`,
   `frameStart = GetBusFrameNumber + lead`. **`kIOReturnIsoTooOld` is the likely
   snag here** (playback schedules on future frames); widen the lead if it fires.
6. **Run** on a dedicated CFRunLoop thread promoted to realtime (reuse V1's
   `make_thread_realtime` policy). Source audio from the existing playback ring
   (the HAL plugin + resampler already fill it), so only the transport changes.
7. **TONE GATE:** feed a known steady **sine** (not music) and listen. A clean,
   steady tone = layout right. Any grit/bit-crush = layout still wrong.
8. **RATE GATE (the sharp one):** test 48k, then 44.1k and 96k. V2 got cleaner at
   96k (per-packet slip `78 - reqcount`: 42 bytes at 48k down to 6 at 96k). If our
   contiguous layout is right, that rate-scaled artifact is GONE at every rate.
9. **LOAD GATE:** window-move / Spotlight typing; confirm `gap_max` stays low and
   `pb_cb/s` holds ~500 (vs V1's collapse to ~313). That is the proof the
   low-latency path beats the jitter.

Pass the gates, then **Stage 2: capture** (EP 0x81, `frReqCount=78`, fixed-stride
`i*78` gather of `frActCount` bytes; layout-insensitive, so it is the easy half).
Value-dump bytes-in == bytes-out vs a known-good V1 capture.

## Open risks

- **Bit-crush root cause unknown.** The decisive layout test is the Stage 1
  playback tone + rate gates; capture (Stage 2) is layout-insensitive (constant
  reqcount), the easy half.
- **`kIOReturnIsoTooOld`** lead-frame tuning (V2 flagged it).
- IOKit/CoreAudio now compile AND test locally on this Mac, so iteration is fast.

## Local build/test loop

- **Build:** `clang` locally (CoreAudio + IOKit frameworks present). All three V1
  components already compile clean here.
- **Install:** needs sudo, and **sudo requires a password** on this Mac, so the
  daemon/plugin install + `killall coreaudiod` steps are handed to the user to run.
- **Test without audio out:** capture from avfoundation audio input `[1]`
  (`ffmpeg -f avfoundation -i ":1" -t 6 -y /tmp/cap.wav`); read jitter lines from
  `/var/log/us122d.err` (`US122_DEBUG=1` in the plist). To wake the daemon, build
  and `open` the menu app (it sets `app_active=1`).
