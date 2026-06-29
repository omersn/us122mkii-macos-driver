/*
 * shmring.h
 *
 * Shared-memory transport between the US-122MKII HAL plugin (inside
 * coreaudiod) and the helper daemon (us122d, owns the USB device).
 *
 * One POSIX shm segment holds:
 *   - a header (magic/version/rate/running flag)
 *   - a PLAYBACK ring: plugin writes, helper reads  (host -> device)
 *   - a CAPTURE  ring: helper writes, plugin reads  (device -> host)
 *
 * Each ring is single-producer / single-consumer, so index updates use
 * atomics with acquire/release ordering and need NO lock across processes.
 *
 * Frames are device-native: 6 bytes (24-bit stereo, 3 bytes/ch). The helper
 * pumps these straight to/from USB with zero conversion. The plugin converts
 * between Core Audio float buffers and these 6-byte frames.
 *
 * Both processes #include this and agree on the layout via SHM_MAGIC/VERSION.
 */

#ifndef SHMRING_H
#define SHMRING_H

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#define SHM_NAME      "/us122_shm"
#define SHM_MAGIC     0x55533132u   /* "US12" */
#define SHM_VERSION   6u            /* v6 adds buffer_ms (configurable setpoint) */
#define SHM_STATUS_LEN 96           /* daemon's last status line, for the menu app */
#define BUFFER_MS_DEFAULT 512u      /* middle preset: balanced default */
#define BUFFER_MS_MIN     100u
#define BUFFER_MS_MAX     2000u

#define FRAME_BYTES   6             /* 24-bit stereo, device-native */

/* Ring capacity in frames. ~500 ms at 96k worst case, power of two so the
   index masking is a cheap AND. 131072 frames = ~2.7 s @48k. */
#define RING_FRAMES   131072u  /* ~2.7s at 48k: deep buffer absorbs heavy-load
                                  stalls (e.g. a Chrome tab spinning up) without
                                  underrunning. Latency cost is fine for
                                  playback; setpoint sits ~1s in. */
#define RING_MASK     (RING_FRAMES - 1u)
#define RING_BYTES    (RING_FRAMES * FRAME_BYTES)

/* One single-producer/single-consumer ring. head = next write slot,
   tail = next read slot. Both monotonic counters masked into the buffer;
   difference gives fill level and never aliases for power-of-two size as
   long as fill never exceeds RING_FRAMES (we enforce that on write). */
typedef struct {
    _Atomic uint32_t head;     /* written by producer */
    _Atomic uint32_t tail;     /* written by consumer */
    uint8_t          data[RING_BYTES];
} shm_ring_t;

typedef struct {
    _Atomic uint32_t magic;
    _Atomic uint32_t version;
    _Atomic uint32_t rate;        /* sample rate plugin requests */
    _Atomic uint32_t running;     /* 1 = plugin wants streaming active */
    _Atomic uint64_t dev_frames;  /* helper's running count of frames clocked
                                     out the device; the plugin's timebase */
    _Atomic uint32_t device_present; /* helper owns: 1 = USB device held & claimed,
                                        0 = absent. Plugin reports the audio device
                                        to coreaudiod only while this is 1, so it
                                        appears/disappears with the hardware. */
    _Atomic uint32_t user_hide;      /* menu-bar app owns: 1 = user chose to HIDE
                                        the virtual device even when present. The
                                        plugin advertises the device only when
                                        device_present==1 AND user_hide==0. */
    _Atomic uint32_t app_active;     /* menu-bar app owns: 1 = app is running and
                                        wants the daemon ACTIVE (polling for and
                                        streaming the device). 0 = app closed, so
                                        the daemon parks DORMANT (no USB activity,
                                        near-zero CPU). Lets the app be the on/off
                                        hub for the whole device. */
    char             status[SHM_STATUS_LEN]; /* daemon writes a short human-readable
                                        status line here on each state transition
                                        (NOT from the realtime path), e.g.
                                        "acquired", "streaming", "device lost".
                                        The menu app shows it as the Daemon line. */
    _Atomic uint32_t buffer_ms;      /* menu-app owns: target buffer depth in ms.
                                        Daemon converts to frames using the current
                                        rate and uses it as the resampler setpoint
                                        and priming threshold. Read at stream start
                                        (changes apply to the next stream). */
    uint32_t         _pad[3];

    shm_ring_t       playback;    /* plugin -> helper */
    shm_ring_t       capture;     /* helper -> plugin */
} shm_layout_t;

/* ---- fill / space queries (safe for either side to call) ---- */

static inline uint32_t ring_fill(const shm_ring_t *r){
    uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    return h - t;                 /* unsigned wrap gives correct fill */
}
static inline uint32_t ring_space(const shm_ring_t *r){
    return RING_FRAMES - ring_fill(r);
}

/* ---- producer side: write up to n frames, return frames written ---- */
static inline uint32_t ring_produce(shm_ring_t *r, const uint8_t *src, uint32_t n){
    uint32_t space = ring_space(r);
    if (n > space) n = space;
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    for (uint32_t i = 0; i < n; i++){
        uint32_t slot = (h + i) & RING_MASK;
        memcpy(r->data + slot*FRAME_BYTES, src + i*FRAME_BYTES, FRAME_BYTES);
    }
    atomic_store_explicit(&r->head, h + n, memory_order_release);
    return n;
}

/* ---- consumer side: read up to n frames, return frames read ---- */
static inline uint32_t ring_consume(shm_ring_t *r, uint8_t *dst, uint32_t n){
    uint32_t fill = ring_fill(r);
    if (n > fill) n = fill;
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    for (uint32_t i = 0; i < n; i++){
        uint32_t slot = (t + i) & RING_MASK;
        memcpy(dst + i*FRAME_BYTES, r->data + slot*FRAME_BYTES, FRAME_BYTES);
    }
    atomic_store_explicit(&r->tail, t + n, memory_order_release);
    return n;
}

/* Consumer variant that zero-fills a shortfall (for the audio callback that
   must always deliver n frames). Returns frames of REAL data delivered. */
static inline uint32_t ring_consume_zerofill(shm_ring_t *r, uint8_t *dst, uint32_t n){
    uint32_t got = ring_consume(r, dst, n);
    if (got < n) memset(dst + got*FRAME_BYTES, 0, (n-got)*FRAME_BYTES);
    return got;
}

/* ---- one-time init by whichever process creates the segment ---- */
static inline void shm_init_layout(shm_layout_t *L, uint32_t rate){
    memset(L, 0, sizeof(*L));
    atomic_store_explicit(&L->rate, rate, memory_order_relaxed);
    atomic_store_explicit(&L->running, 0, memory_order_relaxed);
    atomic_store_explicit(&L->dev_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&L->device_present, 0, memory_order_relaxed);
    atomic_store_explicit(&L->user_hide, 0, memory_order_relaxed);
    atomic_store_explicit(&L->app_active, 0, memory_order_relaxed);
    L->status[0] = '\0';
    atomic_store_explicit(&L->buffer_ms, BUFFER_MS_DEFAULT, memory_order_relaxed);
    atomic_store_explicit(&L->version, SHM_VERSION, memory_order_relaxed);
    /* publish magic LAST: a reader seeing the right magic knows the rest is set */
    atomic_store_explicit(&L->magic, SHM_MAGIC, memory_order_release);
}

/* reader checks the segment is initialized and matches our build */
static inline int shm_valid(const shm_layout_t *L){
    return atomic_load_explicit(&L->magic, memory_order_acquire) == SHM_MAGIC
        && atomic_load_explicit(&L->version, memory_order_acquire) == SHM_VERSION;
}

#endif /* SHMRING_H */
