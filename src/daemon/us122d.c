/*
 * us122d.c
 *
 * Helper daemon for the TASCAM US-122MKII HAL driver.
 * Owns the USB device and runs the proven tight-packed full-duplex engine.
 *
 * Data source/sink is the shared-memory segment (shmring.h), NOT a file:
 *   - pulls playback frames from shm playback ring  -> device EP 0x02
 *   - pushes captured frames from device EP 0x81     -> shm capture ring
 *
 * Lifecycle:
 *   - creates the shm segment if absent (and initializes it), else attaches
 *   - waits for header.running == 1, then streams
 *   - stops when running -> 0, or on SIGINT/SIGTERM
 *
 * The helper does ZERO format conversion. Frames in shm are device-native
 * 6-byte (24-bit stereo). Whoever fills playback (the plugin, or the test
 * writer) is responsible for producing device-native bytes.
 *
 * Build (macOS):
 *   clang us122d.c -o us122d -lpthread \
 *       -I"$(brew --prefix libusb)/include/libusb-1.0" \
 *       -L"$(brew --prefix libusb)/lib" -lusb-1.0
 *
 * Run:
 *   ./us122d            # creates /us122_shm, waits for running flag, streams
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#define _DARWIN_C_SOURCE 1
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <pthread.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <mach/mach_time.h>
#endif
#include <libusb-1.0/libusb.h>
#include "shmring.h"

#define US122D_VERSION "build6-buffer (shm v6)"

/* timestamped log line to stderr (the .err log). Format: HH:MM:SS.mmm msg */
#include <stdarg.h>
#include <errno.h>
static void logts(const char *fmt, ...){
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm tmv; localtime_r(&tv.tv_sec, &tmv);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    fprintf(stderr, "[%s.%03d] ", ts, (int)(tv.tv_usec/1000));
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#define VID 0x0644
#define PID 0x8021
#define IFACE 1
#define ALT 1
#define EP_PLAYBACK 0x02
#define EP_CAPTURE  0x81

#define MAX_PACKET_SIZE     78
#define ISO_PACKETS_PER_URB 16
#define NUM_URBS            16
#define MICROFRAMES_PER_SEC 8000

#define RT_H2D_CLASS_EP (LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_ENDPOINT)
#define UAC_SET_CUR 0x01
#define UAC_SAMPLING_FREQ_CONTROL 0x0100

/* While the app is ACTIVE but no device is plugged in, how often to re-check the
   USB bus. 1.5s balances responsiveness (device noticed within ~1.5s of plugging
   in) against keeping background activity low. Heartbeat log every ~6s. */
#define ACQUIRE_POLL_US        1500000
#define ACQUIRE_HEARTBEAT_TICKS 4

static shm_layout_t *g_shm = NULL;

/* Publish a short status line into shm for the menu app to display. Called only
   from lifecycle/transition code, NEVER from the realtime transfer path, so it
   has zero effect on audio. Plain bounded copy, no lock, no allocation. */
static void set_status(const char *s){
    if (!g_shm) return;
    strncpy(g_shm->status, s, SHM_STATUS_LEN - 1);
    g_shm->status[SHM_STATUS_LEN - 1] = '\0';
}

static volatile int g_stop = 0;          /* exit the whole daemon */
static volatile int g_stop_session = 0;  /* stop current streaming session, return to idle */
static volatile int g_log_run = 0;       /* logger thread runs while this is set */
static int g_active = 0;
static unsigned int g_rate = 48000;
static unsigned int g_accum = 0;
static long g_pb_underruns = 0;
static long g_cap_overruns = 0;

/* --- instrumentation: counts since last sample, for diagnostic logging --- */
static volatile long g_pb_callbacks = 0;     /* playback callbacks fired */
static volatile long g_cap_callbacks = 0;    /* capture callbacks fired */
static volatile long g_pb_completed = 0;     /* playback xfers COMPLETED ok */
static volatile long g_cap_completed = 0;    /* capture xfers COMPLETED ok */
static volatile long g_pb_err = 0;           /* playback xfers with error */
static volatile long g_cap_err = 0;          /* capture xfers with error */
static volatile int  g_last_pb_status = 0;   /* last playback error status */
static volatile int  g_last_cap_status = 0;  /* last capture error status */

static void on_sig(int s){
    /* SIGTERM here would mean something (launchd?) is killing us, distinct from
       a crash. Async-signal-safe enough for a debug write. */
    if (s == SIGTERM){ const char *m="*** received SIGTERM (external kill) ***\n"; write(2,m,strlen(m)); }
    g_stop = 1;
}

/* Crash/termination diagnostics: if the daemon dies under load (the "skip" you
   hear may actually be the process crashing and launchd relaunching it), this
   records WHICH signal killed it so we can tell a segfault from an external
   kill from a clean stop. Write+flush directly, then re-raise to die normally. */
static void on_fatal(int s){
    const char *name = "?";
    switch (s){
        case SIGSEGV: name="SIGSEGV (segfault)"; break;
        case SIGABRT: name="SIGABRT (abort)";    break;
        case SIGBUS:  name="SIGBUS (bad mem)";   break;
        case SIGILL:  name="SIGILL";             break;
        case SIGFPE:  name="SIGFPE";             break;
        case SIGKILL: name="SIGKILL";            break;
    }
    fprintf(stderr, "\n*** FATAL: caught %s (sig %d) at pid %d; dying ***\n",
            name, s, (int)getpid());
    fflush(stderr);
    signal(s, SIG_DFL);
    raise(s);
}

static int set_rate(libusb_device_handle *h, unsigned int rate){
    unsigned char p[3];
    switch(rate){
        case 44100: p[0]=0x44;p[1]=0xAC;p[2]=0x00; break;
        case 48000: p[0]=0x80;p[1]=0xBB;p[2]=0x00; break;
        case 88200: p[0]=0x88;p[1]=0x58;p[2]=0x01; break;
        case 96000: p[0]=0x00;p[1]=0x77;p[2]=0x01; break;
        default: return -1;
    }
    int r = libusb_control_transfer(h, RT_H2D_CLASS_EP, UAC_SET_CUR,
              UAC_SAMPLING_FREQ_CONTROL, EP_CAPTURE, p, 3, 1000);
    return (r==3)?0:(r<0?r:-1);
}

/* PLAYBACK: pull device-native frames from shm, TIGHT-pack into the URB. */
/* --- async resampler state (device consumes; we read ring at variable rate) ---
   We read the playback ring at a fractional position that advances by
   `ratio` samples per output frame. ratio ~= 1.0; a smooth feedback loop nudges
   it slightly so the ring fill eases toward a setpoint. Because the rate
   changes by tiny smoothed amounts (not discrete frame drops), there are no
   clicks and no audible pitch wobble. Linear interpolation between adjacent
   frames removes discontinuities entirely. */
static double g_resamp_frac = 0.0;   /* fractional read position within ring */
static double g_ratio = 1.0;          /* current resample ratio, smoothed */
static double g_setpoint_frames = 73728.0; /* resampler target fill; set at stream
                                        start from shm buffer_ms and the rate.
                                        Read in the realtime path, so it is a
                                        precomputed value, never divided live. */

/* sign-extend a 3-byte (high-3-of-32) device sample to int32 */
static inline int32_t dev3_to_i32(const uint8_t *p){
    return ((int32_t)p[0] << 8) | ((int32_t)p[1] << 16) | ((int32_t)p[2] << 24);
}
static inline void i32_to_dev3(int32_t v, uint8_t *p){
    p[0] = (v >> 8) & 0xff; p[1] = (v >> 16) & 0xff; p[2] = (v >> 24) & 0xff;
}

static void fill_playback(struct libusb_transfer *xfer){
    unsigned char *buf = xfer->buffer;
    int running = 0;

    /* --- update the resample ratio from ring fill (smooth feedback) ---
       Stronger proportional gain than the first pass so the ring actually
       HOLDS the setpoint instead of slowly drifting up over minutes. The
       correction is still spread smoothly across samples by the resampler, so
       there is no audible pitch step; we just react sooner. Clamp kept tight
       (+-1%) so even a large error can never bend pitch audibly. */
    {
        const double setpoint = g_setpoint_frames; /* configurable; set per stream
                                                from buffer_ms. Larger = more
                                                stall tolerance, more latency. */
        double fill = (double)ring_fill(&g_shm->playback);
        double err = fill - setpoint;        /* >0 means too full -> read faster */
        double target_ratio = 1.0 + (err / setpoint) * 0.15; /* firmer */
        if (target_ratio > 1.01)  target_ratio = 1.01;
        if (target_ratio < 0.99)  target_ratio = 0.99;
        g_ratio += (target_ratio - g_ratio) * 0.05;          /* slew limit */
    }

    /* Snapshot available frames ONCE for this whole URB. We will never read
       beyond `avail-1` (need base+1 for interpolation), so the read position
       can never cross the writer's head even though the writer runs
       concurrently. tail is advanced by exactly what we consume, at the end. */
    uint32_t avail = ring_fill(&g_shm->playback);
    uint32_t tail0 = atomic_load_explicit(&g_shm->playback.tail, memory_order_relaxed);
    double pos = g_resamp_frac;  /* read position, frames from tail0 */

    for (int i = 0; i < ISO_PACKETS_PER_URB; i++){
        g_accum += g_rate;
        int frames = g_accum / MICROFRAMES_PER_SEC;
        g_accum %= MICROFRAMES_PER_SEC;
        int bytes = frames * FRAME_BYTES;
        if (bytes > MAX_PACKET_SIZE){ frames = MAX_PACKET_SIZE/FRAME_BYTES; bytes = frames*FRAME_BYTES; }

        for (int f = 0; f < frames; f++){
            uint32_t base = (uint32_t)pos;
            double mu = pos - (double)base;

            /* need frames base and base+1 to be within the snapshot */
            if (base + 1 >= avail){
                /* not enough buffered: output silence, do NOT advance pos
                   (so we resume cleanly when data arrives). */
                memset(buf + running + f*FRAME_BYTES, 0, FRAME_BYTES);
                g_pb_underruns++;
                continue;
            }

            uint32_t slotA = (tail0 + base)     & RING_MASK;
            uint32_t slotB = (tail0 + base + 1)  & RING_MASK;
            const uint8_t *a = g_shm->playback.data + slotA*FRAME_BYTES;
            const uint8_t *b = g_shm->playback.data + slotB*FRAME_BYTES;

            uint8_t *o = buf + running + f*FRAME_BYTES;
            for (int c = 0; c < 2; c++){
                int32_t sa = dev3_to_i32(a + c*3);
                int32_t sb = dev3_to_i32(b + c*3);
                double v = (double)sa + ((double)sb - (double)sa) * mu;
                i32_to_dev3((int32_t)v, o + c*3);
            }
            pos += g_ratio;
        }

        xfer->iso_packet_desc[i].length = bytes;
        running += bytes;
    }

    /* Advance tail by the integer number of frames fully consumed, keeping the
       fractional remainder for next time. Clamp to avail so we never advance
       past what we snapshotted (can't cross the writer). */
    uint32_t consumed = (uint32_t)pos;
    if (consumed > avail) consumed = avail;
    if (consumed > 0)
        atomic_store_explicit(&g_shm->playback.tail, tail0 + consumed, memory_order_release);
    g_resamp_frac = pos - (double)consumed;
}

/* consecutive transfer errors across both directions; if the device stalls,
   every iso transfer completes instantly with an error. Resubmitting blindly
   spins the loop and floods the device. We count errors and stop the session
   cleanly once they exceed a threshold. */
static volatile int g_xfer_errors = 0;
#define MAX_XFER_ERRORS 64

/* ---- jitter instrumentation (measurement only, no behavior change) ----
   The playback callback records, with cheap operations only (a timestamp diff
   and a couple of max/min updates, NO formatting or I/O), how late each
   completion arrives relative to the expected cadence and how shallow the
   in-flight transfer queue got. The logger thread reads and reports these.
   A healthy stream shows small, steady gaps and a stable queue depth; a
   scheduling-starvation skip shows a gap spike and/or the queue draining. */
static double  g_cb_last_ts = 0.0;       /* timestamp of previous pb completion */
static double  g_cb_gap_max = 0.0;       /* worst gap (s) seen this interval */
static double  g_cb_gap_sum = 0.0;       /* for mean gap */
static long    g_cb_gap_n   = 0;
static int     g_active_min = 1<<30;     /* shallowest queue depth this interval */
static long    g_late_events = 0;        /* cumulative count of gaps > LATE_THRESH */
#define LATE_THRESH_S 0.020              /* 20ms: well beyond the ~16ms URB cadence */

static double now_s(void){
    struct timeval tv; gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec/1e6;
}

static void LIBUSB_CALL playback_cb(struct libusb_transfer *xfer){
    g_active--;
    g_pb_callbacks++;

    /* jitter measurement: how long since the previous completion, and how deep
       the queue is right now. Cheap ops only; the logger thread does the rest. */
    {
        double t = now_s();
        if (g_cb_last_ts > 0.0){
            double gap = t - g_cb_last_ts;
            if (gap > g_cb_gap_max) g_cb_gap_max = gap;
            g_cb_gap_sum += gap; g_cb_gap_n++;
            if (gap > LATE_THRESH_S) g_late_events++;
        }
        g_cb_last_ts = t;
        if (g_active < g_active_min) g_active_min = g_active;
    }

    if (g_stop || xfer->status == LIBUSB_TRANSFER_CANCELLED) return;

    if (xfer->status != LIBUSB_TRANSFER_COMPLETED){
        g_pb_err++;
        g_last_pb_status = xfer->status;
        /* real error (NO_DEVICE, ERROR, TIMED_OUT...). Do NOT spin-resubmit. */
        if (++g_xfer_errors > MAX_XFER_ERRORS){
            fprintf(stderr, "playback: too many transfer errors (status=%d), stopping session\n", xfer->status);
            atomic_store_explicit(&g_shm->running, 0, memory_order_release);
            g_stop_session = 1;
            return;
        }
        /* NOTE: do NOT usleep here. This runs on the USB event thread; sleeping
           blocks every other transfer's servicing, which under load starves the
           whole stream and stalls teardown cancellation. Just resubmit. */
    } else {
        g_pb_completed++;
        g_xfer_errors = 0; /* healthy completion resets the counter */
    }

    fill_playback(xfer);
    if (libusb_submit_transfer(xfer) < 0){ g_stop_session = 1; return; }
    g_active++;
}

/* CAPTURE: take the bytes the device actually delivered per packet and push
   them into the shm capture ring. Uses actual_length, not reserved length. */
static void LIBUSB_CALL capture_cb(struct libusb_transfer *xfer){
    g_active--;
    g_cap_callbacks++;
    if (g_stop || g_stop_session || xfer->status == LIBUSB_TRANSFER_CANCELLED) return;

    if (xfer->status != LIBUSB_TRANSFER_COMPLETED){
        g_cap_err++;
        g_last_cap_status = xfer->status;
        if (++g_xfer_errors > MAX_XFER_ERRORS){
            fprintf(stderr, "capture: too many transfer errors (status=%d), stopping session\n", xfer->status);
            atomic_store_explicit(&g_shm->running, 0, memory_order_release);
            g_stop_session = 1;
            return;
        }
        /* no usleep here (see playback_cb): sleeping on the event thread starves
           the stream. resubmit with reset lengths; skip data on errored xfer */
        for (int i = 0; i < ISO_PACKETS_PER_URB; i++)
            xfer->iso_packet_desc[i].length = MAX_PACKET_SIZE;
        if (libusb_submit_transfer(xfer) < 0){ g_stop_session = 1; return; }
        g_active++;
        return;
    }
    g_cap_completed++;
    g_xfer_errors = 0;

    /* If the capture ring is already full, NO app is draining it (no
       recording). Doing per-packet ring work on a full ring 500x/sec thrashes
       this realtime USB thread and can starve playback -> dropouts. So when
       there is no consumer, discard captured data with near-zero work: just
       reset packet lengths and resubmit. The device stays clocked (we still
       consumed the endpoint); we simply don't touch the ring. */
    if (ring_space(&g_shm->capture) == 0){
        /* drop everything this URB; advance tail once so a future consumer
           sees fresh data rather than stale, but do no per-frame work */
        uint32_t h = atomic_load_explicit(&g_shm->capture.head, memory_order_relaxed);
        atomic_store_explicit(&g_shm->capture.tail, h, memory_order_release); /* empty it */
        g_cap_overruns++;
        for (int i = 0; i < ISO_PACKETS_PER_URB; i++)
            xfer->iso_packet_desc[i].length = MAX_PACKET_SIZE;
        if (libusb_submit_transfer(xfer) < 0){ g_stop_session = 1; return; }
        g_active++;
        return;
    }

    for (int i = 0; i < ISO_PACKETS_PER_URB; i++){
        unsigned char *pkt = libusb_get_iso_packet_buffer_simple(xfer, i);
        unsigned int len = xfer->iso_packet_desc[i].actual_length;
        unsigned int frames = len / FRAME_BYTES;
        if (frames && pkt){
            /* normal path: a consumer IS draining. Push to the ring; if it
               filled mid-URB, drop the remainder (handled next callback). */
            uint32_t space = ring_space(&g_shm->capture);
            if (space < frames) frames = space;
            if (frames) ring_produce(&g_shm->capture, pkt, frames);
        }
        /* reset for resubmit */
        xfer->iso_packet_desc[i].length = MAX_PACKET_SIZE;
    }

    if (libusb_submit_transfer(xfer) < 0){ g_stop_session = 1; return; }
    g_active++;
}

/* Diagnostic logger thread: samples shared counters every 250ms and writes a
   line to stderr (the log file). Runs SEPARATELY from the USB event thread so
   a blocking disk write never stalls audio. */
static void *logger_thread(void *arg){
    (void)arg;
    struct timeval t0; gettimeofday(&t0, NULL);
    double last = 0;
    long prev_pb_cb = 0, prev_cap_cb = 0;
    while (g_log_run && !g_stop && !g_stop_session){
        usleep(50000);
        struct timeval now; gettimeofday(&now, NULL);
        double elapsed = (now.tv_sec - t0.tv_sec) + (now.tv_usec - t0.tv_usec)/1e6;
        if (elapsed - last < 0.25) continue;
        double dt = elapsed - last;
        long pb_cb = g_pb_callbacks, cap_cb = g_cap_callbacks;
        /* snapshot + reset the per-interval jitter accumulators */
        double gap_max = g_cb_gap_max;
        double gap_mean = g_cb_gap_n ? (g_cb_gap_sum / (double)g_cb_gap_n) : 0.0;
        int qmin = g_active_min;
        long late = g_late_events;
        g_cb_gap_max = 0.0; g_cb_gap_sum = 0.0; g_cb_gap_n = 0;
        g_active_min = 1<<30;
        fprintf(stderr,
          "[t=%.2f] pb_cb/s=%.0f cap_cb/s=%.0f | pb_ring=%u cap_ring=%u | "
          "pb_ok=%ld pb_err=%ld(st=%d) cap_ok=%ld cap_err=%ld(st=%d) | "
          "underrun=%ld overrun=%ld | gap_max=%.1fms gap_mean=%.1fms qmin=%d late=%ld\n",
          elapsed, (pb_cb-prev_pb_cb)/dt, (cap_cb-prev_cap_cb)/dt,
          ring_fill(&g_shm->playback), ring_fill(&g_shm->capture),
          g_pb_completed, g_pb_err, g_last_pb_status,
          g_cap_completed, g_cap_err, g_last_cap_status,
          g_pb_underruns, g_cap_overruns,
          gap_max*1000.0, gap_mean*1000.0, (qmin==(1<<30)?-1:qmin), late);
        fflush(stderr);   /* force the line out NOW so we can watch jitter live */
        prev_pb_cb = pb_cb; prev_cap_cb = cap_cb;
        last = elapsed;
    }
    return NULL;
}

/* Promote the calling thread to realtime time-constraint scheduling so the
   kernel guarantees it CPU even when the rest of the system is busy. This is
   the same mechanism CoreAudio uses, and it is what factory audio drivers rely
   on. Without it, our user-space USB thread competes at normal priority and
   gets time-sliced away when you use the computer, causing brief audio stops.
   No-op (and harmless) on non-Apple builds / if the call fails. */
static void make_thread_realtime(void){
#ifdef __APPLE__
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double ticks_per_ms = 1e6 * (double)tb.denom / (double)tb.numer;

    /* Time-constraint policy: guarantee CPU on a regular cadence.
       TUNING (measured): under heavy system load the USB event thread was being
       starved to ~113 callbacks/sec (from 500), with packet gaps up to 419ms =
       audible skipping, even though the audio buffer stayed full. The thread was
       getting preempted under contention. Fix: claim a larger guaranteed slice
       and mark it NON-preemptible. Non-preemptible is safe HERE specifically
       because the loop blocks in libusb_handle_events_timeout (it sleeps waiting
       for events, it does not spin), so it cannot wedge the machine the way a
       busy-looping non-preemptible RT thread could. */
    thread_time_constraint_policy_data_t pol;
    pol.period      = (uint32_t)(ticks_per_ms * 1.0);   /* ~1ms cadence */
    pol.computation = (uint32_t)(ticks_per_ms * 0.9);   /* claim most of the period */
    pol.constraint  = (uint32_t)(ticks_per_ms * 1.0);   /* deadline = full period */
    pol.preemptible = 0;                                /* hold CPU under contention */
    kern_return_t kr = thread_policy_set(mach_thread_self(),
        THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol,
        THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS)
        fprintf(stderr, "warning: could not set realtime thread policy (%d); "
                        "audio may stutter under load\n", kr);
    else
        fprintf(stderr, "USB thread promoted to realtime scheduling\n");

    /* Note: we deliberately do NOT also set THREAD_PRECEDENCE_POLICY here.
       Precedence governs timeshare threads; layering it on a time-constraint
       (realtime) thread is redundant at best and on some OS versions could
       nudge the thread out of the RT band. The time-constraint policy already
       places this thread above all normal work. */
#endif
}

int main(void){
    setvbuf(stdout, NULL, _IOLBF, 0);   /* line-buffered so logs flush */
    setvbuf(stderr, NULL, _IONBF, 0);
    logts("==================================================");
    logts("us122d STARTING  version=%s  pid=%d", US122D_VERSION, (int)getpid());
    logts("==================================================");
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    /* crash diagnostics: capture how we die if load triggers a crash */
    signal(SIGSEGV, on_fatal);
    signal(SIGABRT, on_fatal);
    signal(SIGBUS,  on_fatal);
    signal(SIGILL,  on_fatal);
    signal(SIGFPE,  on_fatal);

    /* Wire all current and future pages into RAM. A page fault on the audio
       thread would block it while the page is fetched (the screenshot showed
       ~130k faults). Locking memory removes that entire class of stall. This
       is standard for pro-audio processes. Needs root, which we have via
       launchd; if it fails we log and continue. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        logts("JITTER-CAUSE? mlockall FAILED (errno=%d %s); page faults may stall RT thread",
              errno, strerror(errno));
    else
        logts("memory wired (mlockall ok); page faults ruled out");

    /* --- create or attach the shared segment ---
       Mode 0666 so the plugin running inside coreaudiod (different uid) can
       attach. The umask can still strip bits, so we also fchmod after. */
    int created = 0;
    mode_t oldmask = umask(0);
    int fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (fd >= 0){
        created = 1;
        if (ftruncate(fd, sizeof(shm_layout_t)) != 0){ perror("ftruncate"); return 1; }
        fchmod(fd, 0666);
    } else {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (fd < 0){ perror("shm_open"); return 1; }
    }
    umask(oldmask);
    g_shm = mmap(NULL, sizeof(shm_layout_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_shm == MAP_FAILED){ perror("mmap"); return 1; }
    close(fd);

    if (created){
        shm_init_layout(g_shm, g_rate);
        logts("created %s (%.2f MB) at shm v%u", SHM_NAME,
              sizeof(shm_layout_t)/1048576.0, (unsigned)SHM_VERSION);
    } else {
        if (!shm_valid(g_shm)){
            /* A segment from a DIFFERENT build version is present. Refusing here
               makes launchd restart us into the same stale segment forever.
               Instead: self-heal. Unmap, unlink the stale segment, and recreate
               it fresh at our version. This makes every future version bump
               recover automatically with no manual cleanup. */
            logts("shm version mismatch: stale segment present; recreating fresh");
            munmap(g_shm, sizeof(shm_layout_t));
            g_shm = NULL;
            shm_unlink(SHM_NAME);   /* drop the stale segment */

            mode_t m2 = umask(0);
            fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0666);
            umask(m2);
            if (fd < 0){ logts("FATAL: could not recreate shm after unlink"); return 1; }
            if (ftruncate(fd, sizeof(shm_layout_t)) != 0){ perror("ftruncate"); close(fd); return 1; }
            fchmod(fd, 0666);
            g_shm = mmap(NULL, sizeof(shm_layout_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (g_shm == MAP_FAILED){ perror("mmap"); return 1; }
            shm_init_layout(g_shm, g_rate);
            created = 1;
            logts("recreated %s fresh at shm v%u", SHM_NAME, (unsigned)SHM_VERSION);
        } else {
            logts("attached existing %s at shm v%u", SHM_NAME, (unsigned)SHM_VERSION);
        }
    }

    /* mlockall is unavailable on macOS (errno 78), so wire the critical memory
       per-region with mlock instead. The shm segment holds both audio rings,
       which the realtime thread reads/writes every cycle; locking it prevents
       page faults on that hot memory from stalling the thread under load. */
    if (mlock(g_shm, sizeof(shm_layout_t)) == 0)
        logts("shm wired into RAM (mlock ok, %.1f MB)", sizeof(shm_layout_t)/1048576.0);
    else
        logts("shm mlock failed (errno=%d %s); ring page faults still possible",
              errno, strerror(errno));

    /* --- device lifecycle is now a two-level loop ---
       OUTER: acquire the device (poll until present, open, claim, alt-set),
              set device_present=1. On any disconnect we release and come back
              here, so replugging recovers automatically with no manual restart.
       INNER: the existing idle/stream cycle, runs while the device is held. */
    libusb_context *ctx=NULL;
    if (libusb_init(&ctx)!=0){ fprintf(stderr,"libusb init failed\n"); return 2; }
    libusb_device_handle *h = NULL;

    while (!g_stop){
        /* ---- DORMANT GATE: the menu app is the on/off hub. While app_active
                is 0 (app closed), park here doing NOTHING: no USB enumeration,
                no device held, near-zero CPU. Wake only when the app sets the
                flag. This is what keeps the machine quiet when you are not using
                the device, while still letting the daemon live (root, launchd)
                so there is no per-launch privilege prompt. ---- */
        if (!atomic_load_explicit(&g_shm->app_active, memory_order_acquire)){
            atomic_store_explicit(&g_shm->device_present, 0, memory_order_release);
            set_status("dormant (app closed)");
            logts("DORMANT: app inactive; parking (no USB activity)");
            while (!g_stop && !atomic_load_explicit(&g_shm->app_active, memory_order_acquire))
                usleep(1000000);   /* check once a second; cheap */
            if (g_stop) break;
            logts("WAKE: app active; entering acquire loop");
            set_status("starting…");
        }

        /* ---- ACQUIRE: wait for the device, then open/claim/configure ---- */
        atomic_store_explicit(&g_shm->device_present, 0, memory_order_release);
        h = NULL;
        logts("ACQUIRE: waiting for device (vid=%04x pid=%04x)...", VID, PID);
        set_status("waiting for device");
        int wait_ticks = 0;
        /* bail out of waiting if the app goes inactive (quit while no device) */
        while (!g_stop && atomic_load_explicit(&g_shm->app_active, memory_order_acquire)){
            /* Force a FRESH enumeration each poll. libusb_open_device_with_vid_pid
               can rely on a cached device list that goes stale after an unplug,
               so a replugged device is never seen and reconnect silently fails.
               Walking a freshly-fetched list guarantees we see current hardware. */
            libusb_device **list = NULL;
            ssize_t n = libusb_get_device_list(ctx, &list);
            for (ssize_t i = 0; i < n; i++){
                struct libusb_device_descriptor d;
                if (libusb_get_device_descriptor(list[i], &d) == 0 &&
                    d.idVendor == VID && d.idProduct == PID){
                    if (libusb_open(list[i], &h) != 0) h = NULL;
                    break;
                }
            }
            if (list) libusb_free_device_list(list, 1);
            if (h){ logts("ACQUIRE: open succeeded"); break; }
            usleep(ACQUIRE_POLL_US);
            if (++wait_ticks % ACQUIRE_HEARTBEAT_TICKS == 0)
                logts("ACQUIRE: still waiting for device...");
        }
        if (g_stop) break;
        if (!h) continue;   /* app went inactive while waiting -> dormant gate */

        libusb_set_auto_detach_kernel_driver(h,1);
        if (libusb_claim_interface(h,IFACE)<0){
            logts("ACQUIRE: claim_interface failed; releasing and retrying");
            libusb_close(h); h=NULL;
            usleep(500000);
            continue;   /* back to ACQUIRE */
        }
        if (libusb_set_interface_alt_setting(h,IFACE,ALT)<0){
            logts("ACQUIRE: set_alt_setting failed; resetting device and retrying");
            /* watchdog: a single software reset can clear a device left in a
               confused state by an unclean prior teardown, turning a manual
               physical power-cycle into automatic recovery. */
            libusb_reset_device(h);
            libusb_release_interface(h,IFACE);
            libusb_close(h); h=NULL;
            usleep(500000);
            continue;   /* back to ACQUIRE */
        }

        atomic_store_explicit(&g_shm->device_present, 1, memory_order_release);
        logts("ACQUIRE: device acquired & claimed, present=1. Waiting for running flag.");
        set_status("connected, idle");

        /* ---- INNER: idle/stream cycle, runs until disconnect or daemon exit.
                device_lost is set by the streaming teardown when a transfer
                fails with a NO_DEVICE-class status, sending us back to ACQUIRE. */
        int device_lost = 0;
        while (!g_stop && !device_lost){
        /* idle until the plugin requests streaming. While idle, periodically
           check the device is still physically present. NOTE: libusb_get_
           configuration can return a CACHED value without touching the device,
           so it does NOT reliably detect an unplug. Instead we re-enumerate the
           bus and look for our VID/PID; a fresh device list reflects real
           hardware state with no caching. */
        {
            int idle_probe = 0;
            int app_gone = 0;
            while (!g_stop && atomic_load_explicit(&g_shm->running, memory_order_acquire) == 0){
                usleep(5000);
                /* if the app quit while we sit idle-connected, release the device
                   and head back to the dormant gate promptly. */
                if (!atomic_load_explicit(&g_shm->app_active, memory_order_acquire)){
                    logts("idle: app went inactive; releasing device to go dormant");
                    app_gone = 1;
                    break;
                }
                if (++idle_probe >= 100){   /* ~every 500ms */
                    idle_probe = 0;
                    libusb_device **list = NULL;
                    ssize_t n = libusb_get_device_list(ctx, &list);
                    int still_here = 0;
                    for (ssize_t i = 0; i < n; i++){
                        struct libusb_device_descriptor d;
                        if (libusb_get_device_descriptor(list[i], &d) == 0 &&
                            d.idVendor == VID && d.idProduct == PID){
                            still_here = 1; break;
                        }
                    }
                    if (list) libusb_free_device_list(list, 1);
                    if (!still_here){
                        logts("LOST: device unplugged while idle (not in bus enumeration); releasing");
                        set_status("device disconnected");
                        device_lost = 1;
                        break;
                    }
                }
            }
            if (app_gone){ device_lost = 1; }  /* release + return; gate parks us */
        }
        if (g_stop || device_lost) break;

        g_rate = atomic_load_explicit(&g_shm->rate, memory_order_acquire);
        if (g_rate!=44100 && g_rate!=48000 && g_rate!=88200 && g_rate!=96000) g_rate = 48000;

        /* Convert the app's buffer_ms into a frame setpoint for THIS stream,
           using the actual rate so the buffer feels the same at any rate.
           Clamp to the allowed ms range and keep well under ring capacity so a
           huge value can never exceed the buffer (leave generous headroom; the
           priming/setpoint should stay below ~half the ring). */
        {
            uint32_t bms = atomic_load_explicit(&g_shm->buffer_ms, memory_order_acquire);
            if (bms < BUFFER_MS_MIN) bms = BUFFER_MS_MIN;
            if (bms > BUFFER_MS_MAX) bms = BUFFER_MS_MAX;
            double frames = (double)bms * (double)g_rate / 1000.0;
            double cap = (double)RING_FRAMES * 0.45;   /* stay well under the ring */
            if (frames > cap) frames = cap;
            g_setpoint_frames = frames;
            logts("STREAM: buffer %u ms -> setpoint %.0f frames @ %u Hz",
                  bms, g_setpoint_frames, g_rate);
        }

        g_accum = 0; g_active = 0; g_pb_underruns = 0; g_cap_overruns = 0;
        g_resamp_frac = 0.0; g_ratio = 1.0;
        g_pb_callbacks = g_cap_callbacks = 0;
        g_pb_completed = g_cap_completed = 0;
        g_pb_err = g_cap_err = 0;
        /* reset jitter measurement for this session */
        g_cb_last_ts = 0.0; g_cb_gap_max = 0.0; g_cb_gap_sum = 0.0;
        g_cb_gap_n = 0; g_active_min = 1<<30; g_late_events = 0;
        g_last_pb_status = g_last_cap_status = 0;
        atomic_store_explicit(&g_shm->dev_frames, 0, memory_order_release);

        /* set_rate can transiently fail if the device is in a bad state from
           an unclean prior teardown. Retry a few times before giving up, and
           on total failure clear the running flag and return to idle rather
           than crashing the daemon (which makes launchd thrash). */
        int rate_ok = 0; int last_rate_err = 0;
        for (int attempt = 0; attempt < 5; attempt++){
            last_rate_err = set_rate(h, g_rate);
            if (last_rate_err == 0){ rate_ok = 1; break; }
            fprintf(stderr, "set_rate attempt %d failed (err=%d), retrying...\n", attempt+1, last_rate_err);
            usleep(200000);
        }
        if (!rate_ok){
            atomic_store_explicit(&g_shm->running, 0, memory_order_release);
            /* LIBUSB_ERROR_NO_DEVICE (-4) means it was unplugged: release and
               re-acquire so it recovers on reconnect. Any other persistent
               failure also warrants re-acquiring (a fresh open often clears a
               wedged device) rather than spinning set_rate on a bad handle. */
            if (last_rate_err == LIBUSB_ERROR_NO_DEVICE){
                logts("LOST: device gone during set_rate; releasing");
            } else {
                fprintf(stderr, "set_rate failed after retries; re-acquiring device. "
                                "If this persists, power-cycle the unit.\n");
            }
            device_lost = 1;
            break; /* leave INNER loop -> release -> ACQUIRE */
        }
        printf("streaming at %u Hz\n", g_rate);
        g_stop_session = 0;
        g_xfer_errors = 0;

        /* Wait briefly for the plugin to prime the playback ring before we
           start submitting URBs. Starting into an empty ring means the device
           gets silence at the critical startup moment, which can stall it.
           Give the plugin up to ~300ms to provide some audio; if it never
           does (e.g. an app that only records), start anyway. */
        for (int w = 0; w < 500; w++){   /* up to ~2.5s to prime the deep buffer */
            if (g_stop || g_stop_session) break;
            if (atomic_load_explicit(&g_shm->running, memory_order_acquire) == 0) break;
            if (ring_fill(&g_shm->playback) >= (uint32_t)g_setpoint_frames) break; /* prime to setpoint */
            usleep(5000);
        }

        int buflen = ISO_PACKETS_PER_URB * MAX_PACKET_SIZE;
        struct libusb_transfer *pb[NUM_URBS], *cap[NUM_URBS];
        for (int i=0;i<NUM_URBS;i++){
            pb[i] = libusb_alloc_transfer(ISO_PACKETS_PER_URB);
            unsigned char *pbuf = calloc(1, buflen);
            mlock(pbuf, buflen);   /* wire the packet buffer: no page fault on the RT path */
            libusb_fill_iso_transfer(pb[i], h, EP_PLAYBACK, pbuf, buflen,
                ISO_PACKETS_PER_URB, playback_cb, NULL, 1000);
            libusb_set_iso_packet_lengths(pb[i], MAX_PACKET_SIZE);
            fill_playback(pb[i]);

            cap[i] = libusb_alloc_transfer(ISO_PACKETS_PER_URB);
            unsigned char *cbuf = calloc(1, buflen);
            mlock(cbuf, buflen);
            libusb_fill_iso_transfer(cap[i], h, EP_CAPTURE, cbuf, buflen,
                ISO_PACKETS_PER_URB, capture_cb, NULL, 1000);
            libusb_set_iso_packet_lengths(cap[i], MAX_PACKET_SIZE);
        }
        for (int i=0;i<NUM_URBS;i++){
            if (libusb_submit_transfer(pb[i])  < 0){ g_stop_session=1; break; } g_active++;
            if (libusb_submit_transfer(cap[i]) < 0){ g_stop_session=1; break; } g_active++;
        }

        /* run until: running flag clears, session stalls, or daemon exit.
           CRITICAL: this loop does ONLY USB event handling, no file I/O. A
           blocking disk write here (the old per-250ms fprintf) stalled both
           callback streams together for ~150ms = the periodic short stops.
           Diagnostic logging now runs on a SEPARATE thread (g_logger), and is
           only started when US122_DEBUG is set in the environment. In normal
           production there is NO logger thread at all, so it adds zero wakeups
           or context switches. */
        int want_log = (getenv("US122_DEBUG") != NULL);
        g_log_run = want_log;
        pthread_t logth;
        int have_logth = want_log && (pthread_create(&logth, NULL, logger_thread, ctx) == 0);

        /* make THIS thread (the USB event loop) realtime so the scheduler
           protects it from being starved when the computer is busy. */
        make_thread_realtime();

        logts("STREAM: entering event loop at %u Hz", g_rate);
        { char sb[SHM_STATUS_LEN]; snprintf(sb, sizeof(sb), "streaming @ %u Hz", g_rate); set_status(sb); }
        long wd_last_cb = g_pb_callbacks + g_cap_callbacks;
        struct timeval wd_t; gettimeofday(&wd_t, NULL);
        while (!g_stop && !g_stop_session &&
               atomic_load_explicit(&g_shm->running, memory_order_acquire) == 1){
            struct timeval tv={0,50000}; libusb_handle_events_timeout(ctx,&tv);
            /* watchdog: if NO callback fires for ~1s while we believe we are
               streaming, the device has likely vanished without delivering
               error completions. Break so we tear down and re-acquire instead
               of blocking here forever. */
            long cb_now = g_pb_callbacks + g_cap_callbacks;
            struct timeval now; gettimeofday(&now, NULL);
            if (cb_now != wd_last_cb){
                wd_last_cb = cb_now; wd_t = now;
            } else {
                double idle = (now.tv_sec-wd_t.tv_sec)+(now.tv_usec-wd_t.tv_usec)/1e6;
                if (idle > 1.0){
                    logts("STREAM: no transfer activity for %.1fs; assuming device lost", idle);
                    g_last_pb_status = LIBUSB_TRANSFER_NO_DEVICE; /* route to re-acquire */
                    g_stop_session = 1;
                    break;
                }
            }
        }

        logts("STREAM: event loop exited (stop=%d stop_session=%d running=%d). Tearing down.",
              g_stop, g_stop_session,
              atomic_load_explicit(&g_shm->running, memory_order_acquire));

        g_log_run = 0;
        if (have_logth) pthread_join(logth, NULL);

        /* tear down this streaming session (cancel + drain), WITHOUT exiting
           the daemon. g_stop_session makes the callbacks stop resubmitting.
           CRITICAL: when the device is physically gone, cancelled transfers may
           NEVER produce completion callbacks, so g_active can never reach 0. A
           plain `while (g_active>0)` drain then spins forever and wedges the
           daemon (log goes silent, no recovery). Bound the drain with a wall
           clock: drain what we can for a short window, then abandon the rest. */
        g_stop_session = 1;
        for (int i=0;i<NUM_URBS;i++){ if(pb[i]) libusb_cancel_transfer(pb[i]); if(cap[i]) libusb_cancel_transfer(cap[i]); }
        {
            /* Drain cancellations. The transfers WILL come back (as CANCELLED or
               errored) once the load eases and the event thread catches up; they
               are not lost. Be patient: a long budget here costs nothing because
               we are not streaming, and it lets us free them safely WITHOUT
               destroying the libusb context. (Destroying+recreating the context
               in-process fails on this macOS/libusb, which is why the previous
               approach exited and relaunched.) */
            struct timeval start; gettimeofday(&start, NULL);
            while (g_active > 0){
                struct timeval tv={0,20000}; libusb_handle_events_timeout(ctx,&tv);
                struct timeval now; gettimeofday(&now, NULL);
                double el = (now.tv_sec-start.tv_sec)+(now.tv_usec-start.tv_usec)/1e6;
                if (el > 5.0){   /* generous: under heavy load cancellations lag */
                    logts("STREAM: drain still incomplete after 5s, %d stranded; "
                          "leaving them to libusb and continuing", g_active);
                    break;
                }
            }
        }

        /* Free only what fully drained. If anything is still in flight after the
           generous drain (rare), do NOT free those transfers (use-after-free) and
           do NOT destroy the context (libusb_init then fails). Instead leak those
           few transfer structs, harmless and bounded, the device re-acquire path
           continues on the SAME healthy context. */
        if (g_active == 0){
            for (int i=0;i<NUM_URBS;i++){
                if(pb[i]){ free(pb[i]->buffer); libusb_free_transfer(pb[i]); }
                if(cap[i]){ free(cap[i]->buffer); libusb_free_transfer(cap[i]); }
            }
        } else {
            logts("STREAM: %d transfers never returned; leaking them to stay safe", g_active);
            /* leave pb[]/cap[] and their buffers; libusb still owns them. */
            g_active = 0;
        }

        /* FLUSH the rings: discard anything left from this session so the next
           playback starts from genuinely EMPTY and primes cleanly to the (possibly
           changed) setpoint. Without this, stale frames from the previous stream
           survive in the ring, get played first on the next start, and dominate
           the real latency regardless of the buffer setting. Setting tail=head
           empties a ring without touching the data. */
        {
            uint32_t ph = atomic_load_explicit(&g_shm->playback.head, memory_order_acquire);
            atomic_store_explicit(&g_shm->playback.tail, ph, memory_order_release);
            uint32_t ch = atomic_load_explicit(&g_shm->capture.head, memory_order_acquire);
            atomic_store_explicit(&g_shm->capture.tail, ch, memory_order_release);
        }
        atomic_store_explicit(&g_shm->dev_frames, 0, memory_order_release);

        printf("stopped. playback underruns=%ld  capture overruns=%ld\n",
               g_pb_underruns, g_cap_overruns);
        g_pb_underruns = 0; g_cap_overruns = 0;

        /* Did this session stop because the device went away? The callbacks
           record the last transfer status; NO_DEVICE means it was unplugged.
           If so, break the inner loop so we release and return to ACQUIRE,
           where the audio device disappears until the hardware comes back. */
        if (g_last_pb_status == LIBUSB_TRANSFER_NO_DEVICE ||
            g_last_cap_status == LIBUSB_TRANSFER_NO_DEVICE){
            logts("LOST: device disappeared during streaming; releasing");
            device_lost = 1;
        }
        /* otherwise loop back to idle; daemon stays alive for the next start */
        }  /* end INNER idle/stream loop */

        /* released because the device was lost (or g_stop). Drop the handle and
           clear presence so the plugin removes the device from the list, then
           the outer loop goes back to ACQUIRE and waits for the hardware. */
        atomic_store_explicit(&g_shm->device_present, 0, memory_order_release);
        atomic_store_explicit(&g_shm->running, 0, memory_order_release);
        if (h){ libusb_release_interface(h,IFACE); libusb_close(h); h=NULL; }
        logts("RELEASED: handle closed, present=0; returning to ACQUIRE");

        /* Only rebuild the libusb context on a genuine DEVICE LOSS (unplug),
           where the stale context could otherwise poison reconnect. On a clean
           stop (track ended) we keep the same healthy context, this avoids the
           libusb_exit+libusb_init churn that can fail in-process on macOS. */
        if (!g_stop && device_lost){
            logts("RECYCLE: rebuilding libusb context for clean reconnect");
            libusb_context *newctx = NULL;
            if (libusb_init(&newctx) == 0){
                if (ctx) libusb_exit(ctx);   /* free old only after new succeeds */
                ctx = newctx;
            } else {
                /* re-init failed: keep the existing context rather than exiting
                   the daemon. The acquire loop will retry on it; worst case the
                   next open fails and we loop, but we do NOT drop out of the
                   daemon (which caused the audible relaunch). */
                logts("RECYCLE: libusb re-init failed; keeping existing context and retrying");
            }
        }
    }  /* end OUTER acquire loop */

    if (ctx) libusb_exit(ctx);
    munmap(g_shm, sizeof(shm_layout_t));
    if (created) shm_unlink(SHM_NAME);
    printf("us122d exit.\n");
    return 0;
}
