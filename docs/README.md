# Documentation map

Documentation is organized by stable ownership rather than by project phase or
investigation date.

| Topic | Owner |
| --- | --- |
| Architecture, dependency direction, runtime boundaries | [`architecture.md`](architecture.md) |
| Build, test, guest, sanitizer, and package entry points | [`testing.md`](testing.md) |
| Apple graphics support and limits | [`graphics.md`](graphics.md) |
| Virtio video decoding, encoding and the private wire contract | [`../darwin/device/video/README.md`](../darwin/device/video/README.md) and [`../kernel/common/video/README.md`](../kernel/common/video/README.md) |
| Platform hardware video encoder: discovery, cache, sessions, recovery | [`../darwin/platform/README.md`](../darwin/platform/README.md) |
| Guest VA-API backend over the video decoder | [`../tools/video/va-v4l2/README.md`](../tools/video/va-v4l2/README.md) |
| Attempted performance work | [`optimization-history.md`](optimization-history.md) |
| Performance tools, probes, and profilers | [`../tests/perf/README.md`](../tests/perf/README.md) |
| A64 instruction coverage | [`a64-instruction-coverage.md`](a64-instruction-coverage.md) and `tests/unit/arm64/*.tsv` |
| A64 instruction corpus | [`../tests/unit/arm64/corpus/README.md`](../tests/unit/arm64/corpus/README.md) |
| A32 instruction coverage | [`a32-instruction-coverage.md`](a32-instruction-coverage.md) and `tests/unit/arm64/a32_instruction_inventory.tsv` |
| AArch32 decoder and encoding table | [`../darwin/cpu/arm64/aarch32/README.md`](../darwin/cpu/arm64/aarch32/README.md) |
| AArch32 instruction corpus and its oracle | [`../tests/unit/arm64/aarch32_corpus/README.md`](../tests/unit/arm64/aarch32_corpus/README.md) |
| Darwin runtime details | [`../darwin/README.md`](../darwin/README.md) |
| Kernarium iOS application | [`../Kernarium/README.md`](../Kernarium/README.md) |
| arm64 gadget engine internals | [`../darwin/cpu/arm64/gadgets/README.md`](../darwin/cpu/arm64/gadgets/README.md) |
| arm64 JIT engine internals | [`../darwin/cpu/arm64/jit/README.md`](../darwin/cpu/arm64/jit/README.md) |
| x86-64 gadget engine contracts | [`../darwin/cpu/x86_64/gadgets/README.md`](../darwin/cpu/x86_64/gadgets/README.md) |
| macOS-only x86-64 JIT contracts | [`../darwin/cpu/x86_64/jit/README.md`](../darwin/cpu/x86_64/jit/README.md) |
| External dependencies | [`../dependencies/README.md`](../dependencies/README.md) |
| Kernel families | [`../kernel/README.md`](../kernel/README.md) |
| Shared kernel contracts | [`../kernel/common/README.md`](../kernel/common/README.md) |
| Mainline kernel policy, including the Android kernel product | [`../kernel/mainline-6.18/README.md`](../kernel/mainline-6.18/README.md) |
| LineageOS 24 (Android 17) producer and guest image | [`../android/lineage-24/README.md`](../android/lineage-24/README.md) |
| Linux guest images and the boot ramdisk | the README beside each producer under [`../tools/rootfs/`](../tools/rootfs/); the Vulkan guest image and its receipt: [`../tools/rootfs/vulkan/README.md`](../tools/rootfs/vulkan/README.md) |
| Public and kernel ABI maintenance | [`../kernel/common/abi/README.md`](../kernel/common/abi/README.md) |

`scripts/` holds build, packaging and patch-series infrastructure that CMake
and other tools call. `tools/` holds developer-facing launchers, producers and
generators, together with what each one owns: the guest sources, patch series
and boot files a producer builds from, and the runner for a corpus a generator
writes. CMake and CTest may read or run those files in place.

Short accepted decisions remain under [`decisions/`](decisions/). Machine-
readable manifests, identities, and test inventories are the source of truth
for exact lists and counts.

The repository intentionally has no handoff, progress, evidence-index, or
dated investigation documents. Durable results belong in the owner above;
implementation history belongs in Git.
