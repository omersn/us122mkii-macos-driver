/*
 * us122_descdump.c
 *
 * Read-only USB descriptor dumper for the TASCAM US-122MKII.
 * Target: macOS 10.13 (Intel), libusb-1.0 via Homebrew.
 *
 * What it does:
 *   - Opens VID 0x0644 / PID 0x8021 (running state of the US-122MKII).
 *   - Walks the active configuration descriptor.
 *   - Prints every interface, alternate setting, and endpoint.
 *   - Dumps the raw bytes of any class-specific (audio) descriptors so the
 *     sample format / rates can be decoded by hand.
 *
 * What it does NOT do:
 *   - No interface claim for streaming, no data transfers, no writes.
 *   - Purely descriptor reads. Safe to run repeatedly.
 *
 * Build (on the Mac):
 *   clang us122_descdump.c -o us122_descdump \
 *       -I"$(brew --prefix libusb)/include/libusb-1.0" \
 *       -L"$(brew --prefix libusb)/lib" -lusb-1.0
 *
 * Run:
 *   ./us122_descdump
 *
 * If you get LIBUSB_ERROR_ACCESS, macOS has the device seized. That itself
 * is useful information for the next step; report it back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <libusb-1.0/libusb.h>

#define VID 0x0644
#define PID 0x8021

/* Decode bmAttributes transfer-type bits for an endpoint. */
static const char *xfer_type(uint8_t bmAttributes) {
    switch (bmAttributes & 0x03) {
        case LIBUSB_TRANSFER_TYPE_CONTROL:     return "CONTROL";
        case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: return "ISOCHRONOUS";
        case LIBUSB_TRANSFER_TYPE_BULK:        return "BULK";
        case LIBUSB_TRANSFER_TYPE_INTERRUPT:   return "INTERRUPT";
        default:                               return "UNKNOWN";
    }
}

/* For isochronous endpoints, decode the sync + usage sub-bits. */
static const char *iso_sync(uint8_t bmAttributes) {
    switch ((bmAttributes >> 2) & 0x03) {
        case 0: return "no-sync";
        case 1: return "async";
        case 2: return "adaptive";
        case 3: return "sync";
        default: return "?";
    }
}
static const char *iso_usage(uint8_t bmAttributes) {
    switch ((bmAttributes >> 4) & 0x03) {
        case 0: return "data";
        case 1: return "feedback";
        case 2: return "implicit-feedback-data";
        default: return "?";
    }
}

/* Print the raw bytes of an extra (class-specific) descriptor block. */
static void dump_extra(const unsigned char *extra, int extra_length) {
    if (extra_length <= 0) return;
    printf("        class-specific descriptor bytes (%d):\n", extra_length);
    int i = 0;
    while (i < extra_length) {
        int bLength = extra[i];
        if (bLength <= 0 || i + bLength > extra_length) {
            /* Malformed or trailing; dump the rest flat and stop. */
            printf("          [raw tail]");
            for (; i < extra_length; i++) printf(" %02x", extra[i]);
            printf("\n");
            break;
        }
        int bDescriptorType = (bLength >= 2) ? extra[i + 1] : -1;
        printf("          bLength=%d bDescriptorType=0x%02x :",
               bLength, bDescriptorType);
        for (int j = 0; j < bLength; j++) printf(" %02x", extra[i + j]);
        printf("\n");
        i += bLength;
    }
}

int main(void) {
    libusb_context *ctx = NULL;
    int rc = libusb_init(&ctx);
    if (rc != 0) {
        fprintf(stderr, "libusb_init failed: %s\n", libusb_error_name(rc));
        return 1;
    }

    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) {
        fprintf(stderr,
            "Could not open %04x:%04x. Device not found, or macOS has it "
            "seized (LIBUSB_ERROR_ACCESS). If it shows in System Information "
            "but won't open here, that is itself the finding to report.\n",
            VID, PID);
        libusb_exit(ctx);
        return 2;
    }

    libusb_device *dev = libusb_get_device(h);

    struct libusb_device_descriptor dd;
    rc = libusb_get_device_descriptor(dev, &dd);
    if (rc != 0) {
        fprintf(stderr, "get_device_descriptor failed: %s\n",
                libusb_error_name(rc));
        libusb_close(h);
        libusb_exit(ctx);
        return 3;
    }

    printf("=== DEVICE %04x:%04x ===\n", dd.idVendor, dd.idProduct);
    printf("  bcdUSB=0x%04x  bDeviceClass=0x%02x  bNumConfigurations=%d\n",
           dd.bcdUSB, dd.bDeviceClass, dd.bNumConfigurations);

    struct libusb_config_descriptor *cfg = NULL;
    rc = libusb_get_active_config_descriptor(dev, &cfg);
    if (rc != 0) {
        fprintf(stderr, "get_active_config_descriptor failed: %s\n",
                libusb_error_name(rc));
        libusb_close(h);
        libusb_exit(ctx);
        return 4;
    }

    printf("=== CONFIG: bNumInterfaces=%d  bConfigurationValue=%d ===\n",
           cfg->bNumInterfaces, cfg->bConfigurationValue);

    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        printf("\n  INTERFACE index %d  (num_altsetting=%d)\n",
               i, itf->num_altsetting);

        for (int a = 0; a < itf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *id =
                &itf->altsetting[a];
            printf("    ALT bInterfaceNumber=%d bAlternateSetting=%d "
                   "class=0x%02x subclass=0x%02x proto=0x%02x bNumEndpoints=%d\n",
                   id->bInterfaceNumber, id->bAlternateSetting,
                   id->bInterfaceClass, id->bInterfaceSubClass,
                   id->bInterfaceProtocol, id->bNumEndpoints);

            /* Interface-level class-specific descriptors (e.g. AS general,
               format type with bit depth + sample rates). */
            dump_extra(id->extra, id->extra_length);

            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep =
                    &id->endpoint[e];
                uint8_t addr = ep->bEndpointAddress;
                const char *dir = (addr & 0x80) ? "IN" : "OUT";
                printf("      EP 0x%02x (%s #%d)  type=%s",
                       addr, dir, addr & 0x0f, xfer_type(ep->bmAttributes));
                if ((ep->bmAttributes & 0x03) ==
                    LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
                    printf(" [%s, %s]",
                           iso_sync(ep->bmAttributes),
                           iso_usage(ep->bmAttributes));
                }
                printf("  wMaxPacketSize=%d  bInterval=%d\n",
                       ep->wMaxPacketSize, ep->bInterval);

                /* Endpoint-level class-specific descriptors. */
                dump_extra(ep->extra, ep->extra_length);
            }
        }
    }

    libusb_free_config_descriptor(cfg);
    libusb_close(h);
    libusb_exit(ctx);
    printf("\n=== done ===\n");
    return 0;
}
