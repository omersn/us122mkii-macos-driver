# 05 - Protocol facts (hardware-proven ground truth)

> **Update 2026-06-29 (see `docs/07`):** The "buffer-layout nuance" open question
> near the end of this file is **answered**. There is no per-packet offset field in
> the IOKit isoc frame structs; the low-latency data layout is identical to
> standard isoc; the correct layout is V1's own libusb layout (contiguous packed
> playback by per-packet byte count, fixed-stride `i*78` capture). Details in
> `docs/07-stage0-findings-and-stage1-plan.md`.

Every constant here was proven on the physical device (in V1, and cross-checked
against the serifpersia Linux reference in `reference/linux-reference/`). Treat
this as ground truth. Do not "improve" these values; they are what the hardware
does.

## Device identity

| Fact | Value |
|---|---|
| Vendor ID | `0x0644` |
| Product ID | `0x8021` (running state; the device self-presents at this PID across power cycles, **no firmware upload needed**) |
| Interface | `1` |
| Alt setting | `1` |
| Playback endpoint (iso OUT) | `0x02` |
| Capture endpoint (iso IN) | `0x81` |
| Class | vendor-specific (this is why no stock macOS driver works) |

## Audio format

| Fact | Value |
|---|---|
| Frame size | `6` bytes (24-bit stereo: 2 channels x 3 bytes) |
| Max packet size | `78` bytes (13 frames per packet) |
| Sample packing | 24-bit carried as the **HIGH 3 bytes of a 32-bit sample**, little-endian |
| Microframes per second | `8000` (high-speed; 8 microframes per 1ms bus frame) |
| Duplex | **coupled**: one physical crystal clocks both capture and playback |
| True rate | not exactly nominal; measured ~47718 fps when set to 48000 in V1 (the device is the clock master; capture delivery reveals its real tempo) |

### The 24-bit packing, exactly (proven correct by value-dump in V2)

```c
// float [-1,1] stereo frame -> device-native 6 bytes
int32_t s = (int32_t)(f * 2147483647.0f);  // scale to full int32
dst[0] = (s >> 8)  & 0xff;   // high-3-of-32, little-endian
dst[1] = (s >> 16) & 0xff;
dst[2] = (s >> 24) & 0xff;
```
The reverse (`dev_to_float`) reconstructs `((s0<<8)|(s1<<16)|(s2<<24)) /
2147483647.0`. This conversion is identical to the Linux reference (byte for
byte) and was proven correct: a 0.5 float yields bytes `00 00 40`. Do not change
it; V2 wasted cycles suspecting it and the dump exonerated it.

## Sample-rate control transfer (exact, from the Linux reference)

- `bmRequestType`: class / host-to-device / **endpoint** recipient
- `bRequest`: `UAC_SET_CUR` = `0x01`
- `wValue`: `UAC_SAMPLING_FREQ_CONTROL` = `0x0100`
- `wIndex`: `0x81` (the capture endpoint; sets the shared clock)
- payload: 3-byte little-endian sample rate

| Rate | payload bytes |
|---|---|
| 44100 | `0x44 0xAC 0x00`  (0x00AC44 = 44100) |
| 48000 | `0x80 0xBB 0x00`  (0x00BB80 = 48000) |
| 88200 | `0x88 0x58 0x01`  (0x015888 = 88200) |
| 96000 | `0x00 0x77 0x01`  (0x017700 = 96000) |

In V1 the daemon sends this via a libusb control transfer on the device handle.
V2 sent it via `(*dev)->DeviceRequest`. Both matched the reference and worked;
rate-set is NOT a suspect for any known bug.

## Transfer geometry (note the V1-vs-reference difference)

| Implementation | URBs | packets per URB | packet stride |
|---|---|---|---|
| Linux reference | `NUM_URBS = 4` | `ISO_PACKETS_PER_URB = 8` (= 1 bus frame) | `offset = i * MAX_PACKET_SIZE` |
| V1 daemon | `NUM_URBS = 16` | `ISO_PACKETS_PER_URB = 16` (= 2 bus frames) | (libusb manages) |
| V2 (abandoned) | 16, later tried 8 | tried 16 then 8 to match reference | assumed `i * MAX_PACKET_SIZE` |

Both V1 (16/16) and the reference (4/8) clock the device cleanly, so exact
geometry is not load-bearing for correctness; it trades latency vs runway. The
pacing accumulator (`accum += rate; frames = accum / 8000; bytes = frames *
BYTES_PER_FRAME`) is identical across V1, V2, and the reference, and is proven.

## The buffer-layout nuance that matters for the low-latency work

The Linux reference uses **fixed-stride** packet layout in its iso frame
descriptors: `iso_frame_desc[i].offset = i * MAX_PACKET_SIZE`, with
`.length = MAX_PACKET_SIZE` for capture and the per-packet computed `bytes` for
playback. This is **standard** iso (via `usb_submit_urb`), and it works.

V2 assumed the SAME fixed-stride layout for the **low-latency** IOKit API and
bit-crushed. So the open question for the low-latency rewrite is precise:

> Does Apple's low-latency isoc controller read packet i's data at the same
> fixed `i x maxPacketSize` stride that standard iso uses, or does it expect a
> different layout (e.g. an offset recorded in the `IOUSBLowLatencyIsocFrame`
> frame list)?

If low-latency uses the same fixed-stride layout, then V2's bit-crush was NOT the
buffer layout and the suspect returns to low-latency delivery itself (in which
case the device may simply not tolerate the low-latency path, and V1's jitter is
the floor). If low-latency uses a DIFFERENT layout, then V2's fixed-stride
assumption was the bug and a corrected layout fixes it. **This is the single
question Stage 0 of the roadmap exists to answer.** It can only be answered with
the macOS SDK headers in front of you, which V2's Linux container lacked.

## Things proven NOT to be the cause of any bug (do not re-suspect)

From the V2 debugging, all eliminated by measurement:
- the 24-bit packing / byte order (value dump: bytes are correct)
- the sample-rate control message (matches reference exactly)
- transfer ordering (V2 `reanchor=0` once the single-counter scheduler was in)
- duplex sync (V2 `pb_cb` and `cap_cb` locked in lockstep)
- a real clock-rate error (the ~47500 fps reading was a capture-count artifact;
  the device runs a true 48000, proven because V1 would have drifted otherwise
  and the ring fill held steady with zero underruns)

## Reference files

- `reference/linux-reference/us122mkii.c` - serifpersia's ALSA driver, the
  protocol source of truth. (The repo is for the US-144MKII family; the
  `us122mkii` branch / this file targets the US-122MKII. Use it for protocol
  facts only; it is Linux/ALSA, not macOS.)
- `tools/us122_descdump.c` - dumps the device's USB descriptors; the reference
  for confirming endpoint addresses and pipe ordering on the real hardware.
- `tools/us122_pack.c`, `tools/us122_wavplay.c` - early standalone probes
  (tone/wav playback over raw iso); useful for isolating the transport from
  Core Audio when debugging.
