/*
 * us122_wavplay.c
 *
 * Plays a real WAV file through the TASCAM US-122MKII using the proven
 * transport: tight-packed full-duplex isochronous streaming.
 *
 * Reuses the confirmed-correct core:
 *   - interface 1 / alt 1, rate set via control transfer
 *   - playback EP 0x02 + capture EP 0x81 BOTH running (device is coupled)
 *   - TIGHT packet packing (stride = actual length) <-- the key fix
 *   - wire format 24-bit as high-3-of-32, 6 bytes/frame
 *
 * New, and built to lift directly into the HAL plugin later:
 *   - a lock-based ring buffer decoupling file reads from the USB callback
 *   - a minimal WAV (RIFF/PCM) parser for 16- and 24-bit stereo
 *
 * Scope: playback only of a real file. Capture is still just drained and
 * discarded. No resampling: the WAV must already be 44100/48000/88200/96000.
 *
 * Build:
 *   clang us122_wavplay.c -o us122_wavplay -lpthread \
 *       -I"$(brew --prefix libusb)/include/libusb-1.0" \
 *       -L"$(brew --prefix libusb)/lib" -lusb-1.0
 *
 * Run:
 *   ./us122_wavplay song.wav
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <libusb-1.0/libusb.h>

#define VID 0x0644
#define PID 0x8021
#define IFACE 1
#define ALT 1
#define EP_PLAYBACK 0x02
#define EP_CAPTURE  0x81

#define BYTES_PER_FRAME     6     /* device wire: 24-bit * 2ch */
#define MAX_PACKET_SIZE     78
#define ISO_PACKETS_PER_URB 16
#define NUM_URBS            16
#define MICROFRAMES_PER_SEC 8000

#define RT_H2D_CLASS_EP (LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_ENDPOINT)
#define UAC_SET_CUR 0x01
#define UAC_SAMPLING_FREQ_CONTROL 0x0100

/* ===================== ring buffer (HAL-plugin-reusable) ===================== */
/* Stores device-ready frames: 6 bytes each (L 3 bytes, R 3 bytes). */
typedef struct {
    unsigned char *buf;
    size_t cap_frames;       /* capacity in frames */
    size_t head;             /* write index (frames) */
    size_t tail;             /* read index (frames) */
    size_t count;            /* frames currently stored */
    pthread_mutex_t lock;
    int eof;                 /* producer finished */
    long underruns;
} ring_t;

static void ring_init(ring_t *r, size_t frames){
    r->buf = calloc(frames, BYTES_PER_FRAME);
    r->cap_frames = frames;
    r->head = r->tail = r->count = 0;
    r->eof = 0; r->underruns = 0;
    pthread_mutex_init(&r->lock, NULL);
}
static void ring_free(ring_t *r){ free(r->buf); pthread_mutex_destroy(&r->lock); }

/* producer: write up to n frames, returns frames actually written */
static size_t ring_write(ring_t *r, const unsigned char *src, size_t n){
    pthread_mutex_lock(&r->lock);
    size_t space = r->cap_frames - r->count;
    if (n > space) n = space;
    for (size_t i = 0; i < n; i++){
        memcpy(r->buf + r->head*BYTES_PER_FRAME, src + i*BYTES_PER_FRAME, BYTES_PER_FRAME);
        r->head = (r->head + 1) % r->cap_frames;
    }
    r->count += n;
    pthread_mutex_unlock(&r->lock);
    return n;
}

/* consumer: read up to n frames; zero-fills any shortfall and counts underrun.
   returns frames of REAL audio delivered (not counting zero-fill). */
static size_t ring_read(ring_t *r, unsigned char *dst, size_t n){
    pthread_mutex_lock(&r->lock);
    size_t avail = r->count < n ? r->count : n;
    for (size_t i = 0; i < avail; i++){
        memcpy(dst + i*BYTES_PER_FRAME, r->buf + r->tail*BYTES_PER_FRAME, BYTES_PER_FRAME);
        r->tail = (r->tail + 1) % r->cap_frames;
    }
    r->count -= avail;
    int eof = r->eof;
    pthread_mutex_unlock(&r->lock);

    if (avail < n){
        memset(dst + avail*BYTES_PER_FRAME, 0, (n-avail)*BYTES_PER_FRAME);
        if (!eof) r->underruns++;   /* shortfall mid-stream = real underrun */
    }
    return avail;
}

static int ring_drained(ring_t *r){
    pthread_mutex_lock(&r->lock);
    int done = r->eof && (r->count == 0);
    pthread_mutex_unlock(&r->lock);
    return done;
}

/* ===================== WAV parser (minimal RIFF/PCM) ===================== */
typedef struct {
    FILE *fp;
    uint32_t rate;
    uint16_t channels;
    uint16_t bits;
    long data_start;
    uint32_t data_bytes;
    uint32_t data_read;
} wav_t;

static uint32_t rd_u32(const unsigned char *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t rd_u16(const unsigned char *p){ return p[0]|(p[1]<<8); }

static int wav_open(wav_t *w, const char *path){
    memset(w, 0, sizeof(*w));
    w->fp = fopen(path, "rb");
    if (!w->fp){ perror("open wav"); return -1; }
    unsigned char hdr[12];
    if (fread(hdr,1,12,w->fp)!=12 || memcmp(hdr,"RIFF",4) || memcmp(hdr+8,"WAVE",4)){
        fprintf(stderr,"not a RIFF/WAVE file\n"); return -1;
    }
    /* walk chunks for fmt and data */
    for (;;){
        unsigned char ch[8];
        if (fread(ch,1,8,w->fp)!=8){ fprintf(stderr,"no data chunk\n"); return -1; }
        uint32_t id_is_fmt = !memcmp(ch,"fmt ",4);
        uint32_t id_is_dat = !memcmp(ch,"data",4);
        uint32_t sz = rd_u32(ch+4);
        if (id_is_fmt){
            unsigned char f[16];
            if (sz < 16 || fread(f,1,16,w->fp)!=16){ fprintf(stderr,"bad fmt\n"); return -1; }
            uint16_t fmt = rd_u16(f);
            w->channels = rd_u16(f+2);
            w->rate     = rd_u32(f+4);
            w->bits     = rd_u16(f+14);
            if (fmt != 1){ fprintf(stderr,"only PCM (fmt=1) supported, got %u\n", fmt); return -1; }
            if (sz > 16) fseek(w->fp, sz-16, SEEK_CUR); /* skip extra fmt */
        } else if (id_is_dat){
            w->data_start = ftell(w->fp);
            w->data_bytes = sz;
            break;
        } else {
            fseek(w->fp, sz, SEEK_CUR); /* skip unknown chunk */
        }
    }
    if (w->channels != 2){ fprintf(stderr,"need stereo, got %u ch\n", w->channels); return -1; }
    if (w->bits != 16 && w->bits != 24){ fprintf(stderr,"need 16 or 24-bit, got %u\n", w->bits); return -1; }
    return 0;
}

/* Read up to max_frames from the WAV, convert each to device 6-byte frame.
   Returns frames produced; 0 at EOF. */
static size_t wav_read_frames(wav_t *w, unsigned char *out, size_t max_frames){
    int in_bps = w->bits/8;           /* 2 or 3 bytes per sample in file */
    int in_frame = in_bps * 2;        /* stereo */
    size_t produced = 0;
    unsigned char in[ 3*2 ];          /* one input frame, max 6 bytes */

    while (produced < max_frames){
        if (w->data_read + in_frame > w->data_bytes) break;
        if (fread(in,1,in_frame,w->fp) != (size_t)in_frame) break;
        w->data_read += in_frame;

        unsigned char *o = out + produced*BYTES_PER_FRAME;
        for (int c = 0; c < 2; c++){
            int32_t s;
            if (in_bps == 2){
                /* 16-bit LE -> place in high-3-of-32, i.e. left-shift 16,
                   then emit bytes at >>8,>>16,>>24 like the device wants */
                int16_t v = (int16_t)rd_u16(in + c*2);
                s = ((int32_t)v) << 16;
            } else {
                /* 24-bit LE in file -> sign-extend to 32, then high-3 layout */
                const unsigned char *p = in + c*3;
                int32_t v = p[0] | (p[1]<<8) | (p[2]<<16);
                if (v & 0x800000) v |= ~0xFFFFFF; /* sign extend */
                s = v << 8;
            }
            unsigned char *d = o + c*3;
            d[0] = (s >> 8)  & 0xff;
            d[1] = (s >> 16) & 0xff;
            d[2] = (s >> 24) & 0xff;
        }
        produced++;
    }
    return produced;
}

/* ===================== USB engine (proven) ===================== */
static ring_t g_ring;
static volatile int g_stop = 0;
static int g_active = 0;
static unsigned int g_rate = 48000;
static unsigned int g_accum = 0;

static void on_sigint(int s){ (void)s; g_stop = 1; }

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

/* Fill one playback URB: pull frames from ring, TIGHT-packed. */
static void fill_playback(struct libusb_transfer *xfer){
    unsigned char *buf = xfer->buffer;
    int running = 0; /* tight offset */
    for (int i = 0; i < ISO_PACKETS_PER_URB; i++){
        g_accum += g_rate;
        int frames = g_accum / MICROFRAMES_PER_SEC;
        g_accum %= MICROFRAMES_PER_SEC;
        int bytes = frames * BYTES_PER_FRAME;
        if (bytes > MAX_PACKET_SIZE){ frames = MAX_PACKET_SIZE/BYTES_PER_FRAME; bytes = frames*BYTES_PER_FRAME; }

        /* pull `frames` device-ready frames from the ring (zero-fills on underrun).
           Data is written TIGHTLY at `running`; libusb derives each packet's
           offset by summing prior lengths, so we set only the length. */
        ring_read(&g_ring, buf + running, frames);

        xfer->iso_packet_desc[i].length = bytes;
        running += bytes;
    }
}

static void LIBUSB_CALL playback_cb(struct libusb_transfer *xfer){
    g_active--;
    if (g_stop || xfer->status == LIBUSB_TRANSFER_CANCELLED) return;
    fill_playback(xfer);
    if (libusb_submit_transfer(xfer) < 0){ g_stop = 1; return; }
    g_active++;
}
static void LIBUSB_CALL capture_cb(struct libusb_transfer *xfer){
    g_active--;
    if (g_stop || xfer->status == LIBUSB_TRANSFER_CANCELLED) return;
    for (int i = 0; i < ISO_PACKETS_PER_URB; i++)
        xfer->iso_packet_desc[i].length = MAX_PACKET_SIZE;
    if (libusb_submit_transfer(xfer) < 0){ g_stop = 1; return; }
    g_active++;
}

/* producer thread: read WAV -> ring */
static wav_t g_wav;
static void *producer(void *arg){
    (void)arg;
    const size_t CHUNK = 1024;
    unsigned char *tmp = malloc(CHUNK * BYTES_PER_FRAME);
    for (;;){
        if (g_stop) break;
        size_t got = wav_read_frames(&g_wav, tmp, CHUNK);
        if (got == 0){ /* EOF */
            pthread_mutex_lock(&g_ring.lock); g_ring.eof = 1; pthread_mutex_unlock(&g_ring.lock);
            break;
        }
        size_t off = 0;
        while (off < got && !g_stop){
            size_t w = ring_write(&g_ring, tmp + off*BYTES_PER_FRAME, got - off);
            off += w;
            if (w == 0) usleep(2000); /* ring full, wait for consumer */
        }
    }
    free(tmp);
    return NULL;
}

int main(int argc, char **argv){
    signal(SIGINT, on_sigint);
    if (argc < 2){ fprintf(stderr,"usage: %s file.wav\n", argv[0]); return 1; }

    if (wav_open(&g_wav, argv[1]) < 0) return 1;
    g_rate = g_wav.rate;
    if (g_rate!=44100 && g_rate!=48000 && g_rate!=88200 && g_rate!=96000){
        fprintf(stderr,"WAV rate %u unsupported (need 44100/48000/88200/96000). "
                       "No resampling in this tool.\n", g_rate);
        return 1;
    }
    printf("WAV: %u Hz, %u-bit, %u ch. Streaming...\n", g_wav.rate, g_wav.bits, g_wav.channels);

    /* ring: ~500 ms of audio */
    ring_init(&g_ring, g_rate / 2);

    libusb_context *ctx=NULL;
    if (libusb_init(&ctx)!=0){ fprintf(stderr,"init failed\n"); return 2; }
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx,VID,PID);
    if (!h){ fprintf(stderr,"open failed\n"); libusb_exit(ctx); return 3; }
    libusb_set_auto_detach_kernel_driver(h,1);
    if (libusb_claim_interface(h,IFACE)<0){ fprintf(stderr,"claim failed\n"); libusb_close(h); libusb_exit(ctx); return 4; }
    if (libusb_set_interface_alt_setting(h,IFACE,ALT)<0){ fprintf(stderr,"alt failed\n"); goto out; }
    if (set_rate(h,g_rate)<0){ fprintf(stderr,"set_rate failed\n"); goto out; }

    /* prefill the ring before streaming so we don't underrun at start */
    {
        const size_t CHUNK = 1024;
        unsigned char *tmp = malloc(CHUNK*BYTES_PER_FRAME);
        while (g_ring.count < g_ring.cap_frames*3/4){
            size_t got = wav_read_frames(&g_wav, tmp, CHUNK);
            if (got==0){ pthread_mutex_lock(&g_ring.lock); g_ring.eof=1; pthread_mutex_unlock(&g_ring.lock); break; }
            size_t off=0; while(off<got){ off += ring_write(&g_ring, tmp+off*BYTES_PER_FRAME, got-off); }
        }
        free(tmp);
    }

    pthread_t prod;
    pthread_create(&prod, NULL, producer, NULL);

    int buflen = ISO_PACKETS_PER_URB * MAX_PACKET_SIZE;
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
        if (libusb_submit_transfer(pb[i])  < 0){ g_stop=1; break; } g_active++;
        if (libusb_submit_transfer(cap[i]) < 0){ g_stop=1; break; } g_active++;
    }

    /* run until file drained or Ctrl-C */
    while (!g_stop && !ring_drained(&g_ring)){
        struct timeval tv={0,100000}; libusb_handle_events_timeout(ctx,&tv);
    }
    /* let the tail flush */
    for (int k=0;k<5 && !g_stop;k++){ struct timeval tv={0,100000}; libusb_handle_events_timeout(ctx,&tv); }

    g_stop = 1;
    pthread_join(prod, NULL);
    for (int i=0;i<NUM_URBS;i++){ if(pb[i]) libusb_cancel_transfer(pb[i]); if(cap[i]) libusb_cancel_transfer(cap[i]); }
    while (g_active > 0){ struct timeval tv={0,100000}; libusb_handle_events_timeout(ctx,&tv); }
    for (int i=0;i<NUM_URBS;i++){
        if(pb[i]){ free(pb[i]->buffer); libusb_free_transfer(pb[i]); }
        if(cap[i]){ free(cap[i]->buffer); libusb_free_transfer(cap[i]); }
    }

    printf("done. underruns: %ld\n", g_ring.underruns);
out:
    if (h){ libusb_release_interface(h,IFACE); libusb_close(h); }
    libusb_exit(ctx);
    ring_free(&g_ring);
    if (g_wav.fp) fclose(g_wav.fp);
    return 0;
}
