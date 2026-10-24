# Virtio video decoder

`UML::VideoDevice` exposes a stateful V4L2 decoder through Virtio device 31.
The device owns command validation, stream state, guest buffer mappings, and
completion events. It reuses the common vhost-user transport; decoding belongs
to `UML::VideoDecoder` in the platform layer. No GPU context is required.
Encoding is a separate device, ID 30, described [below](#encoder-device-id-30).

macOS enables the service with `--video`. Kernarium supplies it to each supported
kernel profile when hardware decoding is available.
The shared mainline and Android driver creates `/dev/video0`;
the Steam image provides the `/dev/video-dec0` udev alias used by CEF.

The Apple backend requires VideoToolbox hardware decoding. Its capabilities are
H.264 Baseline, Constrained Baseline, Main, and High; HEVC Main and Main10; and
baseline JPEG, subject to host availability.
H.264 High10 is conditional on the iOS profile probe and is excluded on macOS.
VP9 Profile 0 is added where VideoToolbox offers it as a supplemental decoder,
which every Apple silicon host does; the wire contract and the maintained
driver already name VP9, so this needs no kernel or protocol change. The device
still drops a platform capability the protocol cannot name rather than
publishing an unnamed format on the queue.
Decoded surfaces are copied into guest NV12, planar YUV420, P010, or packed
BGRA buffers. The decoder does not encode.

Packed BGRA is a single plane of 8-bit B, G, R, A samples, `width * 4` bytes per
row and opaque alpha, reached over the wire as `VIRTIO_VIDEO_FORMAT_BGRA8888`
and in the guest as `V4L2_PIX_FMT_ABGR32`. It is offered for every stream
whatever the coded bit depth is, because a client that can only draw through a
32-bit RGB surface would otherwise convert every frame on a guest processor,
which costs far more than decoding it. Selecting it makes the host decoder
produce the packed samples itself, so no processor in the path converts a
frame.

A client negotiates its capture format only after the source-change event, and
a decoder cannot change a running session's output layout without discarding
the reference frames the next picture needs. So that the choice still reaches
the decoder before its first picture, the device reads each packet's headers
without decoding it, reports the size that raises the source change, and holds
the packet until the client has chosen - by naming a capture format or by
starting the capture queue with the one it was given. The held packets are then
decoded from the stream's own random access point, in the chosen layout, so an
ordinary session converts nothing. That wait is bounded by a packet count and a
byte count; reaching either means the client is never going to choose, and the
device decodes what it holds in the planar layout nobody overrode rather than
stalling the stream.

A client that changes its capture format after frames have already been
delivered is asking mid-stream, where the decoder can only adopt the layout at
the next random access point. It states the layout each frame actually carries,
and the device converts the frames that still arrive planar. The conversion
uses the matrix and range the frame reports, which the decoder reads back from
the surface the hardware produced rather than inferring again, so a converted
frame and a packed one are the same picture and a stream does not change
appearance when the client negotiates part way through.

A capture buffer may also be a host-visible Virtio-GPU blob, which has no
guest-physical address because it lives in the vhost-user shared-memory region
the GPU backend publishes. With `VIRTIO_VIDEO_F_RESOURCE_SHM_REGION` the guest
names such a plane by region and offset, and the device resolves it through the
shared-memory index both backends hold, so decoded planes land directly in the
memory the renderer imported. The device offers the feature only when it was
given that index. A range leaves the index before its owner revokes it from the
guest, and every transfer resolves again, so a buffer whose blob has gone
completes with an error rather than reaching memory the guest no longer has.
The guest chooses the plane layout it negotiated: planes packed into one
dma-buf follow one another at their negotiated sizes.

Input uses complete Annex B H.264/HEVC access units, complete JPEG frames, or
complete VP9 frames.
Streams retain timestamps, signal format changes, drain delayed output, and
complete cancellation and EOS through the command and event queues. Pending
event buffers are bounded by the transport's maximum virtqueue size; full
queues apply backpressure instead of completing empty events.

Whether an application decoded in hardware cannot be settled from inside the
guest: a player that falls back to software shows the same correct picture, and
an open device node is not a decode. Setting `UML_VIDEO_ACTIVITY_LOG` to a
writable path makes the device append the profiles it advertised, every stream a
guest opened with its codec and profile, and each stream's packet, byte, and
delivered-frame totals when it closes. It is a file because the emulator's
stderr shares the guest console. Unset, the device opens and writes nothing.

`virtio-video-protocol` exercises buffer layouts across 4 KiB and 16 KiB
memory boundaries, stream lifecycle, EOS, and event backpressure with a synthetic
decoder. `platform-video-decoder` decodes the checked-in generated fixtures on
Apple hardware. Both are included in the `check-gpu` workflow. The optional `guest-v4l2-decode` CTest runs the actual V4L2 driver
against every advertised codec profile and verifies pixels and clean shutdown.
See [testing](../../../docs/testing.md) for the guest probe input.

## Encoder (device ID 30)

The encoder is a second Virtio device instance with the standard encoder ID,
beside the decoder's 31. It has its own vhost-user service, so the asynchronous
encoder callbacks and queues stay apart from the synchronous decoder, a guest
kernel without the encoder never binds it, and a host without a hardware
encoder never creates it. The [wire contract](../../../kernel/common/video/README.md#encoder-device-id-30)
and the platform interface (`darwin/platform/video_encoder.hpp`) are its
contract; the device depends only on that interface, the memory layer and the
device contracts, and never on VideoToolbox.

The device and the platform backend are not in this tree yet: this section and
those two headers are what they are built against.

A guest reaches the encoder as a stateful V4L2 encoder (`stateful-encoder`
video node). Raw frames are copied once, from guest pages or from a host-visible
blob, into a buffer the platform encoder owns, and the guest's input buffer
completes as soon as that copy is done. Coded buffers are Annex B for H.264 and
HEVC and a complete JFIF image for JPEG. Output order is decode order and each
coded buffer carries its source frame's timestamp.

Discovery is the platform's, and it publishes only what an encode on this host
proved: a profile, input layout, size or rate-control mode that did not encode
is absent. The guest therefore learns the host's capability from which formats,
controls and menu items exist, and a control that is not published is
unsupported, which is the truthful answer.

### Encoder controls

The host publishes the controls below through
`VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS`, and the guest driver registers exactly
what is published. IDs and menu indices are the Linux UAPI values in
[`virtio_video_v4l2_ids.h`](../../../kernel/common/video/virtio_video_v4l2_ids.h),
which `sync-protocol.py` checks against the kernel. Every control is grabbed
while the stream encodes, and the driver returns `-EBUSY` for it, unless its
"Dyn" column is yes. "VT" is VideoToolbox, and the VT column names the public
`kVTCompressionPropertyKey_*` key with the prefix dropped, or the host
behavior. Private VT keys are never used.

Rate control and quality:

| Control | Type | Range, menu and default | Dyn | Published when | VT |
| --- | --- | --- | --- | --- | --- |
| `MPEG_VIDEO_BITRATE_MODE` | menu | VBR, CBR, CQ; the skip mask removes unverified modes; default VBR | no | always | VBR: `AverageBitRate` and `DataRateLimits` from the peak. CBR: `ConstantBitRate` (the standard encoder only). CQ: `ConstantQualityFactor`, else `Quality` |
| `MPEG_VIDEO_BITRATE` | int | 1000 to the codec maximum; default 2,000,000 | yes | always | `AverageBitRate` or `ConstantBitRate` |
| `MPEG_VIDEO_BITRATE_PEAK` | int | 1000 to the codec maximum; default 0 (none) | yes | VBR verified | `DataRateLimits`, one second |
| `MPEG_VIDEO_CONSTANT_QUALITY` | int | 1 to 100; default 50 | yes | CQ verified | `ConstantQualityFactor`, else `Quality` = q/100 |
| `MPEG_VIDEO_FRAME_RC_ENABLE` | bool | default 1 | no | constant QP verified | 0 selects constant QP from the `*_FRAME_QP` controls |
| `MPEG_VIDEO_MB_RC_ENABLE` | bool | default 1 | no | always | accepted; the encoder's own spatial adaptation |
| `MPEG_VIDEO_VBV_SIZE`, `MPEG_VIDEO_H264_CPB_SIZE` | int | 0 to 2^31-1 kB; default 0 (encoder default) | no | `VBVBufferDuration` verified | duration = size * 8192 / bitrate seconds |
| `MPEG_VIDEO_H264_MIN_QP`, `_MAX_QP` | int | 0 to 51; defaults 0 and 51 | max only | H.264 | `MinAllowedFrameQP`, `MaxAllowedFrameQP`; the host refuses min above max |
| `MPEG_VIDEO_HEVC_MIN_QP`, `_MAX_QP` | int | 0 to 51 (the 10-bit range only if verified); defaults 0 and 51 | max only | HEVC | as H.264 |
| `MPEG_VIDEO_H264_I_FRAME_QP`, `_P_`, `_B_` | int | 0 to 51; default 26 | no | constant QP verified | the low-latency encoder: `BaseFrameQP` per frame; otherwise Min = Max QP |
| `MPEG_VIDEO_HEVC_I_FRAME_QP`, `_P_`, `_B_` | int | as H.264 | no | as H.264 | as H.264 |
| `JPEG_COMPRESSION_QUALITY` | int | 1 to 100; default 85 | yes | JPEG listed | `Quality` = q/100 |

Structure and headers:

| Control | Type | Range, menu and default | Dyn | Published when | VT |
| --- | --- | --- | --- | --- | --- |
| `MPEG_VIDEO_GOP_SIZE` | int | 0 to 2^31-1; default 0 (no periodic key frame) | no: VT ignores a change while streaming | always | `MaxKeyFrameInterval`; 0 means never |
| `MPEG_VIDEO_B_FRAMES` | int | 0 to the verified maximum; default 0 | no | the standard encoder | 0: `AllowFrameReordering` = NO, always set because VT defaults to YES. Above 0: YES, and VT picks the pattern |
| `MPEG_VIDEO_FORCE_KEY_FRAME` | button | | yes | always | `ForceKeyFrame` on the next submitted frame |
| `MPEG_VIDEO_HEADER_MODE` | menu | SEPARATE, JOINED_WITH_1ST_FRAME; default JOINED | no | always | host Annex B assembly; SEPARATE produces a `CODEC_CONFIG` buffer first |
| `MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR` | bool | default 1 | yes | always | the host inserts VPS, SPS and PPS before each IDR |
| `MPEG_VIDEO_H264_ENTROPY_MODE` | menu | CAVLC, CABAC; default CABAC | no | H.264 | `H264EntropyMode`; CABAC is refused under Baseline |
| `MPEG_VIDEO_H264_8X8_TRANSFORM` | bool | default 1 | no | H.264 High | accepted; the encoder decides |
| `MPEG_VIDEO_H264_HIERARCHICAL_CODING` | bool | default 0 | no | low-latency H.264 verified | 1 selects the low-latency encoder |
| `MPEG_VIDEO_H264_HIERARCHICAL_CODING_TYPE` | menu | P only | no | as above | |
| `MPEG_VIDEO_H264_HIERARCHICAL_CODING_LAYER` | int | 1 to 2; default 1 | no | as above | 2: `BaseLayerFrameRateFraction` 0.5 |
| `MPEG_VIDEO_H264_HIER_CODING_L0_BR`, `_L1_BR` | int | bits per second | no | as above | `BaseLayerBitRateFraction` = L0 / (L0 + L1) |
| `MPEG_VIDEO_HEVC_HIER_CODING_TYPE`, `_LAYER`, `_L0_BR`, `_L1_BR` | as H.264 | as H.264 | no | low-latency HEVC verified | as H.264 |

Profile, level and tier. The menu items are the UAPI indices, each published
only when an encode with it wrote a matching SPS:

| Control | Published menu items | VT |
| --- | --- | --- |
| `MPEG_VIDEO_H264_PROFILE` | BASELINE, CONSTRAINED_BASELINE, MAIN, HIGH, CONSTRAINED_HIGH; HIGH_422 and HIGH_444_PREDICTIVE once NV16 and NV24 input exist | `ProfileLevel` H264_`<profile>`_`<level>` |
| `MPEG_VIDEO_H264_LEVEL` | 1.0 to 5.2 | 1.3, 3.0 to 3.2, 4.0 to 4.2 and 5.0 to 5.2 map to the matching constant. 1.0 to 1.2, 1b and 2.x map to AutoLevel, which picks the lowest level that fits. 6.x is not published |
| `MPEG_VIDEO_HEVC_PROFILE` | MAIN, MAIN_10, and MAIN_STILL_PICTURE when verified | `HEVC_Main_AutoLevel`, `HEVC_Main10_AutoLevel`, `HEVC_MainStill` |
| `MPEG_VIDEO_HEVC_LEVEL` | 1 to the verified maximum (6.2 when 8192x4320 encodes) | VT has AutoLevel only, so a requested level is a ceiling and the stream may carry a lower one; the level actually written is what the stream reports |
| `MPEG_VIDEO_HEVC_TIER` | MAIN | |

HDR and colorimetry:

| Control or field | Type | Published when | VT |
| --- | --- | --- | --- |
| `COLORIMETRY_HDR10_CLL_INFO` | compound, 4 bytes | HEVC Main10 verified | `ContentLightLevelInfo`, with the big-endian SEI payload built by the host |
| `COLORIMETRY_HDR10_MASTERING_DISPLAY` | compound, 24 bytes | HEVC Main10 verified | `MasteringDisplayColorVolume` |
| `SET_STREAM_INFO` colorspace | `v4l2_colorspace` | always | REC709: `ColorPrimaries_ITU_R_709_2`. SMPTE170M: `SMPTE_C`. 470_SYSTEM_BG: `EBU_3213`. BT2020: `ITU_R_2020`. DCI_P3: `P3_D65`. SRGB: 709 primaries |
| `SET_STREAM_INFO` xfer_func | `v4l2_xfer_func` | always | 709: `ITU_R_709_2`. SRGB: `sRGB`. SMPTE2084: `SMPTE_ST_2084_PQ`. NONE: `Linear` |
| `SET_STREAM_INFO` ycbcr_enc | `v4l2_ycbcr_encoding` | always | 601: `ITU_R_601_4`. 709: `ITU_R_709_2`. BT2020: `ITU_R_2020` |
| `SET_STREAM_INFO` quantization | `v4l2_quantization` | always | FULL_RANGE selects the 420f and xf20 input formats; LIM_RANGE selects 420v and x420 |

The same values are attached to every pixel buffer and written into the coded
stream's VUI. `GET_STREAM_INFO` on the coded queue returns what was written,
which is what Android's color-aspects checks read back. `MIN_BUFFERS_FOR_OUTPUT`
is read-only and 2, because the host copies each raw frame on arrival.

Raw input formats (V4L2 OUTPUT) are NV12, YUV420 (`YU12`), P010 (HEVC Main10
only), packed BGRA for Android surface input (VideoToolbox converts), and the
encoder-only NV16 and NV24 for H.264 High 4:2:2 and High 4:4:4 Predictive. The
coded formats are `H264`, `HEVC` and `JPEG`, each listed only when discovery
found a hardware encoder for it. Odd visible sizes are rounded down to even by
VideoToolbox, so the device reports an alignment of 2.

Not published, because the host can neither honor nor emulate the control: the
multi-slice controls (`MaxH264SliceBytes` fails on the hardware encoder and the
slice count is a private key), intra refresh (no public key), the long-term
reference controls (VideoToolbox's are token-acknowledged, not index-based),
`MPEG_VIDEO_AVERAGE_QP` (VideoToolbox reports no per-frame QP), frame skip
(VideoToolbox never skips), the HEVC length-field and refresh-type controls
(the output is always Annex B and IDR), and B-frame hierarchical coding (the
low-latency encoder has no B-frames).

Host capabilities that no guest API can name are documented here and exposed
nowhere:

- ProRes 422, HQ, LT, Proxy, 4444 and 4444 XQ.
- HEVC with alpha, depth and disparity HEVC, and MV-HEVC.
- HEVC range extensions (Main 4:2:2 10, Main 4:4:4, Main 4:4:4 10, Monochrome
  and Monochrome 10), because the V4L2 HEVC profile menu ends at Main 10.
- The HLG transfer function, which V4L2 cannot carry.
- VideoToolbox settings with no V4L2 control: presets, speed against quality,
  spatial adaptive QP levels, the VBV initial delay and HDR10+ per-frame
  metadata.
- H.264 levels 6.0 to 6.2.
