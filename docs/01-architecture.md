# 01 - Architecture (V1, the shipping system)

This is the complete layout of the current, working driver. Read it before the
history or the roadmap, because both refer back to these pieces.

## The shape in one paragraph

V1 is **two processes bridged by POSIX shared memory.** A root daemon (`us122d`)
owns the USB device through libusb and runs the isochronous transfer engine. A
Core Audio HAL plugin (`US122.driver`), loaded inside the system process
`coreaudiod`, presents the audio device to macOS and does the format conversion.
They share two ring buffers (playback and capture) in a named shm segment. A
Swift menu-bar app (`US122Menu`) is the user-facing on/off and status control.
Because the two processes have independent clocks, an async resampler in the
daemon reconciles them.

```
   ┌─────────────────────────────────────────────────────────────────┐
   │ macOS 10.13 (Intel)                                              │
   │                                                                  │
   │   audio apps ──► coreaudiod ──► US122.driver (HAL plugin)        │
   │                                   │  float<->6-byte conversion   │
   │                                   │  presents the device         │
   │                                   ▼                              │
   │                            /us122_shm  (POSIX shared memory)      │
   │                         ┌────────────────────────┐               │
   │                         │ playback ring (frames) │               │
   │                         │ capture  ring (frames) │               │
   │                         │ header: rate, flags... │               │
   │                         └────────────────────────┘               │
   │                                   ▲                              │
   │                                   │  resampler + RT USB engine   │
   │   us122d (root daemon, libusb) ───┘                              │
   │       │                                                          │
   │       ▼ isochronous USB (EP 0x02 OUT, EP 0x81 IN)                │
   │   ┌──────────────────────┐                                       │
   │   │ TASCAM US-122MKII     │  (the physical USB interface)        │
   │   └──────────────────────┘                                       │
   │                                                                  │
   │   US122Menu.app (menu-bar agent) ──► writes shm flags (on/off,   │
   │                                       hide, buffer_ms), reads     │
   │                                       status for the colored dot  │
   └─────────────────────────────────────────────────────────────────┘
```

## Why two processes (this is the central design fact)

The honest reason is **the coreaudiod sandbox question.** A HAL plugin runs
inside `coreaudiod`, a sandboxed Apple system daemon. Whether that sandbox lets
the plugin open a raw USB device was, at V1 design time, unknown and risky. V1
sidesteps the question entirely: USB lives in a normal **root** process (the
daemon), which can unquestionably open the device. The plugin never touches USB;
it only touches shared memory.

The cost of that choice is the hard part of V1: **two independent clocks.** The
daemon is paced by the USB device's crystal. `coreaudiod` is paced by the
system audio clock. They drift. Reconciling them is what the resampler does, and
that resampler plus the cross-process priority fight is the entire reason V2 was
attempted (see `docs/02-history-v1-v2.md`).

(Spoiler from the history: V2 later **proved** coreaudiod CAN open USB on 10.13.
So the sandbox fear was unfounded. But V1 shipped on the safe assumption, and V1
works, so it stands.)

## Component 1: the daemon (`src/daemon/us122d.c`, ~950 lines)

The USB engine. Runs as root under launchd.

**Responsibilities:**
- Own the USB device: enumerate, open, claim interface 1, set alt setting 1, set
  the sample rate via a control transfer.
- Run the isochronous transfer engine: 16 URBs (USB Request Blocks) per
  direction, 16 packets each, submitted ahead and resubmitted in their
  completion callbacks. This keeps the bus continuously fed.
- Do ZERO format conversion. It moves device-native 6-byte frames to and from
  the shm rings exactly as the wire carries them.
- Run the **async resampler**: read the playback ring at a fractional rate that
  tracks a fill-level setpoint, to absorb the clock drift between the device and
  coreaudiod. This is a linear-interpolation resampler with a slewed ratio held
  near 1.0 (clamped to +-1% so it can never bend pitch audibly).
- Manage the **device lifecycle**: detect unplug (three ways: transfer status
  NO_DEVICE, rate-set failure, and a fresh-enumeration idle probe), recover on
  replug, park dormant when the app is inactive.
- Promote its USB event thread to **realtime** scheduling (mach time-constraint
  policy) so the transfers get serviced on time.
- Emit a diagnostic log line every 250ms when `US122_DEBUG=1` (set by the
  plist), carrying callback rates, ring fills, underrun/overrun counts, and the
  jitter metrics added this session (gap_max, qmin, late).

**The threading model (important for the roadmap):** the daemon drives libusb
events itself by calling `libusb_handle_events_timeout` in the stream loop on a
single thread, which it promotes to realtime. libusb does NOT spawn a hidden
event thread here. That single thread is where USB completions are serviced.

**Key constants (all in `us122d.c` near the top):**
- `NUM_URBS = 16`, `ISO_PACKETS_PER_URB = 16`
- `MAX_PACKET_SIZE = 78` bytes (13 frames of 6 bytes)
- endpoints `EP_PLAYBACK = 0x02`, `EP_CAPTURE = 0x81`
- the realtime policy values in `make_thread_realtime()`

## Component 2: the shared-memory contract (`src/daemon/shmring.h`, v6)

The boundary between the two processes. Both the daemon and the plugin include
this header, and the plugin build copies the daemon's copy in so they never
diverge.

**Layout (`shm_layout_t`), all fields verified by offset computation:**
```
magic           @0    0x55533132 ("US12")
version         @4    SHM_VERSION = 6
rate            @8    current sample rate (Hz)
running         @12   plugin sets 1 while streaming, 0 when stopped
dev_frames      @16   (64-bit) device frame counter
device_present  @24   daemon sets 1 when the USB device is held & claimed
user_hide       @28   menu app sets 1 to hide the virtual device
app_active      @32   menu app: 1 = app running (daemon active), 0 = park dormant
status[96]      @36   daemon writes a short status line (OFF the realtime path)
buffer_ms       @132  menu app: target buffer depth in ms (the latency knob)
                      followed by the two rings
```
- `SHM_MAGIC = 0x55533132`, `SHM_NAME = "/us122_shm"`
- `FRAME_BYTES = 6`, `RING_FRAMES = 131072` (~2.7s at 48k)
- `BUFFER_MS_DEFAULT = 512`, min 100, max 2000
- `sizeof(shm_layout_t) = 1573032`

**Flag ownership is strict** (who writes what):
- daemon owns: `device_present`, `dev_frames`, `status`
- menu app owns: `user_hide`, `app_active`, `buffer_ms`
- plugin owns: `running`
- The plugin advertises the device only when `device_present == 1 AND
  user_hide == 0`.

**Version self-heals:** a version mismatch makes the daemon unlink and recreate
the segment rather than crash-loop, so bumping the version is safe.

## Component 3: the HAL plugin (`src/plugin/US122.driver`, ~650 lines C)

The Core Audio AudioServerPlugIn. Loaded inside `coreaudiod`.

**Responsibilities:**
- Implement the AudioServerPlugIn COM interface: the property model that lets
  macOS enumerate and present one full-duplex device (the object model is large
  and hand-written; it answers the properties coreaudiod queries).
- In its realtime IO operation (`DoIOOperation`), convert between Core Audio's
  float samples and the device-native 6-byte frames, reading/writing the shm
  rings. The conversion is the 24-bit "high 3 bytes of a 32-bit sample" packing
  (`float_to_dev` / `dev_to_float`).
- Present the device dynamically: a presence watcher thread polls the shm
  presence flag and fires the Core Audio property-changed notification so the
  device appears/disappears in Sound settings on plug/unplug and on the menu
  app's hide toggle.
- Re-assert `running` and the rate at the top of the IO operation so audio
  resumes automatically after a device reconnect.

**Sandbox caveat:** the plugin cannot log or do IPC reliably from inside the
coreaudiod sandbox; shm is the one channel that works. Device-list change
notifications are reliable in System Settings > Sound but finicky in some apps.

**Bundle identity:** `CFBundleIdentifier = net.senesh.us122.driver`, factory
`US122_Create`, factory UUID in `Info.plist` must match `kFactoryUUID` in source.

## Component 4: the menu-bar app (`src/menu/US122Menu`, ~300 lines Swift)

A `LSUIElement` (no dock icon) menu-bar agent. The user-facing control hub.

**Responsibilities:**
- Show a colored dot: green = streaming, amber = idle, hollow/gray = device
  absent, blinking yellow = starting (5s grace).
- Four info lines: Device Connected/Disconnected, macOS Driver On/Off, Streaming
  On@NHz/Off, Daemon Running/Not running.
- Be the on/off hub: sets `app_active = 1` on launch, `0` on quit (daemon parks
  dormant while inactive). Sets `user_hide = 0` on launch (device appears), `1`
  on quit with a ~350ms grace so the watcher notifies coreaudiod (device
  disappears).
- A **Buffer submenu**: ms presets [128, 256, 512, 1024, 1536], checkmark on the
  active value, grayed while streaming (shows "Buffer: N ms" then), writes
  `buffer_ms`. Applies on the next stream start.
- Confirm dialog on quit-while-streaming.

**Two Swift-specific gotchas baked into the build** (do not regress these):
- Swift cannot call the variadic `shm_open`, so a tiny C shim
  (`shimopen.c` + `bridging.h`) wraps it non-variadically.
- `swiftc` does NOT compile `.c` files; the shim is compiled with `clang`
  first and the `.o` linked in (see `build_us122menu.sh`).
- The update timer is registered in `.common` run-loop mode so the menu updates
  live while open (otherwise it freezes during event tracking).

## The audio clock model (the thing V2 tried to simplify)

The US-122MKII is **duplex-coupled**: one physical crystal clocks both capture
and playback. Its true rate is not exactly nominal (measured around 47718 fps
when set to 48000 in V1). In V1:
- The device is the de-facto clock master.
- The daemon's resampler reads the playback ring at a fractional rate driven
  toward a fill setpoint (`buffer_ms` worth of frames), which absorbs the drift
  between the device's real rate and coreaudiod's 48000 pacing.
- Capture frames land in the capture ring at the device's real rate; the plugin
  consumes them at coreaudiod's rate; the same resampling tolerance covers it.

This works. It is also the complexity V2 set out to delete by collapsing both
clocks into one process. Read the history for how that went.

## Build and install (runs on the Mac, not here)

The scripts in `src/build/` build the bundle, daemon, and menu app, and PRINT
the sudo install commands (they do not run them). The canonical daemon-only
reinstall, the full reinstall (when the plugin/plist/shm-version changed), and
the menu rebuild are all documented in `docs/03-current-state.md`. The build
scripts assume libusb from Homebrew (`brew --prefix libusb`).
