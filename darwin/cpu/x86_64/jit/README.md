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
FEAT_LSE2). The JIT runs long64 tasks; compat32 tasks stay on the gadget engine
under either selection, and the seam keeps that choice per dispatch so that the
JIT can take compat32 later. The arm64 product's x86 test guest builds the
engine for its unit tests only and runs x86 code on the gadget engine.

## Address spaces and memory

One engine belongs to one vCPU thread. It keeps a FEXCore context and thread
per address-space generation and execution mode, at most eight, and releases
the least recently used, because FEXCore's block caches are keyed by guest RIP
alone.

FEXCore's generated code reaches guest memory through an inline software TLB.
A miss calls the engine, which fills the entry from the static MMU or ends the
dispatch with the guest's fault; the miss path preserves every register the
access sequence holds, including the element address of a masked move. An access that crosses a
guest page, or an ordered access that crosses a 16-byte granule, completes
through a bounce buffer in the address space's own fault order. FEXCore
allocates and publishes code only through the executable-memory owner's hooks,
and its call-return stack, which generated code addresses without a bounds
check, sits in that owner's guarded data reservation, so running off it stops
at a guard page. A dispatch starts at most 64 Ki instructions, which keeps
unbalanced calls and returns inside that stack.

The engine joins the code-page registry with a function that retires its own
TLB entry for a page. It counts a page in before the decoder reads the page's
bytes, so a store another vCPU translated lands first and every later store
publishes. The guest runs READ_IMPLIES_EXEC, so only a store to a page the
registry holds publishes a code change.

## State at every exit

A dispatch leaves the engine at a system call, a fault, a TLB miss the runtime
must resolve, an exhausted budget or a pending interrupt, often in the middle
of a block. At each of those exits the guest registers, RFLAGS, the vector
registers and the x87 stack and TOP are precise: the embedded x87 pass writes
its cached stack values back after every x87 operation instead of at the
block's end. Each dispatch also:

- decodes by the address space's execution mode, not by whatever CS holds;
- starts from the host FPCR the guest's MXCSR implies, because FEXCore applies
  MXCSR only when the guest loads it;
- retires exactly one instruction, or one element of a repeated string
  instruction, when RFLAGS.TF is set on entry, and reports a single step;
- clears RF once an instruction completes; the runtime sets it for a fault.

REP MOVS and REP STOS run element by element in page-bounded chunks, spend the
budget as they go and stop between elements, with RF, when it is spent or an
interrupt is pending. A fault leaves RCX, RSI and RDI with the progress made.

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
destination before they write either destination.

The engine does not deliver unmasked SSE or x87 exceptions as `#XM` or `#MF`,
because FEXCore keeps no MXCSR or x87 exception flags, and has no
alignment-check `#AC`. `run_guest_x86_64_exceptions.py --engine jit` fails
exactly those two x86-64 cases.

## Instruction corpus

`x86-64-jit-instruction-corpus` runs the x86-64 instruction corpus through the
engine in the unit suite.
[`jit_corpus_exclusions.tsv`](../../../../tests/unit/x86_64/jit_corpus_exclusions.tsv)
names each record the engine is known to fail with its class; the test fails
when a listed record passes or a row names no record, so a fix removes its rows
in the same change. The classes are VEX gathers through the software TLB,
split locked operands, x87 signed zeros, SSE NaN, denormal and estimate rules,
16-bit SHLD over-counts and a store into its own block.
