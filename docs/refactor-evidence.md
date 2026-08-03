# Refactor evidence

This index records the 2026-08-03 fourth independent audit and preserves
relevant dated observations from earlier campaigns. The repository is not
closed: the fourth audit found gaps in runtime isolation, instruction evidence,
packaging, loader safety, replay enforcement, and test ownership. The
authoritative checklist is in
[`../REFACTOR_HANDOFF.md`](../REFACTOR_HANDOFF.md); commands and external
prerequisites are in [`testing.md`](testing.md).

## Fourth-audit observations

| Observation | Result or evidence |
| --- | --- |
| Fresh unit-only configure and architecture audit | passed; 550 targets / 561 registrations in `../.build/audit-round4-unit-only` |
| Current unit/evidence-producer run | 562/562 passed in `../.build/audit-round3-finish-unit` |
| Standalone instruction validator | 1/1 passed after evidence regeneration; pass is unsound as described below |
| Instruction inventory shape | 983 verified rows: 516 Base and 467 FP&SIMD |
| ABI synchronization | v3, fingerprint `0x4333b73bf8b2ea99`, declaration SHA-256 `4333b73bf8b2ea997904cf0ad1c0ab8975121928af60cd5b8e4b0b0e01749cc6` |
| Dependency manifest and source/archive self-tests | passed; stale generated prefix receipt was correctly rejected |
| Network-enabled raw macOS consumer | failed to link with unresolved `uml_slirp_net_create`, `destroy`, `irq_fd`, `open`, `read`, `stop`, and `write`; log at `/private/tmp/uml-audit-round4-macos-network-consumer.log` |
| Network-disabled Apple consumers and fresh iOS graphs | passed |
| Guest/performance work | no guest boot, LTP, or Geekbench run was part of this audit |

The network-enabled package failure is reproducible without a guest:

```sh
cmake -S tests/consumer/apple \
  -B .build/audit-round4-macos-network-consumer -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/Apple.cmake" \
  -DUML_APPLE_PLATFORM=macos \
  -DUML_EMULATOR_LIBRARY="$PWD/.build/presets/macos-integration/darwin/emulator/libuml_emulator.a" \
  -DUML_EMULATOR_HEADERS="$PWD/darwin/api/include" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build .build/audit-round4-macos-network-consumer \
  --target uml-emulator-external-consumer --parallel 10
```

The regenerated
`../.build/audit-round3-finish-unit/instruction-evidence/stable-id-bindings.tsv`
contains 983 output rows, but only 183 use an explicit `stable-id` binding; 800
use `decoded-label`. They resolve to 902 unique evidence selections. Concrete
false-form bindings include:

- `BASE-0009` (extended register) and `BASE-0011` (shifted register) both select
  `0x2b220027`, disassembled as an extended-register form.
- `FPSIMD-0234` (immediate) and `FPSIMD-0235` (literal) both select
  `0x3c403c5d`, disassembled as an immediate writeback load.

The cause is structural. `form_matches()` falls through from exact compound
qualifiers to broad substring tests, while `section_matches()` infers section
ownership from names and paths. `EngineHarness::check_program()` records every
stored program word with one aggregate engine result, rather than proving the
word retired. `arm64_instruction_evidence.cpp` then ORs engine flags across
variants. The validator therefore proves that it can assign every inventory
row to some passing evidence record, not that the correct instruction form and
every variant ran on every applicable engine.

Other fourth-audit findings are source-level reproductions rather than new
passing tests. Most notably, `Emulator::run()` acquires the global embedded-run
mutex before same-handle admission; the MMU is process-global and generation-
only; JIT converts a valid coverage gap to a guest-illegal terminal; the ELF
loader uses destructive fixed mappings without complete range validation or
unmapping; an undersized SLiRP read leaves stale readiness; and direct boot/GPU
launchers do not share the maintenance sidecar lock. Acceptance tests for each
are specified in the handoff.

## Third-audit closure (historical)

Focused current-source checks close the run-retirement race, real embedded-
kernel multi-handle contract, fresh guest-helper generation, both optional JIT
profiles, reverse/interface/product architecture enforcement, build-time
dependency revalidation, network queue/provider reuse, ABI/device extraction,
and the final-state Linux topic series. These were the claims accepted by that
campaign; the fourth audit supersedes conflicting closure statements. The
earlier broad matrices remain
parseable passing evidence for their dated configurations and are retained
below rather than relabeled as new runs.

## Verification host and tree

- macOS 26.3 (25D125), Darwin 25.3.0, arm64
- CMake 4.1.1 and Ninja 1.11.1
- Apple clang 21.0.0 (`clang-2100.1.1.101`)
- Python 3.14.6
- Xcode 26.6 (17F113)
- outer repository base: `618b68658013b83394efef367570fa2c36113eb0`

The refactor remains an intentionally dirty working tree. No refactor file was
staged or committed, and the pre-existing staged deletion of the former
`linux-6.18` gitlink was preserved.

## Preserved host and guest matrices

| Third-audit campaign | Result | Local evidence |
| --- | --- | --- |
| Current unit selection | 562/562 passed | `../.build/audit-round3-finish-unit` |
| Repeated API and context lifecycle | 50/50 each passed | current unit binaries |
| Tooling/provenance after evidence generation | 18/18 passed | reset fixture excluded |
| Current targeted TSan | 1/1 passed | `../.build/audit-round3-final-tsan` |
| Optional JIT link profile | 2/2 passed | `../.build/audit-round3-final-profile-link` |
| Optional JIT pressure profile | 1/1 passed | `../.build/audit-round3-final-profile-pressure` |
| Bounded guest acceptance, 16 KiB | 5/5 passed | `../.build/audit-round3-final-integration` |
| Bounded guest acceptance, 4 KiB | 1/1 passed | `../.build/audit-round3-final-integration-4k` |

The 16 KiB set covers immutable-overlay systemd, lightweight interpreter,
gadgets, JIT, and deterministic SLiRP. The 4 KiB set proves a second kernel
build and lightweight interpreter boot. Interpreter tests use shell arithmetic
and CPU/HWCAP inspection only; they exclude Node.js, Go, browsers, benchmarks,
and the pressure registrations. Gadget and JIT smokes are separate engine
tests.

The preserved broader campaign remains: 580/580 normal unit/tooling at 16 KiB,
581/581 at 4 KiB, 562/562 ASan+UBSan, 5/5 targeted TSan, and 24/24 fast guest
integration at each page size. Those JUnits remain under
`../.build/results/refactor/final-20260803`; they are dated regression evidence,
not substituted for the fourth-audit acceptance work.

## Preserved instruction and architecture evidence

The third-audit executable-instruction report assigned all 983 inventory IDs
(516 Base and 467 FP&SIMD) to registered CTests and disassembled encodings, with
interpreter/gadget flags and either a JIT flag or an explicit host-feature skip.
The fourth audit shows that this assignment is not proof of correct per-form or
per-variant execution. The generated `stable-id-bindings.tsv` contains one
header plus all 983 assignments in both page-size build trees. Selecting only
`arm64-instruction-evidence` lists and runs one validator without activating
its reset fixture or erasing producer evidence. The validator is
non-destructive to producer TSVs, but it refreshes `stable-id-bindings.tsv`.

Canonical current architecture audits passed with these graphs:

| Configuration | Audited graph |
| --- | --- |
| Unit plus tooling | 551 targets / 580 registered tests |
| 16 KiB integration | 43 targets / 27 registered tests |
| 4 KiB integration | 44 targets / 27 registered tests |
| macOS product only | 21 targets |
| iOS device product only | 21 targets |
| iOS simulator product only | 21 targets |
| JIT link profile | 552 targets / 563 registered tests |
| JIT pressure profile | 551 targets / 562 registered tests |
| TSan unit configuration | 550 targets / 561 registered tests |

The audit covers normalized includes, forward and reverse declaration maps,
direct/object-composition edges, pure-interface and concrete cycles, semantic
test ownership, migration records, conditional registrations, source-size
reviews, documentation paths, and executable-memory primitive ownership. It
consumes the existing File API reply and never reconfigures its own build tree.
Isolated-link tests cover each CPU engine/factory edge. Two-real-kernel-handle
serialization, network-provider isolation, and deterministic overlapping
run-retirement/admission tests cover the process-wide and per-handle contracts.

The final advisory source-size report covers 1,010 production, test/support,
CMake, and declarative files. Fifty-three exceed a guideline, 42 lack optional
review context, and two reviewed files changed in size. These counts are
informational; cohesive files are allowed to remain large.

## Linux, dependencies, ABI, and rootfs

The exact Linux replay uses the signed official `linux-6.18.41.tar.xz` release
package (SHA-256
`17fc72f0f8d4a8a8633a5d20085f5d9c5a5ec51ee896a0b7ae1ec25da31273ea`) and the
10 final-state topic patches in `../kernel/patches/series/manifest.tsv`. The
packaged tree is
`3098cc6dba43578488a8f7f486797af8046ce8b8`; it omits only upstream's
export-ignored `Documentation/.renames.txt` relative to the recorded tag tree.
Independent authenticated replay reproduces final tree
`8adb106de7c19ed9e6c83f622a48e54629053699`. Generated local import/replay
commit IDs are observations because their authentic committer metadata is
intentionally dynamic. The third-audit replay observed managed HEAD
`cbad57bf321f34259311b0cf0fe9135159bf5832`. The Darwin host-service topic that
carries the synchronized ABI has stable ID
`3da04cbf2d5422fb4076708b2bb87df3f444a4a2` and SHA-256
`c23a15e07c6e201acf84f090440f8f42c3515cb79d7a4399b458337d1da574cd`.

The third-audit run performed fresh signed-package Linux replay, canonical
no-fetch verification, 162 changed-file/13 outside-port delta-ceiling checks,
and focused release-package dependency preparation/negative tests. Production
libraries are built from
pinned official release packages into a project-private arm64 static prefix.
Archive, patch, prepared-tree, manifest, and build-policy identities are
enforced; the receipt's canonical full-payload hash is a per-build integrity
observation, not a requirement that independent builds emit byte-identical
libraries. Prefix verification rejects altered or extra payloads, non-archive
or shared content, wrong architecture or deployment target, and
ambient/staging paths. The current matrices also cover ABI synchronization and
the source-tree audit, and both 16 KiB and 4 KiB kernels built and booted.
Kernel and external-library timestamps, build identity,
user/host, compiler notes, package layout/hashes, and binary hashes are dynamic
observations rather than reproducible identities; source inputs, patch bytes,
exact source tree, and ABI identity are the enforced invariants. ABI v3 uses
fingerprint `0x4333b73bf8b2ea99`, derived from normalized canonical host/kernel
headers plus installed `uml/emulator.h`; declaration history and negative tests
reject a declaration or layout change without an explicit identity advance.

The rootfs remained 8,589,934,592 bytes, mode `0444`, SHA-256
`501770dcce8223a3b7b41c475413ccb5f733291e190dcb23964890b50dafc0e5`.
Provisioning, explicit maintenance authorization, recovery artifacts, lock
lifetime, and process-group cleanup are covered by host tests. No writable
rootfs maintenance was performed.

## Apple package and executable memory

`scripts/build-xcframework.sh` rebuilt all three arm64 aggregate archives,
linked an external consumer for every platform, audited the device/simulator
archives, and recreated `UMLEmulator.xcframework`. Each archive has 59 members;
its Info.plist contains macOS arm64, iOS-device arm64, and iOS-simulator arm64.
A local `UMLBackend.xcframework` is an older artifact, not a current product.

Archive hashes, package layout details, tool versions, timestamps, and other
generated build metadata are intentionally dynamic observations. Acceptance is
anchored to source inputs, slice platforms/architectures, public declarations,
archive composition, executable-memory policy, and successful external links.

Interpreter and gadget state plus software-MMU storage are data-only; gadgets
execute ordinary precompiled signed RX text. They contain no W+X allocation
and make no RW/RX transition. Dynamic executable publication is confined to
`darwin/cpu/arm64/jit/jit_arena.cpp`, retained only for explicitly selected,
entitled debugger/jailbreak JIT use. Device and simulator symbol audits found
no `mprotect`, VM-protect, JIT-write-protect, or instruction-cache publication
reference outside that arena, and no production `jit::testing` support.

## Preserved performance observation

A full unprofiled Geekbench 6.7.1 run of the source-built-dependency
Release candidate completed at **80 single / 506 multi**
(result) with systemd as
PID 1, 10 guest cores, 8 GiB, gadgets, network access, and the immutable lower
image under tmpfs OverlayFS. This is +3.90%/-0.39% versus the immediately
preceding 77/508 run and +1.27%/0.00% versus the earlier G79 79/506 point. The
aggregate evidence does not show a broad gadget regression.

The executable SHA-256 observation is
`949a6ef31838e87b3d894dda96c43592a4878a2ac98f79c63dba8b6b7b700af6`.
The preserved console is
`../.build/results/refactor/audit-2026-08-03-release-deps/geekbench-gadgets-16k-console.log`,
SHA-256
`31df1439d98fe6b4773b9a6c05c00f85078e92e2cea701a44b844fe91bf7b86c`.

The prior single-core PDF anomaly resolved: 63 rose to 86, while multi-core PDF
rose from 535 to 547. The compiler scores moved from 48/392 to 51/385. A few
individual multi-core workloads regressed, led by HTML5 Browser at -4.58%, but
the overall multi-core score is effectively flat.

| Geekbench workload | Single | Multi |
| --- | ---: | ---: |
| File Compression | 104 | 601 |
| Navigation | 172 | 1,196 |
| HTML5 Browser | 73 | 562 |
| PDF Renderer | 86 | 547 |
| Photo Library | 78 | 617 |
| Clang | 51 | 385 |
| Text Processing | 50 | 61 |
| Asset Compression | 86 | 734 |
| Object Detection | 31 | 222 |
| Background Blur | 70 | 577 |
| Horizon Detection | 107 | 815 |
| Object Remover | 123 | 707 |
| HDR | 99 | 791 |
| Photo Filter | 101 | 431 |
| Ray Tracer | 98 | 961 |
| Structure from Motion | 74 | 620 |

## External acceptance

The following are intentionally not local pass claims because their inputs are
outside the repository:

- publisher release URL and trusted rootfs public key;
- an immutable image containing the full LTP payload for gadgets and JIT;
- physical sandboxed/entitled iOS devices;
- opt-in public-network, interactive KDE/VNC, rendered WebGL, and browser
  campaigns; and
- repeated controlled gadget/JIT performance runs.

Missing external inputs are not the cause of the repository-owned work reopened
by the fourth audit.
