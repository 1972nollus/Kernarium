# x86-64 gadget engine

The engine executes x86-64 userspace as data-only streams of pointers to
handwritten arm64 gadgets (`engine.S` and `engine/*.inc`). `native_builder.cpp`
turns decoded instructions into those streams, `executor.cpp` owns the
translation cache, and `native_memory.cpp` is the C++ side of the software
TLB. This file records the contracts that are not obvious from the code.

## Gadget banks

Two banks implement the scalar integer instructions.

- The ordinary bank decodes its operands from stream words: a register form
  reads register offsets and accesses the CPU state register-indexed, and a
  memory form resolves a descriptor. It covers every shape the decoder
  produces.
- The register-baked bank is generated at build time by
  `engine/generate_baked.py` into `baked_scalar.inc`, one entry per operand
  combination: MOV, ADD, SUB, AND, OR, XOR, CMP and TEST in their
  register-register, register-immediate, register-memory, memory-register and
  memory-immediate forms, the widening moves, INC, DEC, NEG and NOT on a
  register, LEA, and 64-bit PUSH and POP of a register. Every register offset
  is an immediate in the entry, so the host forwards the CPU-state stores into
  the loads. A `[base + displacement]` address is fused into the entry; an
  indexed address is one shared address entry followed by the access entry.
  INC and DEC are enumerated a second time by the carry their producer left,
  because they preserve the carry flag and so have to settle the pending one
  before recording their own: a settled, borrow, carry or cleared source
  reaches RFLAGS with at most one condition instead of a call to the carry
  materializer.
  `native_baked.hpp` declares the tables. Their enumeration order (registers
  in offset order, the four high-byte registers after the sixteen low ones,
  the sixteen bases followed by "none") is shared with the generator and must
  change with it.

The builder takes a baked entry for every 64-bit-mode instruction the bank
enumerates that has no segment override, address-size override or LOCK
prefix, and the ordinary bank for everything else. `UML_X86_64_GADGET_BAKED=0`
in the environment keeps the ordinary bank for every instruction, for
comparison.

## Host-flag fusion

A producer that computes with a flag-setting host instruction leaves NZCV
describing the x86 flags it defined, and the builder lets the immediately
following SETcc, CMOVcc or Jcc read them instead of materializing the deferred
record. The deferred record is always written as well, so any later consumer
stays correct.

| Producer | Conditions read from NZCV |
| --- | --- |
| CMP, SUB at 32 and 64 bits; every width in the baked bank | all but parity |
| ADD at 32 and 64 bits; every width in the baked bank | all but parity, BE and A; B and AE read the swapped host condition, because host C after an addition is the carry out rather than the inverted borrow |
| TEST, AND at 32 and 64 bits; every width in the baked bank | all but parity and the four carry conditions |
| INC, DEC, NEG on a 32- or 64-bit register | as TEST and AND: N, Z and V describe the operation, C does not |
| memory forms | the baked bank's register-destination forms and its non-writing CMP and TEST; a store's epilogue compares addresses and clobbers NZCV |

The narrow widths qualify through a compare of the two operands shifted to
the top of a 32-bit register, which sets NZCV exactly as the guest-width
operation would. The ordinary bank does not do this, so its narrow producers
keep the deferred path.

A transfer that defines no flags does not end a fusion. MOV, MOVZX and MOVSX
between registers, and LEA, pass the producer's host flags through to the next
consumer, which is what lets a compare reach a CMOVcc or a Jcc that a register
copy sits between. A memory destination still ends it.

## Deferred flags

Every flag-defining scalar operation records left operand, right operand,
result, operation and width, and consumers materialize on demand. SHL, SHR and
SAR record the original value, the masked nonzero count and the result
(operations 8 to 10 of `DeferredFlagOperation`); the materializer derives the
shifted-out carry from them, defines overflow only for a count of one and
leaves the auxiliary flag alone. IMUL and MUL overlay carry and overflow on
whatever producer is pending instead of materializing it. Rotates keep the
eager overlay.

## Blocks and branches

A block ends at an unconditional transfer or at the 64-instruction fetch
window, not at its first conditional branch. The fall-through is translated
into the same stream, so not taking the branch costs one dispatch where it
used to cost a link follow, and the taken edge keeps its link. A build with no
link resolver -- the budget-limited replay of a cached block -- keeps the
terminal form instead and leaves the translated tail unreachable.

The retirement counter carries the dispatch's instruction budget as a negative
bias, so an edge that follows a link tests the sign of one addition instead of
loading the limit and comparing against it. `uml_x86_64_gadget_leave` takes
the bias back out before it reports what the dispatch retired.

## Software TLB

`Executor::prepare` enters an address-space generation, and it must not leave
behind a translation of a page whose mapping has changed. The address space
describes the mapping ranges a reader has not yet seen, so a reader only a few
changes behind replays exactly those ranges and retires just the entries they
cover. The whole table is cleared only when the address space cannot describe
the span, when the span covers more than a quarter of the entries, or when the
memory or the generation changed.

## Floating-point environment

A dispatch runs in the guest's rounding mode. Entry maps MXCSR's two-bit
field onto FPCR's, which numbers the directed modes the other way round, and
every write to MXCSR republishes it; FPCR is written only when the mode would
change, which for a guest that never leaves round-to-nearest is never. The
rounding gadgets therefore round with the host's current mode instead of
branching on MXCSR, and the arithmetic honours the guest's mode as well.

`uml_x86_64_gadget_leave` restores round-to-nearest from FPCR itself rather
than from a copy saved at entry, because a gadget that faults out of a
stack-using sequence can reach it with a different stack pointer.

## Code pages

`code_page_registry.hpp` keeps, per address-space generation, the set of pages
in which any executor of that generation holds a cached translation. A store
publishes a code change only when its page is in that set, which is what
makes publication precise under READ_IMPLIES_EXEC, where every readable page
is executable. The registry guarantees:

- a page enters the set before any translation built from it is handed out
  (`CodePageHold` covers the build), and leaves it when the last cached
  translation of the page in every executor is unlinked or invalidated;
- when a page enters the set, every participating executor's write-side TLB
  entry for it is retired before the registration returns, and an executor
  inside the engine is waited for until it leaves or passes a checkpoint,
  bounded at 200 ms, so no executor keeps a stale "publishes nothing" entry;
- a TLB fill that raced a registration retries: the fill re-reads the
  registry's change counter after publishing its entry.

The Wine ARM64EC CPU service, whose arm64 translations live outside the
registry, publishes on every executable store instead
(`Executor::publish_every_executable_store`).
