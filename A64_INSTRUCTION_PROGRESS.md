# A64 Base and SIMD/FP instruction implementation progress

Source: `ISA_A64_xml_A_profile-2026-06.pdf`, Arm A64 ISA XML for A-profile architecture, 2026-06, document 111182 (ID102025).

Scope: all entries in the PDF inventories `A64 -- Base Instructions (alphabetic order)` (516 entries) and `A64 -- SIMD and Floating-point Instructions (alphabetic order)` (467 entries). Aliases, grouped mnemonics, scalar/vector forms, addressing forms, element widths, architectural feature variants, and every distinct encoding described by an entry must be tested before that entry is complete.

## Completion rules

An entry is complete only when all four boxes are checked:

- `Spec`: encoding constraints and operational pseudocode audited against the 2026-06 PDF.
- `Interpreter`: pure C++ behavior is implemented only for the debug interpreter configuration; it validates reserved encodings, alignment, accesses, and architectural constraints before side effects, favoring early diagnostics and safety over speed.
- `Gadget`: production emits only precompiled, data-only gadgets with no interpreter fallback. Encoding-constant choices (operation, width, ordering, feature form, and ZR/discard cases) are selected at translation time; the valid hot path keeps only unavoidable data-dependent and TLB/fault branches and uses exact host extensions where available.
- `Tests`: positive behavior, flags/FPSR, register-width/lane variants, aliases, feature encodings, memory ordering/fault atomicity, and reserved/undefined encodings are covered as applicable.

## Milestones

- [x] PDF inventories extracted, form-feed boundaries validated, and counted: 983 entries.
- [x] Existing implementation and test coverage classified for every entry.
- [x] All Base entries complete.
- [x] All SIMD and Floating-point entries complete.
- [x] Release build proves the interpreter object and decoded fallback are absent.
- [x] Host-extension fast paths benchmarked and selected where semantically exact.
- [x] Every previously checked gadget family re-audited for translation-time specialization and minimal valid-hot-path branching.
- [x] Full unit and integration test suites pass in debug-interpreter and release-gadget configurations.
- [x] 8 GB, first-partition, in-memory-overlay boot verified and UnixBench baseline recorded.

## Final implementation and gadget audit

- Inventory completion: **983/983** PDF entries (**516/516 Base** and
  **467/467 FP&SIMD**) have Spec, Interpreter, Gadget, and Tests checked.
- Production configuration: Release and ordinary Debug both set
  `UML_ENABLE_GADGET_INTERPRETER_RUNPATH=OFF`; the Release executable has no
  decoded/interpreter execution symbol. CMake rejects an interpreter-enabled
  non-Debug configuration.
- Debug interpreter configuration: the pure C++ decoded path is enabled only
  in `.build/cmake-interpreter` and retains validation-first behavior before
  side effects.
- Translation audit: production lowering carries direct `CpuState` byte
  offsets, immediate values, widths, operation variants, ordering, and
  addressing-mode choices. Original instruction words are emitted only inside
  interpreter compile guards. ADR-to-XZR is folded to a NOP at translation.
- Hot-path audit: exact gadget entry points eliminate runtime opcode/mode
  decoding. TLB hits use one taken conditional branch; pair page-span checks
  use one combined page test; pair and structured SIMD hot epilogues are
  duplicated to avoid unconditional finish branches. Remaining instruction-
  state tests are architectural FPCR/RCW semantics, while remaining memory
  branches are TLB, page-span, alignment/fault, or atomic-retry checks.
- Dead-gadget audit: all **8,644** global gadget definitions were compared
  with every production object reference plus internal assembly
  relocations/native tables. The final set difference is empty after removing
  obsolete generic load/store, conditional-select/compare, integer min/max,
  atomic, FP/SIMD, system-register, P5 compare/branch-fusion, and P10 mapped-
  vector gadgets.
- Host acceleration: gadgets use exact AArch64 host operations and extensions
  where their semantics match, including scalar/vector arithmetic, crypto,
  atomics, FP/Advanced SIMD, BF16, and FP8 native tables; guarded software
  behavior remains for semantics the host cannot reproduce exactly.
- Test organization: instruction coverage is split across **296 focused A64
  test source files**, with separate allocation/encoding, behavior, control-
  state, fault/ordering, and gadget checks as applicable.
- Final standard CTest (guest integrations excluded): **302/302 Release
  production gadgets** and **302/302 Debug interpreter** pass. The LTP runner
  parser remains a unit test. Guest SMP and the verified guest LTP campaign
  are standalone targets, `guest-smp-parallel` and `guest-ltp-verified`.
- Standalone integration regression: the Release verified-LTP campaign passed
  its 2,142 selected guest cases, and the guest SMP topology/parallelism check
  passed before both suites were removed from ordinary CTest.

### 2026-07-24 scalar FABD runtime regression

- Reported failure: a long-running Python process reached scalar
  `FABD D30,D15,D30` (`0x7efed5fe`) and production translation emitted a fatal
  event at that word.
- Root cause: the shared scalar S/D FABD decoder and its test encoder used the
  unallocated bit-23-clear pattern (`0x7e20d400`/`0x7e60d400`). Architecturally
  allocated S/D encodings set bit 23
  (`0x7ea0d400`/`0x7ee0d400`), so ordinary low-register tests could pass while
  the real high-register instruction remained unrecognized.
- Fix: the strict shared decoder now recognizes the complete allocation,
  rejects the unallocated bit-23-clear siblings before side effects, and lets
  production translation select the existing width-specialized native `fabd`
  gadget. The obsolete broad interpreter decode-table entry was removed.
- Unit evidence: exact-encoding interpreter behavior, exact production-gadget
  differential execution, high-register variants, and invalid-sibling
  rejection pass in both `.build/cmake-macos` and
  `.build/cmake-interpreter`. The standard suite passes **302/302**.
- Live evidence: a read-only `rootfs.img` plus tmpfs OverlayFS/systemd guest
  completed a 180-second Python FP-format/parse stress (2,280,000 iterations),
  8,000 dynamic system-register calls, and 250,000 direct executions of
  `0x7efed5fe`, all without a fatal event or result mismatch. OpenJDK 21.0.11
  also started successfully, and the guest shut down cleanly.

### Independent encoding-allocation audit

- Every raw instruction word in the A64 test-support headers, every expected
  mnemonic table, and every native gadget-engine `.inst` word was
  independently disassembled with LLVM's AArch64 decoder rather than relying
  on the emulator decoder and its matching test encoder.
- The audit found and corrected three shared allocation mistakes:
  vector FABS/FNEG required bit 23 set; non-widening FMMLA required bit 29
  clear; and the vector FSQRT decoder needed bit 23 in its mask/value so that
  the valid bit-23-clear FRINT64X family was not claimed as FSQRT.
- Exact canonical words for all affected half/single/double, Q, and
  high-register forms are asserted in focused allocation files. Reserved
  siblings are rejected before source reads or architectural writes by the
  debug interpreter, while production resolves the allocation once during
  translation and keeps the valid gadget hot path branch-free.
- The non-widening FMMLA bit layout was additionally checked against the
  rendered 2026-06 PDF encoding diagram. After correction, every executable
  raw test/gadget word decodes to its intended architectural family; remaining
  LLVM-invalid constants are deliberately partial family values, reserved
  negative tests, or non-instruction FP data.

## UnixBench production baseline

The final accepted-source run completed successfully on 2026-07-24 with
UnixBench 6.0.1 and the Release gadget build (kernel
`6.18.38-g22c669fb9862`). The emulator
exposed 20 vCPUs and was passed `mem=8G`; the guest reported **8,216,340 KiB**
`MemTotal`.

The kernel's initrd feature remains enabled in both the maintained defconfig
and built kernel (`CONFIG_BLK_DEV_INITRD=y`). The benchmark boot used
`scripts/boot-overlay-systemd.sh`, mounted GPT partition 1 (`/dev/ubda1`) from
`rootfs.img` as ext4 read-only, mounted tmpfs for the OverlayFS upper/work
directories, pivoted to the overlay root, and ran systemd as PID 1 before
running exactly
`/root/byte-unixbench/UnixBench/Run`. Guest evidence was:

```text
EXT4-fs (ubda1): mounted filesystem ... ro
MemTotal:        8216340 kB
overlay / overlay rw,...,lowerdir=/mnt/lower,upperdir=/mnt/upper,workdir=/mnt/work
/dev/ubda1 /.uml-lower ext4 ro,relatime
```

| Test | 1-copy result | 1-copy index | 20-copy result | 20-copy index |
|---|---:|---:|---:|---:|
| Dhrystone 2 | 1,817,444.7 lps | 155.7 | 29,589,257.7 lps | 2,535.5 |
| Double-Precision Whetstone | 417.7 MWIPS | 76.0 | 7,072.6 MWIPS | 1,285.9 |
| Execl Throughput | 458.7 lps | 106.7 | 1,805.8 lps | 420.0 |
| File Copy, 1024-byte buffer | 540,525.3 KBps | 1,365.0 | 1,032,100.8 KBps | 2,606.3 |
| File Copy, 256-byte buffer | 138,603.6 KBps | 837.5 | 264,029.6 KBps | 1,595.3 |
| File Copy, 4096-byte buffer | 460,890.9 KBps | 794.6 | 1,715,671.0 KBps | 2,958.1 |
| Pipe Throughput | 672,031.0 lps | 540.2 | 1,187,436.8 lps | 954.5 |
| Pipe Context Switching | 55,321.3 lps | 138.3 | 56,308.9 lps | 140.8 |
| Process Creation | 1,385.8 lps | 110.0 | 1,851.6 lps | 147.0 |
| Shell Scripts, 1 concurrent | 976.7 lpm | 230.4 | 5,700.3 lpm | 1,344.4 |
| Shell Scripts, 8 concurrent | 604.6 lpm | 1,007.7 | 753.7 lpm | 1,256.2 |
| System Call Overhead | 443,769.5 lps | 295.8 | 1,304,863.5 lps | 869.9 |

- **Single-copy System Benchmarks Index Score: 305.0**
- **20-copy System Benchmarks Index Score: 942.1**
- **Composite score: 536.042**
- UnixBench exit status: **0**. The complete run lasted about 57 minutes
  without a fatal guest event, including the sustained FP and 20-vCPU phases.
- In the network-enabled validation boot, guest DNS resolved
  `deb.debian.org`, an HTTP request returned `200 OK`, and OpenJDK 21.0.11
  started successfully. The earlier no-network observation was reproduced
  only when the host emulator process itself was launched inside a
  DNS/network-restricted development sandbox.
- `rootfs.img` before and after: size **8,589,934,592 bytes**, mtime
  **1784725767**, SHA-256
  `9d1398970ea078dcef37bed42ed5eba6ba132f53cca6849420a3f8f188f510ba`.

## Base instructions (516/516 complete)

- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ABS: Absolute value.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADC: Add with carry.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADCS: Add with carry, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADD (extended register): Add extended and scaled register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADD (immediate): Add immediate value.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADD (shifted register): Add optionally-shifted register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADDG: Add with tag.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADDPT: Add checked pointer.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADDS (extended register): Add extended and scaled register, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADDS (immediate): Add immediate value, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADDS (shifted register): Add optionally-shifted register, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADR: Form PC-relative address.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADRP: Form PC-relative address to 4KB page.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AND (immediate): Bitwise AND (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AND (shifted register): Bitwise AND (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ANDS (immediate): Bitwise AND (immediate), setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ANDS (shifted register): Bitwise AND (shifted register), setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - APAS: Associate physical address space: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ASR (immediate): Arithmetic shift right (immediate): an alias of SBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ASR (register): Arithmetic shift right (register): an alias of ASRV.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ASRV: Arithmetic shift right variable.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AT: Address translate: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AUTDA, AUTDZA: Authenticate data address, using key A.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AUTDB, AUTDZB: Authenticate data address, using key B.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AUTIA, AUTIA1716, AUTIASP, AUTIAZ, AUTIZA: Authenticate instruction address, using key A.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AUTIA171615: Authenticate instruction address, using key A.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AUTIASPPC: Authenticate return address using key A, using an immediate offset.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AUTIASPPCR: Authenticate return address using key A, using a register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AUTIB, AUTIB1716, AUTIBSP, AUTIBZ, AUTIZB: Authenticate instruction address, using key B.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AUTIB171615: Authenticate instruction address, using key B.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AUTIBSPPC: Authenticate return address using key B, using an immediate offset.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AUTIBSPPCR: Authenticate return address using key B, using a register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AXFLAG: Convert floating-point condition flags from Arm to external format.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - B: Branch.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - B.cond: Branch conditionally.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BC.cond: Branch consistent conditionally.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BFC: Bitfield clear: an alias of BFM.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BFI: Bitfield insert: an alias of BFM.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BFM: Bitfield move.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BFXIL: Bitfield extract and insert at low end: an alias of BFM.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BIC (shifted register): Bitwise bit clear (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BICS (shifted register): Bitwise bit clear (shifted register), setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BL: Branch with link.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BLR: Branch with link to register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BLRAA, BLRAAZ, BLRAB, BLRABZ: Branch with link to register, with pointer authentication.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BR: Branch to register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BRAA, BRAAZ, BRAB, BRABZ: Branch to register, with pointer authentication.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BRB: Branch record buffer: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BRK: Breakpoint instruction.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BTI: Branch target identification.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CAS, CASA, CASAL, CASL: Compare and swap word or doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CASB, CASAB, CASALB, CASLB: Compare and swap byte in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CASH, CASAH, CASALH, CASLH: Compare and swap halfword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CASP, CASPA, CASPAL, CASPL: Compare and swap pair of words or doublewords in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CASPT, CASPAT, CASPALT, CASPLT: Compare and swap pair unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CAST, CASAT, CASALT, CASLT: Compare and swap unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CB<cc> (immediate): Compare register with immediate and branch.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CB<cc> (register): Compare registers and branch.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBB<cc>: Compare bytes and branch.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBBLE: Compare signed less than or equal to bytes and branch: an alias of CBB<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBBLO: Compare unsigned lower than bytes and branch: an alias of CBB<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBBLS: Compare unsigned lower than or same as bytes and branch: an alias of CBB<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBBLT: Compare signed less than bytes and branch: an alias of CBB<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBGE (immediate): Compare signed greater than or equal to immediate and branch: an alias of CB<cc> (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBH<cc>: Compare halfwords and branch.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBHLE: Compare signed less than or equal to halfwords and branch: an alias of CBH<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBHLO: Compare unsigned lower than halfwords and branch: an alias of CBH<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBHLS: Compare unsigned lower than or same as halfwords and branch: an alias of CBH<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBHLT: Compare signed less than halfwords and branch: an alias of CBH<cc>.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBHS (immediate): Compare unsigned higher than or same as immediate and branch: an alias of CB<cc> (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBLE (immediate): Compare signed less than or equal to immediate and branch: an alias of CB<cc> (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBLE (register): Compare signed less than or equal to register and branch: an alias of CB<cc> (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBLO (register): Compare unsigned lower than register and branch: an alias of CB<cc> (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBLS (immediate): Compare unsigned lower than or same as immediate and branch: an alias of CB<cc> (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBLS (register): Compare unsigned lower than or same as register and branch: an alias of CB<cc> (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBLT (register): Compare signed less than register and branch: an alias of CB<cc> (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBNZ: Compare and branch on nonzero.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CBZ: Compare and branch on zero.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CCMN (immediate): Conditional compare negative (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CCMN (register): Conditional compare negative (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CCMP (immediate): Conditional compare (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CCMP (register): Conditional compare (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CFINV: Invert carry flag.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CFP: Control flow prediction restriction by context: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CHKFEAT: Check feature status.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CINC: Conditional increment: an alias of CSINC.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CINV: Conditional invert: an alias of CSINV.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CLRBHB: Clear branch history.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CLREX: Clear exclusive.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CLS: Count leading sign bits.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CLZ: Count leading zeros.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMN (extended register): Compare negative (extended register): an alias of ADDS (extended register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMN (immediate): Compare negative (immediate): an alias of ADDS (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMN (shifted register): Compare negative (shifted register): an alias of ADDS (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMP (extended register): Compare (extended register): an alias of SUBS (extended register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMP (immediate): Compare (immediate): an alias of SUBS (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMP (shifted register): Compare (shifted register): an alias of SUBS (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMPP: Compare with tag: an alias of SUBPS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CNEG: Conditional negate: an alias of CSNEG.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CNT: Count bits.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - COSP: Clear other speculative prediction restriction by context: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPP: Cache prefetch prediction restriction by context: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFP, CPYFM, CPYFE: Memory copy forward-only.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFPN, CPYFMN, CPYFEN: Memory copy forward-only, reads and writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFPRN, CPYFMRN, CPYFERN: Memory copy forward-only, reads non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFPRT, CPYFMRT, CPYFERT: Memory copy forward-only, reads unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFPRTN, CPYFMRTN, CPYFERTN: Memory copy forward-only, reads unprivileged, reads and writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFPRTRN, CPYFMRTRN, CPYFERTRN: Memory copy forward-only, reads unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFPRTWN, CPYFMRTWN, CPYFERTWN: Memory copy forward-only, reads unprivileged, writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFPT, CPYFMT, CPYFET: Memory copy forward-only, reads and writes unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFPTN, CPYFMTN, CPYFETN: Memory copy forward-only, reads and writes unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFPTRN, CPYFMTRN, CPYFETRN: Memory copy forward-only, reads and writes unprivileged, reads non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFPTWN, CPYFMTWN, CPYFETWN: Memory copy forward-only, reads and writes unprivileged, writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFPWN, CPYFMWN, CPYFEWN: Memory copy forward-only, writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFPWT, CPYFMWT, CPYFEWT: Memory copy forward-only, writes unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFPWTN, CPYFMWTN, CPYFEWTN: Memory copy forward-only, writes unprivileged, reads and writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFPWTRN, CPYFMWTRN, CPYFEWTRN: Memory copy forward-only, writes unprivileged, reads non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYFPWTWN, CPYFMWTWN, CPYFEWTWN: Memory copy forward-only, writes unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYP, CPYM, CPYE: Memory copy.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYPN, CPYMN, CPYEN: Memory copy, reads and writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYPRN, CPYMRN, CPYERN: Memory copy, reads non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYPRT, CPYMRT, CPYERT: Memory copy, reads unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYPRTN, CPYMRTN, CPYERTN: Memory copy, reads unprivileged, reads and writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYPRTRN, CPYMRTRN, CPYERTRN: Memory copy, reads unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYPRTWN, CPYMRTWN, CPYERTWN: Memory copy, reads unprivileged, writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYPT, CPYMT, CPYET: Memory copy, reads and writes unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYPTN, CPYMTN, CPYETN: Memory copy, reads and writes unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYPTRN, CPYMTRN, CPYETRN: Memory copy, reads and writes unprivileged, reads non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYPTWN, CPYMTWN, CPYETWN: Memory copy, reads and writes unprivileged, writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYPWN, CPYMWN, CPYEWN: Memory copy, writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYPWT, CPYMWT, CPYEWT: Memory copy, writes unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYPWTN, CPYMWTN, CPYEWTN: Memory copy, writes unprivileged, reads and writes non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYPWTRN, CPYMWTRN, CPYEWTRN: Memory copy, writes unprivileged, reads non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CPYPWTWN, CPYMWTWN, CPYEWTWN: Memory copy, writes unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CRC32B, CRC32H, CRC32W, CRC32X: CRC32 checksum.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CRC32CB, CRC32CH, CRC32CW, CRC32CX: CRC32C checksum.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CSDB: Consumption of speculative data barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CSEL: Conditional select.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CSET: Conditional set: an alias of CSINC.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CSETM: Conditional set mask: an alias of CSINV.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CSINC: Conditional select increment.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CSINV: Conditional select invert.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CSNEG: Conditional select negation.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CTZ: Count trailing zeros.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - DC: Data cache operation: an alias of SYS. DC ZVA exposes a 64-byte block and uses a checked all-or-nothing interpreter write plus a translation-specialized four-store gadget; OpenJDK 21 startup is covered by live validation.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - DCPS1: Debug change PE state to EL1.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - DCPS2: Debug change PE state to EL2.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - DCPS3: Debug change PE state to EL3.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - DGH: Data gathering hint.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - DMB: Data memory barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - DRPS: Debug restore PE state.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - DSB: Data synchronization barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - DVP: Data value prediction restriction by context: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - EON (shifted register): Bitwise exclusive-OR NOT (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - EOR (immediate): Bitwise exclusive-OR (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - EOR (shifted register): Bitwise exclusive-OR (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ERET: Exception return.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ERETAA, ERETAB: Exception return, with pointer authentication.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ESB: Error synchronization barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - EXTR: Extract register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - GCSB: Guarded Control Stack barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - GCSPOPCX: Guarded Control Stack pop and compare exception return record: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - GCSPOPM: Guarded Control Stack pop: an alias of SYSL.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - GCSPOPX: Guarded Control Stack pop exception return record: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - GCSPUSHM: Guarded Control Stack push: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - GCSPUSHX: Guarded Control Stack push exception return record: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - GCSSS1: Guarded Control Stack switch stack 1: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - GCSSS2: Guarded Control Stack switch stack 2: an alias of SYSL.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - GCSSTR: Guarded Control Stack store register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - GCSSTTR: Guarded Control Stack store register (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - GIC: Generic Interrupt Controller: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - GICR: Generic Interrupt Controller with result: an alias of SYSL.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - GMI: Tag mask insert.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - GSB: Generic Interrupt Controller synchronization barrier: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - HINT: Hint instruction.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - HINTE: Hint instruction (extended).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - HLT: Halt instruction.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - HVC: Hypervisor call.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - IC: Instruction cache operation: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - IRG: Insert random tag.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ISB: Instruction synchronization barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LD64B: Single-copy atomic 64-byte load.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDADD, LDADDA, LDADDAL, LDADDL: Atomic add on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDADDB, LDADDAB, LDADDALB, LDADDLB: Atomic add on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDADDH, LDADDAH, LDADDALH, LDADDLH: Atomic add on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAP: Load-acquire pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAPP: Load-acquire RCpc pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAPR: Load-acquire RCpc register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAPRB: Load-acquire RCpc register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAPRH: Load-acquire RCpc register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAPUR: Load-acquire RCpc register (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAPURB: Load-acquire RCpc register byte (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAPURH: Load-acquire RCpc register halfword (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAPURSB: Load-acquire RCpc register signed byte (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAPURSH: Load-acquire RCpc register signed halfword (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAPURSW: Load-acquire RCpc register signed word (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAR: Load-acquire register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDARB: Load-acquire register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDARH: Load-acquire register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDATXR: Load-acquire unprivileged exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAXP: Load-acquire exclusive pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAXR: Load-acquire exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAXRB: Load-acquire exclusive register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAXRH: Load-acquire exclusive register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDCLR, LDCLRA, LDCLRAL, LDCLRL: Atomic bit clear on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDCLRB, LDCLRAB, LDCLRALB, LDCLRLB: Atomic bit clear on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDCLRH, LDCLRAH, LDCLRALH, LDCLRLH: Atomic bit clear on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDCLRP, LDCLRPA, LDCLRPAL, LDCLRPL: Atomic bit clear on quadword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDEOR, LDEORA, LDEORAL, LDEORL: Atomic exclusive-OR on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDEORB, LDEORAB, LDEORALB, LDEORLB: Atomic exclusive-OR on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDEORH, LDEORAH, LDEORALH, LDEORLH: Atomic exclusive-OR on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDG: Load Allocation Tag.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDGM: Load tag multiple.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDIAPP: Load-Acquire RCpc ordered pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDLAR: Load LOAcquire register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDLARB: Load LOAcquire register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDLARH: Load LOAcquire register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDNP: Load pair of registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDP: Load pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDPSW: Load pair of registers signed word.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDR (immediate): Load register (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDR (literal): Load register (literal).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDR (register): Load register (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDRAA, LDRAB: Load register, with pointer authentication.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDRB (immediate): Load register byte (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDRB (register): Load register byte (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDRH (immediate): Load register halfword (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDRH (register): Load register halfword (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDRSB (immediate): Load register signed byte (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDRSB (register): Load register signed byte (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDRSH (immediate): Load register signed halfword (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDRSH (register): Load register signed halfword (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDRSW (immediate): Load register signed word (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDRSW (literal): Load register signed word (literal).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDRSW (register): Load register signed word (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDSET, LDSETA, LDSETAL, LDSETL: Atomic bit set on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDSETB, LDSETAB, LDSETALB, LDSETLB: Atomic bit set on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDSETH, LDSETAH, LDSETALH, LDSETLH: Atomic bit set on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDSETP, LDSETPA, LDSETPAL, LDSETPL: Atomic bit set on quadword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDSMAX, LDSMAXA, LDSMAXAL, LDSMAXL: Atomic signed maximum on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDSMAXB, LDSMAXAB, LDSMAXALB, LDSMAXLB: Atomic signed maximum on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDSMAXH, LDSMAXAH, LDSMAXALH, LDSMAXLH: Atomic signed maximum on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDSMIN, LDSMINA, LDSMINAL, LDSMINL: Atomic signed minimum on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDSMINB, LDSMINAB, LDSMINALB, LDSMINLB: Atomic signed minimum on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDSMINH, LDSMINAH, LDSMINALH, LDSMINLH: Atomic signed minimum on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDTADD, LDTADDA, LDTADDAL, LDTADDL: Atomic add unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDTCLR, LDTCLRA, LDTCLRAL, LDTCLRL: Atomic bit clear unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDTNP: Load unprivileged pair of registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDTP: Load unprivileged pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDTR: Load register (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDTRB: Load register byte (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDTRH: Load register halfword (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDTRSB: Load register signed byte (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDTRSH: Load register signed halfword (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDTRSW: Load register signed word (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDTSET, LDTSETA, LDTSETAL, LDTSETL: Atomic bit set unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDTXR: Load unprivileged exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDUMAX, LDUMAXA, LDUMAXAL, LDUMAXL: Atomic unsigned maximum on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDUMAXB, LDUMAXAB, LDUMAXALB, LDUMAXLB: Atomic unsigned maximum on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDUMAXH, LDUMAXAH, LDUMAXALH, LDUMAXLH: Atomic unsigned maximum on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDUMIN, LDUMINA, LDUMINAL, LDUMINL: Atomic unsigned minimum on word or doubleword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDUMINB, LDUMINAB, LDUMINALB, LDUMINLB: Atomic unsigned minimum on byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDUMINH, LDUMINAH, LDUMINALH, LDUMINLH: Atomic unsigned minimum on halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDUR: Load register (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDURB: Load register byte (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDURH: Load register halfword (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDURSB: Load register signed byte (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDURSH: Load register signed halfword (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDURSW: Load register signed word (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDXP: Load exclusive pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDXR: Load exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDXRB: Load exclusive register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDXRH: Load exclusive register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LSL (immediate): Logical shift left (immediate): an alias of UBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LSL (register): Logical shift left (register): an alias of LSLV.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LSLV: Logical shift left variable.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LSR (immediate): Logical shift right (immediate): an alias of UBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LSR (register): Logical shift right (register): an alias of LSRV.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LSRV: Logical shift right variable.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MADD: Multiply-add.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MADDPT: Multiply-add checked pointer.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MNEG: Multiply-negate: an alias of MSUB.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MOV (bitmask immediate): Move bitmask immediate value: an alias of ORR (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MOV (inverted wide immediate): Move inverted wide immediate value: an alias of MOVN.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MOV (register): Move register value: an alias of ORR (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MOV (to/from SP): Move register value to or from SP: an alias of ADD (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MOV (wide immediate): Move wide immediate value: an alias of MOVZ.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MOVK: Move wide with keep.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MOVN: Move wide with NOT.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MOVZ: Move wide with zero.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MRRS: Move System register to two adjacent general-purpose registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MRS: Move System register to general-purpose register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MSR (immediate): Move immediate value to special register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MSR (register): Move general-purpose register to System register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MSRR: Move two adjacent general-purpose registers to System register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MSUB: Multiply-subtract.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MSUBPT: Multiply-subtract checked pointer.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MUL: Multiply: an alias of MADD.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MVN: Bitwise NOT: an alias of ORN (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - NEG (shifted register): Negate (shifted register): an alias of SUB (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - NEGS: Negate, setting flags: an alias of SUBS (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - NGC: Negate with carry: an alias of SBC.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - NGCS: Negate with carry, setting flags: an alias of SBCS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - NOP: No operation.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ORN (shifted register): Bitwise OR NOT (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ORR (immediate): Bitwise OR (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ORR (shifted register): Bitwise OR (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PACDA, PACDZA: Pointer Authentication Code for data address, using key A.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PACDB, PACDZB: Pointer Authentication Code for data address, using key B.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PACGA: Pointer Authentication Code, using generic key.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PACIA, PACIA1716, PACIASP, PACIAZ, PACIZA: Pointer Authentication Code for instruction address, using key A.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PACIA171615: Pointer Authentication Code for instruction address, using key A.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PACIASPPC: Pointer Authentication Code for return address, using key A.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PACIB, PACIB1716, PACIBSP, PACIBZ, PACIZB: Pointer Authentication Code for instruction address, using key B.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PACIB171615: Pointer Authentication Code for instruction address, using key B.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PACIBSPPC: Pointer Authentication Code for return address, using key B.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PACM: Pointer authentication modifier.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PACNBIASPPC: Pointer Authentication Code for return address, using key A, not a branch target.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PACNBIBSPPC: Pointer Authentication Code for return address, using key B, not a branch target.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PRFM (immediate): Prefetch memory (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PRFM (literal): Prefetch memory (literal).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PRFM (register): Prefetch memory (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PRFUM: Prefetch memory (unscaled offset).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PSB: Profiling synchronization barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PSSBB: Physical speculative store bypass barrier: an alias of DSB.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RBIT: Reverse bits.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWCAS, RCWCASA, RCWCASAL, RCWCASL: Read check write compare and swap doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWCASP, RCWCASPA, RCWCASPAL, RCWCASPL: Read check write compare and swap quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWCLR, RCWCLRA, RCWCLRAL, RCWCLRL: Read check write atomic bit clear on doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWCLRP, RCWCLRPA, RCWCLRPAL, RCWCLRPL: Read check write atomic bit clear on quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWSCAS, RCWSCASA, RCWSCASAL, RCWSCASL: Read check write software compare and swap doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWSCASP, RCWSCASPA, RCWSCASPAL, RCWSCASPL: Read check write software compare and swap quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWSCLR, RCWSCLRA, RCWSCLRAL, RCWSCLRL: Read check write software atomic bit clear on doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWSCLRP, RCWSCLRPA, RCWSCLRPAL, RCWSCLRPL: Read check write software atomic bit clear on quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWSET, RCWSETA, RCWSETAL, RCWSETL: Read check write atomic bit set on doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWSETP, RCWSETPA, RCWSETPAL, RCWSETPL: Read check write atomic bit set on quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWSSET, RCWSSETA, RCWSSETAL, RCWSSETL: Read check write software atomic bit set on doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWSSETP, RCWSSETPA, RCWSSETPAL, RCWSSETPL: Read check write software atomic bit set on quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWSSWP, RCWSSWPA, RCWSSWPAL, RCWSSWPL: Read check write software swap doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWSSWPP, RCWSSWPPA, RCWSSWPPAL, RCWSSWPPL: Read check write software swap quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWSWP, RCWSWPA, RCWSWPAL, RCWSWPL: Read check write swap doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RCWSWPP, RCWSWPPA, RCWSWPPAL, RCWSWPPL: Read check write swap quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RET: Return from subroutine.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RETAA, RETAB: Return from subroutine, with pointer authentication.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RETAASPPC, RETABSPPC: Return from subroutine, with enhanced pointer authentication using an immediate offset.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RETAASPPCR, RETABSPPCR: Return from subroutine, with enhanced pointer authentication using a register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - REV: Reverse bytes.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - REV16: Reverse bytes in 16-bit halfwords.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - REV32: Reverse bytes in 32-bit words.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - REV64: Reverse bytes: an alias of REV.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RMIF: Rotate, mask insert flags.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ROR (immediate): Rotate right (immediate): an alias of EXTR.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ROR (register): Rotate right (register): an alias of RORV.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RORV: Rotate right variable.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RPRFM: Range prefetch memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SB: Speculation barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SBC: Subtract with carry.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SBCS: Subtract with carry, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SBFIZ: Signed bitfield insert in zeros: an alias of SBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SBFM: Signed bitfield move.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SBFX: Signed bitfield extract: an alias of SBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SDIV (quotient): Signed divide.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SETF8, SETF16: Evaluation of 8-bit or 16-bit flag values.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SETGP, SETGM, SETGE: Memory set with tag setting.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SETGPN, SETGMN, SETGEN: Memory set with tag setting, non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SETGPT, SETGMT, SETGET: Memory set with tag setting, unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SETGPTN, SETGMTN, SETGETN: Memory set with tag setting, unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SETP, SETM, SETE: Memory set.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SETPN, SETMN, SETEN: Memory set, non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SETPT, SETMT, SETET: Memory set, unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SETPTN, SETMTN, SETETN: Memory set, unprivileged and non-temporal.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SEV: Send event.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SEVL: Send event local.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHUH: Shared update hint.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMADDL: Signed multiply-add long.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMAX (immediate): Signed maximum (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMAX (register): Signed maximum (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMC: Secure monitor call.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMIN (immediate): Signed minimum (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMIN (register): Signed minimum (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMNEGL: Signed multiply-negate long: an alias of SMSUBL.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMSTART: Enable access to Streaming SVE mode and SME architectural state: an alias of MSR (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMSTOP: Disable access to Streaming SVE mode and SME architectural state: an alias of MSR (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMSUBL: Signed multiply-subtract long.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMULH: Signed multiply high.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMULL (32-bit): Signed multiply long: an alias of SMADDL.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SSBB: Speculative store bypass barrier: an alias of DSB.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ST2G: Store Allocation Tags.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ST64B: Single-copy atomic 64-byte store without status result.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ST64BV: Single-copy atomic 64-byte store with status result.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ST64BV0: Single-copy atomic 64-byte EL0 store with status result.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STADD, STADDL: Atomic add on word or doubleword, without return: an alias of LDADD, LDADDA, LDADDAL, LDADDL.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STADDB, STADDLB: Atomic add on byte, without return: an alias of LDADDB, LDADDAB, LDADDALB, LDADDLB.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STADDH, STADDLH: Atomic add on halfword, without return: an alias of LDADDH, LDADDAH, LDADDALH, LDADDLH.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STCLR, STCLRL: Atomic bit clear on word or doubleword, without return: an alias of LDCLR, LDCLRA, LDCLRAL, LDCLRL.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STCLRB, STCLRLB: Atomic bit clear on byte, without return: an alias of LDCLRB, LDCLRAB, LDCLRALB, LDCLRLB.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STCLRH, STCLRLH: Atomic bit clear on halfword, without return: an alias of LDCLRH, LDCLRAH, LDCLRALH, LDCLRLH.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STCPH: Store concurrent priority hint.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STEOR, STEORL: Atomic exclusive-OR on word or doubleword, without return: an alias of LDEOR, LDEORA, LDEORAL, LDEORL.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STEORB, STEORLB: Atomic exclusive-OR on byte, without return: an alias of LDEORB, LDEORAB, LDEORALB, LDEORLB.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STEORH, STEORLH: Atomic exclusive-OR on halfword, without return: an alias of LDEORH, LDEORAH, LDEORALH, LDEORLH.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STG: Store Allocation Tag.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STGM: Store Allocation Tag multiple.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STGP: Store Allocation Tag and pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STILP: Store-release ordered pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLLR: Store LORelease register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLLRB: Store LORelease register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLLRH: Store LORelease register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLP: Store-release pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLR: Store-release register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLRB: Store-release register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLRH: Store-release register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLTXR: Store-release unprivileged exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLUR: Store-release register (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLURB: Store-release register byte (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLURH: Store-release register halfword (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLXP: Store-release exclusive pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLXR: Store-release exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLXRB: Store-release exclusive register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLXRH: Store-release exclusive register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STNP: Store pair of registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STP: Store pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STR (immediate): Store register (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STR (register): Store register (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STRB (immediate): Store register byte (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STRB (register): Store register byte (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STRH (immediate): Store register halfword (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STRH (register): Store register halfword (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STSET, STSETL: Atomic bit set on word or doubleword, without return: an alias of LDSET, LDSETA, LDSETAL, LDSETL.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STSETB, STSETLB: Atomic bit set on byte, without return: an alias of LDSETB, LDSETAB, LDSETALB, LDSETLB.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STSETH, STSETLH: Atomic bit set on halfword, without return: an alias of LDSETH, LDSETAH, LDSETALH, LDSETLH.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STSHH: Store shared hint.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STSMAX, STSMAXL: Atomic signed maximum on word or doubleword, without return: an alias of LDSMAX, LDSMAXA, LDSMAXAL,
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STSMAXB, STSMAXLB: Atomic signed maximum on byte, without return: an alias of LDSMAXB, LDSMAXAB, LDSMAXALB,
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STSMAXH, STSMAXLH: Atomic signed maximum on halfword, without return: an alias of LDSMAXH, LDSMAXAH, LDSMAXALH,
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STSMIN, STSMINL: Atomic signed minimum on word or doubleword, without return: an alias of LDSMIN, LDSMINA, LDSMINAL,
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STSMINB, STSMINLB: Atomic signed minimum on byte, without return: an alias of LDSMINB, LDSMINAB, LDSMINALB, LDSMINLB.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STSMINH, STSMINLH: Atomic signed minimum on halfword, without return: an alias of LDSMINH, LDSMINAH, LDSMINALH, LDSMINLH.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STTADD, STTADDL: Atomic add unprivileged, without return: an alias of LDTADD, LDTADDA, LDTADDAL, LDTADDL.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STTCLR, STTCLRL: Atomic bit clear unprivileged, without return: an alias of LDTCLR, LDTCLRA, LDTCLRAL, LDTCLRL.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STTNP: Store unprivileged pair of registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STTP: Store unprivileged pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STTR: Store register (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STTRB: Store register byte (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STTRH: Store register halfword (unprivileged).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STTSET, STTSETL: Atomic bit set unprivileged, without return: an alias of LDTSET, LDTSETA, LDTSETAL, LDTSETL.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STTXR: Store unprivileged exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STUMAX, STUMAXL: Atomic unsigned maximum on word or doubleword, without return: an alias of LDUMAX, LDUMAXA, LDUMAXAL,
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STUMAXB, STUMAXLB: Atomic unsigned maximum on byte, without return: an alias of LDUMAXB, LDUMAXAB, LDUMAXALB,
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STUMAXH, STUMAXLH: Atomic unsigned maximum on halfword, without return: an alias of LDUMAXH, LDUMAXAH, LDUMAXALH,
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STUMIN, STUMINL: Atomic unsigned minimum on word or doubleword, without return: an alias of LDUMIN, LDUMINA, LDUMINAL,
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STUMINB, STUMINLB: Atomic unsigned minimum on byte, without return: an alias of LDUMINB, LDUMINAB, LDUMINALB, LDUMINLB.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STUMINH, STUMINLH: Atomic unsigned minimum on halfword, without return: an alias of LDUMINH, LDUMINAH, LDUMINALH,
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STUR: Store register (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STURB: Store register byte (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STURH: Store register halfword (unscaled).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STXP: Store exclusive pair of registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STXR: Store exclusive register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STXRB: Store exclusive register byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STXRH: Store exclusive register halfword.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STZ2G: Store Allocation Tags, zeroing.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STZG: Store Allocation Tag, zeroing.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STZGM: Store Allocation Tag and zero multiple.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SUB (extended register): Subtract extended and scaled register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SUB (immediate): Subtract immediate value.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SUB (shifted register): Subtract optionally-shifted register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SUBG: Subtract with tag.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SUBP: Subtract pointer.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SUBPS: Subtract pointer, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SUBPT: Subtract checked pointer.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SUBS (extended register): Subtract extended and scaled register, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SUBS (immediate): Subtract immediate value, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SUBS (shifted register): Subtract optionally-shifted register, setting flags.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SVC: Supervisor call.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SWP, SWPA, SWPAL, SWPL: Swap word or doubleword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SWPB, SWPAB, SWPALB, SWPLB: Swap byte in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SWPH, SWPAH, SWPALH, SWPLH: Swap halfword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SWPP, SWPPA, SWPPAL, SWPPL: Swap quadword in memory.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SWPT, SWPTA, SWPTAL, SWPTL: Swap unprivileged.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SXTB: Signed extend byte: an alias of SBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SXTH: Sign extend halfword: an alias of SBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SXTW: Sign extend word: an alias of SBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SYS: System instruction.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SYSL: System instruction with result.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SYSP: 128-bit system instruction.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - TBNZ: Test bit and branch if nonzero.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - TBZ: Test bit and branch if zero.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - TLBI: TLB invalidate operation: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - TLBIP: TLB invalidate pair operation: an alias of SYSP.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - TRCIT: Trace instrumentation: an alias of SYS.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - TSB: Trace synchronization barrier.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - TST (immediate): Test bits (immediate): an alias of ANDS (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - TST (shifted register): Test (shifted register): an alias of ANDS (shifted register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UBFIZ: Unsigned bitfield insert in zeros: an alias of UBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UBFM: Unsigned bitfield move.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UBFX: Unsigned bitfield extract: an alias of UBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UDF: Permanently undefined.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UDIV (quotient): Unsigned divide.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMADDL: Unsigned multiply-add long.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMAX (immediate): Unsigned maximum (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMAX (register): Unsigned maximum (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMIN (immediate): Unsigned minimum (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMIN (register): Unsigned minimum (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMNEGL: Unsigned multiply-negate long: an alias of UMSUBL.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMSUBL: Unsigned multiply-subtract long.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMULH: Unsigned multiply high.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMULL (32-bit): Unsigned multiply long: an alias of UMADDL.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UXTB: Unsigned extend byte: an alias of UBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UXTH: Unsigned extend halfword: an alias of UBFM.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - WFE: Wait for event.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - WFET: Wait for event with timeout.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - WFI: Wait for interrupt.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - WFIT: Wait for interrupt with timeout.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - XAFLAG: Convert floating-point condition flags from external format to Arm format.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - XPACD, XPACI, XPACLRI: Strip Pointer Authentication Code.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - YIELD: Yield.

## SIMD and Floating-point instructions (467/467 complete)

- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ABS: Absolute value (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADD (vector): Add (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADDHN, ADDHN2: Add returning high narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADDP (scalar): Add pair of elements (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADDP (vector): Add pairwise (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ADDV: Add across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AESD: AES single round decryption.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AESE: AES single round encryption.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AESIMC: AES inverse mix columns.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AESMC: AES mix columns.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - AND (vector): Bitwise AND (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BCAX: Bit clear and exclusive-OR.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BF1CVTL, BF1CVTL2, BF2CVTL, BF2CVTL2: 8-bit floating-point convert to BFloat16 (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BFCVT: Single-precision convert to BFloat16 (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BFCVTN, BFCVTN2: Single-precision convert to BFloat16 (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BFDOT (by element): BFloat16 dot product to single-precision (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BFDOT (vector): BFloat16 dot product to single-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BFMLALB, BFMLALT (by element): BFloat16 multiply-add to single-precision (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BFMLALB, BFMLALT (vector): BFloat16 multiply-add to single-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BFMMLA (widening): BFloat16 matrix multiply-accumulate to single-precision.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BIC (vector, immediate): Bitwise bit clear (vector, immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BIC (vector, register): Bitwise bit clear (vector, register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BIF: Bitwise insert if false.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BIT: Bitwise insert if true.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - BSL: Bitwise select.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CLS (vector): Count leading sign bits (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CLZ (vector): Count leading zero bits (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMEQ (register): Compare bitwise equal (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMEQ (zero): Compare bitwise equal to zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMGE (register): Compare signed greater than or equal (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMGE (zero): Compare signed greater than or equal to zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMGT (register): Compare signed greater than (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMGT (zero): Compare signed greater than zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMHI (register): Compare unsigned higher (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMHS (register): Compare unsigned higher or same (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMLE (zero): Compare signed less than or equal to zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMLT (zero): Compare signed less than zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CMTST: Compare bitwise test bits nonzero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - CNT: Population count per byte.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - DUP (element): Duplicate vector element to vector or scalar.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - DUP (general): Duplicate general-purpose register to vector.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - EOR (vector): Bitwise exclusive-OR (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - EOR3: Three-way exclusive-OR.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - EXT: Extract vector from pair of vectors.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - F1CVTL, F1CVTL2, F2CVTL, F2CVTL2: 8-bit floating-point convert to half-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FABD: Floating-point absolute difference (scalar and vector). Correct scalar S/D allocation-bit decoding is covered through both the strict interpreter and translation-specialized native gadgets, including the reported `0x7efed5fe` high-register encoding.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FABS (scalar): Floating-point absolute value (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FABS (vector): Floating-point absolute value (vector). Canonical allocation-word assertions and early rejection of the unallocated bit-23-clear siblings prevent decoder/test agreement on invalid encodings.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FACGE: Floating-point absolute compare greater than or equal (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FACGT: Floating-point absolute compare greater than (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FADD (scalar): Floating-point add (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FADD (vector): Floating-point add (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FADDP (scalar): Floating-point add pair of elements (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FADDP (vector): Floating-point add pairwise (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FAMAX: Floating-point absolute maximum.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FAMIN: Floating-point absolute minimum.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCADD: Floating-point complex add.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCCMP: Floating-point conditional quiet compare (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCCMPE: Floating-point conditional signaling compare (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCMEQ (register): Floating-point compare equal (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCMEQ (zero): Floating-point compare equal to zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCMGE (register): Floating-point compare greater than or equal (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCMGE (zero): Floating-point compare greater than or equal to zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCMGT (register): Floating-point compare greater than (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCMGT (zero): Floating-point compare greater than zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCMLA: Floating-point complex multiply accumulate.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCMLA (by element): Floating-point complex multiply accumulate (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCMLE (zero): Floating-point compare less than or equal to zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCMLT (zero): Floating-point compare less than zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCMP: Floating-point quiet compare (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCMPE: Floating-point signaling compare (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCSEL: Floating-point conditional select (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVT: Floating-point convert precision (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTAS (scalar SIMD&FP): Floating-point convert to signed integer, rounding to nearest with ties to away (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTAS (scalar): Floating-point convert to signed integer, rounding to nearest with ties to away (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTAS (vector): Floating-point convert to signed integer, rounding to nearest with ties to away (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTAU (scalar SIMD&FP): Floating-point convert to unsigned integer, rounding to nearest with ties to away (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTAU (scalar): Floating-point convert to unsigned integer, rounding to nearest with ties to away (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTAU (vector): Floating-point convert to unsigned integer, rounding to nearest with ties to away (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTL, FCVTL2: Floating-point convert to higher precision long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTMS (scalar SIMD&FP): Floating-point convert to signed integer, rounding toward minus infinity (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTMS (scalar): Floating-point convert to signed integer, rounding toward minus infinity (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTMS (vector): Floating-point convert to signed integer, rounding toward minus infinity (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTMU (scalar SIMD&FP): Floating-point convert to unsigned integer, rounding toward minus infinity (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTMU (scalar): Floating-point convert to unsigned integer, rounding toward minus infinity (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTMU (vector): Floating-point convert to unsigned integer, rounding toward minus infinity (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTN (half-precision to 8-bit floating-point): Half-precision convert to 8-bit floating-point (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTN, FCVTN2 (double to single-precision, single to half-precision): Floating-point convert to lower precision narrow (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTN, FCVTN2 (single-precision to 8-bit floating-point): Single-precision convert to 8-bit floating-point (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTNS (scalar SIMD&FP): Floating-point convert to signed integer, rounding to nearest with ties to even (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTNS (scalar): Floating-point convert to signed integer, rounding to nearest with ties to even (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTNS (vector): Floating-point convert to signed integer, rounding to nearest with ties to even (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTNU (scalar SIMD&FP): Floating-point convert to unsigned integer, rounding to nearest with ties to even (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTNU (scalar): Floating-point convert to unsigned integer, rounding to nearest with ties to even (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTNU (vector): Floating-point convert to unsigned integer, rounding to nearest with ties to even (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTPS (scalar SIMD&FP): Floating-point convert to signed integer, rounding toward plus infinity (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTPS (scalar): Floating-point convert to signed integer, rounding toward plus infinity (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTPS (vector): Floating-point convert to signed integer, rounding toward plus infinity (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTPU (scalar SIMD&FP): Floating-point convert to unsigned integer, rounding toward plus infinity (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTPU (scalar): Floating-point convert to unsigned integer, rounding toward plus infinity (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTPU (vector): Floating-point convert to unsigned integer, rounding toward plus infinity (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTXN, FCVTXN2: Floating-point convert to lower precision narrow, rounding to odd (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTZS (scalar SIMD&FP): Floating-point convert to signed integer, rounding toward zero (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTZS (scalar, fixed-point): Floating-point convert to signed fixed-point, rounding toward zero (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTZS (scalar, integer): Floating-point convert to signed integer, rounding toward zero (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTZS (vector, fixed-point): Floating-point convert to signed fixed-point, rounding toward zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTZS (vector, integer): Floating-point convert to signed integer, rounding toward zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTZU (scalar SIMD&FP): Floating-point convert to unsigned integer, rounding toward zero (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTZU (scalar, fixed-point): Floating-point convert to unsigned fixed-point, rounding toward zero (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTZU (scalar, integer): Floating-point convert to unsigned integer, rounding toward zero (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTZU (vector, fixed-point): Floating-point convert to unsigned fixed-point, rounding toward zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FCVTZU (vector, integer): Floating-point convert to unsigned integer, rounding toward zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FDIV (scalar): Floating-point divide (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FDIV (vector): Floating-point divide (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FDOT (8-bit floating-point to half-precision, by element): 8-bit floating-point dot product to half-precision (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FDOT (8-bit floating-point to half-precision, vector): 8-bit floating-point dot product to half-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FDOT (8-bit floating-point to single-precision, by element): 8-bit floating-point dot product to single-precision (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FDOT (8-bit floating-point to single-precision, vector): 8-bit floating-point dot product to single-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FDOT (half-precision to single-precision, by element): Half-precision dot product to single-precision (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FDOT (half-precision to single-precision, vector): Half-precision dot product to single-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FJCVTZS: Floating-point JavaScript convert to signed fixed-point, rounding toward zero.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMADD: Floating-point fused multiply-add (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMAX (scalar): Floating-point maximum (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMAX (vector): Floating-point maximum (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMAXNM (scalar): Floating-point maximum number (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMAXNM (vector): Floating-point maximum number (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMAXNMP (scalar): Floating-point maximum number of pair of elements (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMAXNMP (vector): Floating-point maximum number pairwise (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMAXNMV: Floating-point maximum number across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMAXP (scalar): Floating-point maximum of pair of elements (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMAXP (vector): Floating-point maximum pairwise (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMAXV: Floating-point maximum across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMIN (scalar): Floating-point minimum (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMIN (vector): Floating-point minimum (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMINNM (scalar): Floating-point minimum number (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMINNM (vector): Floating-point minimum number (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMINNMP (scalar): Floating-point minimum number of pair of elements (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMINNMP (vector): Floating-point minimum number pairwise (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMINNMV: Floating-point minimum number across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMINP (scalar): Floating-point minimum of pair of elements (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMINP (vector): Floating-point minimum pairwise (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMINV: Floating-point minimum across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMLA (by element): Floating-point fused multiply-add to accumulator (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMLA (vector): Floating-point fused multiply-add to accumulator (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMLAL, FMLAL2 (by element): Floating-point fused multiply-add long to accumulator (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMLAL, FMLAL2 (vector): Floating-point fused multiply-add long to accumulator (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMLALB, FMLALT (by element): 8-bit floating-point multiply-add to half-precision (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMLALB, FMLALT (vector): 8-bit floating-point multiply-add to half-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMLALLBB, FMLALLBT, FMLALLTB, FMLALLTT (by element): 8-bit floating-point multiply-add to single-precision (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMLALLBB, FMLALLBT, FMLALLTB, FMLALLTT (vector): 8-bit floating-point multiply-add to single-precision (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMLS (by element): Floating-point fused multiply-subtract from accumulator (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMLS (vector): Floating-point fused multiply-subtract from accumulator (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMLSL, FMLSL2 (by element): Floating-point fused multiply-subtract long from accumulator (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMLSL, FMLSL2 (vector): Floating-point fused multiply-subtract long from accumulator (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMMLA (non-widening): Half-precision matrix multiply-accumulate. The canonical `0x4ec0ec00` allocation and native-host `.inst` are asserted independently; the former bit-29-set test sibling is rejected before side effects.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMMLA (widening, 8-bit floating-point to half-precision): 8-bit floating-point matrix multiply-accumulate to half-precision.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMMLA (widening, 8-bit floating-point to single-precision): 8-bit floating-point matrix multiply-accumulate to single-precision.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMMLA (widening, half-precision to single-precision): Half-precision matrix multiply-accumulate to single-precision.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMOV (general): Floating-point move to or from general-purpose register without conversion.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMOV (register): Floating-point move register without conversion.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMOV (scalar, immediate): Floating-point move immediate (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMOV (vector, immediate): Floating-point move immediate (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMSUB: Floating-point fused multiply-subtract (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMUL (by element): Floating-point multiply (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMUL (scalar): Floating-point multiply (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMUL (vector): Floating-point multiply (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMULX: Floating-point multiply extended.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FMULX (by element): Floating-point multiply extended (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FNEG (scalar): Floating-point negate (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FNEG (vector): Floating-point negate (vector). Canonical allocation-word assertions and early rejection of the unallocated bit-23-clear siblings cover both interpreter and gadget paths.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FNMADD: Floating-point negated fused multiply-add (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FNMSUB: Floating-point negated fused multiply-subtract (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FNMUL (scalar): Floating-point multiply-negate (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRECPE: Floating-point reciprocal estimate.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRECPS: Floating-point reciprocal step.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRECPX: Floating-point reciprocal exponent (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINT32X (scalar): Floating-point round to 32-bit integer, using current rounding mode (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINT32X (vector): Floating-point round to 32-bit integer, using current rounding mode (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINT32Z (scalar): Floating-point round to 32-bit integer toward zero (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINT32Z (vector): Floating-point round to 32-bit integer toward zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINT64X (scalar): Floating-point round to 64-bit integer, using current rounding mode (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINT64X (vector): Floating-point round to 64-bit integer, using current rounding mode (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINT64Z (scalar): Floating-point round to 64-bit integer toward zero (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINT64Z (vector): Floating-point round to 64-bit integer toward zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINTA (scalar): Floating-point round to integral, to nearest with ties to away (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINTA (vector): Floating-point round to integral, to nearest with ties to away (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINTI (scalar): Floating-point round to integral, using current rounding mode (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINTI (vector): Floating-point round to integral, using current rounding mode (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINTM (scalar): Floating-point round to integral, toward minus infinity (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINTM (vector): Floating-point round to integral, toward minus infinity (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINTN (scalar): Floating-point round to integral, to nearest with ties to even (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINTN (vector): Floating-point round to integral, to nearest with ties to even (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINTP (scalar): Floating-point round to integral, toward plus infinity (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINTP (vector): Floating-point round to integral, toward plus infinity (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINTX (scalar): Floating-point round to integral exact, using current rounding mode (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINTX (vector): Floating-point round to integral exact, using current rounding mode (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINTZ (scalar): Floating-point round to integral, toward zero (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRINTZ (vector): Floating-point round to integral, toward zero (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRSQRTE: Floating-point reciprocal square root estimate.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FRSQRTS: Floating-point reciprocal square root step.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FSCALE: Floating-point adjust exponent by vector.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FSQRT (scalar): Floating-point square root (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FSQRT (vector): Floating-point square root (vector). The strict allocation mask includes bit 23, with exact canonical-word checks proving the bit-23-clear FRINT64X family is not misclassified.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FSUB (scalar): Floating-point subtract (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - FSUB (vector): Floating-point subtract (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - INS (element): Insert vector element from another vector element.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - INS (general): Insert vector element from general-purpose register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LD1 (multiple structures): Load multiple single-element structures to one, two, three, or four registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LD1 (single structure): Load one single-element structure to one lane of one register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LD1R: Load one single-element structure and replicate to all lanes (of one register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LD2 (multiple structures): Load multiple 2-element structures to two registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LD2 (single structure): Load single 2-element structure to one lane of two registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LD2R: Load single 2-element structure and replicate to all lanes of two registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LD3 (multiple structures): Load multiple 3-element structures to three registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LD3 (single structure): Load single 3-element structure to one lane of three registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LD3R: Load single 3-element structure and replicate to all lanes of three registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LD4 (multiple structures): Load multiple 4-element structures to four registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LD4 (single structure): Load single 4-element structure to one lane of four registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LD4R: Load single 4-element structure and replicate to all lanes of four registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAP1 (SIMD&FP): Load-acquire RCpc one single-element structure to one lane of one register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDAPUR (SIMD&FP): Load-acquire RCpc SIMD&FP register (unscaled offset).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDBFADD, LDBFADDA, LDBFADDAL, LDBFADDL: Atomic BFloat16 add.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDBFMAX, LDBFMAXA, LDBFMAXAL, LDBFMAXL: Atomic BFloat16 maximum.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDBFMAXNM, LDBFMAXNMA, LDBFMAXNMAL, LDBFMAXNML: Atomic BFloat16 maximum number.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDBFMIN, LDBFMINA, LDBFMINAL, LDBFMINL: Atomic BFloat16 minimum.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDBFMINNM, LDBFMINNMA, LDBFMINNMAL, LDBFMINNML: Atomic BFloat16 minimum number.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDFADD, LDFADDA, LDFADDAL, LDFADDL: Atomic floating-point add.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDFMAX, LDFMAXA, LDFMAXAL, LDFMAXL: Atomic floating-point maximum.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDFMAXNM, LDFMAXNMA, LDFMAXNMAL, LDFMAXNML: Atomic floating-point maximum number.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDFMIN, LDFMINA, LDFMINAL, LDFMINL: Atomic floating-point minimum.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDFMINNM, LDFMINNMA, LDFMINNMAL, LDFMINNML: Atomic floating-point minimum number.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDNP (SIMD&FP): Load pair of SIMD&FP registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDP (SIMD&FP): Load pair of SIMD&FP registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDR (immediate, SIMD&FP): Load SIMD&FP register (immediate offset).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDR (literal, SIMD&FP): Load SIMD&FP register (PC-relative literal).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDR (register, SIMD&FP): Load SIMD&FP register (register offset).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDTNP (SIMD&FP): Load unprivileged pair of SIMD&FP registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDTP (SIMD&FP): Load unprivileged pair of SIMD&FP registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LDUR (SIMD&FP): Load SIMD&FP register (unscaled offset).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LUTI2: Lookup table read with 2-bit indices.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - LUTI4: Lookup table read with 4-bit indices.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MLA (by element): Multiply-add to accumulator (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MLA (vector): Multiply-add to accumulator (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MLS (by element): Multiply-subtract from accumulator (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MLS (vector): Multiply-subtract from accumulator (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MOV (element): Move vector element to another vector element: an alias of INS (element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MOV (from general): Move general-purpose register to a vector element: an alias of INS (general).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MOV (scalar): Move vector element to scalar: an alias of DUP (element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MOV (to general): Move vector element to general-purpose register: an alias of UMOV.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MOV (vector): Move vector: an alias of ORR (vector, register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MOVI: Move immediate (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MUL (by element): Multiply (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MUL (vector): Multiply (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MVN: Bitwise NOT (vector): an alias of NOT.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - MVNI: Move inverted immediate (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - NEG (vector): Negate (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - NOT: Bitwise NOT (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ORN (vector): Bitwise inclusive OR NOT (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ORR (vector, immediate): Bitwise inclusive OR (vector, immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ORR (vector, register): Bitwise inclusive OR (vector, register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PMUL: Polynomial multiply.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - PMULL, PMULL2: Polynomial multiply long.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RADDHN, RADDHN2: Rounding add returning high narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RAX1: Rotate and exclusive-OR.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RBIT (vector): Reverse bit order (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - REV16 (vector): Reverse elements in 16-bit halfwords (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - REV32 (vector): Reverse elements in 32-bit words (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - REV64: Reverse elements in 64-bit doublewords (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RSHRN, RSHRN2: Rounding shift right narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - RSUBHN, RSUBHN2: Rounding subtract returning high narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SABA: Signed absolute difference and accumulate.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SABAL, SABAL2: Signed absolute difference and accumulate long.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SABD: Signed absolute difference.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SABDL, SABDL2: Signed absolute difference long.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SADALP: Signed add and accumulate long pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SADDL, SADDL2: Signed add long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SADDLP: Signed add long pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SADDLV: Signed add long across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SADDW, SADDW2: Signed add wide.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SCVTF (scalar SIMD&FP): Signed integer convert to floating-point (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SCVTF (scalar, fixed-point): Signed fixed-point convert to floating-point (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SCVTF (scalar, integer): Signed integer convert to floating-point (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SCVTF (vector, fixed-point): Signed fixed-point convert to floating-point (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SCVTF (vector, integer): Signed integer convert to floating-point (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SDOT (by element): Dot product signed arithmetic (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SDOT (vector): Dot product signed arithmetic (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHA1C: SHA1 hash update (choose).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHA1H: SHA1 fixed rotate.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHA1M: SHA1 hash update (majority).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHA1P: SHA1 hash update (parity).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHA1SU0: SHA1 schedule update 0.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHA1SU1: SHA1 schedule update 1.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHA256H: SHA-256 hash update (part 1).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHA256H2: SHA-256 hash update (part 2).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHA256SU0: SHA-256 schedule update 0.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHA256SU1: SHA-256 schedule update 1.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHA512H: SHA-512 hash update part 1.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHA512H2: SHA-512 hash update part 2.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHA512SU0: SHA-512 schedule update 0.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHA512SU1: SHA-512 schedule update 1.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHADD: Signed halving add.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHL: Shift left (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHLL, SHLL2: Shift left long (by element size).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHRN, SHRN2: Shift right narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SHSUB: Signed halving subtract.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SLI: Shift left and insert (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SM3PARTW1: SM3PARTW1.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SM3PARTW2: SM3PARTW2.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SM3SS1: SM3SS1.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SM3TT1A: SM3TT1A.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SM3TT1B: SM3TT1B.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SM3TT2A: SM3TT2A.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SM3TT2B: SM3TT2B.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SM4E: SM4 encode.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SM4EKEY: SM4 key.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMAX: Signed maximum (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMAXP: Signed maximum pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMAXV: Signed maximum across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMIN: Signed minimum (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMINP: Signed minimum pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMINV: Signed minimum across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMLAL, SMLAL2 (by element): Signed multiply-add long (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMLAL, SMLAL2 (vector): Signed multiply-add long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMLSL, SMLSL2 (by element): Signed multiply-subtract long (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMLSL, SMLSL2 (vector): Signed multiply-subtract long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMMLA (vector): Signed 8-bit integer matrix multiply-accumulate to 32-bit integer (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMOV: Signed move vector element to general-purpose register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMULL, SMULL2 (by element): Signed multiply long (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SMULL, SMULL2 (vector): Signed multiply long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQABS: Signed saturating absolute value.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQADD: Signed saturating add.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQDMLAL, SQDMLAL2 (by element): Signed saturating doubling multiply-add long (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQDMLAL, SQDMLAL2 (vector): Signed saturating doubling multiply-add long.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQDMLSL, SQDMLSL2 (by element): Signed saturating doubling multiply-subtract long (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQDMLSL, SQDMLSL2 (vector): Signed saturating doubling multiply-subtract long.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQDMULH (by element): Signed saturating doubling multiply returning high half (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQDMULH (vector): Signed saturating doubling multiply returning high half.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQDMULL, SQDMULL2 (by element): Signed saturating doubling multiply long (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQDMULL, SQDMULL2 (vector): Signed saturating doubling multiply long.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQNEG: Signed saturating negate.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQRDMLAH (by element): Signed saturating rounding doubling multiply accumulate returning high half (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQRDMLAH (vector): Signed saturating rounding doubling multiply accumulate returning high half (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQRDMLSH (by element): Signed saturating rounding doubling multiply subtract returning high half (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQRDMLSH (vector): Signed saturating rounding doubling multiply subtract returning high half (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQRDMULH (by element): Signed saturating rounding doubling multiply returning high half (by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQRDMULH (vector): Signed saturating rounding doubling multiply returning high half.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQRSHL: Signed saturating rounding shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQRSHRN, SQRSHRN2: Signed saturating rounded shift right narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQRSHRUN, SQRSHRUN2: Signed saturating rounded shift right unsigned narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQSHL (immediate): Signed saturating shift left (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQSHL (register): Signed saturating shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQSHLU: Signed saturating shift left unsigned (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQSHRN, SQSHRN2: Signed saturating shift right narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQSHRUN, SQSHRUN2: Signed saturating shift right unsigned narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQSUB: Signed saturating subtract.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQXTN, SQXTN2: Signed saturating extract narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SQXTUN, SQXTUN2: Signed saturating extract unsigned narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SRHADD: Signed rounding halving add.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SRI: Shift right and insert (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SRSHL: Signed rounding shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SRSHR: Signed rounding shift right (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SRSRA: Signed rounding shift right and accumulate (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SSHL: Signed shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SSHLL, SSHLL2: Signed shift left long (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SSHR: Signed shift right (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SSRA: Signed shift right and accumulate (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SSUBL, SSUBL2: Signed subtract long.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SSUBW, SSUBW2: Signed subtract wide.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ST1 (multiple structures): Store multiple single-element structures from one, two, three, or four registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ST1 (single structure): Store a single-element structure from one lane of one register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ST2 (multiple structures): Store multiple 2-element structures from two registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ST2 (single structure): Store single 2-element structure from one lane of two registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ST3 (multiple structures): Store multiple 3-element structures from three registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ST3 (single structure): Store single 3-element structure from one lane of three registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ST4 (multiple structures): Store multiple 4-element structures from four registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ST4 (single structure): Store single 4-element structure from one lane of four registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STBFADD, STBFADDL: Atomic BFloat16 floating-point add, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STBFMAX, STBFMAXL: Atomic BFloat16 floating-point maximum, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STBFMAXNM, STBFMAXNML: Atomic BFloat16 floating-point maximum number, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STBFMIN, STBFMINL: Atomic BFloat16 floating-point minimum, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STBFMINNM, STBFMINNML: Atomic BFloat16 floating-point minimum number, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STFADD, STFADDL: Atomic floating-point add, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STFMAX, STFMAXL: Atomic floating-point maximum, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STFMAXNM, STFMAXNML: Atomic floating-point maximum number, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STFMIN, STFMINL: Atomic floating-point minimum, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STFMINNM, STFMINNML: Atomic floating-point minimum number, without return.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STL1 (SIMD&FP): Store-release a single-element structure from one lane of one register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STLUR (SIMD&FP): Store-release SIMD&FP register (unscaled offset).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STNP (SIMD&FP): Store pair of SIMD&FP registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STP (SIMD&FP): Store pair of SIMD&FP registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STR (immediate, SIMD&FP): Store SIMD&FP register (immediate offset).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STR (register, SIMD&FP): Store SIMD&FP register (register offset).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STTNP (SIMD&FP): Store unprivileged pair of SIMD&FP registers, with non-temporal hint.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STTP (SIMD&FP): Store unprivileged pair of SIMD&FP registers.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - STUR (SIMD&FP): Store SIMD&FP register (unscaled offset).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SUB (vector): Subtract (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SUBHN, SUBHN2: Subtract returning high narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SUDOT (by element): Dot product with signed and unsigned integers (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SUQADD: Signed saturating accumulate of unsigned value.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - SXTL, SXTL2: Signed extend long: an alias of SSHLL, SSHLL2.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - TBL: Table vector lookup.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - TBX: Table vector lookup extension.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - TRN1: Transpose vectors (primary).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - TRN2: Transpose vectors (secondary).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UABA: Unsigned absolute difference and accumulate.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UABAL, UABAL2: Unsigned absolute difference and accumulate long.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UABD: Unsigned absolute difference (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UABDL, UABDL2: Unsigned absolute difference long.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UADALP: Unsigned add and accumulate long pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UADDL, UADDL2: Unsigned add long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UADDLP: Unsigned add long pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UADDLV: Unsigned sum long across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UADDW, UADDW2: Unsigned add wide.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UCVTF (scalar SIMD&FP): Unsigned integer convert to floating-point (scalar SIMD&FP).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UCVTF (scalar, fixed-point): Unsigned fixed-point convert to floating-point (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UCVTF (scalar, integer): Unsigned integer convert to floating-point (scalar).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UCVTF (vector, fixed-point): Unsigned fixed-point convert to floating-point (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UCVTF (vector, integer): Unsigned integer convert to floating-point (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UDOT (by element): Dot product unsigned arithmetic (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UDOT (vector): Dot product unsigned arithmetic (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UHADD: Unsigned halving add.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UHSUB: Unsigned halving subtract.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMAX: Unsigned maximum (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMAXP: Unsigned maximum pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMAXV: Unsigned maximum across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMIN: Unsigned minimum (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMINP: Unsigned minimum pairwise.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMINV: Unsigned minimum across vector.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMLAL, UMLAL2 (by element): Unsigned multiply-add long (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMLAL, UMLAL2 (vector): Unsigned multiply-add long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMLSL, UMLSL2 (by element): Unsigned multiply-subtract long (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMLSL, UMLSL2 (vector): Unsigned multiply-subtract long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMMLA (vector): Unsigned 8-bit integer matrix multiply-accumulate to 32-bit integer (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMOV: Unsigned move vector element to general-purpose register.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMULL, UMULL2 (by element): Unsigned multiply long (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UMULL, UMULL2 (vector): Unsigned multiply long (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UQADD: Unsigned saturating add.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UQRSHL: Unsigned saturating rounding shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UQRSHRN, UQRSHRN2: Unsigned saturating rounded shift right narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UQSHL (immediate): Unsigned saturating shift left (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UQSHL (register): Unsigned saturating shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UQSHRN, UQSHRN2: Unsigned saturating shift right narrow (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UQSUB: Unsigned saturating subtract.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UQXTN, UQXTN2: Unsigned saturating extract narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - URECPE: Unsigned reciprocal estimate.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - URHADD: Unsigned rounding halving add.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - URSHL: Unsigned rounding shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - URSHR: Unsigned rounding shift right (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - URSQRTE: Unsigned reciprocal square root estimate.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - URSRA: Unsigned rounding shift right and accumulate (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - USDOT (by element): Dot product with unsigned and signed integers (vector, by element).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - USDOT (vector): Dot product with unsigned and signed integers (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - USHL: Unsigned shift left (register).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - USHLL, USHLL2: Unsigned shift left long (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - USHR: Unsigned shift right (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - USMMLA (vector): Unsigned and signed 8-bit integer matrix multiply-accumulate to 32-bit integer (vector).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - USQADD: Unsigned saturating accumulate of signed value.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - USRA: Unsigned shift right and accumulate (immediate).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - USUBL, USUBL2: Unsigned subtract long.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - USUBW, USUBW2: Unsigned subtract wide.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UXTL, UXTL2: Unsigned extend long: an alias of USHLL, USHLL2.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UZP1: Unzip vectors (primary).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - UZP2: Unzip vectors (secondary).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - XAR: Exclusive-OR and rotate.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - XTN, XTN2: Extract narrow.
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ZIP1: Zip vectors (primary).
- [x] Spec [x] Interpreter [x] Gadget [x] Tests - ZIP2: Zip vectors (secondary).
