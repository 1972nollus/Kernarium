# Repository agent guide

This file applies to the whole repository. A nested `AGENTS.md` may add or
override rules for its subtree. Direct user instructions take precedence.

## Start here

This project implements an arm64 Apple host for User-Mode Linux. The host is
C++20, assembly, CMake, and Python tooling with plain-C public and host/kernel
ABIs. Maintained kernels are pinned upstream Git revisions plus generated patch
series.

Before changing a subsystem, read:

- [`docs/README.md`](docs/README.md) for documentation ownership;
- [`docs/architecture.md`](docs/architecture.md) for dependency and runtime
  boundaries;
- [`docs/testing.md`](docs/testing.md) for supported validation; and
- the nearest subsystem README.

Inspect `git status --short` before editing. Preserve unrelated changes and
avoid unrelated formatting churn. There is no repository-wide formatter.

## Build and product boundaries

- CMake 3.27 or newer with Ninja is the first-party build system. Use the
  checked-in presets; `CMakeUserPresets.json` is local and ignored.
- CTest is the test registry. Register first-party tests through the helpers in
  `cmake/UmlTesting.cmake`, never raw `add_test`.
- Linux retains Kbuild/GNU Make. Third-party projects retain their verified
  upstream build systems.
- Supported Apple products are arm64 macOS, iOS device, and iOS Simulator.
- The default guest page profile is 16 KiB. The 4 KiB compatibility profile is
  also supported and must be tested when a change is page-size-sensitive.
- Apple builds consume accepted prebuilt kernel bundles. They must not locate a
  kernel checkout, invoke Kbuild, or publish a kernel artifact.
- Android 17 uses its own 16 KiB ACK kernel and rootfs release. It has no 4 KiB
  product and must not inherit Linux desktop rootfs discovery.

Keep C++20 code warning-clean under `-Wall -Wextra -Werror`, preserve SPDX
headers, and follow adjacent formatting.

## Architecture and source ownership

Preserve this dependency direction:

```text
app -> emulator / device
emulator -> runtime / device / api
runtime -> cpu / memory / device contracts / platform
cpu -> memory / platform
memory -> platform
device -> memory / platform
```

- Do not introduce dependency cycles, engine-to-engine edges, CPU-to-device
  edges, device-to-runtime edges, or lower-layer includes of composition
  layers.
- Source manifests and targets live in their owning directories.
  `darwin/CMakeLists.txt` composes subdirectories and does not own leaf source
  lists.
- Keep test-only hooks in test/support targets and out of production targets.
- Do not include C/C++ implementation files. The audited gadget composer is
  the sole `.ipp` exception; assembly `.inc` contracts are separate.
- Host-project C implementation is forbidden except the external package
  consumer at `tests/consumer/apple/main.c`.
- The gadget engine remains data-only. Executable allocation and
  publication belong only to `darwin/cpu/arm64/jit/jit_arena.cpp`.
- Keep generated ABI copies, kernel patches, and dependency patches under
  their documented generators. Never edit generated output directly.

The detailed runtime, executable-memory, scheduling, device, and cache
contracts live in [`docs/architecture.md`](docs/architecture.md) and subsystem
READMEs rather than in this agent guide.

## Dependencies and kernel sources

Production libraries come only from the verified private static prefixes.
Never fall back to Homebrew, ambient `CMAKE_PREFIX_PATH`, or unrestricted
`pkg-config` discovery.

- `dependencies/prepare.sh` is the only dependency workflow allowed to access
  the network. Prefer verification or `--no-fetch` when inputs already exist.
- Patch-managed external repositories use Git topic commits as their editable
  source. When a change belongs to an existing topic, amend it into that topic
  commit and regenerate the owning patch series. Create a new topic commit only
  for a distinct concern. Never edit generated patch files directly. Every
  series uses the `scripts/patch_series.py` format and is identified by the
  tree it produces from its pinned base, so a pure reorder or message edit that
  keeps that tree invalidates no build.
- Kernel source must be materialized on a case-sensitive filesystem. The
  ignored `kernel/mainline-6.18/source-location.conf` selects the managed
  checkout for standalone kernel work.
- `linux/` is an ignored replay checkout, not outer-repository source. Commit
  kernel changes through the generated series under the owning versioned
  kernel directory.
- Treat every kernel source, patch, and configuration change as shared across
  all maintained kernel lines and every supported configuration or page
  profile. Put a topic in the shared `kernel/common/patches` series, refresh
  every line that applies it, and cover both 16 KiB and 4 KiB profiles where
  those products exist, in the same change. Keep a topic in one line's overlay
  only when the behavior genuinely depends on the guest OS or the target
  product does not exist, and record that reason in the owning versioned
  README. A topic whose context differs between bases gets an adapted variant
  in each overlay, not an exemption.
- Use `prepare-source.sh --recreate` only for a clean stale managed replay.
  Never replace an arbitrary source tree.
- Maintained kernel changes are local to this project; tooling must not suggest
  submitting, uploading, or pushing them.

Let Git, compilers, linkers, Kbuild, and Xcode retain authentic timestamps,
IDs, tool identities, and output hashes. Do not add reproducibility flags whose
only purpose is normalizing incidental metadata. Generated patch series are the
one exception: `scripts/patch_series.py` omits replay commit IDs and patch
ordinals, which describe a replay rather than the change, so a patch file
changes only when its own topic does.

## ABI changes

The installed public API and the host/Linux bridge are independent domains.
Follow [`kernel/common/abi/README.md`](kernel/common/abi/README.md) and update
only the affected domain's `identity.env`.

- Canonical public declarations live under `darwin/api/include/uml/`.
- Canonical bridge declarations live under `kernel/common/abi/`.
- Use `kernel/common/abi/sync.sh --verify` for ordinary verification.
- Rebuild C, C++, and Objective-C consumers after public API changes.

## Validation

Run the narrowest relevant test first, then the single canonical workflow that
owns the changed behavior. Do not repeat equivalent matrices merely to create
more evidence.

Common workflows are:

```sh
cmake --workflow --preset check-unit
cmake --workflow --preset check-unit-4k
cmake --workflow --preset check-integration
cmake --workflow --preset check-integration-4k
cmake --workflow --preset check-gpu
cmake --workflow --preset check-loader-asan-ubsan
```

Use an anchored expression for a focused CTest. Run both page profiles for
page-size-sensitive CPU, memory, ABI, kernel, or guest changes. GPU/VNC,
sanitizers, Apple packaging, Android, LTP, performance, external-network, and
official A64 campaigns are opt-in and are required only when the change crosses
their boundary. See [`docs/testing.md`](docs/testing.md).

Treat every guest illegal-instruction failure as a correctness blocker. Do not
skip, exclude, or route around a workload that encounters `SIGILL`; diagnose
and implement the missing instruction semantics before continuing the campaign.

Every CTest has exactly one primary suite: `unit`, `tooling`, or `integration`.

Every wait on a long-running run, agent, or external service has a hard time
bound: `timeout` on the process, or a deadline inside the polling loop, chosen
from the expected duration. Never wait solely for a marker line, file, or
process exit, because a wrong or missing signal then waits forever. A report
command run after a wait must not read stdin (give `awk`, `grep`, and similar a
real file or `</dev/null`), or an empty path variable blocks it on the harness
pipe.

## Documentation policy

Documentation records durable current contracts and concise historical
outcomes. Git history is the worklog.

- Do not add investigation diaries, handoff files, progress logs, temporary
  failure analysis, machine-specific snapshots, or evidence indexes.
- Remove temporary-state problems and their analysis once the durable result
  is understood. Document only a continuing limitation or contract.
- Explain decisions with a short outcome and its consequences. Do not preserve
  exhaustive comparisons of rejected alternatives.
- Keep code and command examples to the smallest canonical entry point needed
  for quick use. Put detailed mechanics in scripts, `--help`, tests, or code.
- Describe what a component guarantees, not the step-by-step history of how it
  was implemented.
- Keep attempted performance work only in
  [`docs/optimization-history.md`](docs/optimization-history.md). Each entry is
  a brief hypothesis, disposition, and durable lesson; omit raw logs,
  temporary paths, build IDs, and full measurement tables.
- Keep `AGENTS.md` limited to reusable repository-wide instructions. Put
  product and subsystem contracts in their owning documentation, not incident
  narratives here.
- Prefer one owning document and links from other locations. Delete superseded
  documents instead of retaining duplicate status summaries.
- Keep project documentation under `docs/` or beside the subsystem it owns.
  The repository root is reserved for `AGENTS.md`, the one-line `CLAUDE.md`
  compatibility link, and an optional human-facing `README.md`.

Update documentation in the same change whenever its current contract changes.
`CLAUDE.md` must remain exactly one line containing `@AGENTS.md`.

## Workspace and commit hygiene

- Do not commit build trees, kernel replay checkouts, rootfs images, caches,
  IDE state, temporary reports, or generated runtime output.
- After validating a replacement Android release, remove superseded Android
  build, release, transfer, and Simulator-bundle artifacts from the local
  `.build` tree, local temporary directories, the Android build machine, and
  local iOS Simulators. Resolve releases and bundles from their manifests;
  resolve build outputs from their recorded invocation and canonical output
  root; and delete only explicit Android paths. Preserve the current accepted
  release and its active writable bundle, Android source and patch-producer
  checkouts, accepted kernel inputs, dependency prefixes, non-Android guests, and
  unrelated Simulator Documents siblings.
- Do not delete or overwrite a rootfs, dependency prefix, replay checkout, or
  another developer's artifacts without explicit authorization.
- Remove a worktree and delete its branch as soon as their work is merged,
  cherry-picked, or superseded; keep only trees with live work, unmerged
  commits, or a canonical replay that other trees reference. Before removing
  one, confirm no process uses it, commit or archive any uncommitted change
  worth keeping, and repoint scripts or replay references that name it.
- Keep pure moves, behavior changes, performance changes, and generated or
  provenance updates independently reviewable when practical.
- Commit with the repository's configured Git identity. Never put a custom
  author or committer name or email on a commit, whether through `--author`,
  `-c user.email`, or `GIT_AUTHOR_EMAIL`/`GIT_COMMITTER_EMAIL`; the user's
  contact address from a session context is not a Git identity.
- A completed feature or fix ends in a green, bisectable local commit after
  applicable validation. In this outer project repository, this authorizes new
  local commits but not amending, rebasing, pushing, force-pushing, or otherwise
  rewriting its history. The amend restriction does not apply to patch-managed
  external repositories governed by the same-topic rule above.
- If a required external input or permission prevents validation, leave the
  change uncommitted and report the prerequisite.
- Before finishing, report changed files, checks run, checks not run, and any
  remaining external prerequisites.
