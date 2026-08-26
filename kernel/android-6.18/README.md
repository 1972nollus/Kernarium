# Android Common Kernel 6.18 source policy

This line is the ACK 6.18 kernel for the AOSP 17 guest. Exact upstream and
final-tree identities, ABI, patch count, and delta limits live in
[`patches/metadata.env`](patches/metadata.env); ordered generated patch identity
lives in [`patches/series/manifest.tsv`](patches/series/manifest.tsv).

It is independent of both mainline page profiles. Mainline patch bytes are not
this line's source of truth, even when a topic implements the same Darwin
contract.

## Product contract

The line has one 16 KiB ARM64 configuration in
[`config/android.fragment`](config/android.fragment). It is built-in-only:
`CONFIG_MODULES=n`, no `.ko` or DLKM payloads, and no 4 KiB variant.

The configuration includes the Android Binder, SELinux/audit, cgroups, storage
and metadata-encryption stack, DMA-BUF heaps, networking, BPF interpreter,
Virtio devices, and Darwin UML scheduling support required by the product.
The native arm64 eBPF JIT is not exposed because its execution contracts do not
match UML; the AOSP product uses the generic BPF interpreter.

The local series provides the durable Android-specific behavior:

- cooperative timer, reschedule, IPI, irq-work, and RCU progress on persistent
  Darwin vCPU carriers;
- direct touchscreen classification compatible with Android input;
- NAPI receive processing for sustained user networking;
- free-page reporting for file-backed guest RAM where supported;
- virtually mapped 128 KiB task stacks and the common RAM-scaled auxiliary
  address window;
- correct COW-backed UBD request advancement;
- validated initramfs ranges inside the static GPT; and
- anonymous host VM for Android physical memory, with page reporting disabled
  when no file extent exists.

Physical iOS wires the complete anonymous RAM root before guest start. macOS
and Simulator retain demand allocation. Normal image descriptors remain
file-backed.

## Build and artifact boundary

The standalone ACK replay and build are owned by
[`../../android/aosp-17`](../../android/aosp-17/README.md) and run on the Linux
builder. That producer verifies the generated series, configuration, ELF, and
exact bridge imports.

`android/aosp-17/tools/publish-kernel-artifact.py` publishes an already-built
clean ACK result into the common compact artifact format.
[`artifacts/accepted-16k.env`](artifacts/accepted-16k.env) selects the reviewed
local bundle below `.build/kernel/artifacts`.

Kernel publication is independent of AOSP/rootfs publication. Advancing the
accepted ACK artifact must not rebuild or rename the rootfs release, recreate a
`.kern` bundle, or invalidate its persistent COW.

## Patch maintenance

Patch files are generated from the clean ACK topic history. Change commits in
Git and run the regenerator documented by the AOSP producer; never edit patch
bytes, metadata, or manifest rows independently. A changed final tree requires
a fresh replay, build, artifact verification, and the affected Android runtime
qualification before acceptance.
