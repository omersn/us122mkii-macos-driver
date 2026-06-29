/*
 * V2-usbengine-iokit.inc  -  included into V2-usbengine.c (macOS only)
 *
 * The verbose IOKit plumbing: find the device by VID/PID, open it, find and
 * claim interface 1 / alt 1, set the sample rate, allocate low-latency
 * isochronous buffers and frame lists, and kick off the full-duplex stream.
 *
 * Separated from the main engine file purely for readability. Every function
 * here is [VALIDATE]: it compiles by construction and follows Apple's
 * IOUSBLib iso pattern, but isochronous USB cannot be exercised in the Linux
 * build container, so the first real test is on hardware.
 *
 * The flow deliberately mirrors the PROVEN V1 libusb sequence step for step:
 *   open -> set configuration -> claim interface 1 -> set alt 1 ->
 *   set sample rate (control) -> alloc iso buffers -> submit N read + N write
 *   transfers, scheduled on future USB frame numbers -> run the runloop.
 */

/* per-direction transfer pool */
static iso_xfer_t g_pb_xfers[NUM_ISO_XFERS];
static iso_xfer_t g_cap_xfers[NUM_ISO_XFERS];

/* locate the IOUSBDeviceInterface for our VID/PID. Returns +1 found, 0 none. */
static int find_device(IOUSBDeviceInterface ***outDev){
    *outDev = NULL;
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) return 0;
    SInt32 vid = US122_VID, pid = US122_PID;
    CFNumberRef nvid = CFNumberCreate(NULL, kCFNumberSInt32Type, &vid);
    CFNumberRef npid = CFNumberCreate(NULL, kCFNumberSInt32Type, &pid);
    CFDictionarySetValue(match, CFSTR(kUSBVendorID),  nvid);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), npid);
    CFRelease(nvid); CFRelease(npid);

    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, match, &it) != kIOReturnSuccess)
        return 0;

    io_service_t svc;
    int found = 0;
    while ((svc = IOIteratorNext(it)) != 0){
        IOCFPlugInInterface **plug = NULL; SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
                kIOCFPlugInInterfaceID, &plug, &score) == kIOReturnSuccess && plug){
            IOUSBDeviceInterface **dev = NULL;
            (*plug)->QueryInterface(plug,
                CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID*)&dev);
            (*plug)->Release(plug);
            if (dev){ *outDev = dev; found = 1; IOObjectRelease(svc); break; }
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return found;
}

/* find interface 1 on the open device and return its IOUSBInterfaceInterface */
static IOUSBInterfaceInterface **find_interface(IOUSBDeviceInterface **dev){
    IOUSBFindInterfaceRequest req;
    req.bInterfaceClass    = kIOUSBFindInterfaceDontCare;
    req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    req.bAlternateSetting  = kIOUSBFindInterfaceDontCare;

    io_iterator_t it = 0;
    if ((*dev)->CreateInterfaceIterator(dev, &req, &it) != kIOReturnSuccess)
        return NULL;

    io_service_t svc;
    IOUSBInterfaceInterface **chosen = NULL;
    while ((svc = IOIteratorNext(it)) != 0){
        IOCFPlugInInterface **plug = NULL; SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(svc, kIOUSBInterfaceUserClientTypeID,
                kIOCFPlugInInterfaceID, &plug, &score) == kIOReturnSuccess && plug){
            IOUSBInterfaceInterface **intf = NULL;
            (*plug)->QueryInterface(plug,
                CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID), (LPVOID*)&intf);
            (*plug)->Release(plug);
            if (intf){
                UInt8 num = 0xff;
                (*intf)->GetInterfaceNumber(intf, &num);
                if (num == US122_IFACE){ chosen = intf; IOObjectRelease(svc); break; }
                (*intf)->Release(intf);
            }
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return chosen;
}

/* map an endpoint address (0x02 / 0x81) to an IOKit pipeRef on the interface.
   IOKit numbers pipes 1..N in descriptor order; we scan for the matching
   direction+number. [VALIDATE] the pipe scan against the real descriptors. */
static int find_pipe(IOUSBInterfaceInterface **intf, UInt8 ep_addr, UInt8 *outPipe, UInt16 *outMps){
    UInt8 npipes = 0;
    if ((*intf)->GetNumEndpoints(intf, &npipes) != kIOReturnSuccess) return -1;
    UInt8 want_dir = (ep_addr & 0x80) ? kUSBIn : kUSBOut;
    UInt8 want_num = ep_addr & 0x0f;
    for (UInt8 p = 1; p <= npipes; p++){
        UInt8 dir, num, tt, interval; UInt16 mps;
        if ((*intf)->GetPipeProperties(intf, p, &dir, &num, &tt, &mps, &interval) == kIOReturnSuccess){
            if (dir == want_dir && num == want_num){ *outPipe = p; if (outMps) *outMps = mps; return 0; }
        }
    }
    return -1;
}

static int open_device_and_stream(us122_engine_t *e){
    if (!find_device(&e->dev) || !e->dev) return -1;

    if ((*e->dev)->USBDeviceOpen(e->dev) != kIOReturnSuccess){
        (*e->dev)->Release(e->dev); e->dev = NULL; return -1;
    }
    /* set configuration 1 (V1 relied on the device's default config) */
    UInt8 numConf = 0;
    (*e->dev)->GetNumberOfConfigurations(e->dev, &numConf);
    if (numConf > 0){
        IOUSBConfigurationDescriptorPtr cfg = NULL;
        if ((*e->dev)->GetConfigurationDescriptorPtr(e->dev, 0, &cfg) == kIOReturnSuccess && cfg)
            (*e->dev)->SetConfiguration(e->dev, cfg->bConfigurationValue);
    }

    e->intf = find_interface(e->dev);
    if (!e->intf){ close_device(e); return -1; }

    if ((*e->intf)->USBInterfaceOpen(e->intf) != kIOReturnSuccess){ close_device(e); return -1; }
    /* alt setting 1 = the streaming setting (V1 fact) */
    if ((*e->intf)->SetAlternateInterface(e->intf, US122_ALT) != kIOReturnSuccess){ close_device(e); return -1; }

    if (engine_set_rate(e, e->rate) != 0){ close_device(e); return -1; }

    /* resolve pipes for the two iso endpoints, and read their actual reserved
       maxPacketSize. Per Apple docs: SetAlternateInterface succeeds even if the
       pipe got zero (or a mismatched) bandwidth reservation; the pipe's reserved
       size governs how the controller clocks bytes per microframe, regardless of
       our per-packet frReqCount. We never set this before, which can mis-clock
       the stream (correct bytes, wrong timing). Read it, log it, and explicitly
       reserve our size with SetPipePolicy. */
    UInt8 pb_pipe = 0, cap_pipe = 0; UInt16 pb_mps = 0, cap_mps = 0;
    if (find_pipe(e->intf, US122_EP_PLAYBACK, &pb_pipe, &pb_mps) != 0){ close_device(e); return -1; }
    if (find_pipe(e->intf, US122_EP_CAPTURE,  &cap_pipe, &cap_mps) != 0){ close_device(e); return -1; }
    syslog(LOG_NOTICE, "V2 pipes: pb pipe=%u mps=%u, cap pipe=%u mps=%u (want %u)",
           pb_pipe, pb_mps, cap_pipe, cap_mps, US122_MAX_PACKET_SIZE);

    /* explicitly reserve the bandwidth/packet size we actually use. maxInterval
       1 = serviced every (micro)frame. If the descriptor already reserved the
       right size this is a no-op; if it reserved zero or a different size, this
       corrects it. */
    IOReturn pol1 = (*e->intf)->SetPipePolicy(e->intf, pb_pipe,  US122_MAX_PACKET_SIZE, 1);
    IOReturn pol2 = (*e->intf)->SetPipePolicy(e->intf, cap_pipe, US122_MAX_PACKET_SIZE, 1);
    syslog(LOG_NOTICE, "V2 SetPipePolicy pb=0x%x cap=0x%x", pol1, pol2);

    /* Reset both iso pipes before streaming. After SetAlternateInterface the
       pipe can carry stale halt/data-toggle/framing state; libusb and other
       working macOS iso implementations clear the pipe as part of bringup. A
       stale framing offset would deliver correct bytes at the wrong microframe
       boundary (correct data, wrong time) and persist across both isoch APIs,
       which matches the distortion we cannot otherwise explain. */
    IOReturn rp1 = (*e->intf)->ResetPipe(e->intf, pb_pipe);
    IOReturn rp2 = (*e->intf)->ResetPipe(e->intf, cap_pipe);
    syslog(LOG_NOTICE, "V2 ResetPipe pb=0x%x cap=0x%x", rp1, rp2);

    /* attach the interface's async event source to THIS thread's runloop, so
       iso completions are serviced here (the realtime engine thread). */
    CFRunLoopSourceRef src = NULL;
    if ((*e->intf)->CreateInterfaceAsyncEventSource(e->intf, &src) != kIOReturnSuccess || !src){
        close_device(e); return -1;
    }
    e->runloop_src = src;
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);

    /* current USB frame number; schedule transfers a few frames ahead */
    UInt64 frame = 0; AbsoluteTime atime;
    (*e->intf)->GetBusFrameNumber(e->intf, &frame, &atime);
    frame += 50;   /* lead time. Standard isoch is less forgiving than
                      low-latency: too small a lead gets the submit rejected and
                      the stream wedges (no callback ever fires). 50 bus frames
                      (~50ms) is a safe starting lead; we can tighten later. */

    e->accum = 0;

    /* PRE-FILL the playback ring with a silence cushion before the first
       transfer goes out. Core Audio (writer) and the device-paced engine
       (reader) are independent; with no reservoir the ring runs near-empty and
       reads happen at jittered times relative to the device clock = correct
       samples placed at wrong times = the harmonic-shred distortion. A cushion
       of ~half the ring absorbs that jitter. Silence here is inaudible; real
       audio displaces it within the cushion's depth. */
    {
        uint32_t target = RING_FRAMES / 2;
        uint8_t zero[64 * US122_FRAME_BYTES];
        memset(zero, 0, sizeof(zero));
        uint32_t done = 0;
        while (done < target){
            uint32_t chunk = target - done; if (chunk > 64) chunk = 64;
            uint32_t w = ring_write(&e->playback, zero, chunk);
            if (w == 0) break;
            done += w;
        }
    }

    /* allocate low-latency buffers and submit the initial transfer pool for
       BOTH directions. Full-duplex must run together (V1 duplex-coupling
       fact), or the shared clock desyncs. */
    UInt32 bufbytes  = FRAMES_PER_XFER * US122_MAX_PACKET_SIZE;
    UInt32 flbytes   = FRAMES_PER_XFER * sizeof(IOUSBIsocFrame);
    for (int i = 0; i < NUM_ISO_XFERS; i++){
        /* Standard isoch: plain data buffer + plain frame-list array, mirroring
           V1/libusb and the Linux reference (usb_submit_urb). No special
           low-latency buffers. */
        g_pb_xfers[i].e = e; g_pb_xfers[i].is_playback = 1; g_pb_xfers[i].pipe_ref = pb_pipe;
        g_pb_xfers[i].buffer = calloc(1, bufbytes);
        g_pb_xfers[i].frames = calloc(1, flbytes);
        g_cap_xfers[i].e = e; g_cap_xfers[i].is_playback = 0; g_cap_xfers[i].pipe_ref = cap_pipe;
        g_cap_xfers[i].buffer = calloc(1, bufbytes);
        g_cap_xfers[i].frames = calloc(1, flbytes);
        if (!g_pb_xfers[i].buffer || !g_pb_xfers[i].frames ||
            !g_cap_xfers[i].buffer || !g_cap_xfers[i].frames){ close_device(e); return -1; }

        for (int f = 0; f < FRAMES_PER_XFER; f++){
            g_pb_xfers[i].frames[f].frReqCount  = 0;
            g_cap_xfers[i].frames[f].frReqCount = US122_MAX_PACKET_SIZE;
        }
        fill_playback_xfer(e, &g_pb_xfers[i]);
        g_pb_xfers[i].next_frame  = frame + (UInt64)i * XFER_BUS_FRAMES;
        g_cap_xfers[i].next_frame = frame + (UInt64)i * XFER_BUS_FRAMES;
    }

    /* Seed the shared monotonic counters past the initial fan-out. */
    atomic_store_explicit(&e->pb_next_frame,
        frame + (UInt64)NUM_ISO_XFERS * XFER_BUS_FRAMES, memory_order_relaxed);
    atomic_store_explicit(&e->cap_next_frame,
        frame + (UInt64)NUM_ISO_XFERS * XFER_BUS_FRAMES, memory_order_relaxed);

    /* kick off all transfers (standard isoch). Standard isoch is far less
       forgiving than low-latency about scheduling: if the start frame is too
       close to 'now', the submit is rejected and no callback ever fires, so the
       stream silently wedges. Check every result and log the first failure. */
    int pb_ok = 0, cap_ok = 0; IOReturn first_err = kIOReturnSuccess;
    for (int i = 0; i < NUM_ISO_XFERS; i++){
        IOReturn r1 = xfer_submit(e, &g_pb_xfers[i],  iso_playback_cb);
        IOReturn r2 = xfer_submit(e, &g_cap_xfers[i], iso_capture_cb);
        if (r1 == kIOReturnSuccess) pb_ok++; else if (first_err==kIOReturnSuccess) first_err=r1;
        if (r2 == kIOReturnSuccess) cap_ok++; else if (first_err==kIOReturnSuccess) first_err=r2;
    }
    syslog(LOG_NOTICE,
        "V2 us122 engine: streaming at %u Hz (standard isoch) submit pb_ok=%d cap_ok=%d first_err=0x%x startframe=%llu",
        e->rate, pb_ok, cap_ok, first_err, (unsigned long long)frame);
    return 0;
}

static void close_device(us122_engine_t *e){
    if (e->intf){
        (*e->intf)->AbortPipe(e->intf, 0);   /* best-effort abort all pipes */
        if (e->runloop_src){
            CFRunLoopRemoveSource(CFRunLoopGetCurrent(), e->runloop_src, kCFRunLoopDefaultMode);
            CFRelease(e->runloop_src); e->runloop_src = NULL;
        }
        for (int i = 0; i < NUM_ISO_XFERS; i++){
            if (g_pb_xfers[i].frames){ free(g_pb_xfers[i].frames); g_pb_xfers[i].frames = NULL; }
            if (g_pb_xfers[i].buffer){ free(g_pb_xfers[i].buffer); g_pb_xfers[i].buffer = NULL; }
            if (g_cap_xfers[i].frames){ free(g_cap_xfers[i].frames); g_cap_xfers[i].frames = NULL; }
            if (g_cap_xfers[i].buffer){ free(g_cap_xfers[i].buffer); g_cap_xfers[i].buffer = NULL; }
        }
        (*e->intf)->USBInterfaceClose(e->intf);
        (*e->intf)->Release(e->intf);
        e->intf = NULL;
    }
    if (e->dev){
        (*e->dev)->USBDeviceClose(e->dev);
        (*e->dev)->Release(e->dev);
        e->dev = NULL;
    }
}
