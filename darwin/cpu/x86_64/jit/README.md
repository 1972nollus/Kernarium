# x86-64 JIT engine

`fex_engine.cpp` runs x86-64 userspace through FEXCore's x86 frontend, IR
passes and arm64 backend, embedded so that guest memory stays behind the static
MMU. It is a macOS product only;
[Architecture](../../../../docs/architecture.md#cpu-execution-and-scheduling)
records how the build, the audit and the iOS packaging prove it. The guest CPU
state, decoder, CPUID, software TLB entry, code-page registry and split-operand
lock are `uml_x86_64_target` in the parent directory, shared with the
[gadget engine](../gadgets/README.md); this target links that and the
executable-memory owner, never the gadget engine. Outside this directory only
the runtime seam, `darwin/runtime/x86_jit_runtime.cpp`, includes it. This file
records the contracts that are not obvious from the code.

## Selection

`UML_BUILD_X86_64_JIT` builds the engine wherever the x86-64 guest is built,
which is macOS only. On the x86-64 product, `--execution-engine jit` selects it
and the default is the gadget engine, by name; selecting the JIT fails where the
build or host cannot run it (JIT memory with a write window, FEAT_LSE and
FEAT_LSE2). The JIT runs both long64 and compat32 tasks. The arm64 product's
x86 test guest builds the engine for its unit tests only and runs x86 code on
the gadget engine.

## Address spaces and memory

One engine belongs to one vCPU thread. It keeps a FEXCore context and thread
per address-space generation, execution mode and floating-point mode, at most
eight, and releases the least recently used, because FEXCore's block caches
are keyed by guest RIP alone. A compat32 context decodes and computes as 32-bit code.

A compat32 address wraps at 4 GiB, segment base and offset alike: the TLB holds
only wrapped addresses, so the miss path wraps the 64-bit sum the backend
forms, and the string helper wraps each element. The guest kernel's task size,
0xffffe000, bounds the user range. CS is only data: a far transfer, IRET or
selector load changes no decode mode. A load of FS or GS takes its base from
the guest kernel's TLS descriptors, by GDT entry, and any other selector loads
base 0; no selector load faults. These match the gadget engine. compat32 code
makes system calls with INT 0x80; SYSCALL and SYSENTER raise `#UD` there, as
they do in the gadget engine. A compat32 dispatch leaves the upper halves of
the general registers it does not write as it found them, as the gadget engine
does.

FEXCore's generated code reaches guest memory through an inline software TLB. A
miss calls the engine, which fills the entry from the static MMU or ends the
dispatch with the guest's fault; the miss path preserves every register the
access sequence holds, including the element address of a masked move and the
backend's vector temporaries. An access that crosses a guest page, or an
ordered access that crosses a 16-byte granule, completes through a bounce
buffer in the address space's own fault order. A locked operand that crosses a
granule has no single-copy-atomic host access; as in the gadget engine, it is
an ordinary load and store of the bounce buffer under the process-wide
split-operand lock, faulting before anything is stored. A VEX gather translates
its elements one at a time into a temporary, so on a fault the engine itself
gathers the elements before the faulting one into the guest state, as x86 does.
FEXCore allocates and publishes code only through the executable-memory owner's
hooks, and its call-return stack, which generated code addresses without a
bounds check, sits in that owner's guarded data reservation, so running off it
stops at a guard page. A dispatch starts at most 64 Ki instructions, which
keeps unbalanced calls and returns inside that stack.

The engine joins the code-page registry with a function that retires its own
TLB entry for a page. It counts a page in before the decoder reads the page's
bytes, so a store another vCPU translated lands first and every later store
publishes. The guest runs READ_IMPLIES_EXEC, so only a store to a page the
registry holds publishes a code change. A publishing store into the code of
the unit running it, which would go on to run the bytes it replaces, stops
that unit before it stores; its instruction runs again as a unit of its own,
and the dispatch goes on from freshly fetched code.

## State at every exit

A dispatch leaves the engine at a system call, a fault, a TLB miss the runtime
must resolve, an exhausted budget or a pending interrupt, often in the middle
of a block. At each of those exits the guest registers, RFLAGS, the vector
registers and the x87 stack and TOP are precise: the embedded x87 pass writes
its cached stack values back after every x87 operation instead of at the
block's end. A fault leaves the state as its instruction found it, because
the guest resolves the fault, a copy-on-write store among them, and runs the
instruction again: a load whose bytes the same instruction then stores takes
write permission, so a read-modify-write faults before it changes a register
or a flag; POP to memory, PUSHA, a far CALL and ENTER store before they move
RSP; POPA, FLDENV, FRSTOR, FXRSTOR, XRSTOR and IRET probe their whole operand
before they load state from it; and register allocation never commits a value
to a guest register before a later access of the same instruction, or an
earlier instruction's value before such an access. A load whose value goes
unused, such as a compare whose flags are dead, still faults, and so does a
rotate through carry by zero. The exceptions probe's fault-state mode holds
both engines to that. Each dispatch also:

- decodes by the address space's execution mode, not by whatever CS holds;
- starts from the host FPCR the guest's MXCSR implies, because FEXCore applies
  MXCSR only when the guest loads it;
- retires exactly one instruction, or one element of a repeated string
  instruction, when RFLAGS.TF is set on entry, and reports a single step;
- clears RF once an instruction completes; the runtime sets it for a fault.

REP MOVS and REP STOS run element by element in page-bounded chunks, spend the
budget as they go and stop between elements, with RF, when it is spent or an
interrupt is pending. A fault leaves RCX, RSI and RDI with the progress made.
Repeated CMPS and SCAS compute each element's flags as it completes, so a
stop between elements shows the flags of the last compare.

A dispatch reports as retired only the instructions that completed, as the
gadget engine does: a fault, `#UD` or `#DE` retires nothing of its instruction,
a trap or system call retires it, and a repeated string instruction retires
once however many elements spent the budget. A unit's entry, and a loop back
to it, spend the budget for every instruction the unit runs; an exit from
inside the unit hands back the ones it did not reach, and a repeated element
spends a unit that retires nothing. The `--retirement` cases of the corpus
runners hold both engines to the same counts.

A signal handler without SA_RESTORER returns to the sigreturn trampoline,
`DARWIN_UML_SIGRETURN_TRAMPOLINE`, which no guest page backs on the x86-64
slice. The seam turns the fetch fault the engine stops with there into
`rt_sigreturn`, as the gadget runtime does at the same address.

## Code buffer

FEXCore compiles a unit into a temporary buffer sized from an estimate of host
bytes per IR node, 128 when embedded. Embedded FEXCore has no guard-page signal
handler, because the host owns signals, and cannot protect a guard page in
MAP_JIT memory, so the maintained series bounds every write instead: each
emitter write, alignment and cursor move, and the block tail with its RIP
entries, is checked against the buffer's end, and an overrun restarts the
compile with twice the space. A unit that cannot fit an empty code buffer stops
the process. `FexEngineOptions::code_bytes_per_node` lowers the estimate and
`code_buffer_restarts()` counts the restarts, which is how
`x86-64-jit-code-buffer-growth` makes every large unit grow.

## Guest-visible model

CPUID comes from `../cpuid.hpp`, with nothing withheld; AVX and AVX2 run as
pairs of 128-bit operations on hosts without 256-bit SVE. x87 is the full
80-bit format. Stores are totally ordered for every address space: FEXCore
emulates TSO with acquire loads and release stores, and `space_orders_stores()`
is the one place a per-address-space decision will key it.
DIV and IDIV raise `#DE` for a zero divisor or a quotient too wide for its
destination before they write either destination. RDRAND and RDSEED read the
host CSPRNG through a helper, as the gadget engine does, and always succeed.

SIMD floating point gives x86's results whatever the host's features. The
maintained series follows each x86 SIMD floating-point operation, or each
128-bit part of one, with an `EmbedderFPCheck` op that holds the x86 sources
and tests the host's result in line for the cases where arm64 arithmetic can
differ; a cold path hands those to `fp_model.cpp`, which computes x86's result
in integers and SoftFloat because it runs under the guest's FPCR. The model
covers every operation the op names, with MXCSR's rounding, DAZ, FTZ and
flags, so a check needs only to find the cases.

A translation assumes its context's floating-point mode, MXCSR.DAZ and FTZ,
and the dispatch selects the space for the guest's MXCSR. An LDMXCSR, FXRSTOR
or XRSTOR that changes the mode ends the dispatch after itself, and the next
one runs in the new mode's space; one that changes only the rounding stays.
FPCR.FZ is MXCSR.DAZ, which flushes inputs as x86 does but also flushes a
result that is tiny before rounding, where x86 applies FTZ after rounding.
The checks take:

- a NaN result of an arithmetic, square root, horizontal, ADDSUB, DPPS, DPPD
  or FMA operation: x86 propagates the first source's NaN, keeps the sign of
  a NaN it subtracts or negates, and generates the negative indefinite;
- under DAZ and FTZ, a zero product, quotient, FMA or narrowed double from
  nonzero inputs, which x86 may round up to the smallest normal; a sum tiny
  enough to flush is exact, so the host flushes it as x86 does, and a DPPS or
  DPPD checks its products too;
- under FTZ alone, a result up to the smallest normal, which x86 flushes when
  it is tiny after rounding;
- under DAZ alone, a zero result, which may be a tiny one x86 keeps;
- under DAZ, a denormal MIN or MAX result, which FPCR.FZ compares flushed but
  returns as it was;
- a DPPS or DPPD under DAZ or FTZ alone, which rounds in steps its result
  does not show, always;
- RCPPS, RCPSS, RSQRTPS and RSQRTSS, which never use the host's estimates,
  whose results depend on FEAT_RPRES: the op divides in line, which lies
  within x86's error as the gadget engine's quotient does, and sends the
  denormal, zero, huge and negative inputs to the model.

Comparisons, conversions to integers, widening, rounding and half-precision
conversions take DAZ from FPCR.FZ and need no check.

MXCSR keeps every bit LDMXCSR, FXRSTOR and XRSTOR load, the exception flags
included, and FXSAVE and XSAVE store an MXCSR_MASK of 0xFFFF, as the gadget
engine does. Like the gadget engine, the JIT accumulates no flags while MXCSR
masks every exception: the host's cumulative flags are not x86's (FPCR.FZ
raises no precision for a flush and detects tininess before rounding, the
estimates' in-line division raises precision, and no host flag shows a
denormal operand), and folding them in would give flags neither x86 nor the
gadget engine gives.

The x87 status word keeps every masked exception's sticky flag, and ES and B
while an unmasked one is pending. The engine does not deliver unmasked SSE or
x87 exceptions as `#XM` or `#MF`, because FEXCore keeps no MXCSR exception
flags and raises nothing from the x87 ones, and has no alignment-check `#AC`.
`run_guest_x86_64_exceptions.py --engine jit` fails exactly those two cases in
each mode.

An encoding the decoder or the dispatcher refuses raises `#UD`, never a host
failure, and so do FEXCore's own reserved escapes, 0F 3E and 0F 3F. The
undefined set follows hardware, except that compat32 code raises `#UD` for
0x82, LES, LDS, LSS, LFS, LGS, direct far CALL and JMP, SYSCALL and SYSENTER,
as it does on the gadget engine. Other encodings the gadget engine refuses
although hardware runs them, such as LSL or a SETcc whose ModRM reg field is
not 0, the JIT runs.

## Instruction corpus

`x86-64-jit-instruction-corpus` runs the x86-64 instruction corpus through the
engine in the unit suite. The checked-in corpus has no compat32 records; the
JIT runner runs compat32 code, so the pinned FEX i386 corpus and the compat32
`--retirement` cases go through the engine too.
[`jit_corpus_exclusions.tsv`](../../../../tests/unit/x86_64/jit_corpus_exclusions.tsv)
names each record the engine is known to fail with its class; the test fails
when a listed record passes or a row names no record, so a fix removes its rows
in the same change. The list is empty. `x86-64-jit-sse-exactness` runs XE's
differential harness through the engine, against the same x86 model as the
gadget engine's `x86-64-sse-exactness`.
