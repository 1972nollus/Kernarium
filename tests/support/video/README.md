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
