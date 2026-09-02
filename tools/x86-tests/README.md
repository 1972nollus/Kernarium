# x86 instruction unit-corpus producer

`build-unit-corpus.py` is the sole producer for the checked-in x86 instruction
unit corpus. It verifies the dependency-manifest commits, links the K assembly
programs as fixed-address x86-64 ELF, converts each checked K state queue into
compact architectural state deltas, fills missing or disputed K states from a
provenanced native-Intel trace, and adds every profile-valid FEX contract that is
not an exact duplicate. It rejects unclassified FEX feature requirements and
never converts an illegal instruction into an exclusion.

The freestanding `native-k-trace.cpp` helper single-steps generated requests on
an Intel Linux host and records GPR, flag, and XSAVE state without libc. Both
producer stages use LLVM only while regenerating the asset. The resulting
CTest has no dependency on LLVM, NASM, FEX, K, Python, or the native oracle.
See the corpus README under `tests/unit/x86_64/corpus/` for the canonical
workflow and profile contract.
