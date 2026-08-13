# Linux source policy

Linux is built from the official `linux-6.18.41.tar.xz` release package and
detached signature recorded in
[`patches/metadata.env`](patches/metadata.env). Preparation downloads both only
from kernel.org over HTTPS, verifies their pinned SHA-256 identities, verifies
the uncompressed tar stream with the checked-in release-signing key and exact
signer fingerprint, and then checks the extracted packaged Git tree. GnuPG,
`xz`, `tar`, Git, and `shasum` are source-preparation tools, not linked runtime
dependencies.

The official release package intentionally export-ignores
`Documentation/.renames.txt`. Its packaged tree therefore differs from the
upstream `v6.18.41` tag tree by exactly that documentation-only path. The tag
commit and tree remain provenance observations; the authenticated packaged
tree is the build input and the omitted file is not reconstructed from Git.

## Latest verified replay (2026-08-14)

The manifest contains 18 ordered topics and replays to canonical tree
`39cf75ce83c5396d9f1540bf37c6a16a6c78d1cd`. A fresh authenticated replay,
stable-ID/hash verification, topic ordering, declared-overlap policy, ABI
checks, and delta audit passed. The exact delta policy is 157 changed files
with 6 outside the Darwin port. Topic 0015 replaces the former macOS/iOS split
with one scalable Darwin address mechanism, including the page-rounded
one-eighth auxiliary allowance, 32 MiB floor, iOS low placement, and macOS high
placement. Topic 0016 replaces per-task Darwin
pthreads with one persistent carrier per configured vCPU on macOS, iOS
Simulator, and physical iOS. Carrier names use a non-variadic decimal formatter
because Linux AAPCS64 and Apple arm64 variadic call conventions differ. Topic
0017 lets the host-stop descriptor wake those carriers for cooperative
shutdown. Topic 0018 services pending cross-CPU work at IRQ-enabled spin
checkpoints, preventing two fixed carriers from deadlocking in synchronous
cross-calls while preserving Linux interrupt masking.

The host/kernel bridge is ABI v8 with fingerprint `0x3871ad612337ea76`; the
separate installed emulator API remains v4 with fingerprint
`0xd9a69804d3303044`. These are dated accepted identities, while the manifest,
metadata, ABI checks, and replay commands below remain the durable enforcement
mechanisms.

The ordered project patch files and
their SHA-256/stable patch IDs are in
[`patches/series/manifest.tsv`](patches/series/manifest.tsv). The expected
final tree is checked after every replay. The shared core delta is separated
into MMU/large-page, SMP/timer/interrupt, execution/capability, and build-rule
topics so each can be reviewed and rebased independently. Final-state paths
normally belong to one topic. Intentional multi-topic paths are declared with
their exact ordered owners and rationale in
[`patches/topic-path-overlaps.tsv`](patches/topic-path-overlaps.tsv). Darwin
vhost-user transport changes remain in topic 0010 while the generic 64-bit
DMA-mask correction remains independently reviewable and droppable as topic
0014. Topic 0015 changes shared layout consumers owned by earlier topics; its
exact overlaps are declared, including its update to the arm64 profile
constants in `arch/arm64/Makefile.um` and removes the retired platform selector.
The manifest verifier rejects undeclared, stale, or mismatched overlaps.

The signed archive, packaged/final trees, ordered stable patch IDs, and patch
hashes are the reproducible identities. The local archive-import and replay
commit IDs are observations: their real creation timestamps are retained and
may legitimately differ between preparations. Kernel binary hashes, build
identity, and timestamps are observations for the same reason. Configure and
build both run the offline clean-tree verifier and delta audit, so a merely
plausible Makefile, branch name, or locally cloned upstream tree is never
accepted.

Linux must live on a case-sensitive filesystem. A normal macOS checkout cannot
represent several upstream paths that differ only by case. The canonical
checkout is the dedicated APFS case-sensitive volume mounted at
`/Volumes/<case-sensitive-volume>`; its production page-profile builds remain in
`build-16k` and `build-4k` on that volume. The default macOS integration
product and both iOS products consume the same `build-16k/vmlinux`; there is no
platform-specific kernel build. Transient case-sensitive mounts under `/tmp`
are not used for the canonical source or build trees:

```sh
kernel/prepare-source.sh --source /Volumes/<case-sensitive-volume>/linux
kernel/prepare-source.sh --source /Volumes/<case-sensitive-volume>/linux --verify
kernel/prepare-source.sh --source /Volumes/<case-sensitive-volume>/linux \
  --cache /path/to/kernel-cache --no-fetch
kernel/verify-replay.sh --work-root /Volumes/<case-sensitive-volume>
```

The first preparation populates `.build/kernel-cache` unless another cache is
selected. `--no-fetch` requires both authenticated release files to be present
there; ordinary `--verify` never reads the cache or network. The last command
starts from a fresh signed release package and builds both 16 KiB and 4 KiB
configurations out of tree. Kernel builds default to every online host
processor. Resource-constrained hosts may override `UML_KERNEL_JOBS` or pass
`--jobs N` to set a smaller explicit limit. `--recreate` is the only mode allowed to replace a
clean stale managed replay branch, and it never replaces the source directory
itself.

## Kernel configuration

One kernel configuration ships. `kernel/config/common.fragment` is the entire
policy; `kernel/build-kernel.sh` copies it verbatim as the starting `.config`,
applies exactly one page-size fragment through `scripts/config`, and then runs
`olddefconfig` so every unlisted symbol takes its Kconfig default.

```text
kernel/config/common.fragment    the whole shipped policy
kernel/config/page-16k.fragment  guest page size, default
kernel/config/page-4k.fragment   guest page size, compatibility profile
```

Guest page size is the only kernel policy axis that varies, because a single
`.config` cannot hold both values. There is no optional Kconfig policy layer
or address-space selector: a symbol absent from `common.fragment` is absent
from the product. Every Darwin kernel exposes the maximum three-level 16 KiB
software-MMU task range of 128 TiB and treats the native kernel mapping as a
hole inside that range. The ET_DYN kernel is linked at 48 GiB. Device and
Simulator bias it so the active native window ends at the physical-iOS 62 GiB
ceiling; macOS biases the same image to a 96 TiB base. The window contains
exactly the RAM-sized direct map plus a
page-rounded one eighth for auxiliary kernel mappings, with a 32 MiB floor.
Kernarium's 1 GiB guest therefore receives exactly 128 MiB of auxiliary address
capacity and uses the 60.875--62 GiB active window. The standalone macOS app
defaults to 8 GiB and 10 vCPUs; its 1 GiB auxiliary allowance gives it a 9 GiB
active window beginning at 96 TiB. The kernel derives the hole endpoint from
its relocated image, so macOS uses this same exact 16 KiB kernel artifact while
retaining its established unsigned loader rather than the iOS signed carrier:

```sh
kernel/build-kernel.sh \
  --source /Volumes/<case-sensitive-volume>/linux \
  --build /Volumes/<case-sensitive-volume>/build-16k \
  --page-size 16k
```

The common policy builds zswap in, enables it by default, and selects zstd. It
retains zram with zstd as a separately activatable compressed swap device.
Zswap is a cache in front of backing swap rather than backing by itself; it
allocates no pool when the guest has no active swap device or file. Normal KDE
launchers, including Kernarium, deliberately activate none.

Every shipped profile selects a 100 Hz timer tick and `NO_HZ_IDLE`. This keeps
the fixed vCPU carriers responsive while avoiding a 1 kHz compulsory wakeup on
every CPU and suppressing the periodic tick entirely while a guest CPU is
idle. Periodic ticks, `NO_HZ_FULL`, and the 250/300/1000 Hz choices are
explicitly disabled in the canonical fragment.

The page fragments also derive one 128 KiB Linux task-stack byte size: order 3
for 16 KiB pages and order 5 for 4 KiB pages. Fixed carriers execute stackful
task contexts there, but sleeping tasks must not each pin the former 512 KiB
16 KiB-profile allocation. The build helper accepts only unsigned numeric
values for this page-derived setting; arbitrary string policy remains outside
the page fragments.

Because UML deliberately ships without loadable-module support, every
capability a guest distribution can use has to be built in there, including
the control groups systemd requires to start at all. Per-file filesystem
encryption is likewise built in through `CONFIG_FS_ENCRYPTION=y`; the LTP
`statx05` boundary validates encrypted and plain ext4 attribute reporting.

Three constraints bound what may be added. Loadable modules stay off because
module text would be dynamically published executable memory inside the host
process, requiring the same Apple dynamic-code entitlement as the JIT engine.
SELinux, Smack, and AppArmor are all `LSM_FLAG_EXCLUSIVE`, so exactly one can
be active; Smack is the validated choice and the recorded LTP results depend
on it. `CONFIG_HOSTFS` stays off because it lets the guest mount an arbitrary
host directory, which would move the reachable host filesystem from
emulator-side policy into guest control; host data is shared through an
explicit device or image instead.

`CONFIG_HUGETLBFS`, `CONFIG_TRANSPARENT_HUGEPAGE`, and `CONFIG_CAN` are also
deliberately absent from the shipped policy. LTP cases that require those
features are owned by `tests/integration/ltp/disabled-tests.tsv`; do not add a
kernel feature solely to make its packaged test runnable. Change the kernel
policy, LTP manifests, and `docs/LTP_PROGRESS.md` together if that product
decision is revisited.

## Toolchain

`kernel/toolchain/darwin.env` owns portable compiler/linker/tool defaults.
Darwin host tools resolve their unmodified `<elf.h>`, `<libelf.h>`, and
`<byteswap.h>` includes through the
project-owned compatibility headers under `kernel/toolchain/darwin-host`.
Their ELF ABI layouts come from the selected Linux source, and they do not
discover or link a system libelf. Darwin UML is a native arm64 Mach-O build,
so its cross-tool prefix is empty by default; set one only for a host that
genuinely requires it.
Linux 6.18 needs a compiler that supports `__counted_by` flexible arrays in
type expressions (recent upstream LLVM does; some Apple Clang releases do not).
The CMake kernel graph probes this capability during configuration. It prefers
an explicit `UML_KERNEL_CC`, then `UML_KERNEL_TOOLCHAIN_ROOT/bin/clang`, and
then the configured compiler or `clang` on `PATH`. It does not query a host
package manager or bake in an installation layout. `UML_KERNEL_HOST_CC` and
the individual tool overrides remain authoritative. Automatically selected
LLVM companion tools must live beside the selected compiler; `ld.lld` may
also come from `PATH` because distributions can package it separately. The
host compiler builds utilities and is checked for availability only; it need
not support the target compiler's Linux-specific language constructs. The
standalone build script repeats the target compiler check before Kbuild. Set
`UML_KERNEL_TOOLCHAIN_ROOT` or
`UML_KERNEL_{MAKE,CC,HOST_CC,LD,CROSS_COMPILE,AR,NM,OBJCOPY,OBJDUMP,READELF,
STRIP,LLVM,JOBS}` for a non-default host setup.
When `JOBS` is omitted, the build helper uses the online processor count
reported by `getconf`, with Darwin `sysctl` as a fallback.
When LLVM binutils live beside the selected `clang`, the build helper and the
macOS CMake target select that matching family automatically; explicit values
remain authoritative.

[`abi/`](abi/) is the one plain-C host/kernel ABI source. Run
`kernel/abi/sync.sh --kernel-source /path/to/linux` only when intentionally
updating generated copies; build and replay commands use `--verify` and fail
if either side is stale.
