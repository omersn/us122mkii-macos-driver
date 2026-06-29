# 03 - Current state (after the robustness-tuning session)

This documents exactly where the project stands today: what the last session
changed, what is solved, and the precise open problem. The source in `src/` is
the state described here.

## Headline

V1 is **stable and crash-free**, the audio engine is **glitch-proof at the
buffer level** (zero underruns under brutal load), and the one remaining issue
is **load-induced jitter** whose cause is now precisely diagnosed and whose fix
is the work ahead (the low-latency isoc rewrite, `docs/04`).

## What this session set out to do

Fix "skips under load": during playback, certain system activity (typing in the
Spotlight/Apple search bar, and even just moving a window) caused audible skips.
The user is emphatic that this happens under **light** load too (window move),
on a Mac that is otherwise smooth. So "just don't load the machine" is not an
acceptable answer.

## The diagnosis chain (each step was measured, not guessed)

The method throughout: instrument the daemon, reproduce the skip, read the
numbers, change one thing, re-measure.

1. **Added jitter instrumentation.** Per-250ms log line now carries: `pb_cb/s`
   and `cap_cb/s` (transfer completion callback rates), ring fills, ok/err
   counts, underrun/overrun, and the new fields **`gap_max`** (worst gap between
   consecutive playback completions, ms), **`gap_mean`**, **`qmin`** (shallowest
   in-flight queue depth), **`late`** (cumulative gaps > 20ms). The callback
   records only a timestamp and a couple of min/max updates (cheap, no I/O); a
   separate logger thread formats and writes. Enabled by `US122_DEBUG=1`, set in
   the plist.

2. **Discovered the "skip" was sometimes a CRASH, not a glitch.** Early logs
   showed the daemon dying and being relaunched by launchd (new PID), with a
   `*** FATAL: caught SIGSEGV ***` after a teardown that found stranded
   transfers. So one class of "skip" was a ~1-second crash-and-restart dropout.

3. **Fixed the crash (use-after-free in teardown).** When a session stopped under
   load, cancellations did not all complete in time, leaving transfers still
   submitted in libusb's view. The old teardown called `libusb_free_transfer` on
   them anyway, a use-after-free that segfaulted. The fix evolved across several
   builds to its final form (all in the current `us122d.c`):
   - **Never free still-submitted transfers.** On a drain timeout, do not free
     the stranded ones.
   - **Do not destroy and rebuild the libusb context** in the normal stop path.
     An attempt to `libusb_exit` then `libusb_init` in-process FAILS on this
     macOS (`libusb_init` returns an error right after exit), which itself caused
     a daemon exit + relaunch. So that approach was abandoned.
   - **Final approach:** drain patiently (up to 5s, cheap since not streaming);
     if transfers still do not return, **leak** that bounded handful (16 max)
     rather than crash or destroy the context; keep the context intact across the
     session. Only rebuild the context on genuine device loss (reconnect), and
     make that rebuild non-fatal (keep the old context if re-init fails, never
     exit the daemon).
   - **Removed the `usleep(1000)` from both transfer error callbacks.** They ran
     on the USB event thread; sleeping there starves the whole stream and stalls
     cancellation draining. This both helped drains complete and reduced
     starvation.
   Result, hardware-verified: segfault gone, daemon stays alive on one PID, the
   teardown leaks safely instead of crashing.

4. **With the crash gone, the CONTINUOUS skipping during load remained**, and the
   numbers explained it completely. During the load window, `pb_cb/s` collapsed
   from ~500 to 100-200 (once as low as 43), and `gap_max` spiked to 200-419ms,
   while **`underrun` stayed at 0 the entire time** and the ring stayed full
   (climbed toward 131072). That combination has exactly one meaning: the USB
   event thread is being **starved of CPU**, so it cannot push packets to the
   device on schedule. The device runs dry between late bursts, and that is the
   skip. The buffer is full of audio the whole time, which is why buffer size
   never helped.

## What was tried against the jitter, and the results (the elimination table)

Each row is a separate build, tested against the same reproducer, read off the
log. This table is the core empirical result; do not re-litigate it without new
data.

| Change | Hypothesis | Measured result |
|---|---|---|
| Buffer 128ms -> 1536ms | buffer too small | No effect on skips. underrun stayed 0. |
| (counter) | underruns | 0 across every run. Not an underrun problem. |
| RT policy: non-preemptible, computation 0.6 -> 0.9ms | CPU preemption | Faster recovery, spikes still 200-400ms. Helped, did not cure. |
| Per-region `mlock` (shm + transfer buffers) | page faults | No change. `mlockall` is unavailable on macOS 13.7.8 (errno 78); per-region `mlock` succeeded ("shm wired into RAM (mlock ok)") and changed nothing. |

**Conclusion from the table:** a non-preemptible realtime thread, with wired
memory and a deep transfer queue, STILL sees 280-400ms gaps between isoc
completions. The thread is therefore not the bottleneck; it is ready and
waiting. The completions themselves arrive late. The stall is **below** the
event-handling layer, in the USB transport.

## The root cause (sourced; full detail in docs/06)

Stock **libusb on macOS uses the standard `ReadIsochPipeAsync` /
`WriteIsochPipeAsync`** (confirmed in libusb's `darwin_usb.c`). Completions are
delivered up to user space through a CFRunLoop async-port. Under ordinary system
load (WindowServer, Spotlight), that kernel-to-userspace delivery path is what
gets delayed. Apple provides a separate **low-latency** API
(`LowLatencyReadIsochPipeAsync` / `LowLatencyWriteIsochPipeAsync`) that "updates
the frame list at primary interrupt time," bypassing the run-loop delivery
dependency. libusb does not use it. That is the gap, and adopting that API is
the fix (with the heavy caveat from `docs/02`: V2's bit-crush was suspected to
come from this same API, so the buffer layout must be gotten right).

## What is solid right now (keep all of this)

- The audio engine: zero underruns under brutal load. Resampler, rings, flush,
  buffer control all done and proven.
- Crash-free teardown (the use-after-free fix and the context-recycle discipline).
- No `usleep` on the realtime callback path.
- Strengthened RT policy (non-preemptible, 0.9ms computation) - keep it; it
  improves recovery even though it does not cure the spikes.
- The full lifecycle: hotplug, disconnect/reconnect recovery, disappear-from-list,
  the menu app with the buffer submenu, dormant parking when the app is inactive.
- The jitter instrumentation (gap_max, qmin, late) - reuse it to measure the
  low-latency rewrite.

## What is NOT solved

The continuous load-induced jitter. It is the user-space isoc-delivery ceiling
of stock libusb. Not fixable by more buffer, more priority, or memory locking
(all tested negative). The path forward is `docs/04`.

## Install commands (the user runs these on the Mac)

**Daemon-only reinstall** (no shm/plugin/plist change):
```
sudo launchctl unload /Library/LaunchDaemons/net.senesh.us122d.plist
sudo killall us122d 2>/dev/null
chmod +x build_us122_driver.sh
./build_us122_driver.sh
sudo cp us122d /usr/local/bin/us122d
sudo chmod 755 /usr/local/bin/us122d
sudo truncate -s 0 /var/log/us122d.err /var/log/us122d.log
sudo launchctl load /Library/LaunchDaemons/net.senesh.us122d.plist
sudo killall coreaudiod
```

**Full reinstall** (when plugin / plist / shm-version changed) additionally:
```
sudo cp net.senesh.us122d.plist /Library/LaunchDaemons/
sudo chown root:wheel /Library/LaunchDaemons/net.senesh.us122d.plist
sudo rm -rf /Library/Audio/Plug-Ins/HAL/US122.driver
sudo cp -R US122.driver /Library/Audio/Plug-Ins/HAL/
./build_us122menu.sh
```

**Watch the log:** `tail -f /var/log/us122d.err`
Timestamped lifecycle lines go to `.err`; the per-250ms jitter lines also stream
there when `US122_DEBUG=1`.

**Tarball-to-Mac friction note:** tarballs do not preserve the execute bit, so
the user must `chmod +x *.sh` after extracting. This is recurring; mention it.

## The container build-check you CAN run (catches errors before a hardware cycle)

The portable C core compiles against a libusb stub. The pattern used all session:
```
gcc -std=c11 -D_GNU_SOURCE -I/tmp/stubs -I. -c us122d.c -o /tmp/us122d.o
```
(The stub header lives at `/tmp/stubs/libusb-1.0/libusb.h` and was built up
incrementally; recreate it as needed for the symbols you use.) Swift is only
brace/paren/bracket-checked here, never compiled. IOKit and CoreAudio do not
compile here at all, which is the central reason the low-latency work needs the
user's Mac for every real validation.
