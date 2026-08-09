# Repository agent guide

This file applies to the entire repository. A more deeply nested `AGENTS.md`
may add or override instructions for its subtree. Direct user instructions take
precedence over this file.

## Start here

This repository implements an arm64 Apple host for User-Mode Linux. The outer
project is C++20, assembly, CMake, and Python tooling with a plain-C public and
host/kernel ABI; the maintained Linux port is an authenticated upstream
release plus an ordered patch series.

Before changing code, read the documents that own the relevant truth:

1. `docs/REFACTOR_HANDOFF.md` — current accepted state, limitations, and external
   work that is not claimed.
2. `docs/testing.md` — canonical commands, prerequisites, and campaign scope.
3. `docs/architecture.md` — enforced dependency direction and ownership.
4. The relevant subsystem guide: `darwin/README.md`,
   `dependencies/README.md`, `kernel/README.md`, or `kernel/abi/README.md`.

`docs/REFACTORING_PLAN.md` is a historical proposed plan. Its old paths, counts,
open checklists, per-domain label proposal, and blanket implementation-fragment
language are not current requirements where the authoritative documents have
superseded them. `docs/REFACTOR_PROGRESS.md` is the compact current summary and
`docs/refactor-evidence.md` is the evidence index. Preserve explicitly dated
historical sections as history.

## Build model and prerequisites

- CMake 3.27 or newer with Ninja is the canonical first-party build system.
  CTest is the canonical test registry. Use checked-in `CMakePresets.json`;
  `CMakeUserPresets.json` is local and ignored.
- Linux keeps its Kbuild/GNU Make boundary. Third-party projects keep their
  verified upstream build systems.
- Apple builds require arm64 macOS, Xcode command-line tools and SDKs, Clang,
  CMake, Ninja, Python 3, GNU Make, Meson, `pkg-config`, `curl`, and `patch`.
  Dependency and kernel preparation have additional tools documented in their
  READMEs.
- Supported products are arm64 macOS, iOS device, and iOS Simulator. Deployment
  targets are owned by `cmake/toolchains/Apple.cmake`.
- There is no repository-wide formatter. Follow adjacent formatting, retain
  SPDX headers, keep C++20 code warning-clean under `-Wall -Wextra -Werror`,
  and avoid unrelated formatting churn.

## Canonical validation

Use the narrowest relevant target or exact test first, then run the complete
affected workflow before claiming completion. For a focused CTest, prefer an
anchored expression:

```sh
ctest --test-dir .build/presets/macos-unit-16k \
  -R '^exact-test-name$' --output-on-failure
```

Ordinary 16 KiB and 4 KiB host workflows:

```sh
cmake --workflow --preset check-unit
cmake --workflow --preset check-unit-4k
```

GPU/VNC and loader workflows:

```sh
cmake --workflow --preset check-gpu
cmake --workflow --preset check-gpu-asan-ubsan
cmake --workflow --preset check-loader-asan-ubsan
```

Sanitizer and threading gates:

```sh
cmake --preset macos-unit-16k-asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset unit-asan-ubsan -j 10 \
  --resource-spec-file "$PWD/cmake/ctest-resources.json"

cmake --preset macos-unit-16k-tsan
cmake --build --preset tsan
ctest --preset unit-tsan
```

Apple libraries and package:

```sh
cmake --preset ios-device
cmake --build --preset ios-device
cmake --preset ios-simulator
cmake --build --preset ios-simulator
scripts/build-xcframework.sh
```

`architecture-audit` is part of the supported build presets. It checks the
first-party source tree, CMake File API graph, test manifest, and advisory
source-size report. It does not prove performance, generated-code shape, a
kernel boot, or guest behavior.

Run both page profiles for page-size-sensitive CPU, memory, ABI, kernel, or
guest changes. The default guest page size is 16 KiB; 4 KiB is a supported
compatibility profile, not an optional approximation.

## Integration tests and unified EROFS rootfs

Integration requires a clean replayed Linux source on a case-sensitive
filesystem and the verified private dependency prefix:

```sh
cmake --workflow --preset check-integration
cmake --workflow --preset check-integration-4k
```

The checked presets pin `/Volumes/<case-sensitive-volume>/linux` and the matching
`/Volumes/<case-sensitive-volume>/build-16k` or `/Volumes/<case-sensitive-volume>/build-4k` kernel
directory. A non-canonical layout must override both `UML_KERNEL_SOURCE_DIR`
and `UML_KERNEL_BUILD_DIR` explicitly at configure time.

- `rootfs.erofs` is the ignored, unified guest image governed by
  `tools/rootfs/manifest.json`. Verify it with:

  ```sh
  python3 tools/rootfs/verify.py --image rootfs.erofs
  ```

- Ordinary guest runs must use `tools/rootfs/with-verified-rootfs.sh`, which
  verifies the pinned EROFS format and digest before and after execution and
  coordinates with provisioning. Map the shared root image with `ubd0c=` so
  UML does not take a conflicting host-file lock when guests run in parallel,
  and boot it as `root=/dev/ubda rootfstype=erofs`. EROFS itself supplies the
  read-only filesystem contract; do not add Unix-mode, `ubd0r`, or `ro`
  enforcement. The `c` flag disables UML's host-file lock; it does not make the
  raw `/dev/ubda` block device suitable as scratch space. Never direct a test
  that overwrites a block device at the root device, and keep the wrapper's
  pre/post digest verification around every campaign.
- Rootfs updates are whole-image operations: build and sign a new EROFS
  artifact, update its checked manifest and release statement, then install it
  with `tools/rootfs/provision.py --replace`. There is no in-place writable
  maintenance workflow.
- The image owns the enabled tty0 getty and its login policy. Systemd launchers
  may host-feed the tmpfs OverlayFS bootstrap, but must not synthesize or
  override the getty unit in the overlay. Before starting systemd, that
  bootstrap mounts separate tmpfs filesystems at `/var/lib/docker` and
  `/var/lib/containerd` so Docker storage and containerd state are not nested
  on the writable root overlay. `guest-overlay-systemd-smoke` builds its
  freestanding arm64/Linux hello-world payload from repository source, wraps it
  in a deterministic rootfs archive, imports it into the disposable Docker
  state, and runs it with `--pull=never --network none`; it must not depend on a
  registry or external network. KDE/VNC and WebGL launchers must reuse
  the image's non-root `uml` account; never synthesize `plasma` or another
  desktop account in the overlay.
- External-network, KDE/VNC, WebGL, full LTP, SMP, performance, and official
  A64 campaigns are opt-in. Do not silently add them to ordinary unit
  acceptance.
- Full LTP uses `/opt/ltp` from `rootfs.erofs`. Tests that require deliberately
  removed kernel configuration must be listed in `disabled-tests.tsv`; do not
  re-enable those flags merely to increase the LTP count. Each campaign must
  pass the selected `${UML_KERNEL_BUILD_DIR}/modules.builtin`; the runner
  release-checks and copies its complete contents into disposable OverlayFS
  state so built-in-driver discovery matches the embedded kernel. The host
  compresses that generated setup and sends it through independently
  acknowledged commands below 2 KiB; the final guest command executes and
  removes staging under `/run`, and console echo is restored on every path.
  With `CONFIG_MODULES=n`, that same disposable overlay supplies fail-closed
  `kmod`, `modprobe`, and `rmmod` command names: only exact build-proven
  built-ins may receive no-op load success, while removals, parameters,
  unknown names, and unsupported operations fail. Do not add kmod to the
  immutable image for this accommodation. The unified image also carries
  `e2fsprogs` (`mkfs.ext2`, `mkfs.ext3`, `mkfs.ext4`, `mke2fs`, `debugfs`, and
  `e4crypt`), the full iproute2 `ip` utility, `bc`, and `quota`; the fast
  integration smoke pins those userspace prerequisites.
- Benchmarks and supporting runtimes come from `/opt` in the same image.
  Geekbench runs its local payload with `--no-upload`, without an Internet
  preflight or upload permission gate.

## Dependency provenance

Production libraries may come only from the verified private static prefix,
normally `.build/dependencies/macos-arm64`. Never fall back to Homebrew or
other ambient package roots, `CMAKE_PREFIX_PATH`, or unrestricted
`pkg-config` discovery.

- `dependencies/prepare.sh` is the only dependency workflow allowed to access
  the network. Prefer `--no-fetch` or verification-only modes when inputs are
  already present.
- Build or verify with the procedures in `dependencies/README.md`. A typical
  fresh preparation is:

  ```sh
  dependencies/prepare.sh
  dependencies/build.sh
  ```

- Changes to `dependencies/manifest.tsv`, dependency patches, `prepare.sh`, or
  `build.sh` invalidate the prefix receipt. Rebuild the prefix and run the
  dependency tooling tests.
- Keep `dependencies/README.md` synchronized with source, version, patch,
  recipe, prefix-policy, or prerequisite changes.

## Linux patch-series policy

`linux/` is ignored and is a separately managed replay checkout. It is not the
outer repository's source of truth, and edits there must not be committed as
the final change.

- Linux must be materialized on a case-sensitive filesystem. The canonical
  source is the authenticated release metadata and ordered policy under
  `kernel/patches/`.
- Safe verification examples:

  ```sh
  kernel/prepare-source.sh --source /case-sensitive/path/linux --verify
  kernel/audit-delta.sh --source /case-sensitive/path/linux --tree HEAD
  kernel/verify-replay.sh --work-root /case-sensitive/path --skip-build
  ```

  Omit `--skip-build` for a fresh replay plus both production page-size builds.
- Update patch bytes, `kernel/patches/series/manifest.tsv`, stable patch IDs and
  hashes, expected final tree, delta ceilings, ownership/overlap/extraction
  metadata, and `kernel/README.md` together.
- Preserve real timestamps, commit IDs, compiler metadata, build IDs, and
  binary hashes as observations. Do not rewrite them to manufacture byte
  equality.
- `kernel/prepare-source.sh --recreate` is the only supported way to replace a
  clean stale managed replay branch. Never replace an arbitrary source tree.

## ABI and generated copies

There are two independent ABI domains:

- Installed public API: declarations under `darwin/api/include/uml/`, identity
  and history under `darwin/api/abi/`.
- Host/Linux bridge: canonical declarations, identity, and history under
  `kernel/abi/`.

`kernel/abi/sync.sh` is the exact generated-copy authority. Its host outputs
under `darwin/include/` are `uml_darwin_abi.h`,
`uml_darwin_abi_identity.h`, `uml_darwin_cpu.h`, `uml_darwin_host.h`,
`uml_darwin_memory.h`, `uml_darwin_network.h`, `uml_darwin_bridge.h`, and
`uml/tlb_sync_ranges.h`; corresponding replay headers are generated too. Do
not edit those copies directly.
`darwin/include/uml_darwin_guest.h`, `uml_darwin_static_mmu.h`, and
`uml_elf64.h` are authored host-only contracts, not sync outputs.

- Use `kernel/abi/sync.sh --verify` for ordinary verification.
- For an intentional declaration change, follow `kernel/abi/README.md`:
  update only the affected ABI domain's version/fingerprint/history, run the
  sync, and update the Linux topic only when the host/kernel bridge changed.
- A public-only API change does not alter the kernel ABI or patch series.
- Rebuild external C, C++, and Objective-C consumers after public API changes.

## Architecture and implementation rules

Preserve the enforced direction:

```text
app -> emulator / device
emulator -> runtime / device / api
runtime -> cpu / memory / device contracts / platform
cpu -> memory / platform
memory -> platform
device -> memory / platform
```

- No dependency cycles, engine-to-engine edges, CPU-to-device edges,
  device-to-runtime edges, or lower-layer includes of composition layers.
- Source manifests and targets belong to their owning directory.
  `darwin/CMakeLists.txt` composes subdirectories; it does not own leaf source
  lists. Add every new implementation source to its owning target.
- Keep test-only hooks in test/support variants and out of production objects.
- Do not introduce implementation-source includes (`.cpp`, `.cc`, `.cxx`,
  `.inc`, `_impl.hpp`, or `.ipp`) in C/C++. The five-file gadget composer
  exception described below is exact and mechanically audited. Assembly `.inc`
  contracts are separately checked.
- Outer host-project C implementation source is forbidden except the
  intentional external package consumer at `tests/consumer/apple/main.c`.
  Linux patch data naturally contains kernel C sources.
- Do not leave source-adjacent `__pycache__`, `.pyc`, temporary audit output,
  unowned sources, or empty first-party architecture directories.

Executable-memory policy:

- Interpreter and gadget engines remain data-only and sandbox-safe.
- Only `darwin/cpu/arm64/jit/jit_arena.cpp` owns executable allocation,
  RW-to-RX publication, JIT write protection, and instruction-cache
  publication.
- Direct native VM and ELF-loader paths are macOS-only. Do not weaken the iOS
  archive audit that rejects executable-memory primitives outside the JIT
  arena.

## Protected correctness and performance structures

Do not casually remove or reshape these accepted mechanisms:

- **G99:** ThinLTO/IPO on `uml_arm64_gadgets`.
- **G100:** exactly five audited decode-family `.ipp` files included only by
  `darwin/cpu/arm64/gadgets/program_builder.cpp`.
- **G121:** the `seq_cst` published per-thread `ActiveAccessSlot` lifecycle
  protocol in `darwin/memory/mmu/address_space.cpp`.
- **G123:** serialized Darwin `init_mm` synchronization and whole-16-KiB host
  slot repair in the maintained Linux series, mutation-pinned by
  `tests/tooling/kernel/test_darwin_init_mm_tlb_policy.py`. Do not restore the
  diagnostic pre-refactor rollback.

For hot-path changes, preserve guest-visible behavior, exception/fault
priority, retirement counts, memory ordering, engine defaults, HWCAP exposure,
assembly-visible layout, cache/link topology, and the single virtual call
outside engine inner loops.

Correctness tests alone do not prove unchanged performance. Use focused probes
and matched measurements appropriate to the affected path, then record
accepted and rejected trials, controls, and limitations in
`docs/HOT_PATH_EXPERIMENTS.md`. Full Geekbench is a separate manual campaign, not an
ordinary correctness gate. Keep `tests/perf/probes.tsv` exact when adding,
renaming, or removing a performance probe.

## Test taxonomy and manifests

- Every first-party CTest has exactly one primary suite: `unit`, `tooling`, or
  `integration`.
- Register tests only through `uml_add_unit_test`, `uml_add_tooling_test`,
  `uml_add_integration_test`, or `uml_add_guest_integration_test` from
  `cmake/UmlTesting.cmake`. Raw `add_test` is forbidden.
- Unit tests are native C++/assembly and must not require Python, shell, the
  CLI, a kernel, networking, or a rootfs. Tooling tests own repository/build
  contracts and may use CMake, shell, or Python. Guest/rootfs/GUI/network tests
  belong to integration.
- Keep `tests/test-manifest.tsv` exact for every registration, semantic owner,
  suite, and conditional disposition.
- Preserve `tests/pre-refactor-test-inventory.txt`. Record renames and removals
  in `tests/test-migration-ledger.tsv`; never silently drop baseline coverage.
- Secondary domain labels and extra domain subdirectories are optional. The
  enforced taxonomy is suite-level.
- Input materialization is off by default and costs roughly 6.6 GiB per full
  profile. Enable `UML_INSTRUCTION_INPUT_MATERIALIZATION` only for the explicit
  evidence campaign. The official A64 semantics campaign can run for days and
  is outside ordinary CTest.
- Historical 983-entry evidence is not exhaustive variant correctness. Follow
  the current completeness boundary in `docs/A64_INSTRUCTION_PROGRESS.md`.

## Source-size policy

The 800-line production, 600-line test/support, and 300-line CMake guidelines
are advisory. A threshold crossing does not fail acceptance and is not by
itself a reason to split cohesive generated-like, ordering-sensitive, or
hot-path code. Optional rationale lives in `docs/source-size-reviews.tsv`;
stale, duplicate, or malformed rows fail the audit because they make the report
misleading.

## Documentation is part of every change

Keep project documentation under `docs/`. The repository root is reserved for
`AGENTS.md`, its one-line `CLAUDE.md` link, and an optional `README.md`.

Keep documentation and manifests synchronized in the same change whenever
their truth changes:

- architecture, ownership, dependency direction, executable-memory policy, or
  test taxonomy -> `docs/architecture.md` and the relevant module README;
- presets, commands, options, prerequisites, suite selection, rootfs, guest,
  sanitizer, package, or external-campaign behavior -> `docs/testing.md`;
- Darwin layout or runtime contracts -> `darwin/README.md`;
- dependency sources, versions, patches, recipes, prefix policy, or tools ->
  `dependencies/README.md`;
- Linux release, replay, topics, delta, toolchain, or build policy ->
  `kernel/README.md`;
- public/kernel ABI -> `kernel/abi/README.md`, identities, histories, generated
  copies, and consumers as applicable;
- test registration or ownership -> `tests/test-manifest.tsv`, plus the
  migration ledger for renames/removals;
- instruction evidence or completeness -> `docs/A64_INSTRUCTION_PROGRESS.md`;
- performance mechanism or measurement -> `docs/HOT_PATH_EXPERIMENTS.md`;
- LTP policy or result -> `docs/LTP_PROGRESS.md`;
- accepted baseline, acceptance evidence, or limitations -> current sections
  of `docs/REFACTOR_HANDOFF.md`, `docs/REFACTOR_PROGRESS.md`, and
  `docs/refactor-evidence.md`.

Do not rewrite dated historical evidence as though it were current. Record only
checks actually run, distinguish a tested implementation commit from a later
documentation-only commit, and state skipped or unavailable campaigns plainly.

Update this `AGENTS.md` whenever repository layout, supported commands,
invariants, generated-file ownership, or documentation responsibilities
change. `CLAUDE.md` must remain exactly one line containing `@AGENTS.md`.

## Commit and workspace hygiene

A completed feature or bug fix must end in a green, bisectable local commit.
Once the relevant focused checks and the complete affected workflow pass,
stage and commit the coherent change without waiting for a separate
request. This is standing authorization for local commits at verified working
states; it is not authorization to push or rewrite history.

- Inspect `git status --short` before and after work. Preserve unrelated user
  changes and do not stage them.
- Treat each commit as an independently working checkpoint. Do not commit
  temporary diagnostics, incomplete implementations, regressions introduced by
  the change, or an emulator state that fails its applicable validation.
- Keep logically distinct changes in separate commits, but include the tests,
  documentation, manifests, ABI identities, and generated copies required by
  the implementation in the same commit that changes their truth.
- When an external prerequisite or permission prevents the required validation,
  leave the change uncommitted and report the blocker.
- Do not commit `.build/`, `linux/`, rootfs images, PDFs, IDE state, caches, or
  generated build output.
- Do not delete or overwrite a rootfs, replay checkout, dependency prefix, or
  another developer's artifacts without explicit authorization.
- Keep pure moves, behavior changes, performance changes, and provenance or
  generated-file updates independently reviewable.
- Do not push, force-push, rebase, amend, or otherwise rewrite history unless
  the user requests it.
- Before handoff, report changed files, checks actually run, checks not run,
  and remaining external prerequisites.
