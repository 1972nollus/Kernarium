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
  because they preserve the carry flag: a borrow, carry or cleared source
  reaches RFLAGS with at most one condition, a settled one inherits the
  pending bit, and an unknown one takes the lazy prologue described under
  deferred flags instead of a call to the carry materializer.
  `native_baked.hpp` declares the tables. Their enumeration order (registers
  in offset order, the four high-byte registers after the sixteen low ones,
  the sixteen bases followed by "none") is shared with the generator and must
  change with it.

The builder takes a baked entry for every 64-bit-mode instruction the bank
enumerates that has no segment override, address-size override or LOCK
prefix, and the ordinary bank for everything else. `UML_X86_64_GADGET_BAKED=0`
in the environment keeps the ordinary bank for every instruction, for
comparison.

The ordinary bank's memory descriptor resolves base, index, scale, segment and
address size with no branch on which of them are present, which means the
common long-mode address still pays for a segment base it adds as zero. That
addition is not free: the selected base is reached through a computed address
rather than a constant offset from the CPU state, a form the host cannot
disambiguate against the CPU-state stores every gadget makes, and it is the
most expensive instruction of the generic address path. The builder therefore
proves the absence of all three from the operand and sets one descriptor bit
(`UML_X86_64_MEMORY_PLAIN_ADDRESS`), on which the gadget stops once base,
scaled index and displacement are summed. The bit is only ever an optimization:
a descriptor that leaves it clear takes the general tail and resolves the same
address.

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
operation would. The ordinary bank, which carries all compat32 code, computes
on zero-extended operands instead, so its narrow producers transfer only what
survives that: CMP and SUB leave the zero and the four unsigned-order
conditions, TEST and AND leave zero alone, and ADD, whose carry lands in bit 8
or 16 of the host result, and the narrow INC, DEC and NEG keep the deferred
path. Register CMOVE and CMOVNE read the deferred zero result directly when no
producer is fused, without materializing the other flags.

A transfer that defines no flags does not end a fusion. MOV, MOVZX and MOVSX,
LEA, PUSH and POP in the baked bank pass the producer's host flags through to
the next consumer, which is what lets a compare reach a CMOVcc or a Jcc that a
register copy, a load or a store sits between. The bank's translation tests
the TLB tag without touching NZCV, its store epilogue orders addresses by the
sign of a difference, and its miss tails save NZCV around the calls they make.
The ordinary bank's transfers still end a fusion.

## Vector result forwarding

A vector integer register form writes its result to the state and leaves it in
v4, and in v5 with it at 256 bits. When the gadget immediately after it reads
exactly that register at exactly that width as its left operand, the builder
sets bit 62 of the descriptor and the gadget takes the operand from those
registers instead of reading the slot back, which would wait on the store.
Only register forms qualify on either side: v4 and v5 are call-clobbered, so a
memory form's miss or page-crossing path would destroy them. The builder
starts each block with nothing available, so a consumer is never the first
gadget of a block and a budget-limited replay cannot inherit a stale register.
The map2 and floating-point families keep the load; see X50.

## Deferred flags

Every flag-defining scalar operation records left operand, right operand,
result, operation and width, and consumers materialize on demand. SHL, SHR and
SAR record the original value, the masked nonzero count and the result
(operations 8 to 10 of `DeferredFlagOperation`); the materializer derives the
shifted-out carry from them, defines overflow only for a count of one and
leaves the auxiliary flag alone. IMUL and MUL overlay carry and overflow on
whatever producer is pending instead of materializing it. Rotates keep the
eager overlay.

INC and DEC preserve the carry flag. When the builder cannot see the producer
that defined it, the gadget copies that producer's record into the carry slot
(`CpuState::carry_slot`) and records its own with the pending bit set, rather
than settling the carry through the materializer; a second INC or DEC inherits
the bit, and a carry override riding on such a record settles it the old way.
Only a reader of the carry through an INC or DEC record pays for the slot: the
materializers compute that one flag from the slot record through the same
operation-and-width table, and the C++ materializer does the same. Every
record write clears the bit, so a producer that follows leaves nothing
pending.

An operation that defines every arithmetic flag it does not leave undefined --
COMISS and UCOMISS, PTEST, the packed string compares, the bit scans and
counts, and the BMI flag writers -- discards the pending record instead of
materializing it, because nothing of it survives. BT and its siblings leave
ZF alone, so they keep the record and write their carry into its override.

## Blocks and branches

A block ends at an unconditional transfer or at the 64-instruction fetch
window, not at its first conditional branch. The fall-through is translated
into the same stream, so not taking the branch costs one dispatch where it
used to cost a link follow, and the taken edge keeps its link. A build with no
link resolver -- the budget-limited replay of a cached block -- keeps the
terminal form instead and leaves the translated tail unreachable.

A dispatch whose remaining budget is shorter than the block replays it: the
block's first few instructions are assembled into a stream of their own, which
ends where the budget does. What that replay decodes from is the block's saved
source, not guest memory, so it recovers exactly the instructions the block was
built from however the guest's own bytes have moved since. A translation is
therefore built with no decoded instructions in it at all.

A replay is not rare. It happens once for each dispatch that runs out of
budget inside a block, which on the compiler workload is one block entry in
sixteen, so decoding the block again on every one of them costs about two
percent of the guest. It is also concentrated: about a tenth of the blocks a
workload translates are ever replayed, and each of those is replayed about
twenty times. So the first replay of a block keeps what it decoded, in a third
allocation that only such a block has, and every later replay of it reads that
instead. A block no dispatch ever ends inside -- nine in ten of them -- never
pays for the decoded form at all.

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
memory or the generation changed. The table is direct-mapped with 32768
entries, 1 MiB, indexed by the page number folded once at that width;
`native_tlb_entry_count` is the one constant, and the four assembly copies of
the index must change with it.

PUSH, POP, CALL and RET re-check the entry the last stack access translated
through, which the execution context carries, before hashing an address of
their own. A tag equal to the page, with no code bit and, for a write, no
publication left pending by an earlier store, hands back the host address; a
store still joins its epilogue at the overlap test, because the write can land
in the block that is running. Anything else takes the ordinary translation,
which records the new entry for the next access.

## Memory ordering

x86 promises total store order, and a LOCK-prefixed instruction or an XCHG
against memory is in addition a full fence. Guest code relies on both without
saying so; the host promises neither, and an exclusive loop orders only its own
operand. With several vCPUs an unordered engine fails multi-threaded guests
outright: a work-stealing deque hands one task to two threads, which shows up
as a wild pointer, a wakeup that never comes, or a spin that never ends.

Every gadget that touches guest memory therefore leaves through a `dmb ish`.
`translate`, `stack_fast` and the bulk helper macros mark the gadget they
expand in (`x86_marks_guest_memory`), and `x86_memory_order_boundary`, which
sits on every retiring exit, emits the barrier for a marked gadget. A gadget
that works on registers alone pays nothing, a locked gadget is fenced like any
other store, and the accesses inside one instruction keep the order the gadget
gives them. The mark is textual, so a new guest memory path that bypasses those
macros has to mark itself, and a memory form that joins another gadget's tail,
as DIV and IDIV do, runs the boundary before it branches. Leaving the engine
always carries the barrier. The Wine CPU service builds the stronger form, a
barrier after every gadget (`UML_X86_64_STRICT_TSO`).

## Locked operands

A LOCK-prefixed read-modify-write, XCHG against memory, CMPXCHG and XADD update
their operand in an exclusive loop, which the host accepts only at natural
alignment. x86 accepts a locked operand anywhere, and guests do use unaligned
ones, in packed structures above all.

- A host with FEAT_LSE2 takes an exclusive access at any alignment inside one
  16-byte granule. The runtime reports the feature on every run
  (`Executor::permit_unaligned_exclusives`), and an unaligned operand that fits
  in a granule then keeps the exclusive path. A naturally aligned operand never
  reads the permission.
- Every other operand -- one across a granule or a page, a 16-byte CMPXCHG16B
  operand off its alignment, and every unaligned operand on a host without the
  feature, which includes the A12 -- runs the gadget's ordinary load and store
  under one process-wide lock (`split_operand`, `split_lock_leave`). A fault in
  between releases the lock in the fault tail.

The lock makes such an operand atomic against every other operand on that
path, which covers the same instruction running on the other vCPUs. It is not
atomic against an exclusive loop or a plain store that overlaps it; nothing the
host offers covers two granules at once.

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

- membership is exact. A bit per hashed page answers "no" with one load, and
  a set bit is confirmed against a count per page, because the hash keeps
  neighbouring pages adjacent and a run of code pages would otherwise mark a
  run of data pages, such as a thread's stack, as holding code;
- a page enters the set before any translation built from it is handed out
  (`CodePageHold` covers the build), and leaves it when the last cached
  translation of the page in every executor is unlinked or invalidated;
- when a page enters the set, every participating executor's write-side TLB
  entry for it is retired before the registration returns, and an executor
  inside the engine is waited for until it leaves or passes a checkpoint,
  bounded at 200 ms, so no executor keeps a stale "publishes nothing" entry;
- a TLB fill that raced a registration retries: the fill re-reads the
  registry's change counter after publishing its entry;
- counting a page in or out never sleeps: the counts are guarded in 64 stripes
  by the word-sized lock the MMU uses.

The Wine ARM64EC CPU service, whose arm64 translations live outside the
registry, publishes on every executable store instead
(`Executor::publish_every_executable_store`).

A native program owes one publication per page it stores into, and a store
spanning a page boundary owes both of them. The record a publication leaves is
the whole page's -- the page stamp moves, and the code-change range is the page
-- so a further store inside the page this program last published owes nothing;
the program still cools its own overlapping cached blocks by exact range on
every executable store. A store to any other page has no record at all, and
because a page stamp is moved only by a publication, another vCPU validating
against that stamp would keep its translation of the rewritten page
indefinitely. The program therefore carries the page it last published, and a
store whose page differs owes a publication again. A program alternating
between two code pages pays one per store; a JIT patching two call sites, a
code copy crossing a page and a relocating loader each pay one per page they
touch.

A store translation settles both questions while it resolves the address --
whether this page owes a record, and whether the store may skip the
publication call altogether -- because reading the context after the guest
store costs a memory-disambiguation stall. Only the common answer stays in the
hot instruction stream: `write_code` is none, pending or published and only
pending is work, so its low bit answers for it and joins the TLB entry's
cached-code bit in a single test, while the arm that takes a record for a page
some executor holds a translation of sits in the cold section.

The writer's order is the contract, and it is the order
[Keeping a cached translation](#keeping-a-cached-translation) reads in reverse:
the bytes are stored, then the page stamp moves, then the code-change sequence
moves. `finish_store` performs the guest store before its publication call, and
`GuestAddressSpace::invalidate_code` moves the page stamp before publishing the
range. A stamp taken after the bytes it stands for would certify a write the
recorded source predates.

## Keeping a cached translation

A cached program is reused when the guest bytes it was translated from are
still there. Two things answer that: the stamp of each page it covers, which
any code-change publication on that page moves, and the program's own saved
source bytes, compared whenever a stamp has moved.

Their order is the contract. A page stamp certifies only the bytes read after
it was taken, so a stamp is always taken before the source it stands for --
before the bytes a fresh translation saves, and before the comparison a
revalidation makes -- and a successful validation records that same stamp. A
stamp taken afterwards would already cover a write that the recorded source
predates, and the next validation would accept the stale program on it.

A new translation reads its block once. The saved source is the bytes the
decoder consumed, so a cached program and the source that stands for it are one
observation of guest memory, not two that a write could fall between. Reading
the block a second time to fill the source left the two free to disagree, and
nothing downstream could tell: the comparison a moved stamp forces would put
the newer source against the newer memory and accept a program decoded from
bytes that were already gone. Re-reading the stamp around the second read only
narrowed that window, because a writer stores its bytes well before it reaches
the stamp.

What the stamp then has to be is older than the bytes the program was built
from. The registry holds every page the block can span before the decoder
starts and each of those pages is stamped there, which is the whole ordering a
new translation needs. A write landing during the decode resolves through the
ordinary validation: it moves the page's stamp, after its bytes and before the
sequence that brings this vCPU back, and the comparison that follows puts the
decoder's own bytes against guest memory. A page the block could have spanned
but did not is never consulted, so a hot data page next to a text page costs
its neighbour nothing, and a block longer than the cache's own length limit is
used once and not cached.

The address space's code-change sequence says when to revalidate, not whether
a program survives the revalidation. A publication that touched a program moved
the stamp of the page carrying it; one that did not is no business of that
program. So a vCPU publishing at speed -- a JIT heap, or a trampoline sharing a
page with hot data -- costs the other vCPUs of its address space a
revalidation per block, not a translation per block.

A reader that keeps a program guarantees that the program matched guest memory
at an instant inside the validation, and that every change published up to the
sequence the cache was prepared with had landed before that instant, because a
writer stores the bytes, then moves the page stamp, then moves the sequence,
and the reader observed that sequence before it looked. A store still in
flight reaches the vCPU at its next code-change checkpoint, which is where the
x86 cross-modifying-code protocol requires an executing processor to look. A
moved mapping sequence or a raised execution interrupt still abandons the
validation: those say the memory being read through is being replaced, or that
this vCPU has been asked to leave, and no page stamp describes either.

## Measuring what the cache costs

A `UML_GADGET_CACHE_CENSUS=ON` build makes every executor append what its
private cache still held at teardown: the committed slabs, each resident
translation's own storage -- its native stream and saved source, and the
decoded instructions only a replayed block holds -- the link and indirect
records, and the target link table each address space owns. The record format is the arm64 engine's,
so one analyzer reads a capture from either. See
[`tests/perf/README.md`](../../../../tests/perf/README.md).
