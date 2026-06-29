# US-122MKII Driver: Load-Induced Jitter - Problem Definition and Research

## 1. The problem, precisely

**System.** User-space macOS 13.7.8 Ventura (Intel) driver for the TASCAM US-122MKII.
A root libusb daemon owns the USB device and runs async isochronous transfers
(16 URBs x 16 packets, EP 0x81 IN / 0x02 OUT, coupled full-duplex, 48 kHz). The
daemon's USB event thread calls `libusb_handle_events_timeout` in a loop and is
promoted to a mach `THREAD_TIME_CONSTRAINT_POLICY` realtime thread. Audio is
bridged to an AudioServerPlugIn via POSIX shared memory.

**Symptom.** Under heavy *unrelated* system load (typing in the Apple/Spotlight
search bar), the USB event thread is periodically starved. Transfer-completion
callbacks drop from ~500/sec to 100-200/sec; inter-completion gaps spike to
200-400 ms, several times across a ~10 second window. Audible as repeated skips
that wrap around the load.

**Crucially: this is NOT audio buffer starvation.** `underrun` stayed at 0 across
every measured run, through gap spikes over 400 ms. The ring buffer stays full
(climbs toward 131072). Audio is present in the buffer the whole time; it simply
is not being pushed to the device on schedule. The skip is the *device* running
dry between late packet bursts, not the daemon running out of data.

## 2. What was ruled out, by measurement

Each was a separate build, tested against the same reproducer, read off the
instrumented log (pb_cb/s, gap_max, qmin, underrun):

| Hypothesis | Test | Result |
|---|---|---|
| Buffer too small | 128 ms -> 1536 ms | No effect on skips |
| Underruns | underrun counter | 0 across all runs |
| CPU preemption | RT policy non-preemptible, 0.9 ms compute | Faster recovery, spikes remain |
| Page faults | per-region `mlock` (shm + xfer buffers); `mlockall` unavailable (errno 78) | No change |

Conclusion from the table: a non-preemptible realtime thread, with wired memory
and a deep transfer queue, STILL sees 280-400 ms gaps between isoc completions.
That means the thread is not the bottleneck. The thread is ready and waiting; the
completions themselves are arriving late. The stall is *below* the event-handling
layer, in the USB transport path.

## 3. Root cause (sourced)

The stall is structural to how stock libusb performs isochronous transfers on
macOS.

- libusb's Darwin backend (`darwin_usb.c`) uses the **standard**
  `ReadIsochPipeAsync` / `WriteIsochPipeAsync` calls from Apple's IOUSBLib.
  Confirmed in the libusb source. Completions are delivered up to user space
  through a CFRunLoop async-port (mach port) source. Under heavy system load,
  that kernel-to-userspace completion-delivery path is what gets delayed. The
  audio thread and buffer are fine; the *notification* is late.

- Independent corroboration: a libusb maintainer-tracked discussion (libusb
  Discussion #1531) describes the identical wall. With a dedicated high-priority
  thread and many transfers queued ahead, the reporter still sees periodic stream
  interruptions, and concludes it is "more internally in libusb... my guess is
  it's running in userspace and not kernel space, hence the delay." This matches
  our external measurements exactly.

- Apple provides a SECOND isoc API specifically for this problem:
  `LowLatencyReadIsochPipeAsync` / `LowLatencyWriteIsochPipeAsync` (IOUSBLib,
  IOUSBInterfaceInterface192+). Per Apple's docs, these "update the frame list at
  primary interrupt time", i.e. completion/frame data is posted at the hardware
  interrupt level rather than waiting on the run-loop delivery to user space.
  They require buffers allocated with `LowLatencyCreateBuffer` and a frame list
  with an `updateFrequency`. **libusb does not use these.** This is the gap.

So: the factory TASCAM driver and pro/semi-pro USB-audio apps avoid this because
they run isoc either in the kernel or via the low-latency IOUSBLib path. The stock
libusb user-space path we are on rides the ordinary async-port delivery, which is
the thing that stalls under load. Our measurements proved the cause from the
outside; the libusb source and Apple docs confirm it from the inside.

## 4. Where this leaves the project

What is already solid and should be kept:
- The audio engine is glitch-proof at the buffer level (zero underruns under
  brutal load). The resampler, ring, flush, and buffer control are done.
- The crash class is fixed: teardown no longer frees still-submitted transfers
  (use-after-free / SIGSEGV gone) and no longer destroys+rebuilds the libusb
  context in-process (libusb_init-after-exit failure gone). It now drains
  patiently and, worst case, leaks a bounded handful of transfers rather than
  crashing. The daemon stays alive on one PID through the load.
- The `usleep` calls were removed from the transfer error callbacks (they ran on
  the USB event thread and worsened starvation).
- RT policy is strengthened (non-preemptible, 0.9 ms compute) - keep it; it
  improves recovery time even though it does not eliminate the deep spikes.

The remaining load-induced jitter is the user-space isoc-delivery ceiling
described above. It is not fixable by more buffer, more priority, or memory
locking - all tested negative.

## 5. Options to actually beat the jitter (ranked by leverage vs effort)

**A. Low-latency IOUSBLib isoc path (highest leverage, most work).**
Replace the libusb streaming path with direct IOUSBLib calls using
`LowLatencyReadIsochPipeAsync` / `LowLatencyWriteIsochPipeAsync`, with
`LowLatencyCreateBuffer` for the data and frame-list buffers, and a sensible
`updateFrequency`. Keep libusb only for enumeration/open/claim if convenient, or
go fully native for the device. This directly attacks the proven cause: frame
data updated at interrupt time, no run-loop delivery dependency. This is the
mechanism real macOS USB-audio code uses. Effort: significant (new isoc engine),
but bounded and well-documented in IOUSBLib headers.

**B. Patch libusb's darwin backend to use the low-latency calls.**
Same idea as A, but as a local libusb fork. Smaller surface than a full native
rewrite, reuses libusb's transfer bookkeeping. Risk: the low-latency API has a
different buffer/frame-list ownership model than the standard calls, so it is not
a drop-in; the patch is non-trivial. LGPL: changes must be shareable.

**C. Reduce per-completion work / contention (cheap, partial).**
The capture ring overflows during the spike (overrun count climbs) on the coupled
full-duplex device. Trimming capture-side handling, or decoupling capture
servicing, MIGHT reduce contention enough to soften the worst spikes. Cheap to
try, but expected to be a partial mitigation at best, not a cure.

**D. Accept the current state as the user-space floor (zero effort).**
Under normal use (not hammering Spotlight) the stream is clean. The driver is
crash-proof and the engine is glitch-proof. The residual is only under extreme,
artificial load. Document it as a known limitation of user-space isoc on macOS 13.7.8.

## 6. Recommended next step

Before committing to A or B, do one cheap confirmation: instrument the libusb
darwin callback path (or add timing around the IOUSBLib completion) to confirm the
late completions carry `kIOReturnSuccess` (not underrun/error) and that the delay
is in delivery, not on the bus. If confirmed, option A is the justified
investment, and it is the path that pro macOS USB-audio takes.

## Key sources
- libusb async I/O API and isoc queueing requirement (queue multiple transfers
  ahead): libusb.sourceforge.io/api-1.0 (asyncio), sam3u_benchmark.c.
- Stock libusb uses standard ReadIsochPipeAsync/WriteIsochPipeAsync on macOS:
  libusb/libusb os/darwin_usb.c (master).
- Identical user-space stall reported and reasoned as a userspace-delivery issue:
  libusb Discussion #1531.
- Apple low-latency isoc API "updates the frame list at primary interrupt time":
  IOUSBLib InterfaceInterface docs (LowLatencyReadIsochPipeAsync /
  LowLatencyWriteIsochPipeAsync, IOUSBInterfaceInterface192+), and
  LowLatencyCreateBuffer.
