# CLAUDE.md

Operating instructions for Claude Code on this project. Read `docs/00-START-HERE.md`
for orientation; this file is the rules-of-engagement summary.

## What this project is

A from-scratch macOS 13.7.8 Ventura (Intel) audio driver for the TASCAM US-122MKII USB
interface. Working today. Active work: a low-latency isochronous USB rewrite to
cure load-induced jitter (`docs/04-roadmap-lowlatency-isoc.md`).

## Communication rules (the user's stated preferences, firm)

- **English always**, even for Hebrew-content tasks.
- **Never use em dashes.** Use commas, parentheses, or restructure.
- **No bridge phrases.** Do not open with "Since you...", "Based on your...",
  "Given your interest in...". Integrate context silently.
- **No apologies, no moralizing, no corporate filler, no AI self-references.**
  Direct and human.
- **On "this is broken" or an abrupt correction: reset immediately.** Stop the
  current line, restate the core objective and the specific missed constraint,
  strip to the minimal functional requirement, deliver one concrete correct
  thing. Do not defend the mistake.
- **Restate constraints before executing. Flag tradeoffs and wrong assumptions
  proactively.** Output must survive a senior technical or design review:
  concrete, assumption-free.
- **Layered explanations.** Do not assume inner-professional abbreviations;
  define them once.
- **Use JSON for structured context, bullet points with bold leads for logic.**
- **Include options outside the obvious** to prevent narrow-focus tunneling, then
  give a clear recommendation.

## The hard environmental constraint

**You cannot test on the hardware.** The US-122MKII is on the user's physical Mac.
Your environment has no IOKit, no CoreAudio, no libusb, no meaningful Swift
compiler, no USB device. Every real validation happens on the user's Mac; they
paste logs back.

Therefore:
- **Measure before changing.** Instrument, reproduce, read numbers, change one
  thing, re-measure. Never stack speculative fixes.
- **One variable per build.** The user tests each build; multi-change builds
  teach nothing about cause.
- **Guard macOS-only code** so the portable C core still compiles locally; that
  is how you catch typos before a hardware cycle.
- **Mark proven vs. needs-hardware-validation** (`[VALIDATE]` in code, as V2
  did).

## What you CAN compile here

The portable C core against a libusb stub:
```
gcc -std=c11 -D_GNU_SOURCE -I/tmp/stubs -I. -c src/daemon/us122d.c -o /tmp/us122d.o
```
Build the stub header at `/tmp/stubs/libusb-1.0/libusb.h` incrementally for the
symbols you use. Swift is only brace/paren/bracket-checked, never compiled here.
IOKit/CoreAudio do not compile here at all.

Also run the byte/brace sanity check used throughout:
```
python3 -c "s=open('FILE').read(); print('braces:', s.count('{')==s.count('}'))"
```

## Build and deliver (the user runs builds on the Mac)

- Build scripts are in `src/build/`. They print sudo install commands; they do
  not run them. Install sequences are in `docs/03-current-state.md`.
- The user installs and tests; you read the pasted log.
- **Tarball execute-bit gotcha:** tarring drops the execute bit, so the user must
  `chmod +x *.sh` after extracting. Mention it when handing off scripts.

## Where things are

- Shipping V1 source: `src/` (daemon, plugin, menu, plist, build).
- Abandoned V2 (study its low-latency iso code, it is the closest existing
  implementation): `reference/v2-abandoned/`.
- Protocol truth: `reference/linux-reference/us122mkii.c` and
  `docs/05-protocol-facts.md`.

## The one thing not to get wrong

Before writing any `LowLatency*IsochPipeAsync` code, complete Stage 0 of
`docs/04`: determine the exact low-latency buffer/frame-list layout the API
requires. V2 assumed fixed-stride (which is what standard iso uses) and
bit-crushed. That assumption is the prime suspect and the open question. Answer
it with the SDK headers (on the Mac) before building the streaming path, or you
will reproduce V2's wall.

## Git workflow for this phase

The low-latency rewrite is ~15 IOKit iterations you cannot compile locally. Use a
branch per roadmap stage. Commit each stage's working state (confirmed on the
Mac) before tuning the next. This is exactly why the project moved to Claude Code
with git.
