# Video decoder fixtures

These 128 × 96 synthetic color-pattern frames cover H.264 Baseline,
Constrained Baseline, Main, High and High10; HEVC Main and Main10; and baseline
4:2:0 JPEG. Each `.bin` contains one Annex B access unit or JPEG image. Its
`.raw` reference is tightly packed NV12 or P010, with no row padding. The
patterns contain no third-party imagery.

The platform test decodes every profile advertised by the host and compares
all output samples with the software-decoded reference. Hardware rounding may
differ by two sample levels (four for JPEG). H.264 High10 is exercised only on
hosts where the existing hardware capability probe supports it.
