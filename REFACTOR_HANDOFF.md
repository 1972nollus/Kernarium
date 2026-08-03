# Refactor handoff

This is the authoritative status as of 2026-08-03 after a fourth independent
audit. The refactor is **not complete**. The fourth audit found repository-owned
correctness, packaging, isolation, and audit-coverage gaps that invalidate the
previous closure assertion. The third-audit matrices remain useful dated
regression evidence, but passing them is not acceptance for the open items
below. Work needing credentials, special images, physical devices, public
services, or controlled performance hardware remains external acceptance and
is not represented as a local pass.

[`docs/refactor-evidence.md`](docs/refactor-evidence.md) is the evidence
index. [`REFACTOR_PROGRESS.md`](REFACTOR_PROGRESS.md) is the compact summary.

## Working-tree safety

The outer repository is based at
`618b68658013b83394efef367570fa2c36113eb0`, but the refactor remains an
intentionally dirty, unstaged/untracked working-tree move. No refactor file was
committed or staged. Do not commit, stage, reset, clean, broadly check out, or
otherwise rewrite it without separate authorization. Preserve the pre-existing
staged deletion of the former `linux-6.18` gitlink.

Never mutate `rootfs.img` during ordinary validation. It is the immutable lower
image. Guest tests use `tools/rootfs/with-verified-rootfs.sh` and an in-memory
overlay; writable maintenance remains a separately authorized recovery flow.

## Fourth independent audit: authoritative open work

The next chat should address these items in priority order. Do not mark the
refactor complete merely because the existing unit, architecture, ABI, replay,
or package checks pass: several findings are gaps in those checks themselves.

### P1: correctness, isolation, and release blockers

| Finding | Evidence | Required acceptance |
| --- | --- | --- |
| Instruction closure is not mechanically sound | `tests/tooling/arm64/validate_a64_instruction_evidence.py` explicitly binds only 183 of 983 inventory IDs; 800 rows use decoded-mnemonic/form heuristics. The current generated report has only 902 unique evidence selections. `BASE-0009` and `BASE-0011` both bind extended-register encoding `0x2b220027`; `FPSIMD-0234` and literal-form `FPSIMD-0235` both bind immediate encoding `0x3c403c5d`. Program evidence also credits every stored word with one aggregate engine result, including words not proved retired. | Give every evidence-producing variant explicit stable-ID and architectural-form metadata. Record the exact executed/retired words and per-variant engine result; bind JIT feature skips to that exact variant. Reject undeclared duplicate bindings except true declared aliases. Add negative validator fixtures for shifted versus extended register, immediate versus literal, and unreachable program words. Regenerate both page-size reports with all 983 IDs bound without mnemonic fallback. |
| JIT confuses emulator coverage gaps with guest-illegal encodings | At block start, `darwin/cpu/arm64/jit/jit_executor.cpp` maps every `Action::unsupported` to `illegal_instruction`; interpreter and gadgets return `fatal` for a valid but unimplemented encoding. The common contract tests this distinction only for the interpreter. | Run `0xd5000000` through all available engines in the shared contract. All must report `fatal` with zero retirement, while a genuinely undefined encoding remains `illegal_instruction`. Translation must distinguish invalid encoding, unavailable virtual feature, and missing emulator implementation. |
| Same-handle embedded runs violate the public fail-fast contract | `darwin/emulator/src/emulator.cpp` takes the process-wide embedded-run mutex before `RunScope`. A second call on the same handle waits, then runs, although `uml/emulator.h` says concurrent same-handle runs fail. A queued different-handle embedded run also has no defined stop-aware admission behavior. | Deterministically block one embedded run and require a second call on the same handle to return `-UML_EMULATOR_RUNTIME_ERROR` before the first is released. Add a separate queued-handle stop test and define/correct cancellation semantics without weakening process-wide embedded serialization. |
| Software-MMU state is neither handle- nor run-isolated | `darwin/memory/mmu/address_space.cpp` owns a process singleton keyed only by numeric generation; kernel mappings collapse to generation zero, and run/handle retirement does not clear it. Concurrent native-backed handles and sequential embedded kernels can therefore collide or inherit mappings. | Two concurrent handles must map the same generation/address to distinct backings and read their own data. A new embedded run must start without previous user or generation-zero mappings. Cross-thread generation destroy/recreate must resolve only the new canonical state. Route the required context through the bridge without restoring dependency cycles. |
| Embedded ELF loading can overwrite host mappings and trusts unbounded metadata | `darwin/emulator/loader/elf_loader.cpp` uses destructive `MAP_FIXED`, omits `munmap()` on success and error paths, and does not fully bound dynamic entries, relocation tables, symbol/string indexes, destinations, or address arithmetic to loaded segments. | Pre-map a sentinel at the linked window and prove loading refuses without modifying it. Unmap every successful and failed load. Exercise malformed dynamic/relocation images under sanitizers and return status 126 without out-of-range access or arithmetic overflow. |
| Network-enabled installed static library is incomplete | `uml_emulator` references `uml_slirp_net_*` when networking is enabled, but `darwin/emulator/CMakeLists.txt` installs only `libuml_emulator.a`; static linking to `uml_network_provider` does not fold its objects into that archive. A fresh raw consumer fails with seven undefined provider symbols. Packaging currently masks this by building slices with networking off. | Either move default-provider ownership to the app and inject neutral callbacks, or export/package the provider and complete third-party closure. Add network-enabled raw-archive and install-tree consumers. Include a genuine C consumer and encode or document the C++ runtime link requirement. |
| Direct launchers can race rootfs maintenance | `tests/integration/gpu/run_gpu_guest.py` locks the image inode, not the canonical sidecar; `scripts/boot-overlay-systemd.sh` holds no sidecar lock and does no before/after manifest verification. An image shared lock and the maintenance sidecar exclusive lock can coexist. | Every direct launcher must hold the canonical shared sidecar lock for the complete emulator lifetime and verify the manifest before and after. Add a concurrency test proving maintenance cannot acquire its exclusive lock. |
| Active context/provider registries are not strongly exception-safe | `ActiveContextScope` and `ActiveProviderScope` clear existing fallback flags before a potentially throwing `vector::push_back()`. Publication also precedes engine validation and argument/environment allocations, contrary to ADR 0002's stated guarantee. | Insert the new registry entry before mutating prior entries, or use a rollback-safe representation. Inject allocation and pre-entry failures while handle A has working unbound CPU/network callbacks; A must remain usable and prior registry state must be unchanged when publication fails. |
| Public and kernel-internal ABI identities are coupled and the bridge remains wider than Linux uses | Declaration fingerprinting combines installed `uml/emulator.h` with the internal host/kernel bridge. `darwin_uml_set_execution_engine`, `darwin_uml_get_execution_engine`, and `darwin_uml_jit_available` remain in the canonical bridge although current Linux imports none of them. | Split public API and kernel-bridge identities/histories. Remove unused engine controls from the synchronized bridge and use internal test seams. Verify the bridge against the kernel's actual undefined-symbol set; prove internal-only changes do not churn the public identity and public-only changes do not change the Linux replay tree. |

### P2: hardening and modularity

| Finding | Required acceptance |
| --- | --- |
| An undersized SLiRP read removes the frame but leaves its readiness byte | Retain the frame for retry or consume frame and signal atomically. After the queue is empty, polling the IRQ fd must not remain readable. Add a short-buffer/retry/readiness unit test. |
| MMU C entry points can unwind C++ allocation exceptions through Linux C frames | Fault-inject backing, batch, generation, and mapping allocations. Every C entry point must return a defined error, balance batch/walker state, leak no VM mapping, and permit immediate retry. |
| Linux replay accepts topology not represented by the ten-patch manifest | Require the exact commit count, one nonempty patch ID per commit, expected subjects, and no merge commits. Add negative tests for empty, extra, reordered, and merge commits. Preserve exact final-tree verification as a separate invariant. |
| Linux replay CI and next-version checking bypass canonical assumptions | Replace `/private/tmp` in `kernel/audit-delta.sh` with a validated portable temp/work directory and exercise `kernel-replay.yml` on Ubuntu. Make `check-next-version.sh` share manifest hashes/subjects/patch IDs, isolated Git identity, delta ownership, extraction decisions, and per-path next-version policy. Mechanically validate `extraction-decisions.tsv`; it is currently descriptive only. |
| Runtime aggregation still crosses directory scope as raw objects | Replace the `UML_RUNTIME_CORE_OBJECTS` `PARENT_SCOPE` export with target-owned composition. Either make `uml_runtime_core` a real consumed/installed boundary or remove the unused archive. Keep the narrow JIT test variant rather than restoring monolithic recompilation. |
| Test ownership still has conditional coupling | Make integration with tooling disabled and external-network tests enabled a valid configure: the VNC client is currently tooling-owned but integration-referenced. Move/guard the standalone GPU fixture according to its actual test owner. Keep the evidence reset fixture from being pulled into a strict unit-only selection. |
| Public/internal API documentation is inconsistent | Amend or supersede ADRs 0002/0003 to distinguish concurrent native handles from serialized embedded runs. Move the conditional JIT profiling declaration out of the installed public header or deliberately stabilize it. Correct the manifest's “read-only” description for the evidence validator, which writes `stable-id-bindings.tsv`. |
| GPU service file-descriptor ownership is not exception-safe | Replace raw socket-pair integers with RAII ownership and explicit release at transfer. Repeated forced startup failures must leave the process FD count unchanged. Keep host-only GPU fixtures out of production ownership. |
| Dependency/link provenance enforcement is denylist-based at the final link | Change the final-link audit to allow only build-tree objects, Apple SDK/toolchain inputs, and the verified private source-built prefix. Add an arbitrary ambient-path negative fixture. Keep the existing release-package, receipt, archive-member, and private-prefix checks. |

### P3: bounded cleanup

- Prune expired weak entries from the per-thread registration list and prove
  thousands of handle create/run/destroy cycles on one thread remain bounded.
- Make the network provider's implementation include directory private; expose
  only the neutral callback contract.
- Add macOS tooling/unit, product/package consumers, dependency closure, iOS
  graph, and sanitizer gates to CI. Extend source/debris auditing to `.github`.
- Remove or explicitly reject the obsolete `DARWIN_LIBELF_INCLUDE` hook from
  the final Linux series and replace the stale package-manager-specific comment
  in the Darwin ELF compatibility header. The canonical build currently
  supplies the repository-owned header directly.
- Audit or remove historical rootfs payload scripts, ignored writable
  `rootfs.ext2`, `.DS_Store`, and other unconsumed artifacts. Do not remove a
  user artifact merely because it is ignored; establish ownership first.

## Third independent audit: historical closure record

This table records what the third audit accepted at the time. It is retained so
a new chat can understand the work already performed, but it is no longer the
authoritative closure checklist; the fourth-audit section above supersedes any
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
third audit. It is historical; the fourth-audit open-work section is
authoritative where details differ.

The following table records the earlier closure areas, updated for the final
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
| 17 | `UMLEmulator.xcframework` is the sole current Apple aggregate product. Its macOS, iOS-device, and iOS-simulator arm64 archives and external consumers are rebuilt together; an old local `UMLBackend.xcframework` is not evidence. |
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

## Preserved local evidence and fourth-audit checks

The following passing results remain regression evidence; they do not close a
fourth-audit finding unless that finding's acceptance test is explicitly
present. This distinction matters most for instruction evidence, handle/MMU
isolation, loader safety, and network-enabled packaging.

Fourth-audit checks completed without booting a guest or rerunning Geekbench:

| Check | Result | Evidence |
| --- | --- | --- |
| Fresh unit-only configure and architecture audit | passed; 550 targets / 561 registrations | `.build/audit-round4-unit-only` |
| Current unit selection with required evidence reset fixture | 562/562 passed | `.build/audit-round3-finish-unit` |
| Standalone instruction-evidence validator | 1/1 passed, but produced the false-form bindings documented above | `.build/audit-round3-finish-unit/instruction-evidence/stable-id-bindings.tsv` |
| Inventory structure | 983 verified rows: 516 Base / 467 FP&SIMD | `validate_a64_instruction_inventory.py` |
| Dependency manifest and ABI synchronization | passed | `dependencies/prepare.sh --validate-manifest-only`; `kernel/abi/sync.sh --verify` |
| Network-enabled raw package consumer | failed as expected with seven unresolved `uml_slirp_net_*` symbols | `.build/audit-round4-macos-network-consumer`; `/private/tmp/uml-audit-round4-macos-network-consumer.log` |
| Network-disabled package consumers and fresh iOS graphs | passed | fourth-audit package/build configurations |

The fresh source-size report covers 1,010 files: 53 above an advisory
guideline, 42 without optional review context, and two changed since review.
Those counts remain informational.

Preserved third-audit evidence follows:

| Gate | Result | Evidence |
| --- | --- | --- |
| Current unit selection | 562/562 passed | `.build/audit-round3-finish-unit` |
| Repeated API/context race coverage | 50/50 each passed | current unit binaries |
| Tooling/provenance after evidence production | 18/18 passed | reset fixture deliberately excluded |
| Targeted current TSan | 1/1 context lifecycle passed | `.build/audit-round3-final-tsan` |
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
The fourth audit proved that this passing result is not yet evidence that each
row or variant executed the correct form on every applicable engine.

The current 16 KiB integration selection uses only lightweight shell arithmetic
and CPU/HWCAP inspection in the interpreter. The 4 KiB acceptance repeats that
bounded interpreter smoke. Heavy Node.js, Go, browser, and benchmark workloads
were not run through the interpreter, and the pressure registrations were not
selected. Gadget and JIT smokes remain separate engine tests.

The earlier broad matrices remain preserved: normal unit/tooling 580/580 at
16 KiB and 581/581 at 4 KiB, 562/562 ASan+UBSan, 5/5 targeted TSan, and fast
guest integration 24/24 at each page size. They are dated regression evidence,
not substitutes for the focused current-source closure results above.

Apple archive and package hashes are intentionally not reproducibility
identities. The verified slice platforms, architectures, public headers, member
composition, executable-memory policy, and external links are the acceptance
invariants.

The immutable rootfs finished mode `0444`, size 8,589,934,592 bytes, SHA-256
`501770dcce8223a3b7b41c475413ccb5f733291e190dcb23964890b50dafc0e5`.

## Linux and dependency provenance

The Linux series starts from the signed official `linux-6.18.41.tar.xz`
release package (SHA-256
`17fc72f0f8d4a8a8633a5d20085f5d9c5a5ec51ee896a0b7ae1ec25da31273ea`),
authenticated with the detached signature and pinned full signer fingerprint
`647F28654894E3BD457199BE38DBBDC86092693E`. Its packaged tree is
`3098cc6dba43578488a8f7f486797af8046ce8b8`; the official archive intentionally
export-ignores `Documentation/.renames.txt`, the sole difference from upstream
tag tree `9b4299714b8020ae57ca8c6310d747c2299aea0f` at tag commit
`2fe596715f840d053aed5cee5455f701bdcd2b50`. The 10 final-state topics in
`kernel/patches/series/manifest.tsv` reproduce final tree
`8adb106de7c19ed9e6c83f622a48e54629053699`. Local import/replay commit IDs and
their real creation times are observations, not canonical identities. The
final closure replay observed managed HEAD
`cbad57bf321f34259311b0cf0fe9135159bf5832`. The Darwin host-service topic that
carries the synchronized ABI has stable ID
`3da04cbf2d5422fb4076708b2bb87df3f444a4a2` and SHA-256
`c23a15e07c6e201acf84f090440f8f42c3515cb79d7a4399b458337d1da574cd`.

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

ABI v3 fingerprint `0x4333b73bf8b2ea99` is derived from normalized
declaration SHA-256
`4333b73bf8b2ea997904cf0ad1c0ab8975121928af60cd5b8e4b0b0e01749cc6`.
The consecutive declaration history and negative tests require an explicit
identity advance for any declaration or layout change.

## Apple executable-memory policy

The current device and simulator archives contain executable-publication
references only in `cpu/arm64/jit/jit_arena.cpp`. Neither archive has
`mprotect`, VM-protect, JIT write-protect, or instruction-cache publication
references outside the JIT arena; production archives contain no
`jit::testing` support. This preserves JIT for explicitly entitled use without
making RW/RX transitions part of interpreter, gadgets, the software MMU, or
the normal sandbox-compatible iOS path.

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

- provisioning from the publisher's real release URL and trusted public key;
- full per-engine LTP with an immutable image containing the LTP payload;
- physical sandboxed and entitled iOS runtime campaigns;
- opt-in public-network, interactive KDE/VNC, rendered WebGL, and browser
  campaigns; and
- repeated controlled gadget/JIT performance medians.

Their prerequisites and commands are documented in
[`docs/testing.md`](docs/testing.md). Missing external inputs do not leave a
repository item open by themselves; the fourth-audit source items above remain
open independently.
