/*
 * V2-US122.c  -  AudioServerPlugIn for the TASCAM US-122MKII (V2 architecture)
 *
 * V2 vs V1: the USB engine now runs INSIDE this plugin process (see
 * V2-usbengine.[ch]), not in a separate daemon over shared memory. One
 * process, one clock (the device), no resampler. This plugin owns a
 * us122_engine_t and pumps its in-process rings directly from the IO cycle.
 *
 * This file is the Core Audio object model: it presents one full-duplex device
 * and bridges Core Audio's float IO to the engine's device-native frames.
 *
 * Object model (fixed IDs):
 *   kObjectID_PlugIn  = the plugin itself
 *   kObjectID_Device  = the US-122MKII device
 *   kObjectID_Stream_Input / _Output = the two streams
 *
 * Disconnect handling: kAudioDevicePropertyDeviceIsAlive reflects the engine's
 * live device state, and a state callback posts a property-changed
 * notification so the device disappears from the list when unplugged.
 *
 * Build: see V2-build.sh (compiled into V2-US122.driver bundle).
 */

#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <syslog.h>
#include "V2-usbengine.h"

/* ============================ in-process USB engine ============================ */
/* The engine owns the device via IOKit and exposes two lock-free rings. The
   plugin starts/stops it from StartIO/StopIO and pumps it from DoIOOperation. */
static us122_engine_t *g_engine = NULL;

/* forward decl: notify Core Audio that a device property changed */
static void notify_device_property(AudioObjectPropertySelector sel,
                                   AudioObjectPropertyScope scope);

/* engine -> plugin: device presence changed. Fire Core Audio notifications so
   the device appears/disappears in the system list immediately. */
static void on_device_state(us122_dev_state_t st, void *ctx){
    (void)ctx;
    syslog(LOG_NOTICE, "V2 US122: device %s",
           st == US122_DEV_PRESENT ? "present" : "absent");
    notify_device_property(kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal);
    /* also nudge the device list so the UI refreshes */
    notify_device_property(kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal);
}

/* float [-1,1] stereo frame -> device-native 6 bytes. Format selectable at
   runtime via g_format_mode (driven by /tmp/us122_fmt). All modes here keep
   6 bytes/frame (3 per channel); they differ only in how the bits are arranged,
   so they switch live with no stream restart. Each tests one device-interpretation
   hypothesis. Frame-size-changing layouts (true 16/32-bit slots) are a separate
   build. */
#define FRAME_BYTES US122_FRAME_BYTES
_Atomic int g_format_mode = 0;

static inline void float_to_dev(const float *src, uint8_t *dst){
    int mode = atomic_load_explicit(&g_format_mode, memory_order_relaxed);
    for (int c = 0; c < 2; c++){
        float f = src[c];
        if (f >  1.0f) f =  1.0f;
        if (f < -1.0f) f = -1.0f;
        uint8_t *d = dst + c*3;
        int32_t s32 = (int32_t)(f * 2147483647.0f);  /* full 32-bit */
        int32_t s24 = (int32_t)(f * 8388607.0f);     /* literal 24-bit */
        int32_t s16 = (int32_t)(f * 32767.0f);       /* literal 16-bit */
        switch (mode){
            default:
            case 0: /* A: 24 LE, high-3-of-32 (V1 baseline) */
                d[0]=(s32>>8)&0xff;  d[1]=(s32>>16)&0xff; d[2]=(s32>>24)&0xff; break;
            case 1: /* B: 24 LE, low-aligned literal 24-bit */
                d[0]= s24     &0xff; d[1]=(s24>>8) &0xff; d[2]=(s24>>16)&0xff; break;
            case 2: /* C: 24 BE, high-3-of-32 byte-swapped */
                d[0]=(s32>>24)&0xff; d[1]=(s32>>16)&0xff; d[2]=(s32>>8) &0xff; break;
            case 3: /* D: 24 BE, low-aligned literal 24-bit */
                d[0]=(s24>>16)&0xff; d[1]=(s24>>8) &0xff; d[2]= s24     &0xff; break;
            case 4: /* E: 16-bit in HIGH 2 bytes of slot, low byte 0 */
                d[0]=0;              d[1]= s16     &0xff; d[2]=(s16>>8) &0xff; break;
            case 5: /* F: 16-bit in LOW 2 bytes of slot, high byte 0 */
                d[0]= s16     &0xff; d[1]=(s16>>8) &0xff; d[2]=0;              break;
            case 6: /* G: 24 LE but MSB/LSB byte swapped within channel */
                d[0]=(s32>>24)&0xff; d[1]=(s32>>16)&0xff; d[2]=(s32>>8) &0xff;
                { uint8_t t=d[0]; d[0]=d[2]; d[2]=t; } break;
        }
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

#define kDevice_UID          "US122MKII_V2_Device"
#define kDevice_ModelUID     "US122MKII_V2_Model"
#define kDevice_Name         "US-122MKII (V2)"
#define kManufacturer_Name   "TASCAM"
#define kBox_UID             "US122MKII_V2_Box"

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

/* ============================ lifecycle ============================ */
/* Post a property-changed notification to Core Audio for the device object,
   so the system re-reads (e.g. IsAlive) and refreshes the UI. Safe to call
   from the engine thread. */
static void notify_device_property(AudioObjectPropertySelector sel,
                                   AudioObjectPropertyScope scope){
    if (!gDriver.host) return;
    AudioObjectPropertyAddress a;
    a.mSelector = sel;
    a.mScope    = scope;
    a.mElement  = kAudioObjectPropertyElementMain;
    gDriver.host->PropertiesChanged(gDriver.host, kObjectID_Device, 1, &a);
}

/* Notify Core Audio that the (single) rate changed: refresh the device nominal
   rate AND both stream formats, so input and output are re-read as one clock. */
static void notify_rate_changed(void){
    if (!gDriver.host) return;
    AudioObjectPropertyAddress a;
    a.mSelector = kAudioDevicePropertyNominalSampleRate;
    a.mScope = kAudioObjectPropertyScopeGlobal;
    a.mElement = kAudioObjectPropertyElementMain;
    gDriver.host->PropertiesChanged(gDriver.host, kObjectID_Device, 1, &a);
    AudioObjectPropertyAddress fa[2];
    fa[0].mSelector = kAudioStreamPropertyVirtualFormat;
    fa[0].mScope = kAudioObjectPropertyScopeGlobal;
    fa[0].mElement = kAudioObjectPropertyElementMain;
    fa[1].mSelector = kAudioStreamPropertyPhysicalFormat;
    fa[1].mScope = kAudioObjectPropertyScopeGlobal;
    fa[1].mElement = kAudioObjectPropertyElementMain;
    gDriver.host->PropertiesChanged(gDriver.host, kObjectID_Stream_Input, 2, fa);
    gDriver.host->PropertiesChanged(gDriver.host, kObjectID_Stream_Output, 2, fa);
}

static OSStatus US122_Initialize(AudioServerPlugInDriverRef self, AudioServerPlugInHostRef host){
    (void)self;
    gDriver.host = host;
    /* Create the in-process USB engine. It starts a watcher thread but does
       not stream until StartIO. The state callback lets the device appear/
       disappear in the system list as it is plugged/unplugged. */
    if (!g_engine){
        g_engine = us122_engine_create();
        if (g_engine)
            us122_engine_set_state_cb(g_engine, on_device_state, NULL);
        else
            syslog(LOG_ERR, "V2 US122: failed to create USB engine");
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
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            /* Stream format must be settable so Core Audio routes any per-stream
               rate change THROUGH us. This device has ONE clock: if the input
               stream is left to default independently it lands on 88200 while
               output is 48000, and the shared clock tears (the bit-crush). We
               force both streams to the single device rate. */
            *outSettable = (obj==kObjectID_Stream_Input || obj==kObjectID_Stream_Output);
            return 0;
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
            case kAudioPlugInPropertyDeviceList:    *outSize=sizeof(AudioObjectID); return 0;
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
            case kAudioPlugInPropertyDeviceList:
                *(AudioObjectID*)outData=kObjectID_Device; *outSize=sizeof(AudioObjectID); return 0;
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
            case kAudioDevicePropertyDeviceIsAlive: {
                /* Alive when the engine reports the device present. This is what
                   makes the device disappear from the system list on unplug. */
                UInt32 alive = (g_engine && us122_engine_state(g_engine) == US122_DEV_PRESENT) ? 1 : 0;
                *(UInt32*)outData = alive; *outSize=sizeof(UInt32); return 0;
            }
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
            Float64 prev = gDriver.sampleRate;
            gDriver.sampleRate=want;
            int running = gDriver.ioRunning;
            pthread_mutex_unlock(&gDriver.lock);
            /* one clock: re-point the device to the new rate if streaming */
            if (want != prev && running && g_engine)
                us122_engine_start(g_engine, (unsigned int)want);
            notify_rate_changed();
            return 0;
        }
        return kAudioHardwareIllegalOperationError;
    }
    /* Stream format set: this device has a single clock, so whichever stream
       Core Audio tries to format, we collapse its rate onto the one device rate
       and keep input and output identical. We accept the format only if its
       rate is one we support; we keep our fixed 32-bit float stereo layout. */
    if ((obj==kObjectID_Stream_Input || obj==kObjectID_Stream_Output) &&
        (addr->mSelector==kAudioStreamPropertyVirtualFormat ||
         addr->mSelector==kAudioStreamPropertyPhysicalFormat)){
        const AudioStreamBasicDescription *f = (const AudioStreamBasicDescription*)inData;
        Float64 want = f->mSampleRate;
        int ok=0; for (UInt32 i=0;i<kNumRates;i++) if (want==kRates[i]) ok=1;
        if (!ok) return kAudioDeviceUnsupportedFormatError;
        pthread_mutex_lock(&gDriver.lock);
        Float64 prev = gDriver.sampleRate;
        gDriver.sampleRate = want;          /* unify both streams onto one rate */
        int running = gDriver.ioRunning;
        pthread_mutex_unlock(&gDriver.lock);
        if (want != prev && running && g_engine)
            us122_engine_start(g_engine, (unsigned int)want);
        notify_rate_changed();
        return 0;
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
        if (g_engine)
            us122_engine_start(g_engine, (unsigned int)gDriver.sampleRate);
    }
    gDriver.ioRunning++;
    pthread_mutex_unlock(&gDriver.lock);
    return 0;
}
static OSStatus US122_StopIO(AudioServerPlugInDriverRef self, AudioObjectID dev, UInt32 client){
    (void)self;(void)dev;(void)client;
    pthread_mutex_lock(&gDriver.lock);
    if (gDriver.ioRunning>0) gDriver.ioRunning--;
    if (gDriver.ioRunning==0 && g_engine){
        us122_engine_stop(g_engine);
    }
    pthread_mutex_unlock(&gDriver.lock);
    return 0;
}

/* Time-based zero timestamp: advance a sample counter at the nominal rate,
   anchored to mach_absolute_time. In V2 the device is the single clock and the
   engine consumes the playback ring at the device's true rate, so this host-
   side timestamp only needs to be smooth and monotonic; the engine's ring
   absorbs the small nominal-vs-actual difference (deep buffer, one clock). */
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
/* REAL IO: WriteMix pushes output frames into the engine playback ring;
   ReadInput pulls from the engine capture ring. Realtime-safe: lock-free SPSC
   ring ops only, no allocation, no blocking. If the engine is absent, output
   is dropped and input is silence. */
static OSStatus US122_DoIOOperation(AudioServerPlugInDriverRef self, AudioObjectID dev, AudioObjectID stream, UInt32 client, UInt32 op, UInt32 nframes, const AudioServerPlugInIOCycleInfo* info, void* mainBuf, void* secBuf){
    (void)self;(void)dev;(void)stream;(void)client;(void)info;(void)secBuf;

    if (op == kAudioServerPlugInIOOperationReadInput && mainBuf){
        if (g_engine){
            float *out = (float*)mainBuf;
            uint8_t tmp[256*FRAME_BYTES];
            UInt32 done = 0;
            while (done < nframes){
                UInt32 chunk = nframes - done; if (chunk > 256) chunk = 256;
                UInt32 got = us122_engine_cap_read(g_engine, tmp, chunk);
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
        if (g_engine){
            const float *in = (const float*)mainBuf;
            uint8_t tmp[256*FRAME_BYTES];
            UInt32 done = 0;
            /* one-shot value dump: prints actual sample values through each stage
               so we can see WHERE precision dies (the "4-bit" symptom). Fires
               about once a second on a non-silent frame. */
            static UInt64 dbg_last = 0;
            UInt64 nowt = mach_absolute_time();
            int dump = 0;
            if (nowt - dbg_last > 1000000000ULL){
                for (UInt32 k = 0; k < nframes && k < 64; k++){
                    if (in[k*2] != 0.0f || in[k*2+1] != 0.0f){ dump = 1; break; }
                }
                if (dump) dbg_last = nowt;
            }
            if (dump){
                for (UInt32 k = 0; k < 4 && k < nframes; k++){
                    float fl = in[k*2];
                    int32_t s = (int32_t)(fl * 2147483647.0f);
                    uint8_t b0=(s>>8)&0xff,b1=(s>>16)&0xff,b2=(s>>24)&0xff;
                    syslog(LOG_NOTICE, "V2 dump f[%u]: float=%.8f int32=%d bytes=%02x %02x %02x",
                           k, fl, s, b0, b1, b2);
                }
            }
            while (done < nframes){
                UInt32 chunk = nframes - done; if (chunk > 256) chunk = 256;
                for (UInt32 i = 0; i < chunk; i++)
                    float_to_dev(in + (done+i)*2, tmp + i*FRAME_BYTES);
                UInt32 off = 0;
                while (off < chunk){
                    UInt32 w = us122_engine_play_write(g_engine, tmp + off*FRAME_BYTES, chunk - off);
                    if (w == 0) break; /* full: drop, never block in RT context */
                    off += w;
                }
                done += chunk;
            }
        }
        return 0;
    }

    return 0;
}
static OSStatus US122_EndIOOperation(AudioServerPlugInDriverRef self, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 nframes, const AudioServerPlugInIOCycleInfo* info){
    (void)self;(void)dev;(void)client;(void)op;(void)nframes;(void)info; return 0;
}
