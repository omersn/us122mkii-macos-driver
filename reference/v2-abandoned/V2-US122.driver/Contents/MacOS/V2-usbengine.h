/*
 * V2-usbengine.h  -  in-process IOKit USB engine for the TASCAM US-122MKII
 *
 * This is the V2 architecture. In V1 the USB engine lived in a SEPARATE
 * process (us122d) talking to the Core Audio plugin over POSIX shared memory,
 * which forced a second clock, a resampler, and cross-process priority games.
 *
 * V2 puts the USB engine INSIDE the AudioServerPlugIn process. There is ONE
 * clock (the device), no shared memory, no resampler. The plugin's IO cycle
 * and the USB transfers run in the same address space, bridged by a small
 * in-process lock-free ring.
 *
 * This header is the boundary between the plugin (US122 Core Audio object
 * model) and the engine (IOKit isochronous USB). The plugin never touches
 * IOKit; the engine never touches Core Audio. Keeping that wall clean is the
 * point of V2.
 *
 * IMPLEMENTATION NOTE: the .c uses IOKit/IOUSBLib, which only exists on macOS.
 * It cannot be compiled in a Linux container. The structure here is written to
 * be reviewable and to compile on macOS; on-hardware validation points are
 * marked with [VALIDATE] in the .c.
 */

#ifndef V2_USBENGINE_H
#define V2_USBENGINE_H

#include <stdint.h>
#include <stdbool.h>

/* ---- device protocol facts (carried verbatim from the proven V1 daemon) ---- */
#define US122_VID                 0x0644
#define US122_PID                 0x8021   /* running state, no firmware upload */
#define US122_IFACE               1
#define US122_ALT                 1
#define US122_EP_PLAYBACK         0x02     /* iso OUT */
#define US122_EP_CAPTURE          0x81     /* iso IN  */
#define US122_MAX_PACKET_SIZE     78       /* bytes per microframe packet */
#define US122_FRAME_BYTES         6        /* 24-bit stereo: 2ch * 3 bytes */
#define US122_MICROFRAMES_PER_SEC 8000

/* ---- engine status, polled by the plugin for kAudioDevicePropertyDeviceIsAlive ----
   Presence means "the hardware is on the bus", independent of whether we are
   currently streaming. The device should appear in the system list whenever it
   is plugged in, even before any app starts audio. */
typedef enum {
    US122_DEV_ABSENT  = 0,   /* hardware not on the bus */
    US122_DEV_PRESENT = 1    /* hardware present (may or may not be streaming) */
} us122_dev_state_t;

/* Opaque engine handle. One instance for the single device. */
typedef struct us122_engine us122_engine_t;

/* Lifecycle ---------------------------------------------------------------- */

/* Create the engine and begin watching for the device on a dedicated thread.
   Does NOT stream yet. Returns NULL on allocation failure. Safe to call once
   at plugin Initialize. */
us122_engine_t *us122_engine_create(void);

/* Tear down: stop streaming, release IOKit objects, join threads, free. */
void us122_engine_destroy(us122_engine_t *e);

/* Begin streaming at the given sample rate (44100/48000/88200/96000). Idempotent
   if already streaming at that rate; re-rates if different. Non-blocking: the
   engine thread performs the open/claim/stream. Returns 0 on accepted request. */
int us122_engine_start(us122_engine_t *e, unsigned int rate);

/* Stop streaming but keep watching for the device (so it can restart). */
void us122_engine_stop(us122_engine_t *e);

/* Current device presence, for the plugin's IsAlive property. Cheap, lock-free. */
us122_dev_state_t us122_engine_state(us122_engine_t *e);

/* Data path ---------------------------------------------------------------- */
/* The plugin calls these from its realtime IO operation. They are lock-free
   (single-producer/single-consumer against the engine's USB thread) and never
   block.

   Playback: plugin PRODUCES device-native frames (6 bytes each) for the engine
   to send to EP 0x02. Returns frames actually accepted (may be < n if full).

   Capture: plugin CONSUMES device-native frames the engine received on EP 0x81.
   Returns frames actually delivered (may be < n if empty; caller zero-fills). */
uint32_t us122_engine_play_write(us122_engine_t *e, const uint8_t *frames, uint32_t n);
uint32_t us122_engine_cap_read  (us122_engine_t *e, uint8_t *frames, uint32_t n);

/* Current playback ring fill in frames, for the plugin's startup priming and
   optional diagnostics. */
uint32_t us122_engine_play_fill(us122_engine_t *e);

/* Optional: register a callback fired (from the engine thread) when device
   presence changes, so the plugin can post a Core Audio property-changed
   notification immediately rather than waiting for a poll. May be NULL. */
typedef void (*us122_state_cb)(us122_dev_state_t st, void *ctx);
void us122_engine_set_state_cb(us122_engine_t *e, us122_state_cb cb, void *ctx);

#endif /* V2_USBENGINE_H */
