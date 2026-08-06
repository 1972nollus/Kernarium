# Refactor handoff

This is the authoritative status as of the seventh independent audit on
2026-08-04. Repository-owned work remains open. The seventh audit supersedes
the sixth-audit completeness claim: it found two instruction-evidence closure
gaps, multiple runtime/device correctness gaps, unfinished engine and Linux
modularization, and structural-test gaps. The sixth-audit implementation and
host evidence are retained below as historical results, not current closure.

Work needing credentials, special images, physical devices, public services,
interactive graphics, or controlled performance hardware remains external
acceptance and is not represented as a local pass. Rootfs file management is
still deferred as described below; none of the seventh-audit findings reopens
it.

[`docs/refactor-evidence.md`](docs/refactor-evidence.md) is the evidence
index. [`REFACTOR_PROGRESS.md`](REFACTOR_PROGRESS.md) is the compact summary.

## Working-tree safety

The historical refactor base was
`618b68658013b83394efef367570fa2c36113eb0`. The current outer HEAD is
`b931e9b682e7b4bde4a4824c2e2142676052e1e1` (`Major refactor`). At the seventh-
audit baseline, the implementation and documentation existed as 155 modified
or deleted tracked status entries plus 56 untracked status entries (58
individual untracked files), with nothing staged. `git diff --stat` reported
155 files, 6,904 insertions, and 27,459 deletions. These are user-owned changes.
Do not commit, stage, reset, clean, broadly check out, or otherwise rewrite the
working tree without separate authorization. Preserve unrelated pre-existing
working-tree changes.

Rootfs file management is explicitly deferred to a later testing redesign. Do
not implement or expand image hashes, sidecar locking, maintenance
serialization, or rootfs cleanup as part of this refactor. Existing wrappers
may remain in use, and incidental destructive image changes still require
separate authorization.

## Seventh independent audit: open checklist

The following items are current. Fix them before using any historical
"complete" statement below. Source-size thresholds remain advisory; the
modularity findings are about ownership and dependency shape, not line-count
enforcement.

### P1 correctness and acceptance blockers

| Finding | Current evidence | Acceptance requirement |
| --- | --- | --- |
| Instruction allocation and native-feature evidence is still circular | `generate_a64_instruction_allocations.py` takes both the architectural label and selected word from the test inventory/binding catalogs. The external decoder classifies that selected word and the generator infers a mask by single-bit flips; it never independently maps the architectural form to the expected decoder allocation or required host features. A consistently reassigned form regenerated and validated, as did a consistently unrelated feature skip. A two-bit scan found 127 words across 70 stable IDs that satisfy checked-in masks but are rejected or reclassified by the decoder; for example, `0b221427` satisfies one current `ffe00000/0b200000` mask while the decoder rejects it. | Derive form-to-allocation and allocation-to-feature metadata from an independent authoritative source, including dependent/reserved-field constraints. Add end-to-end negatives that mutate every test-owned catalog, regenerate the oracle, and still must fail. Pin or explicitly review the oracle version and test checked-in regeneration parity. Regenerate page-size reports only after these negatives pass. |
| The 983-row evidence is representative-per-entry, not variant-complete | Bindings contain exactly 983 unique stable IDs and the validator rejects multiple evidence records for an ID. The recorder credits only the one case/word matching that binding. `current_variant_model` is a generic label, not an independently enumerated set of widths, forms, aliases, state combinations, invalid/reserved partitions, or feature variants. The large test sweep may exercise many combinations, but the current ledger cannot prove none is missing. | Introduce independent variant identities/manifests and permit multiple variant records per instruction entry. Bind every required valid and invalid partition to its expected state/event/memory result and all eligible engines. Add removal, duplication, and cross-variant-swap negatives. Do not mark an entry complete from one representative word. |
| Instruction-fetch fault direction is erased before Linux receives it | The public engine result distinguishes read, write, and execute, but interpreter results, translated TLB metadata, gadget/JIT fetch failures, the shared translated adapter, and the C bridge reduce the direction to read/write. The bridge exports only a write bit. Linux then guesses execute when the fault address equals the instruction pointer, so a data read at that address or a true execute-permission fault can be retried with the wrong permission. The common harness repeats the same collapse and cannot detect it. | Preserve read/write/execute through all three engines, translated fault metadata, the mirrored host/kernel ABI, and the Linux consumer. Test an unmapped/non-executable entry fetch, a data read whose address equals the current PC, ordinary read/write faults, exact address/PC, zero retirement, and retry behavior; advance ABI identity, refresh the Linux topic, and replay both page modes. |
| Gadget exception cleanup can violate the common engine contract | Gadget blocks intentionally leave the guest floating-point environment active between translated blocks. Ordinary exits restore/materialize it, but if a later allocation or translation throws, the `noexcept` adapter returns fatal after `GadgetExecutor::end_execution()`, which ends only MMU execution. Direct `CpuEngine` callers can receive a fatal result with guest host-state still active and lazy architectural state not materialized. The native-code executor already restores this state in its cleanup. | Inject a second-block allocation/translation failure after the first block activates native floating-point state. Require a documented retirement count, materialized `CpuState`, restored host environment, balanced begin/end, and successful engine reuse. Run the same exception-contract test against the native-code engine. |
| Retired memory-domain guards are unbounded and allocation-fallible | Every run receives a monotonic domain. Both software and initialized 4 KiB native registries permanently insert that domain into `retired_domains_`, even when no address space existed. N runs retain N software keys and another N native keys in 4 KiB mode. Insertion failure is swallowed; a delayed resolver can then recreate state for a retired domain after the live entry is erased. | Make retirement allocation-free and use bounded lifetime/identity state that prevents an in-flight old resolver from publishing after retirement. Add 100,000-run bounded-metadata tests for both page modes, deterministic delayed-resolver/retire interleavings, allocation-failure coverage or a structural no-allocation proof, and immediate next-run retry. |
| GPU shutdown can block forever on an incomplete live stream frame | The connection loop polls only before entering `recvmsg(MSG_WAITALL)` and blocking completion reads. `stop()` merely sets a flag and joins. A peer that sends part of a header or payload and stays open prevents destruction from returning. Current truncation tests explicitly shut down the sender first. | Make framed reads/writes stop-aware with nonblocking polling plus a wake path, or interrupt them through a separately and correctly owned endpoint. Add bounded subprocess tests for partial header and payload, descriptor balance, restart, and normal plus ASan/UBSan GPU gates. |
| Published GPU and input descriptors have overlapping owners | The service retains both peer endpoints in RAII objects while publishing their raw numbers. The same-process guest connector returns those exact descriptors rather than duplicates, and guest cleanup closes them. Service destruction can later close a recycled descriptor number; using the retained number to fix shutdown could also target an unrelated reopened descriptor. | Define transfer/duplication ownership. The guest must own a distinct transferred/duplicated endpoint and the service must retain only a stable, separately owned wake/shutdown capability. Test guest-close followed by descriptor-number reuse and host stop/destruction, plus startup failure and retry. |
| Current production/package evidence predates the final source split | The newest macOS CLI and both kernels predate the current gadget/JIT split; topic 0011 and metadata also postdate both kernels. The current XCFramework macOS archive still contains obsolete `program.cpp.o`, whereas current iOS build archives contain `program_builder.cpp.o` and `program_execution.cpp.o`. Historical package consumers therefore cannot close the current tree. | After source findings are fixed, replay/build both kernel page modes, final-link the current macOS CLI, rebuild all three package slices, rerun architecture/final-link and executable-publication audits, and repeat raw plus installed-tree C/C++ consumers. Record only current-tree artifacts as seventh-audit acceptance. |
| Suite build targets omit registered GPU fixture binaries | `unit-test-binaries` is assembled from arm64-local targets plus runtime and omits the registered `uml_gpu_service_fd_tests`. `tooling-test-binaries` scans only the tooling subdirectory, while the registered VNC smoke commands use two targets owned by the parent tests directory; in a fresh GPU graph the tooling aggregate is empty and those executables are absent. A build preset can therefore finish before CTest attempts a registered command. | Derive suite build dependencies from every registered target command, or explicitly attach every cross-directory binary. Add a fresh-graph audit proving each enabled suite's `$<TARGET_FILE:...>` or target command is reachable from its aggregate, then run fresh GPU tooling, integration, 4 KiB integration, and all workflows. |

### P2 architecture, device, and workflow work

| Finding | Current evidence | Acceptance requirement |
| --- | --- | --- |
| Engine splitting is implementation-header fragmentation | Production `jit_executor.cpp` is a four-line textual composition of roughly 10,095 implementation-header lines into one object. Gadget translation remains one roughly 4,274-line builder body plus about 8,543 transitive selection-header lines. These are not the normal responsibility-owned translation units required by the plan; size-review rationales do not create module boundaries. | Give cache, links, publication, diagnostics, semantic helpers, lifecycle, test hooks, and instruction-family translation normal `.cpp` owners with declaration-only private headers. Make the gadget builder orchestration rather than the complete decode walk. Preserve hot object/disassembly shape and extend the source audit to reject implementation-fragment headers without prohibiting genuine templates or small inline code. |
| Cross-engine include enforcement is bypassable | The real architecture manifest serializes link, object, and aggregate edges, but never emits include-directory ownership. Negative tests inject synthetic `include@` edges. Text scans catch only include strings containing another engine directory, so another engine's include directory plus `#include "basename.hpp"` escapes both gates. | Add explicit engine metadata and serialize/inspect include directories or compile-command/header dependencies. Resolve included files to their owning engine. Add real configure negatives for direct, basename, and transitive includes in every engine direction. |
| Common conformance does not use every exact production engine object | Ordinary instruction conformance constructs a test-local gadget adapter. Its native-code path resolves the production adapter against `uml_arm64_jit_test_support`, which recompiles the whole executor with test macros instead of using the production executor object. Basic production-adapter contract tests pass, but the full instruction sweep does not prove those exact production compositions. | Run ordinary conformance through production factories, adapters, and executor objects. Put forced-path controls and instrumentation behind neutral injected hooks or separate focused tests; do not credit a recompiled implementation as production-engine evidence. |
| Interrupt-token registration silently drops allocation failure | `darwin_uml_static_guest_interrupt_sequence()` catches every failure from thread execution-state acquisition and returns success-like `void`; it does not latch an error. Linux installs the sequence immediately before guest entry, so a one-shot allocation failure can let that entry run with a null/default token and delay cooperative interrupt observation. The comment claiming the next entry will fail is not implemented. | Make registration allocation-free, latch a failure that the next entry consumes before retirement, or change the checked kernel ABI to return an error. Inject the first registration allocation failure, require deterministic no-retirement failure, then prove explicit retry/re-registration and no stale token; update both ABI copies and replay if the signature changes. |
| Linux topic 0002 remains deliberately mixed | The plan explicitly requires SMP/timer/IRQ to be separated from MMU/HugeTLB and unrelated execution/build changes. Topic 0002 is still named for all of them and spans 19 files at about +1,723/-117, including the largest SMP, TLB, HugeTLB, uaccess, timing, execution, and build-wiring areas. | Split it into reviewable final-state topics, at minimum MMU/HugeTLB, SMP/timer/IRQ, execution/capability, and build portability, without changing the final tree. Refresh stable IDs, hashes, order, overlap ownership, extraction decisions, and next-version actions; replay and build both page modes. |
| Checked-in integration presets cannot select the required case-sensitive kernel checkout | `macos-integration` enables the kernel runner but leaves `UML_KERNEL_SOURCE_DIR` at repository `linux`; source verification rejects its normal case-insensitive parent. The manual documentation supplies a case-sensitive-volume path, while the dependency README incorrectly presents the preset alone as sufficient. | Make integration/all presets consume a documented environment or user-preset path and fail early with a clear missing-path diagnostic. From fresh caches, configure `check-integration`, 4 KiB integration, and `check-all`. Correct the dependency README. |
| GPU connection reset and command mutations do not fully own device state | Transport reset clears mappings, queues, and negotiated features but retains contexts, resources, and scanout state; `RESET_OWNER` is a no-op. At least context creation mutates the renderer before allocating bookkeeping and a response, without the rollback used by another resource path. Existing GPU tests do not directly exercise `VirtioGpuDevice::process`. | Add explicit device reset on owner reset and disconnect, destroying renderer state exactly once. Make every mutating command exception-atomic. Add split host-only command suites with fake renderer/display, malformed request coverage, deterministic allocation failures, same-ID retry, and reconnect-to-pristine-state tests under normal and sanitizers. |
| Optional diagnostics impose production cost and retain thread records | Every guest entry reads `steady_clock` even when dispatch profiling is compiled off. The MMU writer path also reads a clock while gate profiling is disabled. When gate profiling is enabled, each participating host thread permanently appends a heap record and writer diagnostics scan the ever-growing vector. | Compile diagnostics into an optional tool/test target or a compile-time zero-cost policy. Prove the ordinary production object has no clock/profile call in these paths. If the gate profiler remains, give per-thread registration RAII retirement and add a thread-churn subprocess proving the registry returns to baseline and contains no dead IDs. |
| Structural audit CTest is too slow for its timeout | `symmetric-engine-edge-audit` performs one positive plus 24 sequential full File API scans. It failed at 60.01 seconds; direct runs passed at 59.71-65.69 seconds. The source audit also recursively traverses large ignored build/Linux/external trees before filtering them, taking about 25 seconds on this workspace. | Parse the architecture graph once or use a minimal synthetic graph, and enumerate only first-party source roots. Target a sub-10-second edge fixture, give realistic timeout margin, and repeat the registered tooling suite under ordinary parallel load. |

### P3 bounded cleanup and hardening

| Finding | Acceptance requirement |
| --- | --- |
| One fatal gadget/helper has no source or relocation reachability | Remove its wrapper, declaration, and assembly body unless it is an intentional documented ABI surface; rerun symbol reachability, units, and layout-sensitive checks. |
| Network restart test controls execute in the production per-frame path | Move pause/counter state into a test-only provider variant or compile-time no-op policy. Prove the production object has no test counter/pause symbols, branch, or per-frame atomic, while the deterministic restart fixture still passes. |
| Runtime lifecycle/failure hooks live in the production `Context` object | Move admission/retirement hooks, state counters, and forced-publication failure fields/branches behind a test-only facade or narrow test object variant. Prove the production object has no `_for_testing` symbols, fields, or conditional branches while keeping deterministic lifecycle coverage. |
| Legacy-context and software-registry static destruction order is implicit | Make process-lifetime ownership/destruction order explicit and add a subprocess exit-order test so context teardown cannot call a destroyed memory registry. |
| Raw CTest registrations could bypass suite-label verification | Reject first-party `add_test` outside `UmlTesting.cmake`, or validate actual labels from CTest JSON against `tests/test-manifest.tsv`; add a wrong/missing-label negative. |
| The Python-free unit contract is ambiguous | `tests/unit` contains no Python or shell files and the selected unit tests execute without Python, satisfying the user's physical-test requirement. ADR 0001 makes the stronger claim that units are buildable without Python, but top-level configure always runs the Python ABI declaration verifier. Either revise the ADR to distinguish Python-free test implementation/execution from Python-used-as-a-build-tool configuration, or preserve the stronger claim by adding a no-Python configure/build smoke and a native verifier. |

### Recommended continuation order

1. Repair the independent allocation/feature oracle and introduce explicit
   variant identities first. Until those gates pass, the 983-row report is
   useful regression evidence but cannot close instruction completeness.
2. Preserve instruction-fetch fault direction through the engine/kernel ABI and
   fix translated-engine exception cleanup and interrupt registration. Then
   replace retired-domain tombstones before building further lifecycle behavior
   on those foundations.
3. Route the common conformance matrix through exact production objects. Split
   implementation fragments into normal translation units and strengthen real
   include ownership while checking generated hot-path shape after each small
   behavior-neutral move.
4. Fix GPU cancellation, descriptor ownership, reset, and command atomicity;
   remove production test/profiling overhead. Split Linux topic 0002 without
   changing the final tree, repair fresh-cache integration inputs and suite
   aggregate completeness, shorten the structural audits, and run both page-
   mode host/sanitizer matrices.
5. Only after source closure, replay/build both kernels and rebuild the current
   CLI plus all package slices/consumers. Then run the separately authorized
   guest and controlled performance acceptance. Continue to defer rootfs file
   management, and do not commit or stage refactor changes without explicit
   authorization.

### Seventh-audit verification

| Check | Fresh result |
| --- | --- |
| 16 KiB host graph | clean configure/build succeeded; strict unit selection passed 562/562 in 468.78 seconds |
| 4 KiB host graph | clean configure/build succeeded; strict unit selection passed 563/563 in 367.62 seconds |
| Architecture/source audit | 4 KiB graph passed at 553 targets / 586 registrations; GPU graphs passed at 562 targets / 587 registrations; source report covers 1,060 files, 60 above advisory guidance, 42 without optional review context, zero changed since review |
| Tooling | 22 of 23 tests passed; only `symmetric-engine-edge-audit` hit its 60-second timeout. The exact command passed directly in 59.71 seconds, confirming semantics but no usable margin |
| Sanitizer/focused gates | full ASan+UBSan 562/562; TSan 4/4; malformed-loader ASan+UBSan 1/1; GPU normal 1/1 and GPU ASan+UBSan 1/1 after granting loopback-port permission |
| Allocation-catalog regeneration | regenerated 983-row file is byte-identical to the checked-in file at SHA-256 `0e383651df7e3d743ab7d7d4ffe2007b6ce41bd6bb09f1ffc8bdb9f0ca77d574`; this proves deterministic reproduction of the current circular method, not independent architectural validity |
| Dependency and policy checks | current pinned release-source manifest, pristine LibVNCServer 0.9.15, private prefix, ABI identities, Linux manifest/topology/next-version fixtures, and isolated topic 0011 checks pass |
| Guest/performance/product acceptance | no guest boot, rootfs mutation, LTP, browser, benchmark, current kernel replay/build, current CLI final link, or current package rebuild was performed; the seventh audit remains open |

### Confirmed current invariants

- All three CPU engines implement the common virtual interface, direct factory
  selection is in place, the native-code engine has no gadget link/object
  fallback, and a valid implementation gap produces a guest-scoped illegal-
  instruction event with zero retirement rather than terminating the emulator.
- Engine selection is frozen per run; same-handle admission, queued stop
  synchronization, per-thread execution-state retirement, bound concurrent-
  handle routing, and public input deep-copy contracts remain covered.
- The unit, tooling, and integration source trees are physically separated.
  `tests/unit` contains no Python or shell scripts. Full LTP remains a custom
  target outside CTest; SMP is standalone or explicitly opt-in integration and
  is absent from the default unit configuration. The manifest has 603 rows:
  567 unit, 26 tooling, and 10 integration. Current registrations use the
  centralized helpers, subject to the future raw-registration bypass above.
- CMake/CTest with Ninja remains the appropriate canonical first-party system
  for the mixed Apple/Linux/upstream-source graph; no parallel Bazel or Meson
  first-party source manifest exists.
- Dependencies are pinned official release-source packages. LibVNCServer is the
  pristine upstream 0.9.15 tree with no project patch. Active source paths no
  longer use the retired layout vocabulary, device modules live under
  `darwin/device`, and no empty first-party source directory remains.
- Source-size thresholds remain advisory. Fresh reporting covers 1,060 files;
  size alone is not an error or an open item.

## Sixth independent audit: historical closure claim

The sixth pass reported all twelve of its then-known findings closed. The
seventh checklist above supersedes that completeness claim. Rootfs file
management remains deliberately deferred to the later testing-image redesign.
No guest boot, LTP, browser, Node.js, Go, benchmark, or controlled performance
campaign was used for the historical claim.

| Area | Resolution |
| --- | --- |
| Independent ARM64 oracle | `generate_a64_instruction_allocations.py` derives the checked-in 983-row allocation catalog from LLVM MC 22.1.8 rather than copying test bindings. The catalog contains 79 distinct masks and only 21 exact-word masks. Validation joins section plus architectural form, opcode provenance, mask, and value. Seven negative fixtures include the formerly accepted self-consistent wrong-form mutation. |
| Exact engine evidence | Fresh 16 KiB and 4 KiB reports each bind all 983 stable IDs. JIT records 670 retired and 40 executed/non-retired cases with 273 exact feature skips; interpreter and gadgets record 893 retired and 90 executed/non-retired cases. The two reports are byte-identical, SHA-256 `d4bf8d90db910807e6adbf2bf19e06f1d32ccd98fd60cfa7b2d26a583d3ed139`. |
| Native 4 KiB ownership and failure safety | Native-shadow state is domain-owned and retired on every normal or abnormal run exit. Since `MAP_FIXED` is process-global, 4 KiB native callbacks now share the process address-space admission gate; a same-VA two-handle test covers serialization, queued cancellation, flush-before-handoff, isolation, and retry. Public `darwin_uml_vm_*` tests retain sequential reuse, abnormal return, and allocation failures in map, protect, unmap, and refresh. C boundaries contain exceptions, preserve sibling state, roll back mappings/protections/bookkeeping, and permit immediate retry. Native shadow storage is data-only and strips execute permission. |
| GPU/VNC ownership | Queue notification descriptors use RAII, memory-table replacement is transactional, and VNC initialization guards the live screen until event-thread ownership succeeds. Production dispatch tests cover repeated replacement, reset-stage failures, retry, descriptor balance, post-listen thread failure, and exact port reuse. Canonical rootfs-free normal and ASan+UBSan GPU workflows require the ownership test whenever the target exists. |
| Engine modularity | Gadget catalog/selection/encoding, build, and execution responsibilities are separated. JIT cache, linking, publication, diagnostics, semantic memory, emission, compilation, verification, lifecycle, and test-only support have distinct internal owners; production and test-only lifecycle compositions are separate. The production JIT object remained byte-for-byte identical across the split. Advisory size reviews were refreshed with zero changed-since-review rows. |
| Engine boundary enforcement | The File API audit classifies every concrete engine and rejects cross-engine link, object, aggregate, include, and source-include edges in both directions. Positive isolated ownership and a 24-case negative matrix exercise the rule. Interpreter and gadget paths cannot reach JIT publication. |
| Linux and dependency policy | The 64-bit vhost-user DMA-mask correction is independent topic 11. Kernel and dependency validators compare the complete patch directory with their manifests and reject unlisted files. Topic-path ownership is singular except for the exact declared `virtio_uml.c` owners 0007/0011; undeclared, stale, and mismatched overlaps fail. A fresh authenticated eleven-topic replay verifies final tree `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d` and the 162/13 delta policy. |
| Bounded cleanup and coverage | Network restart now deterministically pauses with a queued transmit and proves that no prior-session frame executes after reopen. The NEON inventory input lives under `tests/unit/arm64`. The source audit covers 1,060 files: 60 above an advisory guideline, 42 without optional review context, and zero changed since review. |

### Sixth-audit closure verification

| Check | Fresh result |
| --- | --- |
| 16 KiB host graph | build 1130/1130; unit 562/562; tooling 23/23 |
| 4 KiB host graph | build 1070/1070; unit 563/563; exact-evidence validator 1/1 |
| Sanitizers | full ASan+UBSan 562/562; focused native 4 KiB ASan+UBSan 2/2; TSan 4/4; malformed-loader ASan+UBSan 1/1 |
| GPU gates | normal 1/1 and ASan+UBSan 1/1; both audited graphs contain 562 targets / 587 registrations |
| Network gate | rootfs-free deterministic SLiRP ARP/restart test 1/1 |
| Apple product graphs | iOS device and simulator builds/audits passed at 21 targets each; archive symbol scans found no executable-publication reference outside `jit_arena.cpp` |
| Linux/dependencies | fresh authenticated eleven-topic replay, exact final tree, 162/13 delta, topology/overlap policy, exact patch inventories, dependency manifest/negative tests, and ABI identities passed |
| Source organization | 1,060 files reported; 60 above advisory guidance; 42 without optional review context; zero changed since review |

A fresh cached-archive replay reverified the pinned signature, packaged tree,
all eleven patch identities/order, canonical final tree, and 162/13 delta. Its
temporary checkout was removed after verification. Generated timestamps,
commit/build identities, compiler notes, package layouts, and binary hashes
remain dynamic observations rather than byte-reproducibility requirements.

## Sixth independent audit: baseline findings (historically reported closed)

This was the reproduction baseline that reopened the work after the fifth
audit: four correctness/completion blockers, five enforcement or exception-
safety gaps, and three bounded cleanup items. The sixth pass reported every
acceptance requirement in these tables satisfied. The seventh audit reopened
some underlying guarantees, so the original wording is retained only as dated
implementation history.

### Baseline P1 findings (historically reported closed)

| Finding | Historical baseline evidence | Acceptance requirement |
| --- | --- | --- |
| Instruction allocation evidence is not independent | All 983 rows in `tests/unit/arm64/a64_instruction_allocations.tsv` use `ffffffff` masks, and every allocation value is byte-for-byte the encoding already present in `a64_instruction_evidence_bindings.tsv`; form and feature fields also mirror the other catalogs. `validate_a64_instruction_evidence.py` therefore checks one copy against another. A fixture that put the same word from a different architectural form into both sources printed `accepted self-consistent nonmatching architectural encoding`. The existing encoding negative mutates only one side. | Derive or import allocation masks/values from an independent architectural source, not the test bindings. Validate the selected word against that allocation/form metadata and add a negative that changes the same word consistently in every test-owned catalog. Regenerate both page-size evidence reports only after that negative fails for the intended reason. |
| The production 4 KiB native shadow is process-global and survives run retirement | Non-user `darwin_uml_vm_map_generation()` calls discard the generation and enter `darwin_uml_vm_map()` in `darwin/memory/mmu/static_api.cpp`. The 4 KiB shadow uses the process-static `native_address_space()` in `native_address_space.cpp`; `Context::end_run()` retires only the domain-owned software address space. The fresh-run loader fixture calls `darwin_uml_static_vm_*` directly and therefore bypasses the native wrapper whose state it purports to isolate. | Give native-shadow state explicit run/domain ownership or retire it completely on every normal and abnormal run exit. Add 4 KiB sequential-run, abnormal-return, and concurrent-handle tests through `darwin_uml_vm_*`; mappings, materialized host slots, protections, and bookkeeping must neither survive nor cross domains. |
| Guest-driven GPU memory-table replacement leaks queue notification descriptors | `VhostUserServer::configureMemory()` closes old memory mappings and then assigns `queues_ = {}` without closing each `Queue::callDescriptor`; queue setup transfers those raw descriptors into the array. A later valid memory-table replacement, or a failure after the reset point, loses them permanently. The current dispatch-exception test throws after receive and never invokes production reconfiguration. | Make queue notification ownership RAII or explicitly close it before every reset. Exercise real dispatch for repeated successful replacements and for each failure after reset begins; live-descriptor counts must remain constant and retry must work. |
| The two largest engine translation units still combine unrelated responsibilities | `darwin/cpu/arm64/gadgets/program.cpp` is 12,824 lines and combines the symbol catalog, selection tables, decoding, `Builder::build`, and execution entry points. `darwin/cpu/arm64/jit/jit_executor.cpp` is 11,047 lines and combines linking, publication, diagnostics, semantic helpers, emission, compilation, caches, test hooks, and lifecycle. This is the logical split required by `REFACTORING_PLAN.md`; relaxed numeric guidelines do not close mixed ownership. Both source-review rows are now reported changed since review. | Split by the stable responsibility boundaries already listed in the plan, using normal `.cpp`/`.hpp` files. Preserve decoder priority, emitted streams, hot-path layout, and benchmark behavior; keep test hooks out of production ownership. Refresh advisory review records after the logical split, not instead of it. |

### Baseline P2 findings (historically reported closed)

| Finding | Historical baseline evidence | Acceptance requirement |
| --- | --- | --- |
| Cross-engine dependency enforcement is asymmetric | `scripts/audit-cmake-file-api.cmake` rejects native-code-engine to gadget-engine edges only. A copied manifest with the reverse gadget-engine to native-code-engine edge still passed the 551-target audit. The current graph is clean, but the plan forbids every engine-to-engine edge and isolated adapter tests do not inspect dependency closure. | Classify every concrete engine target and reject every cross-engine link, object, aggregate, and include edge in both directions. Add positive isolated-link tests and negative manifest/configuration fixtures for each direction. |
| Native-shadow failure coverage proves only mapping rollback | The 4 KiB fault-injection cases call `darwin_uml_vm_map()` only. Production protect, unmap, and refresh paths have distinct allocation checkpoints and separate C wrappers, but no injected test reaches them. This is a coverage gap rather than a reproduced ordinary-input failure. | Inject every reachable occurrence through each production C wrapper. Require contained exceptions, unchanged mapping/slot/protection and sibling bytes, balanced state, and immediate successful retry. |
| `VncDisplay` can leak a live server when event-thread construction throws | `vnc_display.cpp` initializes and validates the listening server before assigning `eventThread_`. If the `std::thread` constructor throws, the enclosing object destructor is not run and raw `screen_` plus its listening socket remain active. Existing startup injection occurs before display construction. | Hold the initialized screen in an initialization guard until construction completes. Inject after listen and before thread ownership, then prove cleanup, unchanged descriptor count, port reuse, and successful retry. |
| GPU ownership coverage has no canonical rootfs-free gate | Canonical unit and unit-sanitizer presets disable the GPU. Even a GPU-enabled graph does not list `gpu-service-fd-ownership` as a required conditional, so removing its registration while leaving the manifest row conditional would pass the architecture audit. | Add a canonical host-only GPU unit workflow, including an appropriate sanitizer variant, and require `gpu-service-fd-ownership` whenever its target exists. Add the new production reconfiguration and display-construction cases to that gate. |
| One small generic Linux correction remains hidden in the broad device patch | The plan requires three generic corrections to be independent. Topics 9 and 10 are separate, but the 64-bit virtual-transport address-width correction remains an unguarded hunk in broad topic 7 (`kernel/patches/series/0007-...patch`, around lines 886-902). | Extract it into its own tiny, hash-checked, upstreamable topic; update topology, extraction decisions, next-version actions, replay identity, and exact-tree evidence. Drop it independently when a future base contains it. |

### Baseline P3 findings (historically reported closed)

- Kernel and dependency patch validators verify referenced rows but do not
  reject extra unlisted `.patch` files. Current real directories match their
  manifests, but negative fixtures with one extra patch were accepted. Compare
  the complete directory inventory with each manifest and add an extra-file
  negative.
- Network restart coverage proves receive/notification cleanup but never pauses
  the worker with a transmit queued. Add a deterministic queued-transmit
  restart case and prove no prior-session frame executes after reopen.
- `tests/arm64-neon-inventory.s` is a unit-test input referenced from
  `tests/unit/arm64/fp-simd-a.cmake` but still lives outside `tests/unit`. Move
  it under the owning unit directory and update its manifest/reference.

### Historical recommended order (superseded)

1. Repair the independent instruction oracle and its self-consistent negative;
   broad engine evidence is not an acceptance gate until this is sound.
2. Domain-own or retire the 4 KiB native shadow, then complete all native C-
   boundary failure cases and rerun both page sizes plus sanitizers.
3. Repair GPU queue/display ownership and make the rootfs-free GPU gate
   mandatory before relying on descriptor evidence.
4. Make cross-engine enforcement symmetric, then perform the planned logical
   engine-file split in behavior-neutral stages with focused code-generation
   and performance comparisons.
5. Isolate the small Linux correction, enforce complete patch inventories,
   close the transmit-restart and unit-file-placement items, and replay from a
   pristine authenticated release source.
6. Repeat host, Apple-package, guest, and controlled performance acceptance.
   Rootfs management remains explicitly out of scope.

### Sixth-audit baseline verification (historical)

| Check | Fresh result |
| --- | --- |
| 16 KiB host unit selection | 562/562 passed in `.build/audit-round6-main-unit16` |
| 4 KiB host unit selection | 563/563 passed in `.build/audit-round6-main-unit4k` |
| Raw tooling selection | 22/22 passed after evidence production; the self-consistent oracle reproduction above still passed unexpectedly |
| Architecture/source audits | The then-current graphs passed (551-target base and 562-target GPU graph); the advisory source report covered 1,034 files, 53 above guideline, 42 without optional review context, and six changed since review |
| GPU-enabled host fixture | A fresh default-prefix graph built; the then-current `gpu-service-fd-ownership` passed 1/1 but lacked the replacement and post-listen cases above |
| Sanitizers | Fresh canonical loader ASan+UBSan passed 1/1; fresh canonical TSan selection passed exactly 4/4 |
| Linux, ABI, and dependencies | Ten-patch exact replay still matches tree `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d`; 162/13 policy, both page-size imports, topology/action fixtures, ABI identities, pinned-source dependency tests, pristine LibVNCServer 0.9.15, verified default prefix, and final-link allowlist passed |
| Guest/performance campaigns | No guest boot, LTP, browser, or benchmark was run in this audit; these green host results are not performance evidence |

## Fifth independent audit: historical closure claim (superseded)

The following repository-owned P1, P2, and P3 rows were reported closed at the
end of the fifth pass. The sixth audit above found missing semantic independence,
production-path isolation, ownership, enforcement, and logical-split coverage,
so this section is a dated implementation record rather than current closure.
Its detailed tables retain the baseline wording and the acceptance requirements
that were believed satisfied at that time.

| Area | Closure |
| --- | --- |
| Exact ARM64 evidence and engine boundaries | A checked-in 983-row independent allocation catalog validates encoding masks/values separately from binding labels. Evidence records exact required and observed host features; six negatives cover encoding-only and unrelated-feature mutations. Shared native helpers moved to `uml_arm64_execution_helpers`; native-code targets contain no gadget objects, links, symbols, or headers, and the architecture audit now rejects that edge. |
| Runtime publication and admission | Context and provider entries use deferred, allocation-free commit with rollback of every uncommitted mutation. A provider-B failure leaves A unchanged, B retryable, and only a committed overlap fail-closed. Stop mutation and queued-run admission share one mutex, with deterministic post-admission/wait-boundary coverage proving a queued stop returns before the active run is released. |
| MMU correctness | Software mappings, history, and invalidation updates are exception-atomic under deterministic Nth-page, Nth-operation, and history-publication failures. Batches retain exact generation-state identity and reject retired/replaced state with `-EAGAIN`. Native 4 KiB entry points contain C++ exceptions and roll back mappings, containers, and sibling state after every injected allocation site. |
| GPU ownership | Received `SCM_RIGHTS` descriptors acquire fixed-capacity RAII ownership immediately after `recvmsg` and release only after successful transfer. Repeated malformed-version, oversized, truncated, dispatch-exception, allocation-failure, and valid-transfer cases pass without leaks or double-close. Startup failure injection is compiled only into a test-owned service variant. |
| Reproducible gates and policy | Reset, evidence producers, and validation form standalone deterministic presets; raw tooling is non-destructive. A canonical loader ASan+UBSan preset passes. Next-version policy detects applied, already-contained, and genuine-conflict states and emits distinct carry/drop/retest/upstream actions, with fixtures for each outcome. ABI/provider/TSan documentation and ADRs match the implementation. |
| Dependencies and Apple products | The default private prefix was rebuilt from pinned release inputs and passes receipt, payload, architecture, platform, deployment-target, and static-only verification. macOS, iOS-device, and iOS-simulator archives pass architecture and raw/install-tree C/C++ consumer checks. Device/simulator archives expose executable publication only through the explicit JIT arena; interpreter and gadget paths contain neither W+X nor RW/RX publication. |

### Audit-baseline P1 findings (closed)

| Baseline finding | Baseline reproduction | Acceptance requirement (satisfied) |
| --- | --- | --- |
| Exact instruction evidence does not prove encoding-to-form identity or exact native-feature skips | `tests/tooling/arm64/validate_a64_instruction_evidence.py` compares two copies of the same form label and checks only that an encoding has eight hexadecimal characters. It never validates the word against independent allocation/form metadata. Its evidence schema accepts `skipped-host-feature` without the required or missing feature. The negative fixtures change the label rather than keeping both labels correct and changing only the encoding. The real validator accepted `0b220027` self-labelled as `ADD (shifted register)` and also accepted an intentionally unrelated inventory feature requirement. | Validate each encoding with canonical decoder masks/allocation metadata independent of the checked-in binding label. Add a negative fixture that changes only the encoding. Record the exact required feature and observed host capability in binding/evidence rows and compare them with `native_feature_requirements`; continue interpreter and gadget execution when native execution is skipped. Regenerate both page-size reports. |
| The native-code engine still depends on the gadget engine | The plan forbids engine-to-engine edges, but `darwin/cpu/CMakeLists.txt` links both native-code targets to `uml_arm64_gadgets`, and `uml_cpu_jit_component` embeds gadget objects. `jit_executor.cpp` calls a helper defined in `gadgets/memory_transfer.cpp`. The File API audit passes because `scripts/audit-cmake-file-api.cmake` permits every same-layer edge before applying any target-specific rule. | Move the shared transfer primitive to a neutral CPU/memory contract target. Remove gadget links, objects, symbols, headers, and source ownership from every native-code target. Add engine-level metadata or an explicit intra-CPU prohibition so the architecture audit rejects this edge, then pass isolated-link and product graphs. |
| Context and network-provider publication are not one transaction | `Emulator::run()` creates `ActiveContextScope` before `NetworkSession`. A second context publication permanently clears the first context's fallback bit; if the second provider publication then throws, context destruction removes only the new entry and does not restore the first. The focused reproducer printed `failed=1 before=3 after=0`, proving that handle A changed from interpreter routing to fail-closed after handle B failed before entry. Existing tests fail each registry independently and miss this sequence. | Publish context/provider state jointly or roll back every uncommitted cross-registry mutation. Test context-B success followed by provider-B failure while A's unbound CPU and network callbacks work, require A to remain unchanged, and prove B can retry. Preserve fail-closed behavior only for an overlap that actually commits. |
| Software-MMU mutations are not exception-atomic | `map_locked()` inserts a multi-page mapping page by page and publishes its one change record only after the loop. A later allocation failure leaves earlier pages installed with stale sequences. `changed_locked()` increments the mapping sequence, then performs a potentially allocating history insertion, and only afterward publishes the code change. Batch operations have the same partial-application risk. Current injection fails only the first one-page/single-operation allocation. | Preallocate or roll back mapping and history mutations, and make code invalidation fail-safe once executable state changes. Add deterministic Nth-page, Nth-operation, and history-publication failures. Require no partial mappings, coherent mapping/code sequences, no stale translated code, balanced batch state, and successful retry. |
| A mapping batch can commit into a replacement generation | Batch begin stores only a numeric generation and its current invalidation epoch. Another thread can destroy and recreate that generation; batch end resolves the number again, obtains the new state, and accepts the same initial epoch, allowing operations queued for the retired state to populate its replacement. | Retain and validate the exact `GenerationState` identity, and reject a retired or replaced state with `-EAGAIN`. Queue an old mapping, destroy/recreate the generation on another thread, and prove the old batch cannot resurrect its backing. |
| The 4 KiB native-shadow path can throw through C and leave orphaned host mappings | `darwin/memory/mmu/static_api.cpp` directly calls allocating `NativeAddressSpace` methods from `extern "C"` entry points. `materialize_slot_locked()` creates a host mapping before inserting into allocating containers, and map/protect/unmap/refresh mutate incrementally. The 4 KiB native-MMU tests contain no allocation-failure coverage. | Contain every native-shadow C boundary and make host mapping/bookkeeping changes rollback-safe. Inject after host mapping and at each container-growth/multi-page point; no exception may escape, no orphan slot may remain, live sibling pages must remain intact, and retry must succeed. |
| A stop notification for a queued embedded run can be lost | Embedded admission waits on `embedded_kernel_run_changed` under `embedded_kernel_run_mutex`, but the stop predicate changes under the context mutex and notification occurs without the wait mutex. A stop can land after a false predicate check but before the wait begins. The current test signals `second_started` before calling `uml_emulator_run`, so it does not prove the second thread reached the wait boundary. | Synchronize predicate mutation with the admission mutex or use an equivalent monotonic wait primitive. Add deterministic hooks after handle admission and at the wait boundary; a stopped queued run must return before the active run is released. |
| Malformed GPU service messages leak received descriptors | `VhostUserServer::ReceivedMessage` owns raw descriptor integers, and the only close loop is after successful dispatch. Twelve valid-version unsupported requests carrying one descriptor each increased the live service descriptor count from 9 to 21. Invalid-version, oversized, truncated, allocation-failure, and dispatch-exception paths can leak before or after ancillary-data extraction. | Give every received descriptor RAII ownership immediately after `recvmsg`, with explicit release only on successful ownership transfer. Repeated malformed-version, oversized, truncated, unsupported-request, and injected-allocation cases must leave the descriptor count unchanged and valid transfers must neither leak nor double-close. |

### Audit-baseline P2 findings (closed)

| Baseline finding | Acceptance requirement (satisfied) |
| --- | --- |
| Instruction-evidence test orchestration is order-dependent | A raw `tooling` label run selects both reset and validation but not unit producers; it failed 21/22 after reset erased the evidence. The supported reset-excluding selection passes only when a complete unit run has already populated the directory. Give reset, producers, and validation an explicit deterministic pipeline so every advertised preset has clear standalone/precondition semantics, stale producer files are removed before production, strict unit selection stays Python-free, and tooling cannot erase evidence it is about to validate. |
| Malformed-loader sanitizer coverage has no canonical preset | The normal ASan preset disables the kernel runner and selects only `unit`, while the malformed-loader fixture is conditional on the kernel runner and labelled `tooling`. The previously created targeted ASan artifact still passes 1/1, but the documented canonical ASan commands register no loader test. Add a checked-in targeted loader ASan configure/build/test preset or an equivalent locally reproducible gate. |
| Next-version policy is classified but not acted upon | `verify-series-policy.py` validates that policies such as `drop-when-base-contains-fix` are spelled and assigned consistently, but `check-next-version.sh` unconditionally applies every patch and reports only a generic conflict. Make the checker identify already-contained fixes, distinguish the recorded policies, and emit the required drop/retest/upstream action per affected path. Add fixtures for an already-contained fix and a genuine conflict. |

### Audit-baseline P3 findings (closed)

- Move `uml::gpu::testing::StartupFailure`, its global atomic, and failure
  checkpoints out of the production GPU library or compile them only into a
  module-owned test variant.
- Correct `kernel/abi/README.md` to describe the independently versioned public
  API and kernel bridge, their separate histories, and both update procedures.
- Amend or supersede ADRs 0002/0003: the application owns and stops the injected
  network provider, while the installed callback context is caller-owned and
  has no stop callback.
- Correct `docs/testing.md`: the then-current TSan label selected four tests, not five,
  and its evidence-reset fixture is not selected.

### Implemented order

1. Preserve the dirty-tree baseline and split work by invariant, without
   committing or staging.
2. Repair the independent instruction form/feature joins and the forbidden
   native-code-to-gadget edge first; these determine whether the broad engine
   matrix is meaningful.
3. Fix joint registry publication and queued-stop admission, then add the exact
   interleaving tests before changing other lifecycle code.
4. Address software-MMU transaction/history behavior, stale batch identity, and
   4 KiB native-shadow containment as one memory-correctness campaign. Run both
   page sizes and sanitizer variants after every sub-step.
5. Give received GPU descriptors RAII ownership and move the startup-failure
   seam out of production.
6. Make evidence and loader-sanitizer workflows reproducible, execute the
   next-version policy actions, and correct the bounded documentation drift.
7. Rebuild the default private dependency prefix, repeat all host/product and
   guest acceptance matrices, and only then rerun controlled benchmarks.
   Rootfs-management redesign remains deferred.

### Fifth-audit reported verification (historical)

| Check | Dated result |
| --- | --- |
| Deterministic 16 KiB evidence pipeline | architecture/build targets passed; reset, 561 producers, and validation passed 563/563 in 286.37 seconds |
| Deterministic 4 KiB evidence pipeline | architecture passed with 553 targets / 585 registrations; reset, native 4 KiB coverage, all producers, and validation passed 564/564 in 279.60 seconds |
| Exact instruction reports | both page sizes contain 983 stable IDs, 670 retired native executions, 40 executed/non-retired cases, and 273 exact host-feature skips; interpreter and gadgets continued for every native skip |
| Raw tooling-label selection | non-destructive selection passed 22/22 after evidence generation; it does not select reset or erase producer files |
| Focused concurrency and memory checks | joint registry rollback/retry, queued-stop wait-boundary, Nth-failure MMU/history, stale-generation batch, and native 4 KiB rollback cases all passed in the page-size pipelines |
| GPU descriptor ownership | the default-prefix GPU graph passed 562 targets / 586 registrations; 64 repetitions of each malformed/allocation/dispatch case plus valid ownership transfer passed 1/1 |
| Sanitizers | canonical malformed-loader ASan+UBSan passed 1/1; canonical TSan selection passed exactly 4/4 in 4.44 seconds |
| Linux, ABI, dependencies, and packages | authenticated ten-patch replay matches final tree `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d`; 162/13 delta policy, independent public/kernel ABI v4 identities, exact 16/4 KiB active-import checks, then-current manifest/action fixtures, pristine LibVNCServer 0.9.15, rebuilt default-prefix verification, the then-current 16 KiB production kernel/app final-link allowlist, and all three Apple C/C++ package-consumer graphs passed |
| Source organization | the advisory report covers 1,034 files: 53 above guideline, 42 without optional review context, and six changed since review; these are review signals, not a strict line-count gate |
| Guest/performance campaigns | no Node.js, Go, browser, benchmark, or other heavy interpreter workload was run; rootfs redesign remains explicitly deferred and external/device campaigns remain external acceptance |

## Fourth independent audit: historical closure record

These requirements were reported satisfied at the end of the fourth audit.
They are retained as a closure record; the fifth-audit section above records
the stronger joins, state transitions, and reproducible gates now in force.

### P1 requirements (reported closed)

| Original finding | Audit evidence | Closure requirement (satisfied) |
| --- | --- | --- |
| Instruction closure is not mechanically sound | `tests/tooling/arm64/validate_a64_instruction_evidence.py` explicitly bound only 183 of 983 inventory IDs; 800 rows used decoded-mnemonic/form heuristics. The then-generated report had only 902 unique evidence selections. `BASE-0009` and `BASE-0011` both bound extended-register encoding `0x2b220027`; `FPSIMD-0234` and literal-form `FPSIMD-0235` both bound immediate encoding `0x3c403c5d`. Program evidence also credited every stored word with one aggregate engine result, including words not proved retired. | Give every evidence-producing variant explicit stable-ID and architectural-form metadata. Record the exact executed/retired words and per-variant engine result; bind JIT feature skips to that exact variant. Reject undeclared duplicate bindings except true declared aliases. Add negative validator fixtures for shifted versus extended register, immediate versus literal, and unreachable program words. Regenerate both page-size reports with all 983 IDs bound without mnemonic fallback. |
| Engines disagree on guest-scoped handling of unimplemented instructions | At block start, JIT mapped `Action::unsupported` to `illegal_instruction`, while interpreter and gadgets returned a fatal emulator event for a valid but unimplemented encoding. The shared contract tested only the interpreter's then-current behavior. The required policy became that an unsupported instruction fails the guest program without stopping the emulator. | Run `0xd5000000` through every available engine. Each must return the normal guest illegal-instruction event with zero retirement and the faulting PC preserved, without falling back to another engine. Then execute known-good guest work in the same emulator instance to prove it remains usable. Keep internal diagnostics capable of distinguishing an invalid encoding, unavailable virtual feature, and implementation gap without changing the guest-scoped outcome. |
| Same-handle embedded runs violate the public fail-fast contract | `darwin/emulator/src/emulator.cpp` takes the process-wide embedded-run mutex before `RunScope`. A second call on the same handle waits, then runs, although `uml/emulator.h` says concurrent same-handle runs fail. A queued different-handle embedded run also has no defined stop-aware admission behavior. | Deterministically block one embedded run and require a second call on the same handle to return `-UML_EMULATOR_RUNTIME_ERROR` before the first is released. Add a separate queued-handle stop test and define/correct cancellation semantics without weakening process-wide embedded serialization. |
| Software-MMU state is neither handle- nor run-isolated | `darwin/memory/mmu/address_space.cpp` owns a process singleton keyed only by numeric generation; kernel mappings collapse to generation zero, and run/handle retirement does not clear it. Concurrent native-backed handles and sequential embedded kernels can therefore collide or inherit mappings. | Two concurrent handles must map the same generation/address to distinct backings and read their own data. A new embedded run must start without previous user or generation-zero mappings. Cross-thread generation destroy/recreate must resolve only the new canonical state. Route the required context through the bridge without restoring dependency cycles. |
| Embedded ELF loading can overwrite host mappings and trusts unbounded metadata | `darwin/emulator/loader/elf_loader.cpp` uses destructive `MAP_FIXED`, omits `munmap()` on success and error paths, and does not fully bound dynamic entries, relocation tables, symbol/string indexes, destinations, or address arithmetic to loaded segments. | Pre-map a sentinel at the linked window and prove loading refuses without modifying it. Unmap every successful and failed load. Exercise malformed dynamic/relocation images under sanitizers and return status 126 without out-of-range access or arithmetic overflow. |
| Network-enabled installed static library is incomplete | `uml_emulator` references `uml_slirp_net_*` when networking is enabled, but `darwin/emulator/CMakeLists.txt` installs only `libuml_emulator.a`; static linking to `uml_network_provider` does not fold its objects into that archive. A fresh raw consumer fails with seven undefined provider symbols. Packaging currently masks this by building slices with networking off. | Either move default-provider ownership to the app and inject neutral callbacks, or export/package the provider and complete third-party closure. Add network-enabled raw-archive and install-tree consumers. Include a genuine C consumer and encode or document the C++ runtime link requirement. |
| Active context/provider registries are not strongly exception-safe | `ActiveContextScope` and `ActiveProviderScope` clear existing fallback flags before a potentially throwing `vector::push_back()`. Publication also precedes engine validation and argument/environment allocations, contrary to ADR 0002's stated guarantee. | Insert the new registry entry before mutating prior entries, or use a rollback-safe representation. Inject allocation and pre-entry failures while handle A has working unbound CPU/network callbacks; A must remain usable and prior registry state must be unchanged when publication fails. |
| Public and kernel-internal ABI identities are coupled and the bridge remains wider than Linux uses | Declaration fingerprinting combines installed `uml/emulator.h` with the internal host/kernel bridge. `darwin_uml_set_execution_engine`, `darwin_uml_get_execution_engine`, and `darwin_uml_jit_available` remain in the canonical bridge although current Linux imports none of them. | Split public API and kernel-bridge identities/histories. Remove unused engine controls from the synchronized bridge and use internal test seams. Verify the bridge against the kernel's actual undefined-symbol set; prove internal-only changes do not churn the public identity and public-only changes do not change the Linux replay tree. |

### P2 requirements (reported closed)

| Original finding | Closure requirement (satisfied) |
| --- | --- |
| An undersized SLiRP read removes the frame but leaves its readiness byte | Retain the frame for retry or consume frame and signal atomically. After the queue is empty, polling the IRQ fd must not remain readable. Add a short-buffer/retry/readiness unit test. |
| MMU C entry points can unwind C++ allocation exceptions through Linux C frames | Fault-inject backing, batch, generation, and mapping allocations. Every C entry point must return a defined error, balance batch/walker state, leak no VM mapping, and permit immediate retry. |
| Linux replay accepts topology not represented by the ten-patch manifest | Require the exact commit count, one nonempty patch ID per commit, expected subjects, and no merge commits. Add negative tests for empty, extra, reordered, and merge commits. Preserve exact final-tree verification as a separate invariant. |
| Next-version checking bypasses canonical replay policy | Make `check-next-version.sh` share manifest hashes/subjects/patch IDs, isolated Git identity, delta ownership, extraction decisions, and per-path next-version policy. Mechanically validate `extraction-decisions.tsv`; it is currently descriptive only. |
| Runtime aggregation still crosses directory scope as raw objects | Replace the `UML_RUNTIME_CORE_OBJECTS` `PARENT_SCOPE` export with target-owned composition. Either make `uml_runtime_core` a real consumed/installed boundary or remove the unused archive. Keep the narrow JIT test variant rather than restoring monolithic recompilation. |
| Test ownership still has conditional coupling | Make integration with tooling disabled and external-network tests enabled a valid configure: the VNC client is currently tooling-owned but integration-referenced. Move/guard the standalone GPU fixture according to its actual test owner. Keep the evidence reset fixture from being pulled into a strict unit-only selection. |
| Public/internal API documentation is inconsistent | Amend or supersede ADRs 0002/0003 to distinguish concurrent native handles from serialized embedded runs. Move the conditional JIT profiling declaration out of the installed public header or deliberately stabilize it. Correct the manifest's “read-only” description for the evidence validator, which writes `stable-id-bindings.tsv`. |
| GPU service file-descriptor ownership is not exception-safe | Replace raw socket-pair integers with RAII ownership and explicit release at transfer. Repeated forced startup failures must leave the process FD count unchanged. Keep host-only GPU fixtures out of production ownership. |
| Dependency/link provenance enforcement is denylist-based at the final link | Change the final-link audit to allow only build-tree objects, Apple SDK/toolchain inputs, and the verified private source-built prefix. Add an arbitrary ambient-path negative fixture. Keep the existing release-package, receipt, archive-member, and private-prefix checks. |
| LibVNCServer currently has a project-maintained post-release patch layer | Return the manifest to the pristine pinned upstream release, currently 0.9.15: use version `0.9.15`, patch fields `-`, and the canonical unmodified release-tree hash. Remove `dependencies/patches/libvncserver/0001-release-backports.patch` and its patch README, update the build description, rebuild the private prefix, and rerun VNC plus package consumers. Do not replace it with a moving branch snapshot. |

### P3 requirements (reported closed)

- Prune expired weak entries from the per-thread registration list and prove
  thousands of handle create/run/destroy cycles on one thread remain bounded.
- Make the network provider's implementation include directory private; expose
  only the neutral callback contract.
- Remove or explicitly reject the obsolete `DARWIN_LIBELF_INCLUDE` hook from
  the final Linux series and replace the stale package-manager-specific comment
  in the Darwin ELF compatibility header. The canonical build currently
  supplies the repository-owned header directly.
- Audit `.DS_Store` and other unconsumed non-rootfs artifacts. Rootfs payloads,
  images, maintenance tools, hashes, and locks are deferred and are not closure
  items in this refactor.

### Fourth-audit completion evidence

| Area | Final resolution and acceptance |
| --- | --- |
| Exact ARM64 evidence and engine policy | A checked-in 983-row exact catalog replaces mnemonic/form fallback. Evidence records exact entry instruction and per-engine executed/retired state; duplicates require a declared alias. Four negative fixtures reject shifted/extended confusion, immediate/literal confusion, unreachable program words, and undeclared aliases. Fresh 16 KiB and 4 KiB reports each validate 983/983 IDs with 713 JIT executions and 270 exact host-feature skips. All engines map `0xd5000000` to guest illegal-instruction with zero retirement and preserved PC, without fallback, then run known-good work in the same emulator. |
| Handles, MMU, loader, and registries | Same-handle embedded runs fail fast before process-wide admission; queued different-handle runs are stop-aware. Memory domains isolate concurrent handles and run retirement clears user and generation-zero state. The ELF loader refuses occupied fixed windows, validates all dynamic/relocation bounds, and unmaps every path; its combined fixture passes at the production link address and under targeted ASan+UBSan. Registry publication and all MMU C allocation boundaries have deterministic failure injection, rollback, leak checks, and immediate retry coverage. |
| Network and installed API | The default SLiRP provider moved to macOS app ownership and is injected through neutral callbacks, leaving `libuml_emulator.a` provider-complete in a networking-enabled configuration. Three platform slices link raw and installed archives from genuine C and C++ consumers. Short-buffer frame retry/readiness and provider publication failures are covered. Public API v4 (`0xd9a69804d3303044`) and kernel bridge v4 (`0xfe2d71a33e3d446e`) have independent histories and negative identity tests; Linux imports are checked against the narrowed bridge. |
| Composition and test ownership | Runtime closure is target-owned through `uml_runtime_core`; no raw object list crosses directory scope. Integration with tooling disabled and external workflows enabled configures and audits as 39 targets / 10 registrations, with GPU/VNC fixtures owned by integration. A strict unit-label selection does not select the tooling evidence reset. ADRs distinguish 16 KiB native concurrency from 4 KiB/embedded process-address-space serialization, and JIT profiling is internal-only. |
| GPU, link provenance, and dependencies | Socket pairs and server connections use RAII with explicit release; 64 repetitions at each of three forced startup failures leave `/dev/fd` unchanged. Final-link verification canonicalizes every object and permits only the build tree, Apple SDK/toolchain, or verified private prefix; an arbitrary ambient archive is rejected. LibVNCServer is pristine 0.9.15 with canonical tree `3b0f5deb99f726cc0cfb963b783fcc396017fde8e646240a9ed3b01c18077a35`; a fresh transactional prefix, loopback VNC smoke, and package consumers pass. |
| Linux replay and bounded cleanup | Manifest topology enforces ten exact nonempty stable patch IDs, subjects/order, and no merges, with empty/extra/reordered/merge negatives and independent exact-tree verification. The authenticated 6.18.41 release replays to `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d` across the bounded 162-path delta. Next-version and extraction policy share the canonical verifier. `DARWIN_LIBELF_INCLUDE` and its stale package-manager comment are gone. Weak TLS registrations stay bounded through 3,000 cycles, network implementation includes are private, and root `.DS_Store` is removed and now rejected by the source audit. |
| iOS executable memory | Interpreter, gadgets, and MMU storage are data-only and never W+X or RW↔RX. Gadgets execute precompiled signed RX text. Device and simulator archive audits reject executable-memory publication primitives outside `jit_arena.cpp`; dynamic publication remains available only when the JIT engine is explicitly selected for entitled debugger/jailbreak use. |

At the end of the fourth audit this record stated that no repository-owned item
remained. The fifth and sixth audits successively superseded that status, and
the seventh-audit checklist now supersedes all three closure claims. Rootfs
management remains the explicitly
excluded testing redesign; the external campaigns named at the top remain
evidence prerequisites rather than source-refactor tasks.

## Third independent audit: historical closure record

This table records what the third audit accepted at the time. It is retained so
a new chat can understand the work already performed, but it is no longer the
authoritative closure checklist; the seventh-audit section above supersedes any
conflicting statement.

| Audit finding | Final resolution and acceptance |
| --- | --- |
| Same-handle run retirement | Run admission stays closed through execution-state retirement. Generation-scoped registrations and a deterministic retirement/admission hook prevent the previous run from erasing state acquired by the next. The lifecycle test passes normally, under targeted TSan, and in repeated execution. |
| Public multi-handle contract | Embedded-kernel runs are explicitly process-serialized because the fixed linked window and software MMU are process-wide. The installed header documents that narrower contract; a test runs two real embedded-kernel handles through the loader and proves serialization. Native callback-only handles retain documented independent concurrency. |
| Fresh integration builds | Guest helper compilation uses an explicit compiler selected independently of the CXX-only host graph. A generated-file verifier makes a missing helper a hard build failure. Fresh 16 KiB and 4 KiB integration configurations build their apps and audit their graphs. |
| Optional profiling configurations | Option-conditioned test-manifest rows and required registrations cover both JIT GPR-link modes. Fresh link-profile and pressure-profile configurations pass 2/2 and 1/1 respectively. |
| Complete architecture enforcement | The File API audit now rejects undeclared first-party targets, checks the reverse target map and object composition, traverses pure-interface cycles, validates semantic test owners, and runs in product-only builds. Unit/tooling, both integration page sizes, macOS product, iOS device, iOS simulator, profile, and TSan graphs pass. |
| Dependency verification invariant | Manifest, policy, receipt, payload, and verifier inputs trigger build-time/pre-link revalidation. Every archive member is checked for arm64, macOS platform, and minimum OS. Direct device dependencies and the final production link map are audited; the fresh private prefix and both integration apps pass. |
| Linux final-state series | The signed Linux 6.18.41 package now replays 10 final-state topic patches to exact tree `8adb106de7c19ed9e6c83f622a48e54629053699`. The two pending-upstream fixes are isolated topics. Policy owner/disposition, origin-patch membership, allowed next version, 162 changed files, and 13 outside-port files are mechanically checked. Authenticated replay, both kernel builds, a 16 KiB bounded boot set, and a 4 KiB lightweight boot pass. |
| Network queue reuse | SLiRP receive/transmit rings reset under their locks at stop. A two-run stale-frame/IRQ test passes. Active fallback is context-scoped and remains disabled after overlapping-run ambiguity, eliminating the provider-retirement regression found by repeated API testing. |
| Public error codes | The installed API states that functions return zero or the negation of the positive enum value. An external consumer checks the negative result, and the declaration change is bound by ABI v3. |
| Target-owned tests and suites | Only the JIT executor is rebuilt for its test macros; runtime guest selection is reused through module-owned objects. Tooling-only, unit-only, integration-only, sanitizer, and composed presets are explicit. Manifest-owner validation resolves the registered command/source rather than accepting any existing path. |
| Slim ABI and extraction decisions | Host-internal concrete engine entry points and the dead VM queries were removed. ABI v3 binds the synchronized declarations. The remaining `arch/um/drivers` and `scripts` paths have explicit keep/upstream ownership and extraction rationale in the hardened delta policy. |
| Direct device/dependency ownership | GPU renderer/display frameworks and the network provider's concrete library are linked by their direct owners. Imported targets match actual production use; retained auxiliary archives are enumerated and rejected if undocumented. |
| Audit, debris, and documentation | Cache/debris checks cover kernel, dependencies, Darwin, CMake, scripts, tests, and tools. Obsolete artifacts/clones/backups and dead CMake branches were removed. Documentation records that instruction validation rewrites only `stable-id-bindings.tsv`, and the architecture graph includes the app-to-device edge. |

The third-audit checks verified exact Linux tree
`8adb106de7c19ed9e6c83f622a48e54629053699`, ABI v3, 10 patch identities,
162 changed and 13 outside-port files, the schema-2 dependency prefix, all
then-selected Apple product graphs and network-disabled package consumers, and
the immutable rootfs contract. The fourth audit's network-enabled consumer
failure and replay-topology gaps qualify those historical results.

## Decisions applied during closure

- The 800/600/300-line guidelines are advisory, not hard limits. Every
  project-owned production, test/support, and CMake source is reported, while
  cohesive or order-sensitive files may remain larger without an exception.
  Optional context records an owner, factual size at review time, rationale,
  and date in
  [`docs/source-size-reviews.tsv`](docs/source-size-reviews.tsv).
- Interpreter integration is deliberately lightweight: shell arithmetic and
  CPU/HWCAP inspection. Heavy Node.js, Go, browser, and benchmark workloads are
  not run through the interpreter.
- Interpreter state, gadget programs, and software-MMU storage are data-only.
  They never use W+X memory and never perform RW-to-RX or RX-to-RW transitions.
  Gadget code is precompiled signed RX text. Dynamic executable publication is
  confined to the explicitly selected JIT path for entitled debugger or
  jailbreak use; a non-JIT engine cannot reach it.
- Reproducibility means pinned official release archives and signatures,
  patch content and order, ABI identity, and verified prepared source trees.
  Timestamps, committer dates, build user/host, compiler notes, generated build
  identities, package layouts, and output hashes are dynamic observations and
  need not be byte-for-byte deterministic.
- When behavior changed unexpectedly, the focused working-tree diff was
  compared with the pre-refactor implementation. That recovered the systemd
  overlay contract, WFET/WFIT instruction behavior, SLiRP context ownership,
  and the correct atomic-alias test semantics.

## Earlier closure record

This section records the broad implementation state established before the
third audit. It is historical; the seventh-audit checklist is authoritative where
details differ.

The following table records the earlier closure snapshot from the
2026-08-03 tree.

| # | Resolution |
| ---: | --- |
| 1 | Translated execution now owns `begin_execution()` with an RAII guard; throwing-executor tests prove exactly one cleanup and successful re-entry. |
| 2 | Interpreter address-space exceptions are caught at the `noexcept` boundary and translated to `ExitReason::fatal`; throwing-memory tests cover it. |
| 3 | Architectural-state materialization and engine availability live behind neutral CPU contracts; runtime no longer includes concrete JIT, gadget, or CPU-detail helpers. |
| 4 | Host-only parsers, validators, protocols, launchers, fixtures, and inventories now live under tooling; unit owns native behavior and the instruction inventory; integration registrations require a guest/rootfs. |
| 5 | All 983 stable instruction IDs bind to registered CTests and executable encodings, with interpreter/gadget evidence and either JIT execution or an explicit host-feature skip for every row. |
| 6 | External-network, VNC, rendered-WebGL, and native-4K conditional rows are mandatory whenever their owning options/suites are enabled; the architecture audit checks the manifest. |
| 7 | CPU/MMU sources were split by responsibility. The order-sensitive gadget decoder, JIT coordinator, assembly banks, and other cohesive logical units remain intact where splitting would harm clarity, generated-code stability, or measured layout. |
| 8 | The File API audit normalizes include paths, verifies actual source ownership and object composition, covers root targets, and rejects forbidden edges/cycles/publication primitives. The former umbrella bridge is split by public contract. |
| 9 | Gadget, interpreter, JIT, and factory targets declare their concrete direct dependencies. Isolated-link tests prevent aggregation from masking missing edges. |
| 10 | Host capabilities use a neutral platform contract; Apple platform code no longer imports CPU-owned types, and the audit rejects that reverse dependency. |
| 11 | Canonical build/check targets run architecture, durable test-manifest, rename/removal-ledger, advisory source-size reporting, and executable-memory audits. Removing both a test and its current row is detected against the preserved inventory. |
| 12 | Engine choice, stop state, network provider/callbacks, and translated caches are owned by each public emulator handle. Run admission/stop is synchronized, host-thread state retires safely, and deterministic race/churn/reuse tests pass. Lifetime and stop semantics are recorded in ADRs 0002 and 0003. |
| 13 | Fresh 16/4 KiB normal, sanitizer, integration-fast, architecture, and Apple matrices were regenerated under `.build/results/refactor/final-20260803`; no stale JUnit is closure evidence. |
| 14 | Configure and build verification require a clean canonical Linux source tree with the expected exact tree and audited delta, and have no-fetch operation for already prepared sources. |
| 15 | A fresh authenticated `linux-6.18.41.tar.xz` release package replays 10 hash-checked final-state topic patches and verifies exact final tree `8adb106de7c19ed9e6c83f622a48e54629053699`. The 13 outside-port changes are classified and kept within audited ceilings. Dynamic local replay commit/build metadata is recorded only as observation. |
| 16 | Production dependencies are prepared from pinned official release packages with authenticated archive, patch, and final-tree identities. The private-prefix receipt binds that manifest and the build policy; verification rejects an altered or extra payload, non-archive or shared-library content, the wrong architecture or deployment target, and ambient/staging paths. |
| 17 | `UMLEmulator.xcframework` was the sole supported Apple aggregate name in this snapshot. Its macOS, iOS-device, and iOS-simulator arm64 archives and external consumers were rebuilt together; those artifacts are dated and the older local aggregate is not evidence. |
| 18 | One generated ABI identity is checked across normalized canonical/public declarations, declaration history, Linux patches/replay, package headers, configure, and external-consumer builds; declaration drift requires a derived fingerprint and explicit version/history update. |
| 19 | Empty residual `darwin/loader` and `darwin/probes` directories are gone, and the source audit now rejects empty residual architecture directories. |
| 20 | Standalone Python entry points suppress source-adjacent bytecode caches; the source audit rejects regenerated cache/debris directories. |
| 21 | Overlay, maintenance, GPU, VNC, and rendered-workflow launchers use the canonical `.build/presets/macos-integration` defaults. |
| 22 | Architecture documentation names `assembly_offsets.h`; documentation link/path validation prevents stale moved-file references. |
| 23 | Production, generated/declarative, test-support, and CMake sources are all reported under advisory guidelines. Optional review records retain context for cohesive units without turning reviewed size into a future ceiling. |

## Previous audit implementation record

The third audit extended the lifecycle/isolation, target-composition, and
audit-coverage work in this table. It remains a record of implemented fixes;
the fourth audit identifies where their acceptance was incomplete.

| # | Final resolution |
| ---: | --- |
| 1 | `Context` now serializes run admission, stop requests, and run retirement. A deterministic admission-boundary hook proves that a stop issued after admission cannot be erased and that an idle stop is not inherited by a later run. |
| 2 | Per-thread execution state lives in a shared store with generation-tagged weak TLS registrations. Host-thread exit and `end_run()` retire state without allowing stale cleanup to erase a reused registration; churn tests enter the engines and verify bounded cleanup and same-thread reuse. |
| 3 | Official, versioned dependency release packages are authenticated by archive/patch/final-tree identity and built into the verified private arm64 static prefix. CMake and Darwin kernel host tools no longer consume ambient third-party libraries. Real tool versions, timestamps, package layouts, and binary hashes remain dynamic observations. |
| 4 | `kernel/abi/verify-declarations.py` comment-strips and token-normalizes every canonical host/kernel header plus installed `uml/emulator.h`. Its SHA-256-derived fingerprint and consecutive history make a declaration/layout change fail unless the ABI version/history is explicitly advanced; negative tests cover comment-only and declaration changes. |
| 5 | `architecture-audit` consumes the File API reply from ordinary configuration and never reconfigures its own build tree. The stale CTest-registration comment is corrected. |
| 6 | The public implementation copies environment strings, arguments, the configuration, callback table, and callback values before use. The installed header documents ownership plus run/stop/destroy concurrency; mutation/lifetime tests exercise the contract. |
| 7 | CPU/memory composition is target-owned, including a narrow JIT test-support object variant. Raw exported source lists and monolithic recompilation are gone. Unreachable raw stop/static-dispatch exports, host-internal engine entries, dead VM queries, and the unused `clear_stop` seam were removed through ABI v3. |
| 8 | Physical ownership now matches the test taxonomy: native behavior and the inventory are under unit, host parsers/validators/protocols/fixtures are under tooling, and only guest/rootfs execution is under integration. Unit remains Python-free and guest tests retain the immutable-rootfs wrapper. |
| 9 | The five confirmed unreferenced test artifacts were removed; the expanded source audit inventories the supported production, test/support, CMake, and declarative locations. |
| 10 | `arm64-instruction-evidence` is non-destructive to producer evidence when selected alone. Its reset/producer ordering remains in the canonical aggregate, while a standalone selection lists and runs exactly the validator without erasing producer evidence; it still rewrites `stable-id-bindings.tsv`. |

Source-size policy remains advisory. The reporter applies the documented
800/600/300 guidelines, emits a stable TSV, and never rejects a file for line
count; only malformed, duplicate, or stale optional review metadata is an
architecture error. Cohesive files may remain over a guideline.

## Initial fourth-audit reproductions and preserved earlier evidence

The following table is the historical reproduction record that opened the
fourth-audit findings. Its false-form binding and network-link failures are
intentionally retained so the fixes can be audited against the original
symptoms; they are not descriptions of the current tree. Their dated acceptance
results are in the fourth-audit completion table above; the current checklist
is the seventh-audit section at the top of this file.

These initial checks completed without booting a guest or rerunning Geekbench:

| Check | Result | Evidence |
| --- | --- | --- |
| Fresh unit-only configure and architecture audit | passed; 550 targets / 561 registrations | `.build/audit-round4-unit-only` |
| Dated unit selection with required evidence reset fixture | 562/562 passed | `.build/audit-round3-finish-unit` |
| Standalone instruction-evidence validator | 1/1 passed, but produced the false-form bindings documented above | `.build/audit-round3-finish-unit/instruction-evidence/stable-id-bindings.tsv` |
| Inventory structure | 983 verified rows: 516 Base / 467 FP&SIMD | `validate_a64_instruction_inventory.py` |
| Dependency manifest and ABI synchronization | passed | `dependencies/prepare.sh --validate-manifest-only`; `kernel/abi/sync.sh --verify` |
| Network-enabled raw package consumer | failed as expected with seven unresolved `uml_slirp_net_*` symbols | `.build/audit-round4-macos-network-consumer`; `/private/tmp/uml-audit-round4-macos-network-consumer.log` |
| Network-disabled package consumers and fresh iOS graphs | passed | fourth-audit package/build configurations |

That audit's source-size report covered 1,010 files: 53 above an advisory
guideline, 42 without optional review context, and two changed since review.
Those counts remain informational.

Preserved third-audit evidence follows:

| Gate | Result | Evidence |
| --- | --- | --- |
| Dated unit selection | 562/562 passed | `.build/audit-round3-finish-unit` |
| Repeated API/context race coverage | 50/50 each passed | that audit's unit binaries |
| Tooling/provenance after evidence production | 18/18 passed | reset fixture deliberately excluded |
| Targeted TSan | 1/1 context lifecycle passed | `.build/audit-round3-final-tsan` |
| Optional JIT link profile | 2/2 passed | `.build/audit-round3-final-profile-link` |
| Optional JIT pressure profile | 1/1 passed | `.build/audit-round3-final-profile-pressure` |
| Bounded 16 KiB guest acceptance | 5/5 passed | systemd, interpreter, gadgets, JIT, SLiRP |
| Bounded 4 KiB guest acceptance | 1/1 passed | lightweight interpreter boot |
| Architecture, unit/tooling | 551 targets / 580 registrations | fresh File API graph |
| Architecture, integration 16 KiB | 43 targets / 27 registrations | fresh File API graph |
| Architecture, integration 4 KiB | 44 targets / 27 registrations | fresh File API graph |
| Architecture, macOS/iOS products | 21 targets in each of 3 graphs | macOS, iOS device, iOS simulator |
| Signed Linux replay | archive/signature, 10 topics, final tree, and 162/13 policy passed | retained authenticated replay |
| Linux builds | 16 KiB and 4 KiB passed | Homebrew LLVM 22; dynamic build metadata accepted |
| Dependency prefix and production links | passed | fresh schema-2 prefix plus both integration apps |
| Apple aggregate/package | 3 arm64 slices, 59 members each, all external consumers linked | `.build/audit-round3-final-package` |

The audited architecture graphs had no forbidden dependency edge or cycle. The
instruction validator lists all 983 stable rows (516 Base and 467 FP&SIMD) and
passes without erasing producer TSVs; it refreshes `stable-id-bindings.tsv`.
The fourth audit proved that this passing result was not evidence that each row
or variant executed the correct form on every applicable engine. Later audits
reported exact evidence, but the seventh audit found it still records only one
selected case per stable entry and therefore does not supply per-variant proof.

The preserved 16 KiB integration selection used only lightweight shell arithmetic
and CPU/HWCAP inspection in the interpreter. The 4 KiB acceptance repeated that
bounded interpreter smoke. Heavy Node.js, Go, browser, and benchmark workloads
were not run through the interpreter, and the pressure registrations were not
selected. Gadget and JIT smokes remain separate engine tests.

The earlier broad matrices remain preserved: normal unit/tooling 580/580 at
16 KiB and 581/581 at 4 KiB, 562/562 ASan+UBSan, 5/5 targeted TSan, and fast
guest integration 24/24 at each page size. They are dated regression evidence,
not substitutes for the fourth-audit acceptance tests above.

Apple archive and package hashes are intentionally not reproducibility
identities. The verified slice platforms, architectures, public headers, member
composition, executable-memory policy, and external links are the acceptance
invariants.

Historically, the rootfs finished mode `0444`, size 8,589,934,592 bytes, SHA-256
`501770dcce8223a3b7b41c475413ccb5f733291e190dcb23964890b50dafc0e5`.
This is retained as evidence, not as a current refactor gate.

## Linux and dependency provenance

The Linux series starts from the signed official `linux-6.18.41.tar.xz`
release package (SHA-256
`17fc72f0f8d4a8a8633a5d20085f5d9c5a5ec51ee896a0b7ae1ec25da31273ea`),
authenticated with the detached signature and pinned full signer fingerprint
`647F28654894E3BD457199BE38DBBDC86092693E`. Its packaged tree is
`3098cc6dba43578488a8f7f486797af8046ce8b8`; the official archive intentionally
export-ignores `Documentation/.renames.txt`, the sole difference from upstream
tag tree `9b4299714b8020ae57ca8c6310d747c2299aea0f` at tag commit
`2fe596715f840d053aed5cee5455f701bdcd2b50`. The 11 final-state topics in
`kernel/patches/series/manifest.tsv` reproduce final tree
`834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d`. Local import/replay commit IDs and
their real creation times are observations, not canonical identities. The
Darwin host-service topic has stable ID
`14c6b921eac5574938073b9aca3006fc5f9cac14` and SHA-256
`589f1c85669095f43f4a4bf9d6e1d71f6b6923ec0011b65e0711cbf1745b22f0`.
The independent DMA-mask topic has stable ID
`4293e280ee408165ae454f97819e652e00906581` and SHA-256
`b9f1494ad577cbc2bbe62f34daaac27e53f86b9d03a196fbed928be0872aebce`.

Fresh replay downloads only the pinned release archive and signature from
kernel.org, verifies their SHA-256 identities and exact OpenPGP signer,
verifies the packaged tree, applies the manifest, then verifies the exact
final tree and delta. Its isolated GnuPG home uses a short securely-created
temporary path so deep case-sensitive replay roots cannot exceed the agent
socket limit. Prepared-source verification is offline. The opt-in next-version
conflict audit alone uses Git because arbitrary development refs do not have
release packages. Kernel and external dependency records keep dynamically
generated build metadata as observations; verification is anchored to signed
source/patch content and the generated ABI identity.

The installed API and kernel bridge have independent declaration identities.
Public API v4 is fingerprint `0xd9a69804d3303044`, declaration SHA-256
`d9a69804d33030448b780d58c3f553c0e12e4f1e19ab37f89d2719b8b93115f1`;
kernel bridge v4 is fingerprint `0xfe2d71a33e3d446e`, declaration SHA-256
`fe2d71a33e3d446ec929bad4ef91d8f7b59fa2c1718bbd3d137cb24627ba175b`.
Their separate consecutive histories and negative tests require an explicit
identity advance for a declaration or layout change in the affected boundary.

## Dated Apple executable-memory policy evidence

The device and simulator archives built for the sixth audit contained executable-publication
references only in `cpu/arm64/jit/jit_arena.cpp`. Neither archive has
`mprotect`, VM-protect, JIT write-protect, or instruction-cache publication
references outside the JIT arena; production archives contain no
`jit::testing` support. This preserves JIT for explicitly entitled use without
making RW/RX transitions part of interpreter, gadgets, the software MMU, or
the normal sandbox-compatible iOS path. These archives predate the final source
split and must be rebuilt before they can be described as current artifacts.

## Preserved performance result

The fresh source-built-dependency 16 KiB gadget Geekbench result is
**80 single / 506 multi**
(result). It used the
Release executable with SHA-256
`949a6ef31838e87b3d894dda96c43592a4878a2ac98f79c63dba8b6b7b700af6`, 10
guest cores, 8 GiB, systemd, network access, and the immutable lower image
under tmpfs OverlayFS. Relative to the immediately preceding 77/508 run, the
aggregate deltas are **+3.90% / -0.39%**. Relative to the earlier G79 79/506
point they are **+1.27% / 0.00%**. This is the best preserved single-core
aggregate, with multi-core effectively flat; there is no broad gadget
regression.

The earlier single-core PDF anomaly resolved on the clean repeat: 63 rose to
86 (+36.51%), while multi-core PDF rose from 535 to 547. Single/multi compiler
scores changed from 48/392 to 51/385. The largest individual multi-core decline
was HTML5 Browser at -4.58%; it is outweighed at aggregate level and is not a
reason to reject the result under the workload-sensitive comparison policy.

The console is
`.build/results/refactor/audit-2026-08-03-release-deps/geekbench-gadgets-16k-console.log`,
SHA-256
`31df1439d98fe6b4773b9a6c05c00f85078e92e2cea701a44b844fe91bf7b86c`.
Repeated matched gadget/JIT campaigns remain external acceptance, as below.

## External acceptance not claimed

These require inputs outside the repository and are intentionally not local
pass claims:

- full per-engine LTP with a suitable guest image containing the LTP payload;
- physical sandboxed and entitled iOS runtime campaigns;
- opt-in public-network, interactive KDE/VNC, rendered WebGL, and browser
  campaigns; and
- repeated controlled gadget/JIT performance medians.

Their prerequisites and commands are documented in
[`docs/testing.md`](docs/testing.md). Missing external inputs do not leave a
repository item open by themselves, but they also do not close the repository-
owned seventh-audit checklist above. Rootfs provisioning, identity, locking,
and maintenance are intentionally excluded until the testing-image redesign.
