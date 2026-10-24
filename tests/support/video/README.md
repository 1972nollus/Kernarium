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

## Synthetic encoder

`synthetic_encoder.hpp` is a `uml::platform::VideoEncoder` that encodes
nothing. Each raw frame becomes a small Annex B (or JFIF) unit that names the
frame, so a test sees from the guest's coded buffer which frame crossed, in
what order, and whether it was a key frame. Its frame pool pads every plane's
stride, output can be held back and released, and it records the settings,
updates, resets and submitted planes. The Virtio video encoder device tests
link it as `UML::TestSyntheticEncoder`; no production target does.
