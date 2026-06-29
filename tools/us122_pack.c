/*
 * us122_pack.c
 *
 * Targeted test: how iso packet DATA is laid out in the transfer buffer.
 *
 * Discovery: the Linux kernel uses iso_frame_desc.offset (fixed 78-byte
 * stride) and .length (36 actual) as INDEPENDENT fields. libusb on macOS
 * may instead expect each packet's data packed TIGHTLY (offset = running
 * sum of lengths). If so, our 78-strided layout makes libusb read each
 * packet from the wrong offset -> fundamental destroyed, grit with no tone,
 * exactly the reported symptom. Bigger packets only shuffle the corruption.
 *
 * Modes (auto-cycled, labeled):
 *   A = STRIDED 78  (current behavior, known to lose the fundamental)
 *   B = TIGHT pack  (stride = actual bytes; what libusb's model expects)
 *   C = CONSTANT 36, tight (force 6 frames/packet, no accumulator variance)
 *   D = CONSTANT 36, strided 78
 *
 * Full-duplex retained (capture drained in parallel). 48k, 24-bit high-3-of-32.
 *
 * Build:
 *   clang us122_pack.c -o us122_pack \
 *       -I"$(brew --prefix libusb)/include/libusb-1.0" \
 *       -L"$(brew --prefix libusb)/lib" -lusb-1.0
 *
 * Run:
 *   ./us122_pack        # plays A,B,C,D ~4s each
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <libusb-1.0/libusb.h>

#define VID 0x0644
#define PID 0x8021
#define IFACE 1
#define ALT 1
#define EP_PLAYBACK 0x02
#define EP_CAPTURE  0x81

#define BYTES_PER_FRAME     6
#define MAX_PACKET_SIZE     78
#define PACKET_SIZE_BASE    36
#define ISO_PACKETS_PER_URB 16
#define NUM_URBS            16
#define MICROFRAMES_PER_SEC 8000

#define RT_H2D_CLASS_EP (LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_ENDPOINT)
#define UAC_SET_CUR 0x01
#define UAC_SAMPLING_FREQ_CONTROL 0x0100

#define TONE_HZ 440.0
#define RATE 48000
#define SECONDS_PER_OPTION 4

/* mode flags */
static int  M_TIGHT    = 0;  /* 1 = pack tight (stride=len), 0 = stride 78 */
static int  M_CONSTANT = 0;  /* 1 = force 36 bytes/packet, 0 = accumulator */

static volatile int g_stop = 0;
static volatile int g_opt_done = 0;
static int g_active = 0;
static unsigned int g_accum = 0;
static double g_phase = 0.0;

static void on_sigint(int s){ (void)s; g_stop = 1; }

static int set_rate(libusb_device_handle *h){
    unsigned char p[3] = {0x80,0xBB,0x00};
    int r = libusb_control_transfer(h, RT_H2D_CLASS_EP, UAC_SET_CUR,
              UAC_SAMPLING_FREQ_CONTROL, EP_CAPTURE, p, 3, 1000);
    return (r==3)?0:(r<0?r:-1);
}

static void put3(unsigned char *d, double norm){
    int32_t s = (int32_t)lround(norm * 2147483647.0);
    d[0] = (s >> 8)  & 0xff;
    d[1] = (s >> 16) & 0xff;
    d[2] = (s >> 24) & 0xff;
}

/* Returns frames for this packet under current mode. */
static int frames_this_packet(void){
    if (M_CONSTANT) return PACKET_SIZE_BASE / BYTES_PER_FRAME; /* 6 */
    g_accum += RATE;
    int frames = g_accum / MICROFRAMES_PER_SEC;
    g_accum %= MICROFRAMES_PER_SEC;
    return frames;
}

static void fill_playback(struct libusb_transfer *xfer){
    unsigned char *buf = xfer->buffer;
    const double amp = 0.25;
    const double step = 2.0 * M_PI * TONE_HZ / (double)RATE;
    int stride = M_TIGHT ? 0 : MAX_PACKET_SIZE; /* tight computed below */
    int running = 0; /* byte offset for tight packing */

    for (int i = 0; i < ISO_PACKETS_PER_URB; i++){
        int frames = frames_this_packet();
        int bytes = frames * BYTES_PER_FRAME;
        if (bytes > MAX_PACKET_SIZE){ frames = MAX_PACKET_SIZE/BYTES_PER_FRAME; bytes = frames*BYTES_PER_FRAME; }

        int off = M_TIGHT ? running : (i * stride);
        unsigned char *pkt = buf + off;
        for (int f = 0; f < frames; f++){
            double s = sin(g_phase)*amp;
            g_phase += step; if (g_phase > 2.0*M_PI) g_phase -= 2.0*M_PI;
            put3(pkt + f*BYTES_PER_FRAME,     s);
            put3(pkt + f*BYTES_PER_FRAME + 3, s);
        }
        xfer->iso_packet_desc[i].length = bytes;
        running += bytes;
    }
}

static void LIBUSB_CALL playback_cb(struct libusb_transfer *xfer){
    g_active--;
    if (g_stop || g_opt_done || xfer->status == LIBUSB_TRANSFER_CANCELLED) return;
    fill_playback(xfer);
    if (libusb_submit_transfer(xfer) < 0){ g_opt_done = 1; return; }
    g_active++;
}

static void LIBUSB_CALL capture_cb(struct libusb_transfer *xfer){
    g_active--;
    if (g_stop || g_opt_done || xfer->status == LIBUSB_TRANSFER_CANCELLED) return;
    for (int i = 0; i < ISO_PACKETS_PER_URB; i++)
        xfer->iso_packet_desc[i].length = MAX_PACKET_SIZE;
    if (libusb_submit_transfer(xfer) < 0){ g_opt_done = 1; return; }
    g_active++;
}

static void play_option(libusb_context *ctx, libusb_device_handle *h,
                        char letter, int tight, int constant, const char *desc){
    M_TIGHT = tight; M_CONSTANT = constant;
    g_accum = 0; g_phase = 0.0; g_opt_done = 0; g_active = 0;

    printf("\n===== OPTION %c: %s =====\n", letter, desc); fflush(stdout);
    set_rate(h);

    int buflen = ISO_PACKETS_PER_URB * MAX_PACKET_SIZE; /* always alloc max */
    struct libusb_transfer *pb[NUM_URBS], *cap[NUM_URBS];

    for (int i=0;i<NUM_URBS;i++){
        pb[i] = libusb_alloc_transfer(ISO_PACKETS_PER_URB);
        unsigned char *pbuf = calloc(1, buflen);
        libusb_fill_iso_transfer(pb[i], h, EP_PLAYBACK, pbuf, buflen,
            ISO_PACKETS_PER_URB, playback_cb, NULL, 1000);
        libusb_set_iso_packet_lengths(pb[i], MAX_PACKET_SIZE);
        fill_playback(pb[i]);

        cap[i] = libusb_alloc_transfer(ISO_PACKETS_PER_URB);
        unsigned char *cbuf = calloc(1, buflen);
        libusb_fill_iso_transfer(cap[i], h, EP_CAPTURE, cbuf, buflen,
            ISO_PACKETS_PER_URB, capture_cb, NULL, 1000);
        libusb_set_iso_packet_lengths(cap[i], MAX_PACKET_SIZE);
    }
    for (int i=0;i<NUM_URBS;i++){
        if (libusb_submit_transfer(pb[i])  < 0){ g_opt_done=1; break; } g_active++;
        if (libusb_submit_transfer(cap[i]) < 0){ g_opt_done=1; break; } g_active++;
    }

    time_t start = time(NULL);
    while (!g_stop && !g_opt_done && (time(NULL)-start) < SECONDS_PER_OPTION){
        struct timeval tv={0,100000}; libusb_handle_events_timeout(ctx,&tv);
    }
    g_opt_done = 1;
    for (int i=0;i<NUM_URBS;i++){ if(pb[i]) libusb_cancel_transfer(pb[i]); if(cap[i]) libusb_cancel_transfer(cap[i]); }
    while (g_active > 0){ struct timeval tv={0,100000}; libusb_handle_events_timeout(ctx,&tv); }
    for (int i=0;i<NUM_URBS;i++){
        if(pb[i]){ free(pb[i]->buffer); libusb_free_transfer(pb[i]); }
        if(cap[i]){ free(cap[i]->buffer); libusb_free_transfer(cap[i]); }
    }
}

int main(void){
    signal(SIGINT, on_sigint);
    libusb_context *ctx=NULL;
    if (libusb_init(&ctx)!=0){ fprintf(stderr,"init failed\n"); return 1; }
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx,VID,PID);
    if (!h){ fprintf(stderr,"open failed\n"); libusb_exit(ctx); return 2; }
    libusb_set_auto_detach_kernel_driver(h,1);
    if (libusb_claim_interface(h,IFACE)<0){ fprintf(stderr,"claim failed\n"); libusb_close(h); libusb_exit(ctx); return 3; }
    if (libusb_set_interface_alt_setting(h,IFACE,ALT)<0){ fprintf(stderr,"alt failed\n"); libusb_release_interface(h,IFACE); libusb_close(h); libusb_exit(ctx); return 4; }
    if (set_rate(h)<0){ fprintf(stderr,"set_rate failed\n"); libusb_release_interface(h,IFACE); libusb_close(h); libusb_exit(ctx); return 5; }

    printf("Packing test. Listen for which option gives a CLEAN 440 Hz.\n");
    printf("Hypothesis: B or C (tight packing) restores the fundamental.\n");

    play_option(ctx,h,'A',0,0,"STRIDED 78, accumulator  (current; loses tone)");
    if (!g_stop){ for(int t=0;t<10&&!g_stop;t++){struct timeval tv={0,100000};libusb_handle_events_timeout(ctx,&tv);} }
    play_option(ctx,h,'B',1,0,"TIGHT pack, accumulator   (libusb-expected)");
    if (!g_stop){ for(int t=0;t<10&&!g_stop;t++){struct timeval tv={0,100000};libusb_handle_events_timeout(ctx,&tv);} }
    play_option(ctx,h,'C',1,1,"TIGHT pack, constant 36   (no size variance)");
    if (!g_stop){ for(int t=0;t<10&&!g_stop;t++){struct timeval tv={0,100000};libusb_handle_events_timeout(ctx,&tv);} }
    play_option(ctx,h,'D',0,1,"STRIDED 78, constant 36   (control)");

    libusb_release_interface(h,IFACE);
    libusb_close(h); libusb_exit(ctx);
    printf("\ndone. Report which option had the cleanest 440 Hz.\n");
    return 0;
}
