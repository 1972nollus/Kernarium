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
per address-space generation, execution mode, store order and floating-point
mode, at most eight, and releases the least recently used, because FEXCore's
block caches are keyed by guest RIP alone. A compat32 context decodes and computes as 32-bit code.

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
gathers the elements before the faulting one into the guest state and leaves
the mask as x86 does.
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

Each space's FEXCore context keeps a 16 MiB code buffer and a 32 KiB
dispatcher, and each vCPU keeps up to eight spaces. The owner serves both from
its window near the host image, because the generated code calls FEXCore's
fallbacks there; a buffer beyond the window's capacity runs x87-heavy code
about a third slower ([architecture](../../../../docs/architecture.md)). The
window serves buffers in the order spaces map them, however little a space
runs: with ten vCPUs, idle spaces' buffers can fill it, and a busy space's
buffer, grown past 16 MiB, then lands beyond it.

## Guest-visible model

CPUID comes from `../cpuid.hpp`, with nothing withheld; AVX and AVX2 run as
pairs of 128-bit operations on hosts without 256-bit SVE. x87 is the full
80-bit format. DIV and IDIV raise `#DE` for a zero divisor or a quotient too wide for its
destination before they write either destination. RDRAND and RDSEED read the
host CSPRNG through a helper, as the gadget engine does, and always succeed.

SIMD floating point gives x86's results whatever the host's features. The
maintained series follows each x86 SIMD floating-point operation, or each
128-bit part of one, with an `EmbedderFPCheck` op that holds the x86 sources
and tests the host's result in line for the cases where arm64 arithmetic can
differ; a cold path hands those to `fp_model.cpp`, which computes x86's result
in integers and SoftFloat because it runs under the guest's FPCR. The model
covers every operation the op names, with MXCSR's rounding, DAZ, FTZ and
flags, so a check needs only to find the cases. The checks are the same on
every host. FEXCore's FEAT_AFP path stays off: it sets FPCR.NEP with AH, and
NEP gives a one-input scalar instruction a false dependency on its
destination, while AH and FIZ would take over only the NaN and DAZ cases of
plain arithmetic and the M1 has no FEAT_AFP
([optimization history](../../../../docs/optimization-history.md#floating-point-control-features)).

A translation assumes its context's floating-point mode: MXCSR.DAZ and FTZ,
which exceptions MXCSR unmasks, none, only some of invalid operation,
division by zero and overflow (the special checks), or any other (all
checks), whether the x87 control word unmasks any (the x87 checks), and
RFLAGS.AC (the alignment checks). The dispatch selects the space for the
guest's MXCSR, control word and RFLAGS. An LDMXCSR, FLDCW, FLDENV, FRSTOR,
FNINIT, FNSAVE, FXRSTOR, XRSTOR, POPF or IRET that changes the mode ends the
dispatch after itself, and the next one runs in the new mode's space; one
that changes only the rounding or the precision stays.
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
  within x86's error as the gadget engine's quotient does, and sends
  denormal and zero inputs, RCP's huge ones and RSQRT's negative ones,
  infinities and NaNs to the model.

Comparisons, conversions, rounding and half-precision conversions take DAZ
from FPCR.FZ, and their results need no check.

While MXCSR unmasks an exception, the checks also settle each instruction's
exceptions, and every SIMD floating-point instruction has one. The special
checks send the model a result whose exponent is all ones or one below, a
NaN source of a comparison, MIN, MAX, rounding or widening, a source a
conversion to an integer or to half precision cannot hold, and a DPPS or
DPPD source large enough to overflow; a value that passes raises none of
those three exceptions. All checks send every operation. The model gives
x86's outcome: an unmasked exception faults with `#XM`, whose error code is
MXCSR, leaving the destination alone, and invalid operation, a denormal
operand and division by zero take precedence over the exceptions of the
result. An instruction checked in two 128-bit parts settles after its last.
Under the special checks a part that no test sent raises the flags of
masked exceptions in the host's FPSR (below), so when the last part faults,
the instruction runs again as a unit of its own with all checks, and its
fault raises every part's flags. Under all checks only what the model
computes raises flags. Each such fault compiles its instruction once more,
uncached.

MXCSR keeps every bit LDMXCSR, FXRSTOR and XRSTOR load, the exception flags
included, and FXSAVE and XSAVE store an MXCSR_MASK of 0xFFFF, as the gadget
engine does. Its exception flags are sticky, and the JIT keeps them while
the guest masks the exceptions. The host's cumulative FPSR flags hold them
through a dispatch: IOC, DZC, OFC, UFC and IXC are x86's invalid operation,
division by zero, overflow, underflow and precision. A dispatch clears them
as it starts and adds them to MXCSR as it ends; STMXCSR, FXSAVE and XSAVE
add them to the MXCSR they store, and LDMXCSR, FXRSTOR and XRSTOR clear
them with the flags they load. IDC is never a denormal operand: FPCR.FZ
raises it for an input DAZ flushes, where x86 raises nothing. Under all
checks the model settles every instruction's flags into MXCSR, and FPSR's
are left out. Host code a dispatch runs leaves FPSR as it found it:
FEXCore's compiler and block linker, and the model, which computes with
host arithmetic. The x87 helpers compute in software, and a 3DNow!
instruction, which MXCSR does not govern, puts back FPSR and MXCSR after
itself.

Where the host's flags are not x86's, the translation chooses another host
instruction. COMIS compares with FCMPE and UCOMIS with FCMP, and scalar MIN
and MAX with FCMPE. CMP raises what each of x86's 32 predicates raises: FCMEQ
is quiet and FCMGE and FCMGT signal, a quiet relation compares operands whose
NaN lanes a quiet ordered test cleared, and FALSE and TRUE compare for the
flags alone. ROUND raises precision with FRINTX unless its immediate
suppresses it, and a DPPS or DPPD with no destination lane still computes its
sum. An estimate's quotient raises precision, which x86 does not, so FPSR goes
back after it unless MXCSR or FPSR holds precision already; while MXCSR does,
the estimate reads no FPSR, since a read waits for every floating-point
instruction before it. The in-line tests compare an operation's own lanes
alone, since a signalling NaN in another raises the host's invalid operation
even for FCMEQ. What a check sends to the model raises the model's flags,
which the host may lack, such as an FTZ flush's underflow and precision. Under
DAZ alone FPCR.FZ flushes a result x86 keeps and raises underflow where it is
exact; every flushed result goes to the model, so there each part of a checked
instruction saves FPSR first, and the model's flags replace the part's. Under
the special checks an instruction saves FPSR as it starts, and the model puts
it back when a fault or the second run abandons the instruction, whose other
lanes may have raised flags x86's fault never computes.

Three flags stay inexact, as on the gadget engine and as
`x86-64-jit-masked-flags` names them: the denormal operand while DAZ is
clear, which no host flag shows, so it is raised only where the model
computes an instruction; underflow for an inexact product, quotient, FMA,
narrowing or dot-product step that rounds up to the smallest normal, which
the host raises, detecting tininess before rounding, and x86 does not; and
invalid operation for an FMA of infinity times zero plus a quiet NaN, which
the host raises and x86 does not. The last two are exact where the model's
flags replace the host's; anywhere else removing them would take FPSR from
before every such instruction, an MRS on the hottest path.

The x87 status word keeps every masked exception's sticky flag, and ES and B
while an unmasked one is pending, from the 80-bit helpers and from the inline
conversion of a single or double, which raises invalid operation for a
signaling NaN and a denormal operand for a denormal. The embedded stack follows
the tag word: a read of an empty register is a stack underflow and a push onto
a full one an overflow, each raising invalid operation with the stack-fault
flag and going on with the QNaN indefinite, and FXAM classifies its operand
from the 80-bit format. FPTAN, FSINCOS and FXTRACT check the register of
their second result first and on a full stack compute nothing, as on the
gadget engine, and FSIN, FCOS, FPTAN and FSINCOS leave an operand of 2^63 or
more as it is, with C2 set, in a branch of their own. A sine, cosine,
tangent or arctangent of a finite nonzero operand raises precision, and
FXTRACT normalizes a denormal operand. FNSAVE and FNSTENV tag each full register from its
value, as zero, special or valid, as an Intel host does at the store. C1
says whether rounding increased a result's magnitude, and an instruction
that rounds nothing clears or keeps it, as on the gadget engine, and FPREM and FPREM1 compute x87's exact remainders, partial ones
with C2 and complete ones with the quotient's low bits in C0, C3 and C1. An
overflow or underflow the control word unmasks rebiases a register result's
exponent by 24576, as x86 does and the gadget engine does through the same
SoftFloat state, and FSCALE scales across the whole exponent range.

Under the x87 checks, every waiting instruction -- each x87 instruction but
FNINIT, FNCLEX, FNSTSW, FNSTCW, FNSTENV, FNSAVE and the no-ops FNENI, FNDISI
and FNSETPM, and FWAIT, EMMS and any instruction with an MMX register
operand -- raises `#MF` while ES shows an exception pending, before it does
anything, with the status word and the control word, shifted left by 16, as
the error code. An instruction that raises an unmasked invalid operation,
denormal operand or division by zero, or whose store to memory overflows or
underflows unmasked, retires with only its status word changed, except that
a load completes beside a denormal operand and a comparison sets its
condition codes but does not pop, as on an Intel host. Every load of the
control word or the status word makes ES and B show whether a flag is
unmasked.

Under the alignment checks, an access of two, four or eight bytes whose
address is not a multiple of its size raises `#AC` before the instruction
does anything, as on the gadget engine: a load or store, an atomic, each
element of a REP MOVS or STOS (whose first decides), and the operand of a
bit test, whose byte the engine reads. An x87 80-bit operand wants eight
bytes, an environment or state image the width of its fields. Wider vector
operands, masked vector moves, gathers, extended-state images and far
transfers raise none. The gadget engine checks only an instruction's first
access and the JIT every one, which differs only for an instruction whose
later access is aligned differently, such as ENTER with a nesting level.
Whatever RFLAGS.AC holds, FXSAVE and FXRSTOR raise `#GP` for an image not
aligned to 16 bytes, and XSAVE, XSAVEOPT and XRSTOR for one not aligned to
64, as on the gadget engine.

An encoding the decoder or the dispatcher refuses raises `#UD`, never a host
failure, and so do FEXCore's own reserved escapes, 0F 3E and 0F 3F. The
undefined set follows hardware, except that compat32 code raises `#UD` for
0x82, LES, LDS, LSS, LFS, LGS, direct far CALL and JMP, SYSCALL and SYSENTER,
as it does on the gadget engine. The system instructions follow the gadget
engine's [model](../gadgets/README.md#system-instructions) of Linux at CPL 3
with UMIP: the privileged ones raise `#GP` before they touch memory, SIDT
stores Linux's UMIP values, and LSL reads its selector, from memory too,
and the flat user entries, the per-CPU entry and a TLS entry the host
installed (`Embedder.SegmentPresent`).

## Memory order

x86 keeps every store after the stores before it and every load after the
loads before it. The JIT keeps that order for an address space the kernel
marks shared (`DARWIN_UML_DISPATCH_MM_SHARED`, set as the
[common kernel README](../../../../kernel/common/README.md) states) and
drops it elsewhere, where no other agent can tell. The mark selects the
space as the floating-point mode does: an ordered space is a FEXCore context
with TSO on, which gives general-register accesses acquire loads and release
stores and vector accesses barriers, and the engine puts a sequentially
consistent fence on each side of a REP MOVS or STOS, whose host helper
copies plainly. An unordered space uses plain loads and stores. MFENCE,
locked instructions and XCHG stay full barriers in both.

The kernel works the mark out afresh for every guest entry, so it clears
again once the last sharer goes: the last other task leaves the mm, the
last shared mapping is unmapped, or io_uring unpins the last ring. It
starts only inside a kernel entry of a task on the mm (the `clone()` that
starts a second task, the `mmap()` or `shmat()` that maps shared memory, or
the io_uring setup that pins a ring), so while the mm has one task no
relaxed code of it runs on another vCPU at that moment, and a fork child is
marked before its first instruction. A store before the `clone()` reaches
the new task before its first load through the scheduler's release and
acquire as the task starts. When the mark clears, every store of a former
sharer is visible to the first unmarked entry, as the common README states,
so relaxed code may run again. A space's ordered and unordered contexts
both stay cached, within the engine's eight spaces per vCPU, so a process
that shares and stops sharing over and over pays a context switch per
change, not a retranslation. A dispatch the kernel marks shared that runs
unordered code breaks the contract, and the runtime logs it (below).

FEXCore assumes an access through RSP reaches only the thread's own stack
and leaves it plain; an ordered space turns that off (StackTSOEnabled), since
a local variable whose address another thread holds is reached through RSP
by its owner. Pushes, pops, calls and returns stay plain: they reach only
the slots at the top of the thread's own stack.

The mark alone decides; no setting overrides it. With
`UML_X86_MM_SHARED_LOG` set, the runtime logs each change of an address
space's mark and of the ordering its code keeps, and every dispatch the
kernel marked shared that ran unordered code. The unit test
`x86-64-jit-store-order` runs message passing between two engines on two
host threads, with the data and flag reached through a general register or
through RSP: an ordered space never shows the flag without the data, and an
unordered one shows it, so the test would notice the order going missing.
Its `mp-cycle` case runs one space unordered, ordered, unordered and ordered
again on the same engines, as the mark comes and goes.

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
gadget engine's `x86-64-sse-exactness`, with its pass that unmasks a
denormal operand, and a third pass that holds the special checks to the
outcome of all checks.
