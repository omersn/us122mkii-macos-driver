/*
 * US122.c  -  AudioServerPlugIn skeleton for the TASCAM US-122MKII
 *
 * STEP 3 of the HAL driver: make the device APPEAR in macOS sound settings.
 * This file implements the AudioServerPlugIn COM interface with the minimum
 * property model for one full-duplex device. IO is DUMMY (silence): StartIO
 * and StopIO succeed, the IO operations zero-fill output and ignore input.
 * No shared memory, no USB here yet. That is step 4.
 *
 * Object model (fixed IDs):
 *   kObjectID_PlugIn  = the plugin itself
 *   kObjectID_Device  = the US-122MKII device
 *   kObjectID_Stream_Input / _Output = the two streams
 *   plus volume/mute controls can be added later
 *
 * Build: see build_us122_driver.sh (compiled into US122.driver bundle).
 *
 * NOTE: the AudioServerPlugIn interface is large. This skeleton handles the
 * properties coreaudiod queries to enumerate and present a device. If the
 * system asks for a property we do not handle, it logs the selector; we add
 * it. That iteration is expected for a hand-written driver.
 */

#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>
#include <syslog.h>
#include "shmring.h"

/* ============================ shm attach ============================ */
/* The daemon (us122d), launched by launchd, creates the segment and owns the
   USB device. The plugin only ATTACHES and toggles the running flag. If the
   daemon is not present, g_shm stays NULL and IO falls back to silence. */
static shm_layout_t *g_shm = NULL;

static void shm_attach_if_needed(void){
    if (g_shm) return;
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd < 0){
        syslog(LOG_ERR, "US122: shm_open(%s) failed errno=%d", SHM_NAME, errno);
        return;                         /* daemon not up yet; try again later */
    }
    void *p = mmap(NULL, sizeof(shm_layout_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED){
        syslog(LOG_ERR, "US122: mmap failed errno=%d", errno);
        return;
    }
    if (!shm_valid((shm_layout_t*)p)){
        syslog(LOG_ERR, "US122: shm magic/version invalid");
        munmap(p, sizeof(shm_layout_t));
        return;
    }
    g_shm = (shm_layout_t*)p;
    syslog(LOG_NOTICE, "US122: shm attached OK");
}

/* True only when the daemon currently holds a live, claimed USB device.
   Drives whether we advertise the device to coreaudiod at all, so it appears
   and disappears from the device list with the hardware. If shm is not attached
   yet, attempt it; if still absent, treat as not-present. */
static int device_is_present(void){
    if (!g_shm) shm_attach_if_needed();
    if (!g_shm) return 0;
    if (atomic_load_explicit(&g_shm->user_hide, memory_order_acquire)) return 0;
    return atomic_load_explicit(&g_shm->device_present, memory_order_acquire) ? 1 : 0;
}

/* float [-1,1] stereo frame -> device-native 6 bytes (24-bit high-3-of-32) */
static inline void float_to_dev(const float *src, uint8_t *dst){
    for (int c = 0; c < 2; c++){
        float f = src[c];
        if (f >  1.0f) f =  1.0f;
        if (f < -1.0f) f = -1.0f;
        int32_t s = (int32_t)(f * 2147483647.0f); /* full 32-bit */
        uint8_t *d = dst + c*3;
        d[0] = (s >> 8)  & 0xff;   /* high-3-of-32, matches device + daemon */
        d[1] = (s >> 16) & 0xff;
        d[2] = (s >> 24) & 0xff;
    }
}

/* device-native 6 bytes -> float [-1,1] stereo frame */
static inline void dev_to_float(const uint8_t *src, float *dst){
    for (int c = 0; c < 2; c++){
        const uint8_t *s = src + c*3;
        /* reconstruct the 32-bit value: bytes are bits 8..31, low byte is 0 */
        int32_t v = ((int32_t)s[0] << 8) | ((int32_t)s[1] << 16) | ((int32_t)s[2] << 24);
        dst[c] = (float)v / 2147483647.0f;
    }
}

/* ============================ object IDs ============================ */
enum {
    kObjectID_PlugIn        = kAudioObjectPlugInObject, /* always 1 */
    kObjectID_Device        = 2,
    kObjectID_Stream_Input  = 3,
    kObjectID_Stream_Output = 4,
};

#define kDevice_UID          "US122MKII_Device"
#define kDevice_ModelUID     "US122MKII_Model"
#define kDevice_Name         "US-122MKII"
#define kManufacturer_Name   "TASCAM"
#define kBox_UID             "US122MKII_Box"

/* supported rates */
static const Float64 kRates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };
#define kNumRates (sizeof(kRates)/sizeof(kRates[0]))
#define kChannels 2

/* ============================ driver state ============================ */
typedef struct {
    AudioServerPlugInDriverInterface *interface;     /* vtable ptr first */
    AudioServerPlugInDriverRef        ref;           /* self */
    AudioServerPlugInHostRef          host;
    pthread_mutex_t                   lock;
    UInt64                            refCount;
    Float64                           sampleRate;     /* current */
    UInt64                            ioRunning;      /* StartIO count */
} US122_Driver;

static US122_Driver gDriver;

/* forward decls of the interface functions */
static HRESULT  US122_QueryInterface(void *self, REFIID iid, LPVOID *out);
static ULONG    US122_AddRef(void *self);
static ULONG    US122_Release(void *self);
static OSStatus US122_Initialize(AudioServerPlugInDriverRef self, AudioServerPlugInHostRef host);
static OSStatus US122_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*);
static OSStatus US122_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus US122_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus US122_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus US122_PerformDeviceConfigChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static OSStatus US122_AbortDeviceConfigChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static Boolean  US122_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
static OSStatus US122_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean*);
static OSStatus US122_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
static OSStatus US122_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
static OSStatus US122_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);
static OSStatus US122_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus US122_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus US122_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64*, UInt64*, UInt64*);
static OSStatus US122_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean*, Boolean*);
static OSStatus US122_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);
static OSStatus US122_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*, void*, void*);
static OSStatus US122_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);

/* the interface vtable */
static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    US122_QueryInterface,
    US122_AddRef,
    US122_Release,
    US122_Initialize,
    US122_CreateDevice,
    US122_DestroyDevice,
    US122_AddDeviceClient,
    US122_RemoveDeviceClient,
    US122_PerformDeviceConfigChange,
    US122_AbortDeviceConfigChange,
    US122_HasProperty,
    US122_IsPropertySettable,
    US122_GetPropertyDataSize,
    US122_GetPropertyData,
    US122_SetPropertyData,
    US122_StartIO,
    US122_StopIO,
    US122_GetZeroTimeStamp,
    US122_WillDoIOOperation,
    US122_BeginIOOperation,
    US122_DoIOOperation,
    US122_EndIOOperation
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;

/* zero-timestamp bookkeeping for the dummy clock */
static UInt64 gAnchorHostTime = 0;
static Float64 gAnchorSampleTime = 0;

/* ============================ factory ============================ */
/* Referenced by Info.plist CFPlugInFactories. Returns our single interface. */
void *US122_Create(CFAllocatorRef allocator, CFUUIDRef typeUUID);
void *US122_Create(CFAllocatorRef allocator, CFUUIDRef typeUUID){
    (void)allocator;
    if (!CFEqual(typeUUID, CFUUIDGetConstantUUIDWithBytes(NULL,
            0x44,0x3A,0xBA,0xB8,0xE7,0xB3,0x49,0x1A,0xB9,0x85,0xBE,0xB9,0x18,0x70,0x30,0xDB))){
        return NULL; /* not the AudioServerPlugIn type */
    }
    gDriver.interface = gInterfacePtr;
    gDriver.ref       = (AudioServerPlugInDriverRef)&gDriver.interface;
    pthread_mutex_init(&gDriver.lock, NULL);
    gDriver.refCount  = 1;
    gDriver.sampleRate = 48000.0;
    gDriver.ioRunning  = 0;
    return &gDriver.interface;
}

/* ============================ IUnknown ============================ */
static HRESULT US122_QueryInterface(void *self, REFIID iid, LPVOID *out){
    (void)self;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, iid);
    if (CFEqual(req, IUnknownUUID) ||
        CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)){
        CFRelease(req);
        pthread_mutex_lock(&gDriver.lock);
        gDriver.refCount++;
        pthread_mutex_unlock(&gDriver.lock);
        *out = &gDriver.interface;
        return S_OK;
    }
    CFRelease(req);
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG US122_AddRef(void *self){ (void)self;
    pthread_mutex_lock(&gDriver.lock); ULONG c=++gDriver.refCount; pthread_mutex_unlock(&gDriver.lock); return c; }
static ULONG US122_Release(void *self){ (void)self;
    pthread_mutex_lock(&gDriver.lock); ULONG c=--gDriver.refCount; pthread_mutex_unlock(&gDriver.lock); return c; }

/* ============================ presence watcher ============================ */
/* Polls the daemon's device_present flag and, on each change, tells coreaudiod
   that the plugin's device list changed so the device is added to / removed
   from the system list with the hardware. Also notifies the device's IsAlive
   so an app currently holding the device sees it go away. */
static pthread_t gWatcher;
static volatile int gWatcherRun = 0;

static void *presence_watcher(void *arg){
    (void)arg;
    int last = device_is_present();
    while (gWatcherRun){
        usleep(250000);   /* 4x/sec: responsive without busy-spinning */
        int now = device_is_present();
        if (now == last) continue;
        last = now;
        if (!gDriver.host || !gDriver.host->PropertiesChanged) continue;

        /* the device list on the plugin object changed (device added/removed) */
        AudioObjectPropertyAddress devList = {
            kAudioPlugInPropertyDeviceList,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        gDriver.host->PropertiesChanged(gDriver.host, kObjectID_PlugIn, 1, &devList);

        /* and the device's alive state changed (for apps holding it) */
        AudioObjectPropertyAddress alive = {
            kAudioDevicePropertyDeviceIsAlive,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        gDriver.host->PropertiesChanged(gDriver.host, kObjectID_Device, 1, &alive);

        syslog(LOG_NOTICE, "US122: device_present -> %d, notified coreaudiod", now);
    }
    return NULL;
}

/* ============================ lifecycle ============================ */
static OSStatus US122_Initialize(AudioServerPlugInDriverRef self, AudioServerPlugInHostRef host){
    (void)self;
    gDriver.host = host;
    /* start the presence watcher so the device appears/disappears with the
       hardware. shm may not be attached yet; the watcher attaches lazily. */
    if (!gWatcherRun){
        gWatcherRun = 1;
        if (pthread_create(&gWatcher, NULL, presence_watcher, NULL) != 0){
            gWatcherRun = 0;
            syslog(LOG_ERR, "US122: failed to start presence watcher");
        }
    }
    return 0;
}
/* We publish a static device, so dynamic device create/destroy are no-ops. */
static OSStatus US122_CreateDevice(AudioServerPlugInDriverRef s, CFDictionaryRef d, const AudioServerPlugInClientInfo*c, AudioObjectID*o){ (void)s;(void)d;(void)c;(void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus US122_DestroyDevice(AudioServerPlugInDriverRef s, AudioObjectID o){ (void)s;(void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus US122_AddDeviceClient(AudioServerPlugInDriverRef s, AudioObjectID o, const AudioServerPlugInClientInfo*c){ (void)s;(void)o;(void)c; return 0; }
static OSStatus US122_RemoveDeviceClient(AudioServerPlugInDriverRef s, AudioObjectID o, const AudioServerPlugInClientInfo*c){ (void)s;(void)o;(void)c; return 0; }
static OSStatus US122_PerformDeviceConfigChange(AudioServerPlugInDriverRef s, AudioObjectID o, UInt64 a, void*i){ (void)s;(void)o;(void)a;(void)i; return 0; }
static OSStatus US122_AbortDeviceConfigChange(AudioServerPlugInDriverRef s, AudioObjectID o, UInt64 a, void*i){ (void)s;(void)o;(void)a;(void)i; return 0; }

/* ============================ property helpers ============================ */
/* Build an ASBD for a stream: 24-bit audio carried as 32-bit float to the
   host (Core Audio works in float; our shm/device conversion is in step 4). */
static void fill_asbd(AudioStreamBasicDescription *a, Float64 rate){
    memset(a, 0, sizeof(*a));
    a->mSampleRate       = rate;
    a->mFormatID         = kAudioFormatLinearPCM;
    a->mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    a->mBytesPerPacket   = 4 * kChannels;
    a->mFramesPerPacket  = 1;
    a->mBytesPerFrame    = 4 * kChannels;
    a->mChannelsPerFrame = kChannels;
    a->mBitsPerChannel   = 32;
}

/* ============================ HasProperty ============================ */
static Boolean US122_HasProperty(AudioServerPlugInDriverRef self, AudioObjectID obj, pid_t client, const AudioObjectPropertyAddress* addr){
    (void)self;(void)client;
    UInt32 dataSize=0;
    return US122_GetPropertyDataSize(self, obj, client, addr, 0, NULL, &dataSize) == 0;
}

static OSStatus US122_IsPropertySettable(AudioServerPlugInDriverRef self, AudioObjectID obj, pid_t client, const AudioObjectPropertyAddress* addr, Boolean* outSettable){
    (void)self;(void)obj;(void)client;
    switch (addr->mSelector){
        case kAudioDevicePropertyNominalSampleRate:
            *outSettable = true; return 0;
        default:
            *outSettable = false; return 0;
    }
}

/* ============================ GetPropertyDataSize ============================ */
static OSStatus US122_GetPropertyDataSize(AudioServerPlugInDriverRef self, AudioObjectID obj, pid_t client, const AudioObjectPropertyAddress* addr, UInt32 qDataSize, const void* qData, UInt32* outSize){
    (void)self;(void)client;(void)qDataSize;(void)qData;
    switch (obj){
    case kObjectID_PlugIn:
        switch (addr->mSelector){
            case kAudioObjectPropertyBaseClass:     *outSize=sizeof(AudioClassID); return 0;
            case kAudioObjectPropertyClass:         *outSize=sizeof(AudioClassID); return 0;
            case kAudioObjectPropertyOwner:         *outSize=sizeof(AudioObjectID); return 0;
            case kAudioObjectPropertyManufacturer:  *outSize=sizeof(CFStringRef); return 0;
            case kAudioObjectPropertyOwnedObjects:  *outSize=sizeof(AudioObjectID); return 0;
            case kAudioPlugInPropertyDeviceList:    *outSize = device_is_present() ? sizeof(AudioObjectID) : 0; return 0;
            case kAudioPlugInPropertyTranslateUIDToDevice: *outSize=sizeof(AudioObjectID); return 0;
            case kAudioPlugInPropertyResourceBundle:*outSize=sizeof(CFStringRef); return 0;
        }
        break;
    case kObjectID_Device:
        switch (addr->mSelector){
            case kAudioObjectPropertyBaseClass:     *outSize=sizeof(AudioClassID); return 0;
            case kAudioObjectPropertyClass:         *outSize=sizeof(AudioClassID); return 0;
            case kAudioObjectPropertyOwner:         *outSize=sizeof(AudioObjectID); return 0;
            case kAudioObjectPropertyName:          *outSize=sizeof(CFStringRef); return 0;
            case kAudioObjectPropertyManufacturer:  *outSize=sizeof(CFStringRef); return 0;
            case kAudioObjectPropertyOwnedObjects:  *outSize=2*sizeof(AudioObjectID); return 0;
            case kAudioDevicePropertyDeviceUID:     *outSize=sizeof(CFStringRef); return 0;
            case kAudioDevicePropertyModelUID:      *outSize=sizeof(CFStringRef); return 0;
            case kAudioDevicePropertyTransportType: *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyRelatedDevices:*outSize=sizeof(AudioObjectID); return 0;
            case kAudioDevicePropertyClockDomain:   *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyDeviceIsAlive: *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyDeviceIsRunning:*outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyDeviceCanBeDefaultDevice: *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyLatency:       *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyStreams:       *outSize=sizeof(AudioObjectID); return 0; /* per-scope, adjusted below */
            case kAudioObjectPropertyControlList:   *outSize=0; return 0;
            case kAudioDevicePropertySafetyOffset:  *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyNominalSampleRate: *outSize=sizeof(Float64); return 0;
            case kAudioDevicePropertyAvailableNominalSampleRates: *outSize=kNumRates*sizeof(AudioValueRange); return 0;
            case kAudioDevicePropertyIsHidden:      *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyZeroTimeStampPeriod: *outSize=sizeof(UInt32); return 0;
        }
        /* streams list is scope-dependent */
        if (addr->mSelector == kAudioDevicePropertyStreams){
            if (addr->mScope==kAudioObjectPropertyScopeInput || addr->mScope==kAudioObjectPropertyScopeOutput) *outSize=sizeof(AudioObjectID);
            else *outSize=2*sizeof(AudioObjectID);
            return 0;
        }
        break;
    case kObjectID_Stream_Input:
    case kObjectID_Stream_Output:
        switch (addr->mSelector){
            case kAudioObjectPropertyBaseClass:     *outSize=sizeof(AudioClassID); return 0;
            case kAudioObjectPropertyClass:         *outSize=sizeof(AudioClassID); return 0;
            case kAudioObjectPropertyOwner:         *outSize=sizeof(AudioObjectID); return 0;
            case kAudioStreamPropertyIsActive:      *outSize=sizeof(UInt32); return 0;
            case kAudioStreamPropertyDirection:     *outSize=sizeof(UInt32); return 0;
            case kAudioStreamPropertyTerminalType:  *outSize=sizeof(UInt32); return 0;
            case kAudioStreamPropertyStartingChannel:*outSize=sizeof(UInt32); return 0;
            case kAudioStreamPropertyLatency:       *outSize=sizeof(UInt32); return 0;
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:*outSize=sizeof(AudioStreamBasicDescription); return 0;
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: *outSize=kNumRates*sizeof(AudioStreamRangedDescription); return 0;
        }
        break;
    }
    return kAudioHardwareUnknownPropertyError;
}

/* ============================ GetPropertyData ============================ */
static OSStatus US122_GetPropertyData(AudioServerPlugInDriverRef self, AudioObjectID obj, pid_t client, const AudioObjectPropertyAddress* addr, UInt32 qDataSize, const void* qData, UInt32 inDataSize, UInt32* outSize, void* outData){
    (void)self;(void)client;(void)qDataSize;(void)qData;(void)inDataSize;

    switch (obj){
    /* ---------------- PlugIn ---------------- */
    case kObjectID_PlugIn:
        switch (addr->mSelector){
            case kAudioObjectPropertyBaseClass: *(AudioClassID*)outData=kAudioObjectClassID; *outSize=sizeof(AudioClassID); return 0;
            case kAudioObjectPropertyClass:     *(AudioClassID*)outData=kAudioPlugInClassID; *outSize=sizeof(AudioClassID); return 0;
            case kAudioObjectPropertyOwner:     *(AudioObjectID*)outData=kAudioObjectUnknown; *outSize=sizeof(AudioObjectID); return 0;
            case kAudioObjectPropertyManufacturer: *(CFStringRef*)outData=CFSTR(kManufacturer_Name); *outSize=sizeof(CFStringRef); return 0;
            case kAudioObjectPropertyOwnedObjects:
                *(AudioObjectID*)outData=kObjectID_Device; *outSize=sizeof(AudioObjectID); return 0;
            case kAudioPlugInPropertyDeviceList:
                if (device_is_present()){
                    *(AudioObjectID*)outData=kObjectID_Device; *outSize=sizeof(AudioObjectID);
                } else {
                    *outSize=0;   /* no devices: it disappears from the list */
                }
                return 0;
            case kAudioPlugInPropertyTranslateUIDToDevice:
                *(AudioObjectID*)outData=kObjectID_Device; *outSize=sizeof(AudioObjectID); return 0;
            case kAudioPlugInPropertyResourceBundle:
                *(CFStringRef*)outData=CFSTR(""); *outSize=sizeof(CFStringRef); return 0;
        }
        break;
    /* ---------------- Device ---------------- */
    case kObjectID_Device:
        switch (addr->mSelector){
            case kAudioObjectPropertyBaseClass: *(AudioClassID*)outData=kAudioObjectClassID; *outSize=sizeof(AudioClassID); return 0;
            case kAudioObjectPropertyClass:     *(AudioClassID*)outData=kAudioDeviceClassID; *outSize=sizeof(AudioClassID); return 0;
            case kAudioObjectPropertyOwner:     *(AudioObjectID*)outData=kObjectID_PlugIn; *outSize=sizeof(AudioObjectID); return 0;
            case kAudioObjectPropertyName:      *(CFStringRef*)outData=CFSTR(kDevice_Name); *outSize=sizeof(CFStringRef); return 0;
            case kAudioObjectPropertyManufacturer: *(CFStringRef*)outData=CFSTR(kManufacturer_Name); *outSize=sizeof(CFStringRef); return 0;
            case kAudioDevicePropertyDeviceUID:  *(CFStringRef*)outData=CFSTR(kDevice_UID); *outSize=sizeof(CFStringRef); return 0;
            case kAudioDevicePropertyModelUID:   *(CFStringRef*)outData=CFSTR(kDevice_ModelUID); *outSize=sizeof(CFStringRef); return 0;
            case kAudioDevicePropertyTransportType: *(UInt32*)outData=kAudioDeviceTransportTypeUSB; *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyClockDomain:   *(UInt32*)outData=0; *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyDeviceIsAlive: *(UInt32*)outData=device_is_present()?1:0; *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyDeviceIsRunning: *(UInt32*)outData=(gDriver.ioRunning>0)?1:0; *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyDeviceCanBeDefaultDevice: *(UInt32*)outData=1; *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: *(UInt32*)outData=1; *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyLatency:    *(UInt32*)outData=512; *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertySafetyOffset: *(UInt32*)outData=64; *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyZeroTimeStampPeriod: *(UInt32*)outData=2048; *outSize=sizeof(UInt32); return 0;
            case kAudioDevicePropertyIsHidden:   *(UInt32*)outData=0; *outSize=sizeof(UInt32); return 0;
            case kAudioObjectPropertyControlList: *outSize=0; return 0;
            case kAudioDevicePropertyNominalSampleRate: *(Float64*)outData=gDriver.sampleRate; *outSize=sizeof(Float64); return 0;
            case kAudioDevicePropertyAvailableNominalSampleRates: {
                AudioValueRange* r=(AudioValueRange*)outData;
                for (UInt32 i=0;i<kNumRates;i++){ r[i].mMinimum=kRates[i]; r[i].mMaximum=kRates[i]; }
                *outSize=kNumRates*sizeof(AudioValueRange); return 0;
            }
            case kAudioObjectPropertyOwnedObjects: {
                AudioObjectID* o=(AudioObjectID*)outData;
                o[0]=kObjectID_Stream_Input; o[1]=kObjectID_Stream_Output;
                *outSize=2*sizeof(AudioObjectID); return 0;
            }
            case kAudioDevicePropertyStreams: {
                AudioObjectID* o=(AudioObjectID*)outData;
                if (addr->mScope==kAudioObjectPropertyScopeInput){ o[0]=kObjectID_Stream_Input; *outSize=sizeof(AudioObjectID); }
                else if (addr->mScope==kAudioObjectPropertyScopeOutput){ o[0]=kObjectID_Stream_Output; *outSize=sizeof(AudioObjectID); }
                else { o[0]=kObjectID_Stream_Input; o[1]=kObjectID_Stream_Output; *outSize=2*sizeof(AudioObjectID); }
                return 0;
            }
            case kAudioDevicePropertyRelatedDevices:
                *(AudioObjectID*)outData=kObjectID_Device; *outSize=sizeof(AudioObjectID); return 0;
        }
        break;
    /* ---------------- Streams ---------------- */
    case kObjectID_Stream_Input:
    case kObjectID_Stream_Output:
        switch (addr->mSelector){
            case kAudioObjectPropertyBaseClass: *(AudioClassID*)outData=kAudioObjectClassID; *outSize=sizeof(AudioClassID); return 0;
            case kAudioObjectPropertyClass:     *(AudioClassID*)outData=kAudioStreamClassID; *outSize=sizeof(AudioClassID); return 0;
            case kAudioObjectPropertyOwner:     *(AudioObjectID*)outData=kObjectID_Device; *outSize=sizeof(AudioObjectID); return 0;
            case kAudioStreamPropertyIsActive:  *(UInt32*)outData=1; *outSize=sizeof(UInt32); return 0;
            case kAudioStreamPropertyDirection: *(UInt32*)outData=(obj==kObjectID_Stream_Input)?1:0; *outSize=sizeof(UInt32); return 0;
            case kAudioStreamPropertyTerminalType: *(UInt32*)outData=(obj==kObjectID_Stream_Input)?kAudioStreamTerminalTypeMicrophone:kAudioStreamTerminalTypeSpeaker; *outSize=sizeof(UInt32); return 0;
            case kAudioStreamPropertyStartingChannel: *(UInt32*)outData=1; *outSize=sizeof(UInt32); return 0;
            case kAudioStreamPropertyLatency:   *(UInt32*)outData=0; *outSize=sizeof(UInt32); return 0;
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
                fill_asbd((AudioStreamBasicDescription*)outData, gDriver.sampleRate);
                *outSize=sizeof(AudioStreamBasicDescription); return 0;
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: {
                AudioStreamRangedDescription* d=(AudioStreamRangedDescription*)outData;
                for (UInt32 i=0;i<kNumRates;i++){
                    fill_asbd(&d[i].mFormat, kRates[i]);
                    d[i].mSampleRateRange.mMinimum=kRates[i];
                    d[i].mSampleRateRange.mMaximum=kRates[i];
                }
                *outSize=kNumRates*sizeof(AudioStreamRangedDescription); return 0;
            }
        }
        break;
    }
    return kAudioHardwareUnknownPropertyError;
}

/* ============================ SetPropertyData ============================ */
static OSStatus US122_SetPropertyData(AudioServerPlugInDriverRef self, AudioObjectID obj, pid_t client, const AudioObjectPropertyAddress* addr, UInt32 qDataSize, const void* qData, UInt32 inDataSize, const void* inData){
    (void)self;(void)client;(void)qDataSize;(void)qData;(void)inDataSize;
    if (obj==kObjectID_Device && addr->mSelector==kAudioDevicePropertyNominalSampleRate){
        Float64 want=*(const Float64*)inData;
        for (UInt32 i=0;i<kNumRates;i++) if (want==kRates[i]){
            pthread_mutex_lock(&gDriver.lock);
            gDriver.sampleRate=want;
            pthread_mutex_unlock(&gDriver.lock);
            return 0;
        }
        return kAudioHardwareIllegalOperationError;
    }
    return kAudioHardwareUnknownPropertyError;
}

/* ============================ IO (DUMMY) ============================ */
static OSStatus US122_StartIO(AudioServerPlugInDriverRef self, AudioObjectID dev, UInt32 client){
    (void)self;(void)dev;(void)client;
    pthread_mutex_lock(&gDriver.lock);
    if (gDriver.ioRunning==0){
        gAnchorHostTime = mach_absolute_time();
        gAnchorSampleTime = 0;
        shm_attach_if_needed();
        if (g_shm){
            /* tell the daemon our rate and request streaming */
            atomic_store_explicit(&g_shm->rate, (uint32_t)gDriver.sampleRate, memory_order_release);
            atomic_store_explicit(&g_shm->dev_frames, 0, memory_order_release);
            atomic_store_explicit(&g_shm->running, 1, memory_order_release);
        }
    }
    gDriver.ioRunning++;
    pthread_mutex_unlock(&gDriver.lock);
    return 0;
}
static OSStatus US122_StopIO(AudioServerPlugInDriverRef self, AudioObjectID dev, UInt32 client){
    (void)self;(void)dev;(void)client;
    pthread_mutex_lock(&gDriver.lock);
    if (gDriver.ioRunning>0) gDriver.ioRunning--;
    if (gDriver.ioRunning==0 && g_shm){
        atomic_store_explicit(&g_shm->running, 0, memory_order_release);
    }
    pthread_mutex_unlock(&gDriver.lock);
    return 0;
}

/* Dummy clock: advance a sample counter at the nominal rate, anchored to
   mach time. Real device clock (from shm dev_frames) comes in step 4. */
static OSStatus US122_GetZeroTimeStamp(AudioServerPlugInDriverRef self, AudioObjectID dev, UInt32 client, Float64* outSample, UInt64* outHostTime, UInt64* outSeed){
    (void)self;(void)dev;(void)client;
    static mach_timebase_info_data_t tb = {0,0};
    if (tb.denom==0) mach_timebase_info(&tb);
    UInt32 period = 2048;

    /* Stable time-based clock. The zero timestamp advances at the nominal
       sample rate, anchored to mach_absolute_time at StartIO. This is
       independent of the capture callback (which can spike if the device
       hiccups). Each call returns the most recent period boundary that has
       elapsed in real time. This is the standard HAL approach and keeps
       coreaudiod's IO loop stable. */
    UInt64 now = mach_absolute_time();
    UInt64 elapsedNs = (now - gAnchorHostTime) * tb.numer / tb.denom;
    Float64 elapsedFrames = (Float64)elapsedNs * gDriver.sampleRate / 1e9;
    UInt64 wholePeriods = (UInt64)(elapsedFrames / period);
    Float64 sampleTime = (Float64)(wholePeriods * period);

    /* host time corresponding to that sample boundary */
    Float64 ns = (sampleTime / gDriver.sampleRate) * 1e9;
    UInt64 hostTime = gAnchorHostTime + (UInt64)(ns * tb.denom / tb.numer);

    *outSample   = sampleTime;
    *outHostTime = hostTime;
    *outSeed     = 1;
    return 0;
}

static OSStatus US122_WillDoIOOperation(AudioServerPlugInDriverRef self, AudioObjectID dev, UInt32 client, UInt32 op, Boolean* outWill, Boolean* outInPlace){
    (void)self;(void)dev;(void)client;
    Boolean will=false;
    switch (op){
        case kAudioServerPlugInIOOperationReadInput:
        case kAudioServerPlugInIOOperationWriteMix:
            will=true; break;
    }
    if (outWill) *outWill=will;
    if (outInPlace) *outInPlace=true;
    return 0;
}
static OSStatus US122_BeginIOOperation(AudioServerPlugInDriverRef self, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 nframes, const AudioServerPlugInIOCycleInfo* info){
    (void)self;(void)dev;(void)client;(void)op;(void)nframes;(void)info; return 0;
}
/* REAL IO: WriteMix pushes output frames into the playback ring; ReadInput
   pulls from the capture ring. Realtime-safe: lock-free SPSC ring ops only,
   no allocation, no blocking. If the daemon is absent (g_shm NULL), output is
   dropped and input is silence. */
static OSStatus US122_DoIOOperation(AudioServerPlugInDriverRef self, AudioObjectID dev, AudioObjectID stream, UInt32 client, UInt32 op, UInt32 nframes, const AudioServerPlugInIOCycleInfo* info, void* mainBuf, void* secBuf){
    (void)self;(void)dev;(void)stream;(void)client;(void)info;(void)secBuf;

    /* Reconnect resync: if we are still doing IO (the app never stopped) but the
       daemon has cleared the running flag, the device dropped and came back and
       the daemon is now sitting idle waiting for a fresh start request. Re-assert
       rate + running so streaming resumes automatically with no user action.
       Cheap and realtime-safe: an atomic load, and a store only on the 1->0 edge. */
    if (g_shm && gDriver.ioRunning > 0){
        if (atomic_load_explicit(&g_shm->running, memory_order_acquire) == 0){
            atomic_store_explicit(&g_shm->rate, (uint32_t)gDriver.sampleRate, memory_order_release);
            atomic_store_explicit(&g_shm->dev_frames, 0, memory_order_release);
            atomic_store_explicit(&g_shm->running, 1, memory_order_release);
        }
    }

    if (op == kAudioServerPlugInIOOperationReadInput && mainBuf){
        if (g_shm){
            float *out = (float*)mainBuf;
            uint8_t tmp[256*FRAME_BYTES];
            UInt32 done = 0;
            while (done < nframes){
                UInt32 chunk = nframes - done; if (chunk > 256) chunk = 256;
                UInt32 got = ring_consume(&g_shm->capture, tmp, chunk);
                UInt32 i = 0;
                for (; i < got; i++)
                    dev_to_float(tmp + i*FRAME_BYTES, out + (done+i)*2);
                for (; i < chunk; i++){      /* zero-fill shortfall */
                    out[(done+i)*2+0] = 0.0f;
                    out[(done+i)*2+1] = 0.0f;
                }
                done += chunk;
            }
        } else {
            memset(mainBuf, 0, nframes * 4 * kChannels);
        }
        return 0;
    }

    if (op == kAudioServerPlugInIOOperationWriteMix && mainBuf){
        if (g_shm){
            const float *in = (const float*)mainBuf;
            uint8_t tmp[256*FRAME_BYTES];
            UInt32 done = 0;
            while (done < nframes){
                UInt32 chunk = nframes - done; if (chunk > 256) chunk = 256;
                for (UInt32 i = 0; i < chunk; i++)
                    float_to_dev(in + (done+i)*2, tmp + i*FRAME_BYTES);
                /* push; if ring full, drop remainder (overrun, audible glitch) */
                UInt32 off = 0;
                while (off < chunk){
                    UInt32 w = ring_produce(&g_shm->playback, tmp + off*FRAME_BYTES, chunk - off);
                    if (w == 0) break; /* full: drop, don't block in RT context */
                    off += w;
                }
                done += chunk;
            }
        }
        /* if no daemon: silently drop output */
        return 0;
    }

    return 0;
}
static OSStatus US122_EndIOOperation(AudioServerPlugInDriverRef self, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 nframes, const AudioServerPlugInIOCycleInfo* info){
    (void)self;(void)dev;(void)client;(void)op;(void)nframes;(void)info; return 0;
}
