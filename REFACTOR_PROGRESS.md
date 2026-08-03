# Refactor progress

Status as of 2026-08-03 after a fourth independent audit: the
repository-owned refactor is **not complete**. New correctness, isolation,
packaging, and verification gaps were found in code that passed the previous
closure campaign. [`REFACTOR_HANDOFF.md`](REFACTOR_HANDOFF.md) is the
authoritative, acceptance-test-level checklist. Do not treat a green legacy
matrix as closure for a newly identified gap.

[`docs/refactor-evidence.md`](docs/refactor-evidence.md) is the detailed
evidence index. [`REFACTORING_PLAN.md`](REFACTORING_PLAN.md) remains the original
specification and records the clarified policy that generated build metadata
does not need to be byte-for-byte deterministic.

## Open fourth-audit priorities

1. Replace heuristic instruction-evidence binding with explicit per-variant,
   per-engine evidence. The passing report has 183 explicit and 800 heuristic
   bindings, 902 unique selections for 983 rows, and confirmed wrong-form
   matches.
2. Correct JIT coverage-gap handling: valid but unimplemented instructions
   must be fatal emulator errors, not guest illegal instructions.
3. Enforce same-handle embedded-run fail-fast semantics and stop-aware queued
   admission. Isolate/reset software-MMU state by handle/run.
4. Harden the embedded ELF loader against destructive fixed mappings, leaked
   mappings, malformed metadata, and arithmetic/range errors.
5. Make the network-enabled installed static product linkable; current package
   consumers only prove the network-disabled configuration.
6. Route every direct guest launcher through the canonical rootfs sidecar lock
   and before/after manifest verification.
7. Make active CPU/network registry publication strongly exception-safe and
   contain MMU allocation failures at every C boundary.
8. Split the public API identity from the kernel bridge identity and remove
   bridge functions Linux does not import.
9. Harden Linux replay topology, Ubuntu portability, next-version policy, and
   extraction-decision enforcement.
10. Finish target/test ownership, short-read network readiness, GPU descriptor
    RAII, final-link allowlisting, public C consumer coverage, and bounded
    cleanup items listed in the handoff.

## What remains verified

- A current unit/evidence-producer selection passed **562/562**, and the
  standalone evidence validator passed **1/1**. The latter is structurally
  unsound and is not instruction-completeness acceptance.
- A fresh unit-only configure and architecture audit passed with **550 targets
  / 561 registrations**. Fresh macOS tooling, both unit page-size graphs,
  profile variants, iOS graphs, and representative sanitizer checks also
  configured/audited successfully.
- The inventory parser reports **983 rows**: 516 Base and 467 FP&SIMD. ABI v3
  synchronization still passes with fingerprint `0x4333b73bf8b2ea99` and
  declaration SHA-256
  `4333b73bf8b2ea997904cf0ad1c0ab8975121928af60cd5b8e4b0b0e01749cc6`.
- The signed Linux 6.18.41 package plus ten topics still produces exact tree
  `8adb106de7c19ed9e6c83f622a48e54629053699`; the final delta remains 162
  paths, 13 outside-port. Exact tree content does not detect an extra empty
  replay commit, which remains open.
- Release-package dependency preparation, source/archive self-tests, and
  private-prefix policy checks pass. The existing generated prefix is correctly
  rejected when its receipt records an older verifier input.
- Network-disabled Apple consumers and fresh iOS archives link. A
  network-enabled raw macOS consumer fails on seven unresolved provider
  symbols, which is the open packaging defect.
- The source-size report covers 1,010 files: 53 above an advisory guideline, 42
  without optional review context, and two changed since review. Line limits
  remain guidelines, not failures.
- The immutable rootfs remains mode `0444`, size 8,589,934,592 bytes, SHA-256
  `501770dcce8223a3b7b41c475413ccb5f733291e190dcb23964890b50dafc0e5`.
- Interpreter/gadget state and MMU storage remain data-only; executable
  publication remains confined to the selected JIT arena. This does not imply
  MMU ownership/isolation is correct.

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

Publisher credentials, an LTP-enabled immutable image, physical sandboxed or
entitled iOS devices, opt-in public-network/interactive-GUI/rendered-browser
campaigns, and repeated controlled gadget/JIT performance runs remain external
prerequisites. They are not silent pass claims. The open repository work above
is independent of those external prerequisites.
