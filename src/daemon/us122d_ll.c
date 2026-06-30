/*
 * us122d_ll.c  -  US-122MKII helper daemon, LOW-LATENCY IOKit transport (Stage 3)
 *
 * Drop-in alternative to the libusb us122d. It speaks the SAME shm contract
 * (shmring.h), so the existing HAL plugin and menu app work unchanged, but the
 * USB transport is Apple's low-latency isochronous IOUSBLib API instead of
 * libusb. That is the jitter cure proven in tools/us122_lowlat_tone.c
 * (Stage 1: gap_max <= 3.6ms under load vs libusb's 115-419ms).
 *
 * V1 (us122d.c) is left intact for A/B. Run this one instead to test.
 *
 * What it keeps from V1:
 *   - the shm rings + flags (device_present/app_active/user_hide/running/...)
 *   - the dormant gate (parks while app_active==0)
 *   - the interpolating resampler reading the playback ring toward the
 *     buffer_ms setpoint (the two-clock reconciliation)
 *   - the capture-to-ring logic (push actual bytes; cheap-discard when no consumer)
 *
 * What changes: the transport. Native IOKit open/claim/rate/pipes + low-latency
 * LowLatencyWrite/ReadIsochPipeAsync on a realtime CFRunLoop thread, CONTIGUOUS
 * playback packing (the Stage 0 layout fix).
 *
 * Build (on the Mac):
 *   clang src/daemon/us122d_ll.c -o us122d_ll \
 *       -framework IOKit -framework CoreFoundation -lpthread -Wall -O2
 *
 * NOT YET HARDWARE-VALIDATED as of writing. First run pending.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>

#include "shmring.h"

#define US122_V3_VERSION "V3 low-latency (build1)"

#define VID 0x0644
#define PID 0x8021
#define IFACE 1
#define ALT 1
#define EP_PLAYBACK 0x02
#define EP_CAPTURE  0x81

#define MAX_PACKET_SIZE     78
#define MICROFRAMES_PER_SEC 8000
#define UFRAMES_PER_FRAME   8
#define FRAMES_PER_XFER     8
#define XFER_BUS_FRAMES     (FRAMES_PER_XFER / UFRAMES_PER_FRAME)
#define NUM_XFERS           32
#define UPDATE_FREQ         1
#define LEAD_FRAMES         50

#define UAC_SET_CUR 0x01
#define UAC_SAMPLING_FREQ_CONTROL 0x0100

typedef IOUSBDeviceInterface500    DevI;
typedef IOUSBInterfaceInterface500 IntfI;

typedef struct {
    UInt8 pipe; int is_pb;
    void *buf; IOUSBLowLatencyIsocFrame *frames;
    UInt64 next_frame;
} xfer_t;

static shm_layout_t *g_shm = NULL;
static DevI  **g_dev  = NULL;
static IntfI **g_intf = NULL;
static CFRunLoopSourceRef g_src = NULL;
static xfer_t g_pb[NUM_XFERS], g_cap[NUM_XFERS];

static volatile sig_atomic_t g_stop = 0;     /* process exit */
static volatile int g_session_end = 0;       /* end this stream, go dormant/acquire */
static unsigned g_rate = 48000;
static unsigned g_accum = 0;

/* resampler state (mirrors V1 us122d.c) */
static double   g_resamp_frac = 0.0;
static double   g_ratio = 1.0;

/* counters / instrumentation */
static _Atomic long g_pb_cb=0, g_cap_cb=0, g_pb_err=0, g_cap_over=0, g_underrun=0;
static _Atomic unsigned long long g_pb_next=0, g_cap_next=0;
static _Atomic long g_reanchor=0;
static int g_xfer_errors = 0;
#define MAX_XFER_ERRORS 64
static double   g_tb_n2ms = 0;
static uint64_t g_last_pb_ticks = 0;
static double   g_gap_max_ms = 0;

/* ---- device 24-bit packing (identical to V1) ---- */
static inline int32_t dev3_to_i32(const uint8_t *p){
    return (int32_t)(((uint32_t)p[0]<<8) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<24));
}
static inline void i32_to_dev3(int32_t v, uint8_t *o){
    o[0]=(v>>8)&0xff; o[1]=(v>>16)&0xff; o[2]=(v>>24)&0xff;
}

static void set_status(const char *s){
    if (!g_shm) return;
    strncpy(g_shm->status, s, SHM_STATUS_LEN-1);
    g_shm->status[SHM_STATUS_LEN-1] = '\0';
}
static void logts(const char *s){
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(stderr, "[ll %ld.%03ld] %s\n", (long)ts.tv_sec, ts.tv_nsec/1000000, s);
}

static void make_thread_realtime(void){
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double n2a = (double)tb.numer / (double)tb.denom;
    thread_time_constraint_policy_data_t p;
    p.period      = (uint32_t)(1000000.0 / n2a);
    p.computation = (uint32_t)( 900000.0 / n2a);
    p.constraint  = (uint32_t)(1000000.0 / n2a);
    p.preemptible = 0;
    thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

/* ---- playback fill: V1's interpolating resampler -> contiguous IOKit buffer ---- */
static void fill_pb(xfer_t *x){
    uint8_t *buf = (uint8_t*)x->buf;

    /* set the read ratio to hold the ring near the buffer_ms setpoint. Gentle
       proportional control, clamped to +-1% so pitch never audibly bends
       (same envelope V1 uses). */
    uint32_t bufms = atomic_load_explicit(&g_shm->buffer_ms, memory_order_relaxed);
    if (bufms < BUFFER_MS_MIN) bufms = BUFFER_MS_DEFAULT;
    uint32_t setpoint = (uint32_t)(((uint64_t)bufms * g_rate) / 1000);
    if (setpoint < 1) setpoint = 1;
    uint32_t avail = ring_fill(&g_shm->playback);
    double err = (double)avail - (double)setpoint;
    double adj = err / (double)setpoint * 0.25;
    if (adj > 0.01) adj = 0.01; else if (adj < -0.01) adj = -0.01;
    g_ratio = 1.0 + adj;

    uint32_t tail0 = atomic_load_explicit(&g_shm->playback.tail, memory_order_relaxed);
    double pos = g_resamp_frac;
    int running = 0;
    uint64_t clocked = 0;

    for (int i = 0; i < FRAMES_PER_XFER; i++){
        g_accum += g_rate;
        int frames = g_accum / MICROFRAMES_PER_SEC;
        g_accum %= MICROFRAMES_PER_SEC;
        int bytes = frames * FRAME_BYTES;
        if (bytes > MAX_PACKET_SIZE){ frames = MAX_PACKET_SIZE/FRAME_BYTES; bytes = frames*FRAME_BYTES; }

        for (int f = 0; f < frames; f++){
            uint32_t base = (uint32_t)pos;
            double mu = pos - (double)base;
            uint8_t *o = buf + running + f*FRAME_BYTES;
            if (base + 1 >= avail){
                memset(o, 0, FRAME_BYTES);       /* underrun: silence, hold pos */
                atomic_fetch_add_explicit(&g_underrun, 1, memory_order_relaxed);
                continue;
            }
            uint32_t slotA = (tail0 + base)     & RING_MASK;
            uint32_t slotB = (tail0 + base + 1) & RING_MASK;
            const uint8_t *a = g_shm->playback.data + slotA*FRAME_BYTES;
            const uint8_t *b = g_shm->playback.data + slotB*FRAME_BYTES;
            for (int c = 0; c < 2; c++){
                int32_t sa = dev3_to_i32(a + c*3);
                int32_t sb = dev3_to_i32(b + c*3);
                double v = (double)sa + ((double)sb - (double)sa) * mu;
                i32_to_dev3((int32_t)v, o + c*3);
            }
            pos += g_ratio;
        }
        x->frames[i].frReqCount = (UInt16)bytes;
        x->frames[i].frActCount = 0;
        x->frames[i].frStatus   = 0;
        running += bytes;          /* CONTIGUOUS (the Stage 0 fix) */
        clocked += frames;
    }

    uint32_t consumed = (uint32_t)pos;
    if (consumed > avail) consumed = avail;
    if (consumed > 0)
        atomic_store_explicit(&g_shm->playback.tail, tail0 + consumed, memory_order_release);
    g_resamp_frac = pos - (double)consumed;

    atomic_fetch_add_explicit(&g_shm->dev_frames, clocked, memory_order_relaxed);
}

static IOReturn submit(xfer_t *x, IOAsyncCallback1 cb){
    if (x->is_pb)
        return (*g_intf)->LowLatencyWriteIsochPipeAsync(g_intf, x->pipe, x->buf,
                   x->next_frame, FRAMES_PER_XFER, UPDATE_FREQ, x->frames, cb, x);
    return (*g_intf)->LowLatencyReadIsochPipeAsync(g_intf, x->pipe, x->buf,
                   x->next_frame, FRAMES_PER_XFER, UPDATE_FREQ, x->frames, cb, x);
}

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
        double gap = (double)(now - g_last_pb_ticks) * g_tb_n2ms;
        if (gap > g_gap_max_ms) g_gap_max_ms = gap;
    }
    g_last_pb_ticks = now;
    if (g_stop || g_session_end) return;
    if (result != kIOReturnSuccess && result != kIOReturnUnderrun){
        atomic_fetch_add(&g_pb_err, 1);
        if (++g_xfer_errors > MAX_XFER_ERRORS){ logts("playback: too many errors; ending session"); g_session_end = 1; return; }
    } else {
        g_xfer_errors = 0;
    }
    fill_pb(x);
    x->next_frame = next_sched(&g_pb_next);
    submit(x, pb_cb);
}

static void cap_cb(void *refcon, IOReturn result, void *arg0){
    (void)arg0;
    xfer_t *x = (xfer_t*)refcon;
    atomic_fetch_add(&g_cap_cb, 1);
    if (g_stop || g_session_end) return;
    if (result != kIOReturnSuccess && result != kIOReturnOverrun){
        if (++g_xfer_errors > MAX_XFER_ERRORS){ logts("capture: too many errors; ending session"); g_session_end = 1; return; }
    }
    uint8_t *buf = (uint8_t*)x->buf;
    if (ring_space(&g_shm->capture) == 0){
        /* no consumer draining: empty cheaply, do no per-frame work */
        uint32_t h = atomic_load_explicit(&g_shm->capture.head, memory_order_relaxed);
        atomic_store_explicit(&g_shm->capture.tail, h, memory_order_release);
        atomic_fetch_add(&g_cap_over, 1);
    } else {
        for (int i = 0; i < FRAMES_PER_XFER; i++){
            uint32_t len = x->frames[i].frActCount;
            uint32_t frames = len / FRAME_BYTES;
            if (frames){
                uint32_t space = ring_space(&g_shm->capture);
                if (space < frames) frames = space;
                if (frames) ring_produce(&g_shm->capture, buf + (size_t)i*MAX_PACKET_SIZE, frames);
            }
        }
    }
    for (int i = 0; i < FRAMES_PER_XFER; i++){
        x->frames[i].frReqCount = MAX_PACKET_SIZE;
        x->frames[i].frActCount = 0;
        x->frames[i].frStatus   = 0;
    }
    x->next_frame = next_sched(&g_cap_next);
    submit(x, cap_cb);
}

/* ---------------- IOKit bring-up (from the validated tester) ---------------- */
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
    req.bInterfaceClass=kIOUSBFindInterfaceDontCare; req.bInterfaceSubClass=kIOUSBFindInterfaceDontCare;
    req.bInterfaceProtocol=kIOUSBFindInterfaceDontCare; req.bAlternateSetting=kIOUSBFindInterfaceDontCare;
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
                UInt8 num=0xff; (*intf)->GetInterfaceNumber(intf, &num);
                if (num == IFACE){ chosen = intf; IOObjectRelease(svc); break; }
                (*intf)->Release(intf);
            }
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return chosen;
}
static int find_pipe(UInt8 ep_addr, UInt8 *outPipe){
    UInt8 npipes = 0;
    if ((*g_intf)->GetNumEndpoints(g_intf, &npipes) != kIOReturnSuccess) return -1;
    UInt8 want_dir = (ep_addr & 0x80) ? kUSBIn : kUSBOut;
    UInt8 want_num = ep_addr & 0x0f;
    for (UInt8 p = 1; p <= npipes; p++){
        UInt8 dir, num, tt, interval; UInt16 mps;
        if ((*g_intf)->GetPipeProperties(g_intf, p, &dir, &num, &tt, &mps, &interval) == kIOReturnSuccess)
            if (dir == want_dir && num == want_num){ *outPipe = p; return 0; }
    }
    return -1;
}
static IOReturn set_rate(unsigned rate){
    unsigned char p[3];
    switch (rate){
        case 44100: p[0]=0x44;p[1]=0xAC;p[2]=0x00; break;
        case 48000: p[0]=0x80;p[1]=0xBB;p[2]=0x00; break;
        case 88200: p[0]=0x88;p[1]=0x58;p[2]=0x01; break;
        case 96000: p[0]=0x00;p[1]=0x77;p[2]=0x01; break;
        default: return kIOReturnBadArgument;
    }
    IOUSBDevRequest req;
    req.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBEndpoint);
    req.bRequest = UAC_SET_CUR; req.wValue = UAC_SAMPLING_FREQ_CONTROL;
    req.wIndex = EP_CAPTURE; req.wLength = 3; req.pData = p; req.wLenDone = 0;
    return (*g_intf)->ControlRequest(g_intf, 0, &req);
}

static void session_cleanup(void){
    if (g_intf){
        (*g_intf)->AbortPipe(g_intf, 0);
        if (g_src){ CFRunLoopRemoveSource(CFRunLoopGetCurrent(), g_src, kCFRunLoopDefaultMode); CFRelease(g_src); g_src = NULL; }
        for (int i = 0; i < NUM_XFERS; i++){
            if (g_pb[i].buf)     (*g_intf)->LowLatencyDestroyBuffer(g_intf, g_pb[i].buf);
            if (g_pb[i].frames)  (*g_intf)->LowLatencyDestroyBuffer(g_intf, g_pb[i].frames);
            if (g_cap[i].buf)    (*g_intf)->LowLatencyDestroyBuffer(g_intf, g_cap[i].buf);
            if (g_cap[i].frames) (*g_intf)->LowLatencyDestroyBuffer(g_intf, g_cap[i].frames);
            memset(&g_pb[i], 0, sizeof(xfer_t)); memset(&g_cap[i], 0, sizeof(xfer_t));
        }
        (*g_intf)->USBInterfaceClose(g_intf);
        (*g_intf)->Release(g_intf); g_intf = NULL;
    }
    if (g_dev){ (*g_dev)->USBDeviceClose(g_dev); (*g_dev)->Release(g_dev); g_dev = NULL; }
}

/* open device, claim, set rate, alloc low-latency buffers, submit pool. */
static int session_start(void){
    g_dev = find_device();
    if (!g_dev) return -1;
    if ((*g_dev)->USBDeviceOpen(g_dev) != kIOReturnSuccess){ (*g_dev)->Release(g_dev); g_dev=NULL; return -1; }
    UInt8 nconf=0; (*g_dev)->GetNumberOfConfigurations(g_dev, &nconf);
    if (nconf > 0){
        IOUSBConfigurationDescriptorPtr cfg=NULL;
        if ((*g_dev)->GetConfigurationDescriptorPtr(g_dev, 0, &cfg)==kIOReturnSuccess && cfg)
            (*g_dev)->SetConfiguration(g_dev, cfg->bConfigurationValue);
    }
    g_intf = find_interface();
    if (!g_intf){ session_cleanup(); return -1; }
    if ((*g_intf)->USBInterfaceOpen(g_intf) != kIOReturnSuccess){ session_cleanup(); return -1; }
    if ((*g_intf)->SetAlternateInterface(g_intf, ALT) != kIOReturnSuccess){ session_cleanup(); return -1; }
    if (set_rate(g_rate) != kIOReturnSuccess){ session_cleanup(); return -1; }

    UInt8 pb_pipe=0, cap_pipe=0;
    if (find_pipe(EP_PLAYBACK,&pb_pipe)!=0 || find_pipe(EP_CAPTURE,&cap_pipe)!=0){ session_cleanup(); return -1; }
    (*g_intf)->SetPipePolicy(g_intf, pb_pipe,  MAX_PACKET_SIZE, 1);
    (*g_intf)->SetPipePolicy(g_intf, cap_pipe, MAX_PACKET_SIZE, 1);
    (*g_intf)->ResetPipe(g_intf, pb_pipe);
    (*g_intf)->ResetPipe(g_intf, cap_pipe);

    if ((*g_intf)->CreateInterfaceAsyncEventSource(g_intf, &g_src) != kIOReturnSuccess || !g_src){ session_cleanup(); return -1; }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), g_src, kCFRunLoopDefaultMode);

    UInt64 frame=0; AbsoluteTime at;
    (*g_intf)->GetBusFrameNumber(g_intf, &frame, &at);
    frame += LEAD_FRAMES;
    g_accum = 0; g_resamp_frac = 0.0; g_ratio = 1.0; g_xfer_errors = 0;

    UInt32 bufbytes = FRAMES_PER_XFER * MAX_PACKET_SIZE;
    UInt32 flbytes  = FRAMES_PER_XFER * sizeof(IOUSBLowLatencyIsocFrame);
    for (int i = 0; i < NUM_XFERS; i++){
        g_pb[i].is_pb=1;  g_pb[i].pipe=pb_pipe;
        g_cap[i].is_pb=0; g_cap[i].pipe=cap_pipe;
        if ((*g_intf)->LowLatencyCreateBuffer(g_intf, &g_pb[i].buf,  bufbytes, kUSBLowLatencyWriteBuffer)!=kIOReturnSuccess ||
            (*g_intf)->LowLatencyCreateBuffer(g_intf, (void**)&g_pb[i].frames, flbytes, kUSBLowLatencyFrameListBuffer)!=kIOReturnSuccess ||
            (*g_intf)->LowLatencyCreateBuffer(g_intf, &g_cap[i].buf, bufbytes, kUSBLowLatencyReadBuffer)!=kIOReturnSuccess ||
            (*g_intf)->LowLatencyCreateBuffer(g_intf, (void**)&g_cap[i].frames, flbytes, kUSBLowLatencyFrameListBuffer)!=kIOReturnSuccess){
            session_cleanup(); return -1;
        }
        memset(g_pb[i].buf, 0, bufbytes); memset(g_cap[i].buf, 0, bufbytes);
        for (int f=0; f<FRAMES_PER_XFER; f++){ g_pb[i].frames[f].frReqCount=0; g_cap[i].frames[f].frReqCount=MAX_PACKET_SIZE; }
        fill_pb(&g_pb[i]);
        g_pb[i].next_frame  = frame + (UInt64)i*XFER_BUS_FRAMES;
        g_cap[i].next_frame = frame + (UInt64)i*XFER_BUS_FRAMES;
    }
    atomic_store(&g_pb_next,  frame + (unsigned long long)NUM_XFERS*XFER_BUS_FRAMES);
    atomic_store(&g_cap_next, frame + (unsigned long long)NUM_XFERS*XFER_BUS_FRAMES);

    int pb_ok=0, cap_ok=0; IOReturn first_err=kIOReturnSuccess;
    for (int i = 0; i < NUM_XFERS; i++){
        IOReturn r1 = submit(&g_pb[i], pb_cb);
        IOReturn r2 = submit(&g_cap[i], cap_cb);
        if (r1==kIOReturnSuccess) pb_ok++; else if (first_err==kIOReturnSuccess) first_err=r1;
        if (r2==kIOReturnSuccess) cap_ok++; else if (first_err==kIOReturnSuccess) first_err=r2;
    }
    fprintf(stderr, "[ll] streaming %u Hz: pb_ok=%d cap_ok=%d first_err=0x%x startframe=%llu\n",
            g_rate, pb_ok, cap_ok, first_err, (unsigned long long)frame);
    return (pb_ok > 0) ? 0 : -1;
}

/* ---- shm create/attach (daemon owns the segment) ---- */
static int shm_setup(void){
    int fd = shm_open(SHM_NAME, O_CREAT|O_RDWR, 0666);
    if (fd < 0){ perror("shm_open"); return -1; }
    /* size only if freshly created; macOS rejects ftruncate on an already-sized
       shm object (the libusb daemon may have created it). */
    struct stat st;
    if (fstat(fd, &st) == 0 && (size_t)st.st_size < sizeof(shm_layout_t)){
        if (ftruncate(fd, sizeof(shm_layout_t)) != 0){ perror("ftruncate"); close(fd); return -1; }
    }
    void *p = mmap(NULL, sizeof(shm_layout_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED){ perror("mmap"); return -1; }
    g_shm = (shm_layout_t*)p;
    if (!shm_valid(g_shm)){
        shm_init_layout(g_shm, g_rate);
        logts("created/initialized shm");
    } else {
        logts("attached existing shm");
    }
    return 0;
}

static void on_sig(int s){ (void)s; g_stop = 1; CFRunLoopStop(CFRunLoopGetCurrent()); }

int main(void){
    setvbuf(stderr, NULL, _IOLBF, 0);
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    g_tb_n2ms = (double)tb.numer / (double)tb.denom / 1e6;

    if (shm_setup() != 0) return 1;
    int g_debug = (getenv("US122_DEBUG") != NULL);  /* gate the per-0.5s jitter log */
    int force = (getenv("US122_LL_FORCE") != NULL); /* debug: stream regardless of app_active */
    if (force) atomic_store_explicit(&g_shm->user_hide, 0, memory_order_release); /* debug: let plugin show it */
    { char b[96]; snprintf(b, sizeof b, "us122d (%s) starting%s", US122_V3_VERSION, force?" [FORCE debug]":""); logts(b); }

    while (!g_stop){
        /* DORMANT gate: park while the menu app is closed */
        if (!force && !atomic_load_explicit(&g_shm->app_active, memory_order_acquire)){
            atomic_store_explicit(&g_shm->device_present, 0, memory_order_release);
            set_status("dormant (app closed)");
            while (!g_stop && !atomic_load_explicit(&g_shm->app_active, memory_order_acquire))
                usleep(200000);
            continue;
        }

        /* ACQUIRE: try to open + stream the device */
        g_rate = atomic_load_explicit(&g_shm->rate, memory_order_relaxed);
        if (g_rate!=44100 && g_rate!=48000 && g_rate!=88200 && g_rate!=96000) g_rate = 48000;
        g_session_end = 0;
        if (session_start() != 0){
            set_status("waiting for device");
            session_cleanup();
            usleep(500000);
            continue;
        }
        atomic_store_explicit(&g_shm->device_present, 1, memory_order_release);
        set_status("streaming (low-latency)");
        make_thread_realtime();

        /* STREAM: service iso completions; watch app_active + session end.
           Log a jitter line every ~0.5s. */
        double last = 0; long lpb = 0; uint64_t t0 = mach_absolute_time();
        while (!g_stop && !g_session_end &&
               (force || atomic_load_explicit(&g_shm->app_active, memory_order_acquire))){
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
            /* the plugin can change the sample rate (Audio MIDI Setup). Restart
               the session at the new rate (packet sizing is rate-dependent). */
            unsigned want = atomic_load_explicit(&g_shm->rate, memory_order_relaxed);
            if ((want==44100||want==48000||want==88200||want==96000) && want != g_rate){
                logts("rate change requested; restarting session at new rate");
                break;
            }
            double el = (double)(mach_absolute_time()-t0) * tb.numer / tb.denom / 1e9;
            if (g_debug && el - last >= 0.5){
                long pb = atomic_load(&g_pb_cb);
                fprintf(stderr, "[ll t=%.1f] pb_cb=%ld(+%ld/s) gap_max=%.1fms underrun=%ld cap_over=%ld pb_err=%ld reanchor=%ld pb_ring=%u\n",
                        el, pb, (long)((pb-lpb)/(el-last)), g_gap_max_ms,
                        atomic_load(&g_underrun), atomic_load(&g_cap_over), atomic_load(&g_pb_err),
                        atomic_load(&g_reanchor), ring_fill(&g_shm->playback));
                g_gap_max_ms = 0; last = el; lpb = pb;
            }
        }

        logts("session ending; tearing down");
        atomic_store_explicit(&g_shm->device_present, 0, memory_order_release);
        g_session_end = 1;
        session_cleanup();
    }

    logts("us122d_ll exiting");
    if (g_shm){ atomic_store_explicit(&g_shm->device_present, 0, memory_order_release); set_status("daemon stopped"); }
    return 0;
}
