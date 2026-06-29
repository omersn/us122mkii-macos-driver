# 00 - START HERE

You are picking up a multi-session hardware project mid-stream. This document
orients you in five minutes. Read it fully before touching anything.

## What this project is

A working macOS audio driver for the **TASCAM US-122MKII**, a USB audio
interface that TASCAM abandoned (no driver for any modern macOS). The target
machine is **macOS 10.13 High Sierra on Intel**. The driver is built from
scratch: reverse-engineered USB protocol, a Core Audio HAL plugin, a helper
daemon, shared-memory IPC.

It already works. Audio plays and records cleanly. This is not a rescue of a
broken thing; it is the next phase of a thing that functions.

## The one thing you must understand before writing any code

This project has a **V1** (shipping, the current architecture) and an abandoned
**V2** (a rearchitecture that got 95% done then hit an unresolved wall). The
work ahead is to give V1 a specific new capability that was **the suspected
cause of V2's failure**. That is not a contradiction once you read the history,
but if you skip the history you will walk straight into the same wall V2 hit.

In one sentence: **V2 used Apple's low-latency isochronous USB API and produced
unexplained "4-bit bit-crush" distortion; the planned V1 improvement is to adopt
that same low-latency API to fix a jitter problem.** Whether those two facts
collide, and how to avoid the collision, is the heart of `docs/02-history-v1-v2.md`
and `docs/04-roadmap-lowlatency-isoc.md`. Read both before starting Stage 1.

## Read these in order

1. **`docs/01-architecture.md`** - the complete system layout. How the daemon,
   plugin, shared memory, menu app, and the audio clock fit together. Start here.
2. **`docs/02-history-v1-v2.md`** - the V1/V2 saga, exactly where and why V2
   failed, and the irony above explained in full. The most important doc.
3. **`docs/03-current-state.md`** - what works today, what this last session
   changed, and the open problem (load-induced jitter).
4. **`docs/04-roadmap-lowlatency-isoc.md`** - the staged plan for the work ahead
   (the low-latency isoc rewrite of V1's transport).
5. **`docs/05-protocol-facts.md`** - the device protocol reference card. Every
   hardware-proven constant in one place. Treat as ground truth.
6. **`docs/06-jitter-research.md`** - the external research (libusb source, Apple
   docs, community reports) that diagnosed the jitter and pointed at the fix.

## The hard constraint that shapes everything

**You cannot test on the hardware.** The US-122MKII is on the user's physical
Mac in Berlin. Your environment compiles only the portable C core against stubs;
it has no IOKit, no CoreAudio, no libusb, no Swift compiler that matters, and no
USB device. Every build is validated by the user on their Mac, then they paste
logs back. This makes the loop slow and the cost of a wrong guess high.

Consequences for how you work:
- **Measure before changing.** This project's whole method is: instrument,
  reproduce, read the numbers, change exactly one thing, re-measure. Do not
  stack speculative fixes.
- **One variable per build.** The user installs and tests each build. If you
  change three things and it improves, you have learned nothing about which.
- **Guard macOS-only code so the portable core still compiles here.** That is
  how you catch typos and logic errors before the user spends a hardware cycle.
- **Flag what is proven vs. what needs hardware validation.** V2's code marked
  these with `[VALIDATE]`. Continue that discipline.

## Working style the user expects (from their stated preferences)

- **English always.** Even for Hebrew-content tasks.
- **Never use em dashes.** Use commas, parentheses, or restructure. This is firm.
- **No bridge phrases.** Do not open with "Since you...", "Based on your...",
  "Given your interest in...". Integrate context silently and get to the point.
- **No apologies, no moralizing, no corporate filler.** Direct and human.
- **Reset immediately on "this is broken."** Stop, restate the objective and the
  missed constraint, strip to the minimal correct thing, deliver that fast. Do
  not defend a mistake.
- **Restate constraints before executing. Flag tradeoffs and wrong assumptions
  proactively.** The bar is: output must survive a senior technical review.
- **Layered explanations.** Do not assume inner-professional abbreviations are
  known; define them once.

## Where the code lives

```
src/daemon/      us122d.c, shmring.h        - the V1 USB daemon (libusb) + shm contract
src/plugin/      US122.driver/...           - the Core Audio HAL plugin
src/menu/        US122Menu/...              - the Swift menu-bar control app
src/plist/       net.senesh.us122d.plist    - launchd config for the daemon
src/build/       build_*.sh                 - build + install scripts (run on the Mac)
reference/v2-abandoned/                      - the FULL V2 source (study its iso code)
reference/linux-reference/                   - serifpersia's Linux driver (protocol truth)
tools/                                       - standalone USB probes (descriptor dump, etc.)
docs/                                        - everything above
```

## The immediate next task

Stage 1 of `docs/04-roadmap-lowlatency-isoc.md`: build a low-latency IOUSBLib
isochronous **capture** path (read-only, easiest to verify) and prove it reaches
the ring glitch-free under load. But first read docs 01, 02, and 04. The V2
buffer-layout wall is the thing to understand before you write the first
`LowLatencyReadIsochPipeAsync` call.
