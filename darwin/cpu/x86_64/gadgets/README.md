# x86-64 gadget engine

The engine executes x86-64 userspace as data-only streams of pointers to
handwritten arm64 gadgets (`engine.S` and `engine/*.inc`). `native_builder.cpp`
turns decoded instructions into those streams, `executor.cpp` owns the
translation cache, and `native_memory.cpp` is the C++ side of the software
TLB. This file records the contracts that are not obvious from the code.

Private labels take the Mach-O assembler-temporary prefix `L`, which keeps
them out of the symbol table. A branch to one from another section encodes its
distance past the nearest named symbol before it as a signed 24-bit addend, so
such a target must lie within 8 MiB of one; the cold section names only its
start and is 0.3 MB. The assembler rejects a larger addend, and a named local
symbol ahead of the target fixes it. `gadget-engine-temporary-labels` fails on
a `.L` symbol in the engine object.

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
  An access carries its instruction's RIP and length in one stream word, the
  RIP in bits 47:0 and the length in bits 59:56, because only its fault and
  overlap exits read them.
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
the bit, and a carry override riding on such a record is settled through the
materializer.
Only a reader of the carry through an INC or DEC record pays for the slot: the
materializers compute that one flag from the slot record through the same
operation-and-width table, and the C++ materializer does the same. Every
record write clears the bit, so a producer that follows leaves nothing
pending.

Only a record defines a carry, so only a record is parked. An empty record
means the carry is already settled in RFLAGS, which is what every gadget that
materializes and republishes the flags leaves behind -- ADCX and ADOX, the
flag writers, and any consumer that had to materialize -- and the slot's table
would answer a carry of zero for it. INC and DEC therefore take an empty
record the way they take another INC or DEC record: nothing to park, and only
a carry override on it is settled through the materializer.

An operation that defines every arithmetic flag it does not leave undefined --
COMISS and UCOMISS, PTEST, the packed string compares, the bit scans and
counts, and the BMI flag writers -- discards the pending record instead of
materializing it, because nothing of it survives. BT and its siblings leave
ZF alone, so they keep the record and write their carry into its override.

## Blocks and branches

A block ends at an unconditional transfer or at the 64-instruction fetch
window, not at its first conditional branch. The fall-through is translated
into the same stream, so not taking the branch costs one dispatch rather than
a link follow, and the taken edge keeps its link. A build with no
link resolver -- the budget-limited replay of a cached block -- keeps the
terminal form instead and leaves the translated tail unreachable.

Only a block that the fetch window cut short ends in a terminator, which
follows the link to the next instruction or leaves with its RIP. Every gadget
a branch selects leaves or follows a link on each of its paths -- a
conditional branch that ends a block takes its two-exit form -- so a block
that ended at a branch carries none. See X82.

A dispatch's instruction budget bounds how long it runs, not where a block
may stop: the block that crosses the budget runs to its end, as the arm64
engines' blocks do, so a dispatch can retire up to one block more than its
budget. Only a single step, and the instruction that a POPF or IRET setting TF
leaves before its trap, asks for an exact budget. That is the one case where a
block is replayed: its first few instructions are assembled into a stream of
their own, which ends where the budget does. What that replay decodes from is
the block's saved source, not guest memory, so it recovers exactly the
instructions the block was built from however the guest's own bytes have moved
since. A translation is therefore built with no decoded instructions in it at
all; the first replay of a block keeps what it decoded, in a third allocation
that only a stepped block has. See X73.

A repeated string instruction retires once however many elements it moves.
Its gadget stops between elements after `UML_X86_64_STRING_REPEAT_BUDGET` of
work -- elements, or 64-byte blocks of a page-bounded run -- with RIP still
naming it, and `execute` reports that yield. The dispatch charges the same
amount to its instruction budget, so a loop of long REP strings returns to the
kernel about as often as ordinary code does; the kernel services its
cooperative timer only there. A step keeps its exact budget.

A step of a repeated string instruction is one iteration, as x86 raises #DB
after each. `execute` given `single_step` enters the instruction through its
`_step` entry, which sets a repeat budget of one and joins the gadget's element
loop: the iteration stops between elements with RIP naming the instruction and
RF set, or retires the instruction when it ends it. The entries follow the
string gadgets and leave their layout alone.

The retirement counter carries the dispatch's instruction budget as a negative
bias, so an edge that follows a link tests the sign of one addition instead of
loading the limit and comparing against it. `uml_x86_64_gadget_leave` takes
the bias back out before it reports what the dispatch retired.

## Resume flag

RF suppresses instruction breakpoints for one instruction and is cleared as
that instruction starts, so nothing the instruction stores or reports shows it:
a SYSCALL right after an IRET that loaded RF leaves R11 without it, as a native
run of the corpus case `iret-resume-flag-clears-at-next-instruction.s` shows.
The engine models no instruction breakpoints, so RF is only state to carry and
clear. It arrives in two ways: IRET loads it from its image at CPL 3, and the
kernel's flags carry it after a signal return or a tracer's write. IRET always
leaves the engine and the kernel's flags always start a dispatch, so `execute`
clears RF as it enters the engine and no gadget reads or clears it. An exit
before the engine runs an instruction, for a pending interrupt or a zero
budget, keeps RF, as an interrupt taken before that instruction does.

Hardware also sets RF in the flags it saves for every fault but an instruction
breakpoint, so the resumed instruction does not break again, and saves a
trap's flags as they are. The runtime does the same in the registers it hands
the kernel: a page fault, #UD, #SS, #GP, #AC, #DE, #MF or #XM exit reports RF
set, and a breakpoint, INT 4 or single-step exit reports the live flags, in
which RF is clear. Hardware sets RF as well when an interrupt stops a repeated
string instruction between elements, so the string gadgets set it at the
repeat budget's exit, the one place they stop with elements done and elements
left; `execute` clears it again when the instruction resumes. Signal frames
and a tracer therefore see what native Linux shows.

## Software TLB

`Executor::prepare` enters an address-space generation, and it must not leave
behind a translation of a page whose mapping has changed. The address space
describes the mapping ranges a reader has not yet seen, so a reader only a few
changes behind replays exactly those ranges and retires just the entries they
cover. The whole table is cleared only when the address space cannot describe
the span, when the span covers more than a quarter of the entries, or when the
memory or the generation changed. The table is direct-mapped with 32768
entries, 1 MiB, indexed by the page number folded once at that width;
`native_tlb_entry_count` is the one constant, and the three assembly copies of
the index (`translate` and `translate_plain` in engine/common.inc, and the
baked entries' translation in engine/generate_baked.py) must change with it.

A translation may miss, and a miss is a call. Nothing saves the host vector
registers around it, so a gadget may not hold a guest value in one across its
own translation: a stored value is read out of the state after the destination
is settled, and a page-crossing helper is handed the state's address rather
than a copy made before the call. The same rule is why only register forms
take part in vector result forwarding.

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
always carries the barrier.

## Locked operands

A LOCK-prefixed read-modify-write, XCHG against memory, CMPXCHG and XADD update
their operand in an exclusive loop, which the host accepts only inside one
16-byte granule. x86 accepts a locked operand anywhere, and guests do use
unaligned ones, in packed structures above all.

- The engine runs only on hosts with FEAT_LSE2, which take an exclusive access
  at any alignment inside one 16-byte granule, so an unaligned operand that
  fits in a granule keeps the exclusive path. Every Apple-silicon Mac has the
  feature; the runtime checks it once and refuses to run x86 code without it.
- Every other operand -- one across a granule or a page, and a 16-byte
  CMPXCHG16B operand off its alignment -- runs the gadget's ordinary load and
  store under one process-wide lock (`split_operand`, `split_lock_leave`). A
  fault in between releases the lock in the fault tail.

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

SSE and AVX arithmetic and FMA return x86's values where the host's differ. A
NaN result is the first source quieted when that is a NaN, else the second,
else the negative indefinite; for an FMA the first of a, b and c in x86's
a * b + c, with its own sign whatever the form negates. Where the elements come
from one source, x86 hardware's order decides, as the SDM ranks only distinct
sources: HADD and HSUB return a pair's lower NaN, and each DPPS or DPPD result
lane sums in its own order, DPPS lane i p[i ^ 1] + p[i] and then the other
pair, DPPD lane i p[i] + p[i ^ 1]. MXCSR.DAZ flushes denormal inputs, those of
comparisons, rounding and conversions included, and a dot product's
intermediates; MXCSR.FTZ flushes a result that is tiny after rounding: below
the smallest normal when rounded as if the exponent were unbounded, which a
product, quotient, fused result or narrowed double the host rounds up to the
smallest normal may be, so that one is run again scaled into the normal range.
MIN and MAX return a source unchanged. RCP and RSQRT ignore MXCSR, take a
denormal input as zero and return zero for RCP's tiny results; elsewhere the
host's exact quotient lies within x86's estimate error. These repairs live in
the cold section, which a gadget enters only for a special result, an
estimate's special input, under FTZ a result no larger than the smallest
normal, or a dot product under DAZ or FTZ.

`uml_x86_64_gadget_leave` restores round-to-nearest by clearing FPCR's mode
field, so entry needs no slot for the caller's copy. Every path into it has the
stack pointer back at the entry frame, from which it restores the callee-saved
registers.

## Exceptions the guest enables

x86 raises #XM for a SIMD floating-point exception that MXCSR unmasks, #MF at
the next waiting x87 instruction after an exception that the control word
unmasks, and #AC for a misaligned access while RFLAGS.AC is set. Most guests
enable none of them and a dispatch then runs as it always has, but a Delphi or
C++Builder program runs with its default control word $1332 and MXCSR $1900,
which unmask invalid operation, division by zero and overflow.

The gadgets check themselves for x87 exceptions, and for SIMD ones while MXCSR
masks underflow and precision. While the guest unmasks such an exception, each
run sets `exception_checks` in CpuState (`gadget_checks`), and:

- x87: a softfloat helper that raises an unmasked exception records it in
  `exception_raised` instead of the status word, and the gadget tests that
  after each helper call. Stack faults, and control word loads that unmask a
  flag already set, leave the same way.
- SIMD: the gadget tests its values for anything that could have raised an
  unmasked exception -- a result whose exponent is all ones or one below (a
  NaN, an infinity, or the largest finite value an overflow rounds to), a NaN
  input to a comparison, minimum or maximum, or a conversion to an integer out
  of its range. A dot product, whose intermediate results no test of its
  result sees, always leaves.

Either leaves before it stores anything, with
`UML_X86_64_EVENT_EXCEPTION_CHECK` and the stream position. The runtime finds
the instruction, in the entered stream or in the cached block a link reached,
and runs it again alone and checked: that raises #XM, leaves the x87 exception
pending, or completes an instruction whose special value raised nothing. The
x87 test costs a load and a branch per helper call, and most SIMD tests sit on
a path the gadget already took only for a NaN, so code under $1332 and $1900
runs within about 3% of the masked speed. An instruction the test sends to the
runtime that raises nothing, such as an addition to an infinity, costs about
140 ns. While the gadgets check SIMD exceptions, the flags of masked ones
accumulate only for the instructions the runtime runs checked, as they never
do while MXCSR masks every exception.

The other checks split blocks. While `checked_exceptions` finds one enabled --
RFLAGS.AC, MXCSR unmasking underflow or precision, which no value shows, or an
unmasked x87 exception pending -- the runtime dispatches through an executor
built for that set, whose blocks end before every instruction that could raise
one. Such an instruction is a block of its own, a check head, that no link
enters; `execute_checked` runs it alone, with its checks, and runs every other
block, linked, as `execute` does:

- #MF: a waiting instruction -- every x87 instruction but FNINIT, FNCLEX,
  FNSTSW, FNSTCW, FNSTENV and FNSAVE, and FWAIT, EMMS and every MMX
  instruction -- faults before it runs while an unmasked exception is pending.
- #AC: an access of two, four or eight bytes off its alignment faults before it
  runs: memory operands at their width or x87 operand size, stack accesses,
  and a string instruction's first element. 16- and 32-byte vector accesses,
  gathers and the extended-state images are exempt, as on x86.
- #XM, run checked: the host's cumulative exception flags are cleared around
  the instruction and read back as MXCSR's; a scalar instruction's host code
  computes lane 0 alone, COMIS compares with FCMPE, and a conversion to an
  integer out of range raises the host's invalid-operation flag. An unmasked
  exception restores the state from before it, a memory destination
  included, and faults; MXCSR then holds only the flags checked before the
  computation when one of those was unmasked, and no precision flag when
  underflow was. Masked flags accumulate.
- x87, run checked: the softfloat helpers set the status word's flags. An
  unmasked invalid operation, denormal or division by zero, or an unmasked
  overflow or underflow of a store, restores the registers, the stack and
  memory, and any unmasked exception sets ES and B for the next waiting
  instruction.

An instruction that could change what a run checks retires and leaves the
engine, so linked code never runs past it unchecked: POPF setting AC; LDMXCSR,
FXRSTOR or XRSTOR unmasking a SIMD exception while the gadgets do not check
them, or unmasking underflow or precision; and FLDCW, FLDENV, FRSTOR, FXRSTOR
or XRSTOR unmasking an x87 exception while the gadgets are not checking or
unmasking one whose flag is set. That costs POPF one branch and FLDCW and
LDMXCSR three instructions when nothing is unmasked. LDMXCSR keeps the masks
and flags the guest loads.

A check head costs a dispatch, about 100 ns on an M-series Mac, so SSE
arithmetic runs about 25 times slower while MXCSR unmasks underflow or
precision. Memory access while AC is set runs about 25 times slower too, and
stays so: programs rarely set AC. Code without such instructions keeps its
speed. The host detects neither a denormal operand nor an exact tiny result,
so #XM misses those two; it reports precision for an unmasked overflow whose
result is exact but for its exponent, where x86 does not; and an x87 overflow
or underflow unmasked for a register destination leaves the masked result
rather than x87's rebiased one.

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
  inside the engine is waited for until it leaves the engine, bounded at
  200 ms, so no executor keeps a stale "publishes nothing" entry;
- a TLB fill that raced a registration retries: the fill re-reads the
  registry's change counter after publishing its entry;
- counting a page in or out never sleeps: the counts are guarded in 64 stripes
  by the word-sized lock the MMU uses.

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

`finish_store` performs the guest store before its publication call, and
`GuestAddressSpace::invalidate_code` moves the page stamp before publishing the
range, which is the writer's order that
[Keeping a cached translation](#keeping-a-cached-translation) relies on.

## Keeping a cached translation

A cached program is reused while the guest bytes it was translated from are
still there. Two things answer that: the stamp of each page it covers, which
any code-change publication on that page moves, and the program's saved source
bytes, compared whenever a stamp has moved.

A page stamp certifies only the bytes read after it was taken, so a stamp is
always taken before the source it stands for, and a successful validation
records that same stamp. A new translation reads its block once: the registry
holds and stamps every page the block can span before the decoder starts, and
the saved source is the bytes the decoder consumed, so the program and its
source are one observation of guest memory. A write landing during the decode
moves the page's stamp, and the next validation compares the decoder's own
bytes against guest memory. A page the block could have spanned but did not is
never consulted, and a block longer than the cache's own length limit is used
once and not cached. A revalidation likewise takes the stamps before it
compares the source.

The address space's code-change sequence says when to revalidate, not whether
a program survives the revalidation: a publication that touched a program moved
the stamp of its page, and one that did not leaves the program alone. A vCPU
publishing at speed -- a JIT heap, or a trampoline sharing a page with hot data
-- costs the other vCPUs of its address space a revalidation per block, not a
translation per block.

A writer stores the bytes, then moves the page stamp, then moves the sequence,
and a reader observes the sequence before it looks. A kept program therefore
matched guest memory at an instant inside the validation, and every change
published up to the sequence the cache was prepared with had landed before
that instant. A store still in flight reaches the vCPU at its next code-change
checkpoint, which is where the x86 cross-modifying-code protocol requires an
executing processor to look. A moved mapping sequence or a raised execution
interrupt still abandons the validation: those say the memory being read
through is being replaced, or that this vCPU has been asked to leave, and no
page stamp describes either.

## Measuring what the cache costs

A `UML_GADGET_CACHE_CENSUS=ON` build makes every executor append what its
private cache still held at teardown: the committed slabs, each resident
translation's own storage -- its native stream and saved source, and the
decoded instructions only a replayed block holds -- the link and indirect
records, and the target link table each address space owns. The record format is the arm64 engine's,
so one analyzer reads a capture from either. See
[`tests/perf/README.md`](../../../../tests/perf/README.md).
