# x86 instruction unit-corpus producer

`build-unit-corpus.py` produces new independently validated x86 instruction
cases for promotion to the checked-in source tables. It verifies the
dependency-manifest commits, links the K assembly
programs as fixed-address x86-64 ELF, converts each checked K state queue into
compact architectural state deltas, fills missing or disputed K states from a
provenanced native-Intel trace, and adds every profile-valid FEX contract that is
not an exact duplicate, without FEX's reduced-precision x87 request. It
assembles the repository cases the same way and accepts each only when its
declared final state is the native one. It rejects unclassified FEX feature
requirements and never converts an illegal instruction into an exclusion.

The freestanding `native-k-trace.cpp` helper runs generated requests on an
Intel Linux host and records GPR, flag, and XSAVE state without libc. It
single-steps K programs and runs each repository case uninterrupted to its
`HLT`, so no trap flag of its own reaches the flags a case stores. Both
producer stages use LLVM only while regenerating the asset. The resulting
CTest has no runtime dependency on LLVM, NASM, FEX, K, Python, or the native oracle.
See the corpus README under `tests/unit/x86_64/corpus/` for the canonical
workflow and profile contract.

`masked-flags-subset.py` chooses the checked-in cases of the
`x86-64-masked-flags` tests from XE2's masked-flag request sets and the Xeon's
responses: one case per combination of instruction, DAZ and FTZ, the Xeon's
flags and the flags each engine's host instructions raise. With `--all` it
writes every case, which the same test executable runs as a whole-set check.
