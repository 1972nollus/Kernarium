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
only on hosts whose hardware capability probe reports them.
