# Documentation map

Documentation is organized by stable ownership rather than by project phase or
investigation date.

| Topic | Owner |
| --- | --- |
| Architecture, dependency direction, runtime boundaries | [`architecture.md`](architecture.md) |
| Build, test, guest, sanitizer, and package entry points | [`testing.md`](testing.md) |
| Apple graphics support and limits | [`graphics.md`](graphics.md) |
| Virtio video decoding and private wire contract | [`../darwin/device/video/README.md`](../darwin/device/video/README.md) and [`../kernel/common/video/README.md`](../kernel/common/video/README.md) |
| Attempted performance work | [`optimization-history.md`](optimization-history.md) |
| A64 instruction coverage | [`a64-instruction-coverage.md`](a64-instruction-coverage.md) and `tests/unit/arm64/*.tsv` |
| A64 instruction corpus | [`../tests/unit/arm64/corpus/README.md`](../tests/unit/arm64/corpus/README.md) |
| Darwin runtime details | [`../darwin/README.md`](../darwin/README.md) |
| Kernarium iOS application | [`../Kernarium/README.md`](../Kernarium/README.md) |
| x86-64 gadget engine contracts | [`../darwin/cpu/x86_64/gadgets/README.md`](../darwin/cpu/x86_64/gadgets/README.md) |
| External dependencies | [`../dependencies/README.md`](../dependencies/README.md) |
| Kernel families | [`../kernel/README.md`](../kernel/README.md) |
| Mainline kernel policy | [`../kernel/mainline-6.18/README.md`](../kernel/mainline-6.18/README.md) |
| Android ACK policy | [`../kernel/android-6.18/README.md`](../kernel/android-6.18/README.md) |
| AOSP 17 producer and guest image | [`../android/aosp-17/README.md`](../android/aosp-17/README.md) |
| Public and kernel ABI maintenance | [`../kernel/common/abi/README.md`](../kernel/common/abi/README.md) |

Short accepted decisions remain under [`decisions/`](decisions/). Machine-
readable manifests, identities, and test inventories are the source of truth
for exact lists and counts.

The repository intentionally has no handoff, progress, evidence-index, or
dated investigation documents. Durable results belong in the owner above;
implementation history belongs in Git.
