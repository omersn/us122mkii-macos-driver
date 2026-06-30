/*
 * us122_lowlat_tone.c
 *
 * Stage 1 of the low-latency rewrite (Path A): a STANDALONE native-IOKit
 * low-latency isochronous PLAYBACK tester for the TASCAM US-122MKII.
 *
 * It plays a steady sine tone out EP 0x02 using Apple's low-latency isoch API
 * (LowLatencyCreateBuffer + LowLatencyWriteIsochPipeAsync), and runs the coupled
 * capture EP 0x81 drained/discarded (the device is duplex-coupled). No daemon,
 * no shm, no Core Audio: a self-contained tone so the verdict is "listen".
 *
 * WHY this exists (see docs/07): V2 used the SAME IOKit transport and ran on
 * this device with clean timing but bit-crushed audio. This tester controls for
 * every V2 suspect at once:
 *   - LAYOUT: playback packed CONTIGUOUSLY by per-packet byte count (running +=
 *     bytes), exactly as V1/libusb does and proven clean. V2 used fixed-stride
 *     i*MAX_PACKET_SIZE; this does NOT. (the Stage 0 answer)
 *   - PIPE RESERVATION: SetPipePolicy(MAX_PACKET_SIZE, 1) on both pipes.
 *   - STALE FRAMING: ResetPipe on both pipes before streaming.
 *   - SOURCE JITTER: a fresh sine generated in the callback, no ring/2-clock.
 *
 * The IOKit open/claim/rate/pipe/schedule flow is lifted from the V2 engine
 * (reference/v2-abandoned/...-iokit.inc), which streamed cleanly; only the
 * transport calls and the buffer layout differ.
 *
 * Free the device first: quit US122Menu (daemon releases on app_active=0), then
 * run this. No sudo needed.
 *
 * Build:
 *   clang tools/us122_lowlat_tone.c -o /tmp/us122_lowlat_tone \
 *       -framework IOKit -framework CoreFoundation -lpthread -Wall -O2
 * Run:
 *   /tmp/us122_lowlat_tone [rate=48000] [seconds=4] [hz=440]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>

#define VID 0x0644
#define PID 0x8021
#define IFACE 1
#define ALT 1
#define EP_PLAYBACK 0x02
#define EP_CAPTURE  0x81

#define FRAME_BYTES         6      /* 24-bit stereo: 2ch * 3 bytes */
#define MAX_PACKET_SIZE     78     /* 13 frames per microframe packet */
#define MICROFRAMES_PER_SEC 8000
#define UFRAMES_PER_FRAME   8
#define FRAMES_PER_XFER     8      /* microframe packets per transfer (= 1 bus frame) */
#define XFER_BUS_FRAMES     (FRAMES_PER_XFER / UFRAMES_PER_FRAME)  /* = 1 */
#define NUM_XFERS           32     /* outstanding transfers per direction */
#define UPDATE_FREQ         1      /* low-latency frame-list update, ms (0-8) */
#define LEAD_FRAMES         50     /* initial scheduling lead (bus frames) */

/* class / host-to-device / endpoint-recipient, UAC SET_CUR sampling freq */
#define UAC_SET_CUR 0x01
#define UAC_SAMPLING_FREQ_CONTROL 0x0100

typedef IOUSBDeviceInterface500    DevI;
typedef IOUSBInterfaceInterface500 IntfI;

typedef struct {
    UInt8                       pipe;
    int                         is_pb;
    void                       *buf;     /* LowLatencyCreateBuffer data buffer */
    IOUSBLowLatencyIsocFrame   *frames;  /* LowLatencyCreateBuffer frame list */
    UInt64                      next_frame;
} xfer_t;

static DevI  **g_dev  = NULL;
static IntfI **g_intf = NULL;
static CFRunLoopSourceRef g_src = NULL;
static xfer_t g_pb[NUM_XFERS], g_cap[NUM_XFERS];

static volatile sig_atomic_t g_stop = 0;
static unsigned g_rate = 48000;
static double   g_freq = 440.0;
static double   g_amp  = 0.12;        /* ~ -18 dBFS, safe to monitor */
static double   g_phase = 0.0;
static unsigned g_accum = 0;          /* pacing accumulator (single thread) */

static _Atomic long g_pb_cb = 0, g_cap_cb = 0, g_pb_err = 0;
static _Atomic long g_cap_frames = 0; /* total captured frames -> measured rate */
static _Atomic long g_reanchor = 0;
static _Atomic unsigned long long g_pb_next = 0, g_cap_next = 0;

/* jitter instrumentation (all touched only on the single runloop thread) */
static double   g_tb_n2ms = 0;        /* mach ticks -> ms */
static uint64_t g_last_pb_ticks = 0;
static double   g_gap_max_ms = 0;     /* worst pb gap this log interval */
static long     g_late = 0;           /* cumulative pb gaps > 20 ms */

static void on_sigint(int s){ (void)s; g_stop = 1; CFRunLoopStop(CFRunLoopGetCurrent()); }

/* ---- realtime promotion (mirrors the daemon's policy) ---- */
static void make_thread_realtime(void){
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double n2a = (double)tb.numer / (double)tb.denom; /* ns -> abs ticks */
    thread_time_constraint_policy_data_t p;
    p.period      = (uint32_t)(1000000.0 / n2a);  /* 1 ms */
    p.computation = (uint32_t)( 500000.0 / n2a);  /* 0.5 ms */
    p.constraint  = (uint32_t)(1000000.0 / n2a);  /* 1 ms */
    p.preemptible = 0;
    thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

/* ---- one device sine frame, device-native 6 bytes (high-3-of-32 LE) ---- */
static inline void sine_frame(uint8_t *o){
    double s = sin(g_phase) * g_amp;
    g_phase += 2.0 * M_PI * g_freq / (double)g_rate;
    if (g_phase >= 2.0 * M_PI) g_phase -= 2.0 * M_PI;
    int32_t v = (int32_t)(s * 2147483647.0);
    for (int c = 0; c < 2; c++){
        uint8_t *d = o + c*3;
        d[0] = (v >> 8)  & 0xff;
        d[1] = (v >> 16) & 0xff;
        d[2] = (v >> 24) & 0xff;
    }
}

/* fill one playback transfer: CONTIGUOUS packing by per-packet byte count.
   This is the Stage 0 fix and the whole point: NOT fixed stride. */
static void fill_pb(xfer_t *x){
    uint8_t *buf = (uint8_t*)x->buf;
    int running = 0;
    for (int i = 0; i < FRAMES_PER_XFER; i++){
        g_accum += g_rate;
        int frames = g_accum / MICROFRAMES_PER_SEC;
        g_accum %= MICROFRAMES_PER_SEC;
        int bytes = frames * FRAME_BYTES;
        if (bytes > MAX_PACKET_SIZE){ frames = MAX_PACKET_SIZE/FRAME_BYTES; bytes = frames*FRAME_BYTES; }
        for (int f = 0; f < frames; f++) sine_frame(buf + running + f*FRAME_BYTES);
        x->frames[i].frReqCount = (UInt16)bytes;
        x->frames[i].frActCount = 0;
        x->frames[i].frStatus   = 0;
        running += bytes;        /* <-- contiguous, the proven layout */
    }
}

static IOReturn submit(xfer_t *x, IOAsyncCallback1 cb){
    if (x->is_pb)
        return (*g_intf)->LowLatencyWriteIsochPipeAsync(g_intf, x->pipe, x->buf,
                   x->next_frame, FRAMES_PER_XFER, UPDATE_FREQ, x->frames, cb, x);
    return (*g_intf)->LowLatencyReadIsochPipeAsync(g_intf, x->pipe, x->buf,
                   x->next_frame, FRAMES_PER_XFER, UPDATE_FREQ, x->frames, cb, x);
}

/* advance the shared monotonic frame counter; reanchor if we fall behind the bus */
static UInt64 next_sched(_Atomic unsigned long long *ctr){
    unsigned long long f = atomic_fetch_add(ctr, XFER_BUS_FRAMES);
    UInt64 now = 0; AbsoluteTime at;
    if ((*g_intf)->GetBusFrameNumber(g_intf, &now, &at) == kIOReturnSuccess){
        long lead = (long)((int64_t)f - (int64_t)now);
        if (lead < 3){
            unsigned long long nb = now + 6;
            atomic_fetch_add(&g_reanchor, 1);
            atomic_store(ctr, nb + XFER_BUS_FRAMES);
            f = nb;
        }
    }
    return (UInt64)f;
}

static void pb_cb(void *refcon, IOReturn result, void *arg0){
    (void)arg0;
    xfer_t *x = (xfer_t*)refcon;
    atomic_fetch_add(&g_pb_cb, 1);
    uint64_t now = mach_absolute_time();
    if (g_last_pb_ticks){
        double gap_ms = (double)(now - g_last_pb_ticks) * g_tb_n2ms;
        if (gap_ms > g_gap_max_ms) g_gap_max_ms = gap_ms;
        if (gap_ms > 20.0) g_late++;
    }
    g_last_pb_ticks = now;
    if (g_stop) return;
    if (result != kIOReturnSuccess && result != kIOReturnUnderrun) atomic_fetch_add(&g_pb_err, 1);
    fill_pb(x);
    x->next_frame = next_sched(&g_pb_next);
    submit(x, pb_cb);
}

static void cap_cb(void *refcon, IOReturn result, void *arg0){
    (void)arg0;
    xfer_t *x = (xfer_t*)refcon;
    atomic_fetch_add(&g_cap_cb, 1);
    if (g_stop) return;
    (void)result; /* drain/discard; only count frames to measure the device rate */
    for (int i = 0; i < FRAMES_PER_XFER; i++){
        atomic_fetch_add(&g_cap_frames, (long)(x->frames[i].frActCount / FRAME_BYTES));
        x->frames[i].frReqCount = MAX_PACKET_SIZE;
        x->frames[i].frActCount = 0;
        x->frames[i].frStatus   = 0;
    }
    x->next_frame = next_sched(&g_cap_next);
    submit(x, cap_cb);
}

/* ---------------- IOKit bring-up (adapted from V2-usbengine-iokit.inc) ------ */
static DevI **find_device(void){
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) return NULL;
    SInt32 vid = VID, pid = PID;
    CFNumberRef nvid = CFNumberCreate(NULL, kCFNumberSInt32Type, &vid);
    CFNumberRef npid = CFNumberCreate(NULL, kCFNumberSInt32Type, &pid);
    CFDictionarySetValue(match, CFSTR(kUSBVendorID),  nvid);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), npid);
    CFRelease(nvid); CFRelease(npid);

    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != kIOReturnSuccess) return NULL;
    io_service_t svc; DevI **dev = NULL;
    while ((svc = IOIteratorNext(it)) != 0){
        IOCFPlugInInterface **plug = NULL; SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
                kIOCFPlugInInterfaceID, &plug, &score) == kIOReturnSuccess && plug){
            (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500), (LPVOID*)&dev);
            (*plug)->Release(plug);
        }
        IOObjectRelease(svc);
        if (dev) break;
    }
    IOObjectRelease(it);
    return dev;
}

static IntfI **find_interface(void){
    IOUSBFindInterfaceRequest req;
    req.bInterfaceClass    = kIOUSBFindInterfaceDontCare;
    req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    req.bAlternateSetting  = kIOUSBFindInterfaceDontCare;
    io_iterator_t it = 0;
    if ((*g_dev)->CreateInterfaceIterator(g_dev, &req, &it) != kIOReturnSuccess) return NULL;
    io_service_t svc; IntfI **chosen = NULL;
    while ((svc = IOIteratorNext(it)) != 0){
        IOCFPlugInInterface **plug = NULL; SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(svc, kIOUSBInterfaceUserClientTypeID,
                kIOCFPlugInInterfaceID, &plug, &score) == kIOReturnSuccess && plug){
            IntfI **intf = NULL;
            (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500), (LPVOID*)&intf);
            (*plug)->Release(plug);
            if (intf){
                UInt8 num = 0xff; (*intf)->GetInterfaceNumber(intf, &num);
                if (num == IFACE){ chosen = intf; IOObjectRelease(svc); break; }
                (*intf)->Release(intf);
            }
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return chosen;
}

static int find_pipe(UInt8 ep_addr, UInt8 *outPipe, UInt16 *outMps){
    UInt8 npipes = 0;
    if ((*g_intf)->GetNumEndpoints(g_intf, &npipes) != kIOReturnSuccess) return -1;
    UInt8 want_dir = (ep_addr & 0x80) ? kUSBIn : kUSBOut;
    UInt8 want_num = ep_addr & 0x0f;
    for (UInt8 p = 1; p <= npipes; p++){
        UInt8 dir, num, tt, interval; UInt16 mps;
        if ((*g_intf)->GetPipeProperties(g_intf, p, &dir, &num, &tt, &mps, &interval) == kIOReturnSuccess)
            if (dir == want_dir && num == want_num){ *outPipe = p; if (outMps) *outMps = mps; return 0; }
    }
    return -1;
}

static IOReturn set_rate(unsigned rate){
    unsigned char p[3];
    switch (rate){
        case 44100: p[0]=0x44; p[1]=0xAC; p[2]=0x00; break;
        case 48000: p[0]=0x80; p[1]=0xBB; p[2]=0x00; break;
        case 88200: p[0]=0x88; p[1]=0x58; p[2]=0x01; break;
        case 96000: p[0]=0x00; p[1]=0x77; p[2]=0x01; break;
        default: return kIOReturnBadArgument;
    }
    IOUSBDevRequest req;
    req.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBEndpoint);
    req.bRequest      = UAC_SET_CUR;
    req.wValue        = UAC_SAMPLING_FREQ_CONTROL;
    req.wIndex        = EP_CAPTURE;   /* 0x81: sets the shared clock */
    req.wLength       = 3;
    req.pData         = p;
    req.wLenDone      = 0;
    return (*g_intf)->ControlRequest(g_intf, 0, &req);
}

static int open_and_stream(void){
    g_dev = find_device();
    if (!g_dev){ fprintf(stderr, "device not found (vid 0644/pid 8021)\n"); return -1; }

    IOReturn o = (*g_dev)->USBDeviceOpen(g_dev);
    if (o != kIOReturnSuccess){ fprintf(stderr, "USBDeviceOpen=0x%x (is the daemon still holding it? quit US122Menu)\n", o); return -1; }

    UInt8 nconf = 0; (*g_dev)->GetNumberOfConfigurations(g_dev, &nconf);
    if (nconf > 0){
        IOUSBConfigurationDescriptorPtr cfg = NULL;
        if ((*g_dev)->GetConfigurationDescriptorPtr(g_dev, 0, &cfg) == kIOReturnSuccess && cfg)
            (*g_dev)->SetConfiguration(g_dev, cfg->bConfigurationValue);
    }

    g_intf = find_interface();
    if (!g_intf){ fprintf(stderr, "interface %d not found\n", IFACE); return -1; }
    if ((*g_intf)->USBInterfaceOpen(g_intf) != kIOReturnSuccess){ fprintf(stderr, "USBInterfaceOpen failed\n"); return -1; }
    if ((*g_intf)->SetAlternateInterface(g_intf, ALT) != kIOReturnSuccess){ fprintf(stderr, "SetAlternateInterface failed\n"); return -1; }
    IOReturn rr = set_rate(g_rate);
    fprintf(stderr, "set_rate(%u)=0x%x\n", g_rate, rr);

    UInt8 pb_pipe=0, cap_pipe=0; UInt16 pb_mps=0, cap_mps=0;
    if (find_pipe(EP_PLAYBACK, &pb_pipe, &pb_mps) != 0){ fprintf(stderr, "playback pipe not found\n"); return -1; }
    if (find_pipe(EP_CAPTURE,  &cap_pipe, &cap_mps) != 0){ fprintf(stderr, "capture pipe not found\n"); return -1; }
    fprintf(stderr, "pipes: pb=%u(mps=%u) cap=%u(mps=%u)\n", pb_pipe, pb_mps, cap_pipe, cap_mps);

    /* reserve our packet size, and clear stale framing/toggle (V2 suspects) */
    (*g_intf)->SetPipePolicy(g_intf, pb_pipe,  MAX_PACKET_SIZE, 1);
    (*g_intf)->SetPipePolicy(g_intf, cap_pipe, MAX_PACKET_SIZE, 1);
    (*g_intf)->ResetPipe(g_intf, pb_pipe);
    (*g_intf)->ResetPipe(g_intf, cap_pipe);

    if ((*g_intf)->CreateInterfaceAsyncEventSource(g_intf, &g_src) != kIOReturnSuccess || !g_src){
        fprintf(stderr, "CreateInterfaceAsyncEventSource failed\n"); return -1;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), g_src, kCFRunLoopDefaultMode);

    UInt64 frame = 0; AbsoluteTime at;
    (*g_intf)->GetBusFrameNumber(g_intf, &frame, &at);
    frame += LEAD_FRAMES;

    UInt32 bufbytes = FRAMES_PER_XFER * MAX_PACKET_SIZE;
    UInt32 flbytes  = FRAMES_PER_XFER * sizeof(IOUSBLowLatencyIsocFrame);
    for (int i = 0; i < NUM_XFERS; i++){
        g_pb[i].is_pb = 1;  g_pb[i].pipe = pb_pipe;
        g_cap[i].is_pb = 0; g_cap[i].pipe = cap_pipe;
        if ((*g_intf)->LowLatencyCreateBuffer(g_intf, &g_pb[i].buf,  bufbytes, kUSBLowLatencyWriteBuffer)     != kIOReturnSuccess ||
            (*g_intf)->LowLatencyCreateBuffer(g_intf, (void**)&g_pb[i].frames,  flbytes, kUSBLowLatencyFrameListBuffer) != kIOReturnSuccess ||
            (*g_intf)->LowLatencyCreateBuffer(g_intf, &g_cap[i].buf, bufbytes, kUSBLowLatencyReadBuffer)      != kIOReturnSuccess ||
            (*g_intf)->LowLatencyCreateBuffer(g_intf, (void**)&g_cap[i].frames, flbytes, kUSBLowLatencyFrameListBuffer) != kIOReturnSuccess){
            fprintf(stderr, "LowLatencyCreateBuffer failed at xfer %d\n", i); return -1;
        }
        memset(g_pb[i].buf, 0, bufbytes);
        memset(g_cap[i].buf, 0, bufbytes);
        for (int f = 0; f < FRAMES_PER_XFER; f++){
            g_pb[i].frames[f].frReqCount  = 0;
            g_cap[i].frames[f].frReqCount = MAX_PACKET_SIZE;
        }
        fill_pb(&g_pb[i]);
        g_pb[i].next_frame  = frame + (UInt64)i * XFER_BUS_FRAMES;
        g_cap[i].next_frame = frame + (UInt64)i * XFER_BUS_FRAMES;
    }
    atomic_store(&g_pb_next,  frame + (unsigned long long)NUM_XFERS * XFER_BUS_FRAMES);
    atomic_store(&g_cap_next, frame + (unsigned long long)NUM_XFERS * XFER_BUS_FRAMES);

    int pb_ok = 0, cap_ok = 0; IOReturn first_err = kIOReturnSuccess;
    for (int i = 0; i < NUM_XFERS; i++){
        IOReturn r1 = submit(&g_pb[i],  pb_cb);
        IOReturn r2 = submit(&g_cap[i], cap_cb);
        if (r1 == kIOReturnSuccess) pb_ok++; else if (first_err==kIOReturnSuccess) first_err=r1;
        if (r2 == kIOReturnSuccess) cap_ok++; else if (first_err==kIOReturnSuccess) first_err=r2;
    }
    fprintf(stderr, "submitted pb_ok=%d cap_ok=%d first_err=0x%x startframe=%llu\n",
            pb_ok, cap_ok, first_err, (unsigned long long)frame);
    return (pb_ok > 0) ? 0 : -1;
}

static void cleanup(void){
    if (g_intf){
        (*g_intf)->AbortPipe(g_intf, 0);
        if (g_src){ CFRunLoopRemoveSource(CFRunLoopGetCurrent(), g_src, kCFRunLoopDefaultMode); CFRelease(g_src); g_src = NULL; }
        for (int i = 0; i < NUM_XFERS; i++){
            if (g_pb[i].buf)     (*g_intf)->LowLatencyDestroyBuffer(g_intf, g_pb[i].buf);
            if (g_pb[i].frames)  (*g_intf)->LowLatencyDestroyBuffer(g_intf, g_pb[i].frames);
            if (g_cap[i].buf)    (*g_intf)->LowLatencyDestroyBuffer(g_intf, g_cap[i].buf);
            if (g_cap[i].frames) (*g_intf)->LowLatencyDestroyBuffer(g_intf, g_cap[i].frames);
        }
        (*g_intf)->USBInterfaceClose(g_intf);
        (*g_intf)->Release(g_intf); g_intf = NULL;
    }
    if (g_dev){ (*g_dev)->USBDeviceClose(g_dev); (*g_dev)->Release(g_dev); g_dev = NULL; }
}

int main(int argc, char **argv){
    if (argc > 1) g_rate = (unsigned)atoi(argv[1]);
    double seconds = (argc > 2) ? atof(argv[2]) : 4.0;
    if (argc > 3) g_freq = atof(argv[3]);
    if (g_rate!=44100 && g_rate!=48000 && g_rate!=88200 && g_rate!=96000){
        fprintf(stderr, "rate must be 44100/48000/88200/96000\n"); return 1;
    }
    signal(SIGINT, on_sigint);
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    g_tb_n2ms = (double)tb.numer / (double)tb.denom / 1e6;
    printf("US-122MKII low-latency tone: %u Hz, %.0f Hz sine, %.1fs, amp=%.2f\n",
           g_rate, g_freq, seconds, g_amp);

    if (open_and_stream() != 0){ cleanup(); return 2; }
    make_thread_realtime();

    uint64_t t0 = mach_absolute_time();
    double last = 0, prev_el = 0; long last_pcb = 0, last_cf = 0;
    for (;;){
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
        double el = (double)(mach_absolute_time()-t0) * tb.numer / tb.denom / 1e9;
        if (g_stop || el >= seconds) break;
        if (el - last >= 0.5){
            long pcb = atomic_load(&g_pb_cb), cf = atomic_load(&g_cap_frames);
            double iv = el - prev_el; if (iv <= 0) iv = 0.5;
            printf("[t=%.1f] pb_cb=%ld(+%ld/s) gap_max=%.1fms late=%ld reanchor=%ld pb_err=%ld | dev~%.0f fps\n",
                   el, pcb, (long)((pcb-last_pcb)/iv), g_gap_max_ms, g_late,
                   atomic_load(&g_reanchor), atomic_load(&g_pb_err), (cf-last_cf)/iv);
            g_gap_max_ms = 0;            /* per-interval worst gap */
            last = el; prev_el = el; last_pcb = pcb; last_cf = cf;
        }
    }
    g_stop = 1;
    long cf = atomic_load(&g_cap_frames);
    double el = (double)(mach_absolute_time()-t0) * tb.numer / tb.denom / 1e9;
    printf("done. pb_cb=%ld cap_cb=%ld pb_err=%ld reanchor=%ld | measured device rate ~%.0f fps\n",
           atomic_load(&g_pb_cb), atomic_load(&g_cap_cb), atomic_load(&g_pb_err),
           atomic_load(&g_reanchor), el>0 ? cf/el : 0.0);
    cleanup();
    return 0;
}
