# Mainline Linux 6.18 source policy

This directory owns the maintained stable Linux 6.18 source identity, local
Darwin UML patch series, complete kernel configuration, standalone build
policy, and accepted artifact descriptors.

Exact upstream commit/tree, final tree, ABI identity, patch count, and delta
limits live in [`patches/metadata.env`](patches/metadata.env). Ordered patch
identity lives in [`patches/series/manifest.tsv`](patches/series/manifest.tsv).
Those machine-readable files are the source of truth; replay commit IDs and
binary hashes are observations.

## Source preparation and replay

Linux must be checked out on a case-sensitive filesystem. Copy
`source-location.conf.example` to the ignored `source-location.conf` and set
its single absolute `UML_KERNEL_SOURCE_DIR` value.

```sh
kernel/mainline-6.18/scripts/prepare-source.sh
kernel/mainline-6.18/scripts/prepare-source.sh --verify
kernel/mainline-6.18/scripts/verify-replay.sh \
  --work-root /case-sensitive/path
```

Preparation uses the pinned Git cache and creates the managed replay branch.
`--no-fetch` requires the exact commit to exist locally. `--recreate` may
replace only a clean stale managed replay; it never replaces an arbitrary
checkout.

`verify-replay.sh` starts from the pinned upstream commit, applies the complete
series, checks the final tree and topology policy, and by default builds both
page profiles. Use `--skip-build` only when source replay is the intended
scope. Kernel builds use all online processors unless `UML_KERNEL_JOBS` or
`--jobs` supplies a limit.

## Patch maintenance

Patch files below `patches/series` are generated. Amend, reorder, add, or drop
commits in the clean managed topic branch, update the reviewed final-tree
identity when source changes, then run:

```sh
kernel/mainline-6.18/scripts/rebase-patches.sh
```

The regenerator rebases onto the pinned stable commit and updates patch bytes,
manifest IDs, hashes, subjects, order, and topology policy. Never edit patch
files or their manifest manually. Update declared topic-path overlaps and delta
limits only when the resulting series requires it.

The series is maintained locally. Next-base tooling may recommend local carry,
drop, or rebase actions but has no submission or push workflow.

## Kernel configuration

`config/common.fragment` is the complete shipped policy. The build copies it
as the starting `.config`, applies exactly one page fragment, runs
`olddefconfig`, and verifies that every requested symbol resolved as declared.

The profiles are:

- 16 KiB pages from `kernel/common/config/page-16k.fragment` (default); and
- 4 KiB pages from `config/page-4k.fragment` (compatibility).

Both use a 128 KiB virtually mapped task stack, a 128 TiB software-MMU task
range, a relocatable ET_DYN image, 100 Hz plus `NO_HZ_IDLE`, built-in-only
drivers, and the same host/kernel ABI. There is no platform-specific image or
address-space configuration axis.

The native mapping window contains guest RAM plus an auxiliary address-space
allowance equal to RAM, bounded from 1 GiB to 4 GiB. Vmapped task stacks and
other kernel virtual mappings use that allowance; their physical pages still
come from the guest's `mem=` allocation.

Both page profiles request the runtime's anonymous physical-memory token. The
kernel's linear physical window becomes the RAM root, so MMU and host-service
consumers do not retain a second full-span mapping. Physical iOS wires the
guest-allocatable ranges before use; macOS and Simulator allocate on demand.
Free-page reporting remains available to a file-backed provider and disables
itself after the anonymous provider rejects range punching.

Darwin's cooperative scheduler services timer deadlines at safe kernel spin,
retry, TLB-walk, and `cond_resched()` boundaries. The Darwin network receive
path uses NAPI, and the display-mapped absolute pointer is a direct touchscreen.
Direct UBD images advertise discard ranges up to 128 MiB and service each range
with one host hole punch. The project adds no behavior to upstream UML's COW
implementation, and Apple launchers never construct a COW image pair.

Lifecycle quiesce uses full Linux system suspend. The management console
acknowledges only after the suspend entry boundary and remains the explicit
wake source, so monotonic guest uptime excludes time while an Apple host has
frozen the application. Multi-vCPU suspend parks secondary carriers through
CPU hotplug and restores them before guest tasks resume.

Loadable modules remain disabled. The required guest filesystems, networking,
Virtio devices, cgroups, BPF interpreter, zram, zswap/zstd, and supported LSM
policy are built in. Hostfs is disabled. Deliberately unsupported kernel
features are reflected in the LTP disabled manifest rather than enabled solely
for test count.

Kernel IP autoconfiguration is disabled. Writable guest images start their own
DHCP client and DNS resolver after init begins; launchers do not need kernel
`ip=` configuration for those images.

The framebuffer console uses standard VT/fbcon on Virtio-GPU. Host logs use
`ttyS0`; four serial lines and the standard Virtio-console driver are built in.
The legacy UML stdio console, DRM log client, legacy BSD PTYs, and loadable
module path remain disabled.

Zswap is built in and enabled with zstd but allocates no pool until the guest
activates backing swap. Normal desktop launchers do not create backing swap and
must not count zswap as guest memory.

## Standalone build and publication

Build each profile outside the source tree:

```sh
kernel/mainline-6.18/build/build-kernel.sh \
  --build /case-sensitive/build-16k --page-size 16k
kernel/mainline-6.18/build/build-kernel.sh \
  --build /case-sensitive/build-4k --page-size 4k
```

Publish each completed build with
`build/publish-artifact.py`. The publisher requires a clean replay at the
tracked final tree, validates resolved policy and ELF/ABI geometry, and copies
only the compact runtime/support bundle. It never builds, fetches, or changes a
source checkout. Use the script's `--help` for output and release options.

`artifacts/accepted-16k.env` and `accepted-4k.env` select reviewed local
publications below `.build/kernel/artifacts`. Apple products validate the whole
selected bundle and derive paths from its manifest; they do not inspect source
or Kbuild state.

## Toolchain

`build/toolchain/darwin.env` owns portable compiler/linker defaults and the
project compatibility headers used by Darwin host tools. Linux 6.18 requires a
compiler with `__counted_by` type-expression support; the preflight checks this
before Kbuild. Select a compatible upstream LLVM toolchain explicitly when the
default Apple compiler lacks the feature.

`UML_KERNEL_TOOLCHAIN_ROOT` and the documented `UML_KERNEL_*` variables may
override compiler, host compiler, linker, binutils, make, cross prefix, and job
count. Companion LLVM tools must match the selected compiler family.

The canonical bridge lives under [`../common/abi`](../common/abi/). Ordinary
build and replay verification use `kernel/common/abi/sync.sh --verify`; only an
intentional ABI change regenerates copies and the affected kernel topic.
