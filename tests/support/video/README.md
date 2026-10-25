# Video decoder fixtures

These 128 × 96 synthetic color-pattern frames cover H.264 Baseline,
Constrained Baseline, Main, High and High10; HEVC Main and Main10; baseline
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

## HEVC streams for order and cropping

Three more HEVC streams exercise what a single access unit cannot. All of them
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

They are produced with ffmpeg and x265, for example
`ffmpeg -f lavfi -i "color=c=black:size=64x64:rate=30,format=yuv420p,geq=lum='16+10*N':cb=128:cr=128" -frames:v 20 -c:v libx265 -x265-params "bframes=3:b-pyramid=1:keyint=12:min-keyint=12:open-gop=0:no-scenecut=1:b-adapt=0:no-info=1:repeat-headers=1" -f hevc hevc-bframes.bin`
(`open-gop=1:keyint=8` for the open groups), and the window by
`-c copy -bsf:v hevc_metadata=crop_left=6:crop_right=2:crop_top=4:crop_bottom=2`
on a 128 × 96 stream coded from `hevc-main.raw`.

## Synthetic encoder

`synthetic_encoder.hpp` is a `uml::platform::VideoEncoder` that encodes
nothing. Each raw frame becomes a small Annex B (or JFIF) unit that names the
frame, so a test sees from the guest's coded buffer which frame crossed, in
what order, and whether it was a key frame. Its frame pool pads every plane's
stride, output can be held back and released, and it records the settings,
updates, resets and submitted planes. The Virtio video encoder device tests
link it as `UML::TestSyntheticEncoder`; no production target does.
