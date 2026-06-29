/*
 * V2-usbengine.c  -  IOKit isochronous USB engine for the US-122MKII
 *
 * Runs INSIDE the AudioServerPlugIn process (V2 architecture). Owns the device
 * via IOKit/IOUSBLib, runs full-duplex isochronous transfers on a dedicated
 * realtime thread, and exposes two in-process lock-free rings to the plugin.
 *
 * Why IOKit instead of libusb (V1 used libusb):
 *   - direct control of the isochronous frame scheduling and completion path
 *   - no extra abstraction layer between us and the USB host controller
 *   - native CFRunLoop integration for the async completions
 *
 * The protocol facts (endpoints, packing, rate-set control request, duplex
 * coupling) are carried verbatim from the proven V1 daemon. Only the transport
 * mechanism changed.
 *
 * [VALIDATE] markers indicate points that compile by construction but must be
 * confirmed on hardware, because IOKit iso behavior cannot be tested in the
 * Linux build container.
 *
 * Build: compiled into the V2-US122.driver bundle; links IOKit + CoreFoundation.
 */

#include "V2-usbengine.h"
/* defined in V2-US122.c: the diagnostic sweep selects the device byte format */
extern _Atomic int g_format_mode;
/* updateFrequency for the low-latency isoch calls (0-8). 0 = update frame list
   only at end of transfer; 1-8 = update every N ms. Passing 0 was suspected of
   causing the harmonic-shred distortion; tunable live via /tmp/us122_uf. */
_Atomic int g_update_freq = 1;  /* retained for log compatibility; unused by standard isoch */

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <syslog.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <mach/mach_time.h>
#endif

/* ============================ in-process ring ============================ */
/* Single-producer / single-consumer lock-free ring of device-native frames.
   Same proven design as V1's shmring, minus the shared-memory/cross-process
   parts (we are one process now, so this is just a plain buffer). */

#define RING_FRAMES 8192u              /* ~170ms @48k. V2 is single-clock, so it
                                          needs only a small buffer to smooth IO
                                          jitter, NOT V1's giant cross-process
                                          shock absorber. The 2.7s buffer caused
                                          seconds of delay. */
#define RING_MASK   (RING_FRAMES - 1u)

typedef struct {
    _Atomic uint32_t head;             /* producer writes */
    _Atomic uint32_t tail;             /* consumer reads */
    uint8_t data[RING_FRAMES * US122_FRAME_BYTES];
} ring_t;

static inline uint32_t ring_fill(const ring_t *r){
    return atomic_load_explicit(&r->head, memory_order_acquire)
         - atomic_load_explicit(&r->tail, memory_order_acquire);
}
static inline uint32_t ring_space(const ring_t *r){
    return RING_FRAMES - ring_fill(r);
}
static uint32_t ring_write(ring_t *r, const uint8_t *src, uint32_t n){
    uint32_t space = ring_space(r);
    if (n > space) n = space;
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    for (uint32_t i = 0; i < n; i++){
        uint32_t slot = (h + i) & RING_MASK;
        memcpy(r->data + slot*US122_FRAME_BYTES, src + i*US122_FRAME_BYTES, US122_FRAME_BYTES);
    }
    atomic_store_explicit(&r->head, h + n, memory_order_release);
    return n;
}
static uint32_t ring_read(ring_t *r, uint8_t *dst, uint32_t n){
    uint32_t fill = ring_fill(r);
    if (n > fill) n = fill;
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    for (uint32_t i = 0; i < n; i++){
        uint32_t slot = (t + i) & RING_MASK;
        memcpy(dst + i*US122_FRAME_BYTES, r->data + slot*US122_FRAME_BYTES, US122_FRAME_BYTES);
    }
    atomic_store_explicit(&r->tail, t + n, memory_order_release);
    return n;
}

/* ============================ engine state ============================ */

#define NUM_ISO_XFERS   32   /* outstanding iso transfers per direction. More
                                in-flight = more runway before a thread hitch
                                forces a re-anchor (which drops a time slot and
                                causes the chopped sound). With 8-uframe xfers,
                                32 in flight = 32 bus frames ~= 32ms headroom. */
#define FRAMES_PER_XFER 8    /* MICROFRAME packets per transfer. Matches the
                                proven Linux reference (ISO_PACKETS_PER_URB=8):
                                exactly 1 bus frame per transfer, the natural
                                high-speed unit. 16 spanned 2 bus frames and
                                required perfect cross-boundary scheduling.
                                NOTE: a duplicate #define below was silently
                                forcing this back to 16 in every prior build. */
#define UFRAMES_PER_FRAME 8  /* high-speed: 8 microframes per 1ms bus frame */
/* bus frames spanned by one transfer's worth of microframes. The async iso
   call takes frameStart in BUS frames (ms) but numFrames in MICROFRAMES, so
   the next transfer must be scheduled this many BUS frames later, NOT
   FRAMES_PER_XFER. Getting this wrong starves the device (we saw 1/8 rate). */
#define XFER_BUS_FRAMES (FRAMES_PER_XFER / UFRAMES_PER_FRAME)

struct us122_engine {
    pthread_t        thread;
    volatile int     thread_run;       /* engine thread loops while set */
    volatile int     want_stream;      /* plugin asked to stream */
    volatile unsigned int rate;        /* requested sample rate */

    _Atomic int      dev_state;        /* us122_dev_state_t */

    ring_t           playback;         /* plugin -> device (EP 0x02) */
    ring_t           capture;          /* device -> plugin (EP 0x81) */

    us122_state_cb   state_cb;
    void            *state_cb_ctx;

    /* pacing accumulator (frames-per-microframe), same as V1 */
    unsigned int     accum;

    /* Shared monotonic next-frame counters, one per direction. Every transfer
       is scheduled from this single sequence so packets always go to
       consecutive, in-order frame windows. Re-anchoring INDIVIDUAL transfers to
       "now+N" (the previous approach) let transfers collide and reorder under
       jitter, scrambling the stream = noise. A single counter prevents that. */
    _Atomic uint64_t pb_next_frame;
    _Atomic uint64_t cap_next_frame;

    /* --- diagnostics (atomic, logged ~1/sec) --- */
    _Atomic long     dbg_io_writes;    /* frames written by Core Audio IO */
    _Atomic long     dbg_pb_cb;        /* playback iso completions */
    _Atomic long     dbg_cap_cb;       /* capture iso completions */
    _Atomic long     dbg_pb_read;      /* frames read from ring into iso buf */
    _Atomic long     dbg_pb_underrun;  /* frames zero-filled (ring empty) */
    _Atomic long     dbg_reanchor;     /* times the schedule re-anchored (drops) */
    _Atomic long     dbg_cap_frames;   /* actual frames device sent (true rate) */
    _Atomic long     dbg_cap_short;    /* capture packets shorter than expected */
    _Atomic long     dbg_cap_zero;     /* capture packets with zero frames */
    _Atomic long     dbg_pb_lead;      /* scheduled frame minus live bus frame */
    _Atomic unsigned int dev_rate_x1000; /* measured device rate, frames/sec*1000 */

#ifdef __APPLE__
    IOUSBInterfaceInterface **intf;    /* claimed audio interface */
    IOUSBDeviceInterface   **dev;      /* the device */
    CFRunLoopSourceRef       runloop_src;
    CFRunLoopRef             runloop;  /* engine thread's runloop */
    UInt64                   frame_start_pb;
    UInt64                   frame_start_cap;
#endif
};

/* ---- presence helper ---- */
static void set_state(us122_engine_t *e, us122_dev_state_t st){
    int prev = atomic_exchange_explicit(&e->dev_state, (int)st, memory_order_release);
    if (prev != (int)st && e->state_cb)
        e->state_cb(st, e->state_cb_ctx);
}

/* ======================================================================== */
/*  The macOS-only IOKit engine. Everything inside __APPLE__ needs hardware  */
/*  validation; the structure mirrors the proven V1 libusb flow 1:1.         */
/* ======================================================================== */
#ifdef __APPLE__

/* ---- realtime scheduling for the engine thread (validated in V1) ---- */
static void make_thread_realtime(void){
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double ticks_per_ms = 1e6 * (double)tb.denom / (double)tb.numer;
    thread_time_constraint_policy_data_t pol;
    pol.period      = (uint32_t)(ticks_per_ms * 1.0);
    pol.computation = (uint32_t)(ticks_per_ms * 0.6);
    pol.constraint  = (uint32_t)(ticks_per_ms * 1.0);
    pol.preemptible = 1;
    thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
        (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

/* ---- sample-rate control request (exact bytes from V1) ----
   bmRequestType = class, host->device, endpoint recipient (0x22)
   bRequest      = SET_CUR (0x01)
   wValue        = SAMPLING_FREQ_CONTROL (0x0100)
   wIndex        = endpoint (capture EP 0x81)
   payload       = 3-byte little-endian sample rate */
static int engine_set_rate(us122_engine_t *e, unsigned int rate){
    unsigned char p[3];
    switch(rate){
        case 44100: p[0]=0x44;p[1]=0xAC;p[2]=0x00; break;
        case 48000: p[0]=0x80;p[1]=0xBB;p[2]=0x00; break;
        case 88200: p[0]=0x88;p[1]=0x58;p[2]=0x01; break;
        case 96000: p[0]=0x00;p[1]=0x77;p[2]=0x01; break;
        default: return -1;
    }
    IOUSBDevRequest req;
    req.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBEndpoint);
    req.bRequest      = 0x01;            /* SET_CUR */
    req.wValue        = 0x0100;          /* SAMPLING_FREQ_CONTROL */
    req.wIndex        = US122_EP_CAPTURE;/* endpoint */
    req.wLength       = 3;
    req.pData         = p;
    /* [VALIDATE] control request must go to the DEVICE interface; on some
       devices the endpoint-recipient rate-set must target the interface's
       device handle. V1 sent it on the device handle via libusb control. */
    IOReturn kr = (*e->dev)->DeviceRequest(e->dev, &req);
    return (kr == kIOReturnSuccess) ? 0 : -1;
}

/* ---- iso completion callbacks ----
   IOKit calls these on the engine thread's runloop when an iso transfer
   batch completes. We refill (playback) or drain (capture) and resubmit.
   This mirrors V1's playback_cb / capture_cb but using IOKit frame lists. */

typedef struct {
    us122_engine_t          *e;
    int                      is_playback;
    UInt8                    pipe_ref;   /* IOKit pipe index */
    IOUSBIsocFrame          *frames;     /* standard isoch frame list (plain array) */
    void                    *buffer;     /* data buffer for this xfer */
    UInt64                   next_frame; /* USB frame number to schedule on */
} iso_xfer_t;

/* per-packet accessors (standard isoch frame list) */
static inline void xfer_set_reqcount(iso_xfer_t *x, int i, UInt32 bytes){
    x->frames[i].frReqCount = (UInt16)bytes;
}
static inline UInt32 xfer_get_actcount(iso_xfer_t *x, int i){
    return x->frames[i].frActCount;
}

/* Submit one transfer on the standard isoch transport. */
static IOReturn xfer_submit(us122_engine_t *e, iso_xfer_t *x, IOAsyncCallback1 cb){
    if (x->is_playback)
        return (*e->intf)->WriteIsochPipeAsync(e->intf, x->pipe_ref,
              x->buffer, x->next_frame, FRAMES_PER_XFER, x->frames, cb, x);
    else
        return (*e->intf)->ReadIsochPipeAsync(e->intf, x->pipe_ref,
              x->buffer, x->next_frame, FRAMES_PER_XFER, x->frames, cb, x);
}

/* fill one playback transfer's buffer from the ring, set per-packet lengths.
   Same pacing accumulator as V1 (frames per microframe). Tight-packs data:
   contiguous bytes, only lengths vary, which V1 proved is required. */
static void fill_playback_xfer(us122_engine_t *e, iso_xfer_t *x){
    uint8_t *buf = (uint8_t*)x->buffer;
    /* Zero the entire buffer first so any bytes between a packet's real data
       and its MAX_PACKET_SIZE slot end are silence, never stale garbage. If the
       controller transmits the full stride, this prevents noise in the tails. */
    memset(buf, 0, (size_t)FRAMES_PER_XFER * US122_MAX_PACKET_SIZE);
    /* Pace playback by the DEVICE'S measured rate, not the nominal 48000. Core
       Audio's clock (48000) and the device crystal (~47750 here) are different
       clocks even in one process. Sizing packets at 48000 while the device
       consumes ~47750 overruns the device FIFO ~250 frames/sec = periodic
       glitch on sustained tones (harmonic shred). e->dev_rate is updated from
       capture, the device's own clock. Falls back to nominal until measured. */
    unsigned int pace = atomic_load_explicit(&e->dev_rate_x1000, memory_order_relaxed);
    unsigned int rate_use = pace ? pace : (e->rate * 1000);
    for (int i = 0; i < FRAMES_PER_XFER; i++){
        e->accum += rate_use;
        int frames = e->accum / (US122_MICROFRAMES_PER_SEC * 1000);
        e->accum %= (US122_MICROFRAMES_PER_SEC * 1000);
        int bytes = frames * US122_FRAME_BYTES;
        if (bytes > US122_MAX_PACKET_SIZE){
            frames = US122_MAX_PACKET_SIZE / US122_FRAME_BYTES;
            bytes  = frames * US122_FRAME_BYTES;
        }
        uint8_t *slot = buf + (size_t)i * US122_MAX_PACKET_SIZE;
        uint32_t got = ring_read(&e->playback, slot, (uint32_t)frames);
        atomic_fetch_add_explicit(&e->dbg_pb_read, (long)got, memory_order_relaxed);
        if ((uint32_t)frames > got)
            atomic_fetch_add_explicit(&e->dbg_pb_underrun, (long)(frames-got), memory_order_relaxed);
        xfer_set_reqcount(x, i, (UInt32)bytes);
    }
}

static void iso_playback_cb(void *refcon, IOReturn result, void *arg0){
    (void)arg0;
    iso_xfer_t *x = (iso_xfer_t*)refcon;
    us122_engine_t *e = x->e;
    atomic_fetch_add_explicit(&e->dbg_pb_cb, 1, memory_order_relaxed);
    if (!e->thread_run || !e->want_stream) return;
    if (result != kIOReturnSuccess && result != kIOReturnUnderrun){
        /* [VALIDATE] treat persistent errors as device loss, mirroring V1's
           NO_DEVICE handling. kIOReturnUnderrun is normal for iso OUT. */
        set_state(e, US122_DEV_ABSENT);
        return;
    }
    fill_playback_xfer(e, x);
    /* Draw the next frame window from the SHARED monotonic counter so all
       transfers stay in strict order. Only if the whole stream has fallen
       behind the bus do we jump the shared counter forward (once), keeping all
       transfers aligned rather than scattering them. */
    uint64_t f = atomic_fetch_add_explicit(&e->pb_next_frame, XFER_BUS_FRAMES, memory_order_relaxed);
    UInt64 nowframe = 0; AbsoluteTime at;
    if ((*e->intf)->GetBusFrameNumber(e->intf, &nowframe, &at) == kIOReturnSuccess){
        /* record drift: how far ahead of the live bus our scheduled frame is.
           A healthy stream holds a roughly constant lead; a growing or shrinking
           lead means our frame counter is drifting out of phase with the bus,
           which would place packets slightly early/late = harmonic shred. */
        long lead = (long)((int64_t)f - (int64_t)nowframe);
        atomic_store_explicit(&e->dbg_pb_lead, lead, memory_order_relaxed);
        if (lead < 3){  /* standard isoch needs comfortable margin, not just >0 */
            uint64_t newbase = nowframe + 6;
            atomic_fetch_add_explicit(&e->dbg_reanchor, 1, memory_order_relaxed);
            atomic_store_explicit(&e->pb_next_frame, newbase + XFER_BUS_FRAMES, memory_order_relaxed);
            f = newbase;
        }
    }
    x->next_frame = f;
    xfer_submit(e, x, iso_playback_cb);
}

static void iso_capture_cb(void *refcon, IOReturn result, void *arg0){
    (void)arg0;
    iso_xfer_t *x = (iso_xfer_t*)refcon;
    us122_engine_t *e = x->e;
    atomic_fetch_add_explicit(&e->dbg_cap_cb, 1, memory_order_relaxed);
    if (!e->thread_run || !e->want_stream) return;
    if (result != kIOReturnSuccess && result != kIOReturnOverrun){
        set_state(e, US122_DEV_ABSENT);
        return;
    }
    /* push received frames into the capture ring (only if a consumer is
       draining; otherwise discard cheaply, the V1 lesson that prevented the
       full-ring thrash from starving playback). */
    uint8_t *buf = (uint8_t*)x->buffer;
    uint32_t off = 0;
    if (ring_space(&e->capture) > 0){
        for (int i = 0; i < FRAMES_PER_XFER; i++){
            uint32_t len = xfer_get_actcount(x, i);
            uint32_t frames = len / US122_FRAME_BYTES;
            atomic_fetch_add_explicit(&e->dbg_cap_frames, (long)frames, memory_order_relaxed);
            /* At 48k we expect 6 frames/packet. Fewer = device clock is slower
               OR a short/dropped packet. Counting these separates the two. */
            if (frames == 0) atomic_fetch_add_explicit(&e->dbg_cap_zero, 1, memory_order_relaxed);
            else if (frames < 6) atomic_fetch_add_explicit(&e->dbg_cap_short, 1, memory_order_relaxed);
            if (frames) ring_write(&e->capture, buf + off, frames);
            xfer_set_reqcount(x, i, US122_MAX_PACKET_SIZE); /* reset for next */
            off += US122_MAX_PACKET_SIZE;
        }
    } else {
        /* no consumer: empty the ring cheaply, do no per-frame work */
        atomic_store_explicit(&e->capture.tail,
            atomic_load_explicit(&e->capture.head, memory_order_relaxed),
            memory_order_release);
        for (int i = 0; i < FRAMES_PER_XFER; i++)
            xfer_set_reqcount(x, i, US122_MAX_PACKET_SIZE);
    }
    uint64_t f = atomic_fetch_add_explicit(&e->cap_next_frame, XFER_BUS_FRAMES, memory_order_relaxed);
    UInt64 nowframe = 0; AbsoluteTime at;
    if ((*e->intf)->GetBusFrameNumber(e->intf, &nowframe, &at) == kIOReturnSuccess){
        long lead = (long)((int64_t)f - (int64_t)nowframe);
        if (lead < 3){
            uint64_t newbase = nowframe + 6;
            atomic_fetch_add_explicit(&e->dbg_reanchor, 1, memory_order_relaxed);
            atomic_store_explicit(&e->cap_next_frame, newbase + XFER_BUS_FRAMES, memory_order_relaxed);
            f = newbase;
        }
    }
    x->next_frame = f;
    xfer_submit(e, x, iso_capture_cb);
}

/* ---- device discovery + open ----
   Find the US-122MKII by VID/PID, open it, claim interface 1, set alt 1,
   set the sample rate, allocate low-latency iso buffers, and prime the
   transfer ring. Returns 0 on success. [VALIDATE] the whole open path. */
static int open_device_and_stream(us122_engine_t *e);   /* fwd */
static void close_device(us122_engine_t *e);             /* fwd */
static int find_device(IOUSBDeviceInterface ***outDev);  /* fwd (defined in .inc) */

/* cheap check: is the hardware on the bus right now? Used for presence
   (IsAlive) independent of streaming. Does not open the device. */
static int hardware_present(void){
    IOUSBDeviceInterface **dev = NULL;
    if (find_device(&dev) && dev){
        (*dev)->Release(dev);
        return 1;
    }
    return 0;
}

/* ---- engine thread: owns the runloop that services iso completions ---- */
static void *engine_thread(void *arg){
    us122_engine_t *e = (us122_engine_t*)arg;
    make_thread_realtime();
    e->runloop = CFRunLoopGetCurrent();

    int streaming = 0;

    while (e->thread_run){
        int present = (atomic_load_explicit(&e->dev_state, memory_order_acquire) == US122_DEV_PRESENT);

        /* --- presence tracking (independent of streaming) --- */
        if (!streaming){
            /* when not streaming, poll the bus so the device appears/disappears
               in the system list as it is plugged/unplugged. */
            int now = hardware_present();
            if (now && !present)      set_state(e, US122_DEV_PRESENT);
            else if (!now && present) set_state(e, US122_DEV_ABSENT);
        }

        /* --- streaming start/stop --- */
        if (e->want_stream && !streaming){
            if (open_device_and_stream(e) == 0){
                streaming = 1;
                set_state(e, US122_DEV_PRESENT);   /* definitely present now */
                syslog(LOG_NOTICE, "V2 engine: streaming loop active");
            } else {
                /* not ready (maybe unplugged): wait and retry */
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, true);
                continue;
            }
        }
        if (!e->want_stream && streaming){
            close_device(e);
            streaming = 0;
            /* re-probe presence on next loop iteration */
        }

        /* service iso completions (while streaming) or just idle-poll. A short
           timeout keeps presence polling responsive without busy-spinning. */
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, streaming ? 0.1 : 0.5, true);

        /* ~1/sec: read the selected format mode from the control file and log
           counters. The menu script writes a single integer to /tmp/us122_fmt;
           we apply it live (all current modes are 6 bytes/frame). */
        if (streaming){
            static double last = 0;
            double now = (double)mach_absolute_time();
            static double t2ns = 0;
            if (t2ns == 0){ mach_timebase_info_data_t tb; mach_timebase_info(&tb); t2ns = (double)tb.numer/(double)tb.denom; }
            double now_s = now * t2ns / 1e9;
            if (now_s - last >= 1.0){
                double dt = now_s - last;
                last = now_s;
                /* measure the device's true rate from how fast capture frames
                   actually arrive (capture is clocked by the device crystal),
                   and feed it to playback pacing so we stop overrunning the
                   device FIFO. Smooth lightly to reject jitter. */
                static long cap_prev = 0;
                long cap_now = atomic_load_explicit(&e->dbg_cap_frames, memory_order_relaxed);
                if (cap_prev > 0 && dt > 0.2){
                    double meas = (double)(cap_now - cap_prev) / dt;       /* frames/sec */
                    if (meas > 30000.0 && meas < 200000.0){                /* sane guard */
                        unsigned int prev = atomic_load_explicit(&e->dev_rate_x1000, memory_order_relaxed);
                        unsigned int measx = (unsigned int)(meas * 1000.0 + 0.5);
                        unsigned int smoothed = prev ? (unsigned int)(0.7*prev + 0.3*measx) : measx;
                        atomic_store_explicit(&e->dev_rate_x1000, smoothed, memory_order_relaxed);
                    }
                }
                cap_prev = cap_now;
                int newmode = -1;
                FILE *cf = fopen("/tmp/us122_fmt", "r");
                if (cf){ if (fscanf(cf, "%d", &newmode) != 1) newmode = -1; fclose(cf); }
                if (newmode >= 0 && newmode <= 6){
                    int cur = atomic_load_explicit(&g_format_mode, memory_order_relaxed);
                    if (newmode != cur){
                        atomic_store_explicit(&g_format_mode, newmode, memory_order_relaxed);
                        syslog(LOG_NOTICE, "V2 FMT >>> mode now %d <<<", newmode);
                    }
                }
                syslog(LOG_NOTICE,
                    "V2 engine: mode=%d devrate=%u lead=%ld io_writes=%ld pb_cb=%ld cap_cb=%ld pb_read=%ld pb_underrun=%ld cap_frames=%ld pb_fill=%u",
                    atomic_load_explicit(&g_format_mode, memory_order_relaxed),
                    atomic_load_explicit(&e->dev_rate_x1000, memory_order_relaxed),
                    atomic_load_explicit(&e->dbg_pb_lead, memory_order_relaxed),
                    atomic_load_explicit(&e->dbg_io_writes, memory_order_relaxed),
                    atomic_load_explicit(&e->dbg_pb_cb, memory_order_relaxed),
                    atomic_load_explicit(&e->dbg_cap_cb, memory_order_relaxed),
                    atomic_load_explicit(&e->dbg_pb_read, memory_order_relaxed),
                    atomic_load_explicit(&e->dbg_pb_underrun, memory_order_relaxed),
                    atomic_load_explicit(&e->dbg_cap_frames, memory_order_relaxed),
                    ring_fill(&e->playback));
            }
        }

        /* if streaming but the device vanished, the iso callbacks set ABSENT;
           tear down so we return to presence-polling and can recover. */
        if (streaming && atomic_load_explicit(&e->dev_state, memory_order_acquire) == US122_DEV_ABSENT){
            close_device(e);
            streaming = 0;
        }
    }
    if (streaming) close_device(e);
    set_state(e, US122_DEV_ABSENT);
    return NULL;
}

/* The detailed IOKit open/claim/iso-alloc lives below. It is verbose; the
   shape mirrors Apple's USB iso sample code and the V1 sequence. */
#include "V2-usbengine-iokit.inc"

#else  /* !__APPLE__ : Linux build stub so the structure compiles/reviews */

static int  open_device_and_stream(us122_engine_t *e){ (void)e; return -1; }
static void close_device(us122_engine_t *e){ (void)e; }
static void *engine_thread(void *arg){
    us122_engine_t *e = (us122_engine_t*)arg;
    while (e->thread_run){ struct timespec ts={0,100000000}; nanosleep(&ts,NULL); }
    return NULL;
}

#endif /* __APPLE__ */

/* ============================ public API ============================ */

us122_engine_t *us122_engine_create(void){
    us122_engine_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->rate = 48000;
    atomic_store_explicit(&e->dev_state, US122_DEV_ABSENT, memory_order_relaxed);
    e->thread_run = 1;
    if (pthread_create(&e->thread, NULL, engine_thread, e) != 0){
        free(e);
        return NULL;
    }
    return e;
}

void us122_engine_destroy(us122_engine_t *e){
    if (!e) return;
    e->want_stream = 0;
    e->thread_run  = 0;
#ifdef __APPLE__
    if (e->runloop) CFRunLoopWakeUp(e->runloop);
#endif
    pthread_join(e->thread, NULL);
    free(e);
}

int us122_engine_start(us122_engine_t *e, unsigned int rate){
    if (!e) return -1;
    e->rate = rate;
    e->want_stream = 1;
    e->accum = 0;
#ifdef __APPLE__
    if (e->runloop) CFRunLoopWakeUp(e->runloop);
#endif
    return 0;
}

void us122_engine_stop(us122_engine_t *e){
    if (!e) return;
    e->want_stream = 0;
#ifdef __APPLE__
    if (e->runloop) CFRunLoopWakeUp(e->runloop);
#endif
}

us122_dev_state_t us122_engine_state(us122_engine_t *e){
    if (!e) return US122_DEV_ABSENT;
    return (us122_dev_state_t)atomic_load_explicit(&e->dev_state, memory_order_acquire);
}

uint32_t us122_engine_play_write(us122_engine_t *e, const uint8_t *frames, uint32_t n){
    if (!e) return 0;
    uint32_t w = ring_write(&e->playback, frames, n);
    atomic_fetch_add_explicit(&e->dbg_io_writes, (long)w, memory_order_relaxed);
    return w;
}
uint32_t us122_engine_cap_read(us122_engine_t *e, uint8_t *frames, uint32_t n){
    if (!e) return 0;
    return ring_read(&e->capture, frames, n);
}
uint32_t us122_engine_play_fill(us122_engine_t *e){
    if (!e) return 0;
    return ring_fill(&e->playback);
}
void us122_engine_set_state_cb(us122_engine_t *e, us122_state_cb cb, void *ctx){
    if (!e) return;
    e->state_cb = cb;
    e->state_cb_ctx = ctx;
}
