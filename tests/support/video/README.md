# Video decoder fixtures

These 128 × 96 synthetic color-pattern frames cover H.264 Baseline,
Constrained Baseline, Main, High, Constrained High and High10; HEVC Main and Main10; baseline
4:2:0 JPEG; and VP9 Profile 0. Each `.bin` contains one Annex B access unit, a
JPEG image, or one VP9 frame with no container. Its `.raw` reference is tightly
packed NV12 or P010, with no row padding. The patterns contain no third-party
imagery; the VP9 fixture is the same pattern losslessly re-encoded from
`h264-baseline.raw`.

The platform test decodes every profile advertised by the host and compares
all output samples with the software-decoded reference. Hardware rounding may
differ by two sample levels (four for JPEG). H.264 High10 and VP9 are exercised
only on hosts whose hardware capability probe reports them. The guest V4L2
probe uses the same VP9 fixture; VP9 states its profile in the stream, so the
driver exposes no profile menu and the advertised format is the whole
capability.

`h264-resolution-change.bin` is three Constrained Baseline sequences of eight
pictures each at 128 × 96, 96 × 64 and 160 × 112, with an access unit delimiter
opening every access unit and the parameter sets repeated at every IDR picture.
Every picture is flat gray, 8 levels brighter than the one before it (luma
`16 + 8 * N` for picture N of 24, chroma 128), so a test names a decoded picture
by its brightness and sees any that is lost, repeated or out of order. It is
three runs of
`ffmpeg -f lavfi -i "color=c=black:size=128x96:rate=30,format=yuv420p,geq=lum='16+8*(N+0)':cb=128:cr=128" -frames:v 8 -c:v libx264 -profile:v baseline -x264-params "keyint=250:min-keyint=250:bframes=0:scenecut=0:aud=1:repeat-headers=1:ref=1" -bsf:v filter_units=remove_types=6 -f h264 a.h264`
(sizes 96x64 and 160x112 with `N+8` and `N+16`), concatenated; the filter
removes the encoder identification string.

## HEVC streams for order and cropping

Six more HEVC streams exercise what a single access unit cannot. All of them
are Annex B, with the parameter sets repeated at every random access point, and
none carries an encoder identification string.

- `hevc-bframes.bin` is 20 pictures of 64 × 64, every one a flat gray 10 levels
  brighter than the one it follows in display order (luma `16 + 10 * N`), coded
  with hierarchical B-pictures in two closed groups of 12 and 8 (two IDR
  pictures, `sps_max_num_reorder_pics` 2). The brightness names the display
  position of a decoded picture, so a test sees the order without a reference
  file.
- `hevc-open-gop.bin` is 24 pictures of the same kind with open groups: the
  CRA pictures at decode positions 5 and 13 are followed by RASL pictures that
  reference the picture before them. A decoder that starts at a CRA must not
  show those.
- `hevc-window.bin` is the pattern of `hevc-main.raw` coded as 128 × 96 with a
  conformance window of 6 samples on the left, 2 on the right, 4 on top and 2 on
  the bottom, so the stream shows 120 × 90. `hevc-window.raw` is the picture a
  software decoder shows.
- `hevc-full-range.bin` is the pattern of `hevc-main.raw` with the full-range
  flag set in the VUI, and `hevc-full-range.raw` what a software decoder
  returns, with no range conversion.
- `h264-constrained-high.bin` and `.raw` are `h264-high.bin` and its
  reference with `constraint_set4_flag` and `constraint_set5_flag` set in the
  SPS (the byte after `profile_idc` 100 is 0x0c), which is what Constrained
  High states for a stream with only frame macroblocks and no B-pictures; the
  picture is the single I picture High has, so the same reference serves.
- `h264-high-level-5-2.bin` is one flat gray High picture of 4096 × 2304, the
  largest frame level 5.2 allows, with level_idc 52:
  `ffmpeg -f lavfi -i "color=c=gray:size=4096x2304:rate=30,format=yuv420p" -frames:v 1 -c:v libx264 -profile:v high -level:v 5.2 -x264-params repeat-headers=1 -bsf:v filter_units=remove_types=6 -f h264 h264-high-level-5-2.bin`.
  The platform test decodes it whenever the capability list states level 5.2.
- `hevc-high-tier-4k.bin` and `hevc-high-tier-8k.bin` are one flat gray
  picture of 4096 × 2160 at level 5.2 and 8192 × 4320 at level 6.2, both High
  tier. The platform test decodes each the host's capability list says it can,
  so that nothing is advertised that the hardware does not decode.

They are produced with ffmpeg and x265, for example
`ffmpeg -f lavfi -i "color=c=black:size=64x64:rate=30,format=yuv420p,geq=lum='16+10*N':cb=128:cr=128" -frames:v 20 -c:v libx265 -x265-params "bframes=3:b-pyramid=1:keyint=12:min-keyint=12:open-gop=0:no-scenecut=1:b-adapt=0:no-info=1:repeat-headers=1" -f hevc hevc-bframes.bin`
(`open-gop=1:keyint=8` for the open groups), and the window by
`-c copy -bsf:v hevc_metadata=crop_left=6:crop_right=2:crop_top=4:crop_bottom=2`
on a 128 × 96 stream coded from `hevc-main.raw`.

## H.264 streams for order and cropping

Four more streams do the same for H.264. All are Annex B with the parameter
sets repeated at every IDR picture, and none carries an encoder identification
string (SEI units are removed).

- `h264-bframes.bin` is 20 pictures of 64 × 64 with the flat gray ramp of
  `hevc-bframes.bin` (luma `16 + 10 * N`), High profile, coded with B-pictures
  and reference B-pictures in two closed groups of 12 and 8 (two IDR pictures,
  `max_num_reorder_frames` 2, `max_dec_frame_buffering` 4, picture order count
  type 0, explicit reference marking). It is
  `ffmpeg -f lavfi -i "color=c=black:size=64x64:rate=30,format=yuv420p,geq=lum='16+10*N':cb=128:cr=128" -frames:v 20 -c:v libx264 -profile:v high -x264-params "bframes=3:b-pyramid=normal:keyint=12:min-keyint=12:scenecut=0:b-adapt=0:ref=3:repeat-headers=1:aud=0" -bsf:v filter_units=remove_types=6 -f h264 h264-bframes.bin`.
- `h264-no-restriction.bin` is the same stream with `bitstream_restriction_flag`
  cleared in the SPS (x264 always writes the limits, so the bits after the flag
  are dropped by hand), so that the decoder falls back to what the level allows.
- `h264-poc2.bin` is Main profile with picture order count type 2 and no
  B-pictures, coded with the same source and `bframes=0:ref=2`.
- `h264-window.bin` is the pattern of `hevc-main.raw` coded as 128 × 96 Main
  profile with a frame cropping rectangle of 6 samples on the left, 2 on the
  right, 4 on top and 2 on the bottom (`h264_metadata=crop_left=6:crop_right=2:crop_top=4:crop_bottom=2`),
  so the stream shows 120 × 90. `h264-window.raw` is the picture a software
  decoder shows.

## Synthetic encoder

`synthetic_encoder.hpp` is a `uml::platform::VideoEncoder` that encodes
nothing. Each raw frame becomes a small Annex B (or JFIF) unit that names the
frame, so a test sees from the guest's coded buffer which frame crossed, in
what order, and whether it was a key frame. Its frame pool pads every plane's
stride, output can be held back and released, and it records the settings,
updates, resets and submitted planes. The Virtio video encoder device tests
link it as `UML::TestSyntheticEncoder`; no production target does.
