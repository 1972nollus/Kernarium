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

The ordered project patch files and
their SHA-256/stable patch IDs are in
[`patches/series/manifest.tsv`](patches/series/manifest.tsv). The expected
final tree is checked after every replay. The shared core delta is separated
into MMU/large-page, SMP/timer/interrupt, execution/capability, and build-rule
topics so each can be reviewed and rebased independently. Final-state paths
normally belong to one topic. The sole intentional multi-topic path is declared
with its exact ordered owners and rationale in
[`patches/topic-path-overlaps.tsv`](patches/topic-path-overlaps.tsv): Darwin
vhost-user transport changes remain in topic 0010 while the generic 64-bit
DMA-mask correction remains independently reviewable and droppable as topic
0014.
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
represent several upstream paths that differ only by case. A case-sensitive
APFS volume or sparse image is therefore required:

```sh
kernel/prepare-source.sh --source /Volumes/<case-sensitive-volume>/linux
kernel/prepare-source.sh --source /Volumes/<case-sensitive-volume>/linux --verify
kernel/prepare-source.sh --source /Volumes/<case-sensitive-volume>/linux \
  --cache /path/to/kernel-cache --no-fetch
kernel/verify-replay.sh --work-root /Volumes/<case-sensitive-volume> --jobs 2
```

The first preparation populates `.build/kernel-cache` unless another cache is
selected. `--no-fetch` requires both authenticated release files to be present
there; ordinary `--verify` never reads the cache or network. The last command
starts from a fresh signed release package and builds both 16 KiB and 4 KiB
configurations out of tree. Two kernel jobs is the conservative checked-in
default because parallel LLVM links can otherwise exceed the acceptance
host's memory; hosts with more headroom may override `UML_KERNEL_JOBS` or
`--jobs`. `--recreate` is the only mode allowed to replace a
clean stale managed replay branch, and it never replaces the source directory
itself.

`kernel/config/` owns page-size fragments. `kernel/toolchain/darwin.env` owns
portable compiler/linker/tool defaults. Darwin host tools get their ELF ABI
layouts from the selected Linux source through the project-owned compatibility
header under `kernel/toolchain/darwin-host`; they do not discover or link a
system libelf. Darwin UML is a native arm64 Mach-O build, so its cross-tool
prefix is empty by default; set one only for a host that genuinely requires it.
Linux 6.18 needs a compiler that supports `__counted_by` flexible arrays in
type expressions (recent upstream LLVM does; some Apple Clang releases do not).
The build script diagnoses that incompatibility before Kbuild. Set
`UML_KERNEL_{MAKE,CC,HOST_CC,LD,CROSS_COMPILE,AR,NM,OBJCOPY,OBJDUMP,READELF,
STRIP,LLVM,JOBS}` for a non-default host setup.
When LLVM binutils live beside the selected `clang`, the build helper and the
macOS CMake target select that matching family automatically; explicit values
remain authoritative.

[`abi/`](abi/) is the one plain-C host/kernel ABI source. Run
`kernel/abi/sync.sh --kernel-source /path/to/linux` only when intentionally
updating generated copies; build and replay commands use `--verify` and fail
if either side is stale.
