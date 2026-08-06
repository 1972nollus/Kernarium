# Refactor progress

Status as of 2026-08-04: the seventh independent audit reopened repository-
owned work. Its highest-priority findings are an instruction-evidence oracle
that is reproducible but not independent, a ledger that proves one selected
case per instruction entry rather than all variants, erased instruction-fetch
fault direction, incomplete gadget exception cleanup, unbounded/fallible
memory-domain retirement, GPU shutdown and descriptor-ownership hazards,
incomplete suite build aggregates, and stale current-product evidence. Rootfs
file management remains explicitly deferred
to a later testing redesign. Work needing a special guest image, credentials,
physical devices, public services, interactive graphics, or controlled
performance hardware remains external acceptance rather than an unfinished
source change.
[`REFACTOR_HANDOFF.md`](REFACTOR_HANDOFF.md) is the authoritative acceptance-
level checklist.

[`docs/refactor-evidence.md`](docs/refactor-evidence.md) is the detailed
evidence index. [`REFACTORING_PLAN.md`](REFACTORING_PLAN.md) remains the original
specification and records the clarified policy that generated build metadata
does not need to be byte-for-byte deterministic.

## Seventh-audit open work

- Replace the binding-derived allocation/feature generator with an independent
  authoritative form-to-allocation and allocation-to-feature oracle. Model
  dependent and reserved fields, and add whole-pipeline negatives that remain
  invalid after regenerating every test-owned catalog.
- Give every required valid/invalid instruction variant an independent identity
  and expected state, event, fault, and memory result. The current 983 unique
  records each credit only one selected case, so their checkmarks do not prove
  the entry's width/form/alias/state/feature partitions are exhaustive.
- Preserve instruction-fetch fault direction through interpreter, translated
  engines, TLB metadata, the C/kernel ABI, and Linux retry handling. Repair
  gadget exception cleanup so direct engine callers always receive materialized
  state with the host environment restored, and make interrupt-token
  registration failure deterministic rather than silently discarded.
- Replace permanent retired-domain tombstones with bounded, allocation-free
  identity/lifetime protection. Make GPU framed I/O stop-aware, define distinct
  service/guest endpoint ownership, reset device state across owners, and make
  command mutations exception-atomic with direct host-only tests.
- Turn the gadget and native-code implementation fragments into ordinary
  responsibility-owned translation units. Resolve included headers to engine
  ownership in the architecture audit, and run common conformance through the
  exact production engine compositions rather than test-local recompilations.
- Split mixed Linux topic 0002 into reviewable final-state concerns; make fresh
  integration presets accept the required case-sensitive kernel checkout; make
  every suite aggregate build every registered cross-directory fixture; and
  rebuild both kernels, the current CLI, all three package slices, and package
  consumers only after source work is complete.
- Make the symmetric engine-edge fixture and source-tree audit comfortably fit
  their registered timeouts. Remove the unreachable fatal gadget, production
  runtime/network test instrumentation, unconditional disabled-profiling work,
  unbounded profiler records, implicit registry destruction ordering, and the
  raw-CTest label-verification bypass. Clarify whether “Python-free unit” means
  test implementation/execution or also the configure/build toolchain.

Fresh seventh-audit host verification passed **562/562** 16 KiB units,
**563/563** 4 KiB units, and full ASan+UBSan **562/562** from current-source
build directories. Architecture, TSan **4/4**, loader ASan+UBSan **1/1**, and
normal/sanitized GPU ownership gates passed. The ordinary tooling run passed
**22/23**: the symmetric engine-edge
test timed out at 60.01 seconds but its exact command passed directly in 59.71
seconds, confirming a timeout-margin defect rather than a semantic failure.
Regenerating the 983-row allocation file was byte-identical at SHA-256
`0e383651df7e3d743ab7d7d4ffe2007b6ce41bd6bb09f1ffc8bdb9f0ca77d574`; that
only proves deterministic reproduction of the dependent method. No guest,
benchmark, current kernel/product, or rootfs campaign was used for this audit.

## Sixth-audit closure claim (historical; superseded)

The seventh-audit findings above supersede this completeness claim. The
following records what the sixth pass reported and remains useful dated
evidence.

- The ARM64 allocation catalog is generated independently with LLVM MC 22.1.8:
  983 rows, 79 distinct masks, and only 21 exact-word masks. The validator joins
  architectural form plus opcode provenance and seven negatives reject the
  formerly accepted self-consistent wrong-form case. Both page-size reports
  contain all 983 IDs and are byte-identical at SHA-256
  `d4bf8d90db910807e6adbf2bf19e06f1d32ccd98fd60cfa7b2d26a583d3ed139`.
- Native 4 KiB shadow state is domain-owned and retired on normal and abnormal
  run exit. Sequential, concurrent, and abnormal public-handle tests plus map,
  protect, unmap, and refresh fault injection prove exception containment,
  rollback, isolation, and retry. Native/non-JIT storage is data-only.
- GPU queue descriptors and VNC initialization use RAII/transactional
  ownership. Production replacement/failure/retry and exact-port-reuse cases
  run in canonical rootfs-free normal and ASan+UBSan GPU workflows.
- Gadget and JIT responsibilities are split into logical owners while
  preserving decoder order and the production JIT object's bytes. Cross-engine
  link/object/aggregate/include/source-include edges are rejected symmetrically
  by a positive fixture and 24 negative cases.
- The virtual-transport DMA-mask fix is independent Linux topic 11. Kernel and
  dependency policy reject extra unlisted patches. Queued-transmit restart is
  deterministic, and the NEON inventory now lives under `tests/unit/arm64`.
- Source sizing remains advisory: 1,060 files are reported, 60 exceed a
  guideline, 42 lack optional review context, and zero changed since review.

Fresh closure verification passed 562/562 16 KiB units, 23/23 tooling checks,
563/563 4 KiB units, 562/562 full ASan+UBSan, 2/2 focused native 4 KiB
ASan+UBSan, 4/4 TSan, 1/1 loader ASan+UBSan, 1/1 normal GPU, 1/1 sanitizer GPU,
and 1/1 rootfs-free network coverage. iOS device and simulator 21-target graphs
passed; archive scans found no executable publication outside the explicit JIT
arena. A fresh authenticated eleven-topic Linux replay passes exact topic-path
overlap policy, verifies the 162/13 delta, and reaches canonical tree
`834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d`.

No guest boot, Node.js, Go, browser, benchmark, or controlled performance
workload was run for this closure. Physical sandboxed/entitled iOS and other
external campaigns remain explicit acceptance work, not repository refactor
items.

## Fifth-audit closure claim (historical)

This section records what the fifth pass reported. The sixth audit above
supersedes its completeness claim; statements below use their original wording
so the implementation history remains understandable.

- Exact ARM64 evidence now joins every encoding to an independent 983-row
  allocation catalog and records exact required/observed host features. Six
  negatives cover encoding-only and unrelated-feature mutations. Both page
  sizes produce 983 stable IDs with 710 native executions and 273 exact skips.
- Native-code targets no longer link, embed, include, or call gadget-owned code;
  shared transfer/TLB/environment support is neutral, and the architecture
  audit rejects any future native-code-to-gadget edge. Interpreter and gadget
  paths contain no executable-memory publication primitive, W+X mapping, or
  RW/RX transition; explicit JIT ownership remains available.
- Context/provider publication is a rollback-safe joint transaction. MMU
  mappings, history, invalidation, stale batch identity, and native 4 KiB shadow
  state are exception-atomic under deterministic Nth-failure coverage. Queued
  embedded stop admission has deterministic wait-boundary coverage.
- GPU descriptors acquire RAII ownership immediately after `recvmsg`; repeated
  malformed, short, oversized, dispatch-exception, and allocation-failure cases
  do not leak. Startup failure injection is present only in a test variant.
- Evidence reset/production/validation has standalone presets, raw tooling is
  non-destructive, the loader has a canonical ASan+UBSan gate, and next-version
  policy emits tested carry/drop/retest/upstream actions for applied, contained,
  and conflicting patches. ABI, provider ownership, TSan, and replay docs
  matched behavior at that time.
- The default dependency prefix was rebuilt from pinned release inputs and
  passes its full verifier. macOS, iOS device, and iOS simulator archives pass
  architecture and raw/install-tree C/C++ consumers; iOS symbol audits confine
  executable publication to the explicit JIT arena.

Dated fifth-audit verification:

- deterministic 16 KiB evidence pipeline: **563/563** in 286.37 seconds;
- deterministic 4 KiB evidence pipeline: **564/564** in 279.60 seconds, with
  architecture at **553 targets / 585 registrations**;
- non-destructive raw tooling: **22/22**;
- default-prefix GPU ownership fixture: **1/1** after 64 repetitions per error
  class; GPU architecture at **562 targets / 586 registrations**;
- canonical malformed-loader ASan+UBSan: **1/1**; TSan passed exactly **4/4**
  in 4.44 seconds;
- authenticated Linux replay matches canonical final tree
  `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d`; replay commit identity and build
  metadata remain intentionally dynamic;
- independent public and kernel bridge ABI v4 identities, 162/13 Linux delta
  policy, next-version/topology negatives, rebuilt dependency verification,
  exact 16/4 KiB active-import checks, pristine LibVNCServer 0.9.15, the then-current
  16 KiB production kernel/app final link, all three package graphs, and C/C++
  consumers pass; and
- source size remains advisory: 1,034 files, 53 above guideline, 42 without
  optional review context, and six changed since review.

No guest boot, Node.js, Go, browser, or benchmark workload was run during this
closure. Rootfs redesign and external/device campaigns remain outside this
repository-owned completion claim.

## Fourth-audit closure (historical)

The following was the reported end state of the fourth audit. It remains useful
dated evidence but is superseded by the later historical closure records and
the current seventh-audit checklist above.

- Exact instruction evidence uses a checked-in 983-row catalog, exact encoding
  and architectural-form joins, per-engine executed/retired results, declared
  aliases only, and four negative fixtures. Fresh 16 KiB and 4 KiB reports each
  validate all 983 IDs: 713 JIT executions and 270 exact feature skips.
- Every engine maps a valid implementation gap to the guest illegal-instruction
  event without retirement or fallback and remains reusable. Same-handle runs
  fail fast; different embedded handles remain process-serialized with
  stop-aware queued admission.
- Software-MMU state is domain/run isolated, C allocation failures are
  contained and retryable, and the ELF loader uses non-destructive bounded
  mappings with complete cleanup and malformed-image coverage.
- The installed emulator archive is provider-neutral. The macOS app owns and
  injects SLiRP, while networking-enabled raw and installed archives link from
  genuine C and C++ consumers. Public API identity v4 is independent from the
  kernel bridge v4 identity.
- SLiRP readiness, runtime target composition, test/fixture ownership, API
  documentation, GPU descriptor RAII, and final-link provenance now have
  focused acceptance tests. The final-link audit is an allowlist, including an
  arbitrary ambient-archive rejection fixture.
- LibVNCServer is the pristine pinned 0.9.15 release tree
  `3b0f5deb99f726cc0cfb963b783fcc396017fde8e646240a9ed3b01c18077a35`.
  A fresh verified prefix, loopback VNC smoke, and all package consumers pass.
- Linux replay enforces ten exact nonempty patch IDs, subjects/order, and no
  merges. The authenticated release replays to exact tree
  `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d` over 162 paths, including removal
  of the obsolete `DARWIN_LIBELF_INCLUDE` hook.
- Expired weak registrations are pruned under 3,000-cycle churn, network
  implementation includes are private, and root `.DS_Store` debris is removed
  and rejected by the source audit.

Rootfs file management is deferred to a later testing redesign. Image hashes,
sidecar locking, maintenance serialization, and rootfs cleanup are not current
refactor exit criteria.

## Fourth-audit verification (historical)

- The then-current 4 KiB host units passed **562/562**; that audit's 16 KiB unit
  selection passed **561/561**, with focused MMU, GPU, VNC, lifecycle, gadget,
  and package checks repeated after its final edits.
- The embedded-loader fixture passes at the exact production link window and
  in a targeted ASan+UBSan build covering occupied-window refusal, malformed
  dynamic/relocation metadata, cleanup, MMU isolation, and run admission.
- Architecture audits pass for the 559-target unit/tooling/GPU graph, the
  tooling-disabled external-integration graph (39 targets / 10 registrations),
  and all three package product graphs.
- Public ABI v4 is fingerprint `0xd9a69804d3303044`, SHA-256
  `d9a69804d33030448b780d58c3f553c0e12e4f1e19ab37f89d2719b8b93115f1`.
  Kernel bridge ABI v4 is independently fingerprinted as
  `0xfe2d71a33e3d446e`.
- The fresh private dependency prefix passes source-tree, receipt, payload,
  archive-member, architecture, platform, and deployment-target verification.
  Build timestamps, identities, compiler notes, and binary hashes remain
  dynamic observations.
- The three arm64 package slices pass raw/install-tree C and C++ consumer
  links. iOS device and simulator symbol audits find no executable-memory
  publication primitive outside `jit_arena.cpp`; non-JIT paths perform no
  RW/RX transition and never map W+X.
- The source-size report covers 1,023 files: 54 above an advisory guideline, 43
  without optional review context, and five changed since review. These are
  informational, not line-count failures.
- The previously recorded rootfs mode, size, and digest remain historical
  observations only; rootfs management is no longer a refactor acceptance
  gate.
- Interpreter/gadget state and MMU storage remain data-only; executable
  publication is reachable only through the explicitly selected JIT arena.

## Preserved earlier regression matrices

These dated results remain useful broad evidence and were not deleted: normal
unit/tooling **580/580 at 16 KiB** and **581/581 at 4 KiB**; **562/562
ASan+UBSan**; **5/5 targeted TSan**; and fast guest integration **24/24** at
each page size. They are not substituted for the targeted current-source
acceptance tests in the handoff.

The preserved 16 KiB gadget Geekbench result is **80 single / 506 multi**
(result), +3.90%/-0.39%
against the immediately preceding 77/508 run. No heavy benchmark was rerun as
part of the fourth audit.

## External acceptance

An LTP-enabled guest image, physical sandboxed or
entitled iOS devices, opt-in public-network/interactive-GUI/rendered-browser
campaigns, and repeated controlled gadget/JIT performance runs remain external
prerequisites. They are not silent pass claims and are separate from the open
repository-owned seventh-audit checklist.
Rootfs file management is deferred rather than treated as external acceptance.
