#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Emit the x86-64 gadget entries that have their guest register offsets baked in.

The ordinary families decode every operand from a stream word and reach
CpuState through a register index: "ldr x11, [_cpu, x9]" feeding "str x13,
[_cpu, x9]". The host forwards a store to a dependent load at rename only when
both name a base register and a constant displacement, so a chain of such
entries pays about 6.8 cycles per hop where the immediate form pays 2.5.
Baking the offsets gives every access the immediate form and drops the decode
with it. That costs one entry per operand combination -- sixteen destinations
by sixteen sources for a register pair, a value by a base for a
[base + displacement] access -- which is why this bank is generated rather than
written: an assembler macro that enumerates it takes the LLVM assembler minutes.
Indexed addresses split into an address entry, enumerated by base, index and
scale, followed by an access entry enumerated by the value alone.

Every entry keeps the ordinary family's semantics: the same lazy flag record,
page-crossing test, translation, publication and overlap checks. Only the cold
tails -- a crossing access, a translation miss, a publication and an
overlapping store -- are shared, reached through x19 as the continuation, so
the hot bytes of an entry stay dense.

Each family publishes a table of its entries in the enumeration order so the
builder indexes rather than names them: registers in offset order, the four
high-byte registers after the sixteen low ones, and "none" after the sixteen
bases. `native_baked.hpp` declares the tables; keep the two in step.
"""
import argparse
import sys

GPR = list(range(0, 128, 8))
BYTE = GPR + [1, 9, 17, 25]
BASES = GPR + ['none']
SCALES = [0, 1, 2, 3]
WIDTHS = {  # width -> (load, store, host register prefix, store register prefix)
    8: ('ldrb', 'strb', 'w', 'w'),
    16: ('ldrh', 'strh', 'w', 'w'),
    32: ('ldr', 'str', 'w', 'x'),
    64: ('ldr', 'str', 'x', 'x'),
}
# operation -> (host instruction, deferred flag operation, writes, flag-setting host form at 32/64)
ARITH = {
    'add': ('add', 1, 1, True),
    'sub': ('sub', 2, 1, True),
    'and': ('and', 3, 1, True),
    'or': ('orr', 3, 1, False),
    'xor': ('eor', 3, 1, False),
    'cmp': ('sub', 2, 0, True),
    'test': ('and', 3, 0, True),
}


class Out:
    def __init__(self):
        self.lines = []
        self.serial = 0
        self.count = 0

    def emit(self, text=''):
        self.lines.append(text)

    def label(self, stem):
        self.serial += 1
        return f'.Lx86_bk_{stem}_{self.serial}'

    def gadget(self, name):
        self.count += 1
        self.emit(f'gadget b_{name}')

    def table(self, name, entries):
        self.emit('#if __APPLE__')
        self.emit('\t.section __DATA,__const')
        self.emit('#else')
        self.emit('\t.section .rodata')
        self.emit('#endif')
        self.emit('\t.p2align 3')
        self.emit(f'\t.globl NAME(uml_x86_64_baked_{name})')
        self.emit(f'NAME(uml_x86_64_baked_{name}):')
        for entry in entries:
            self.emit(f'\t.quad NAME(uml_x86_64_gadget_b_{entry})')
        self.emit('\t.text')


def regs_for(width):
    return BYTE if width == 8 else GPR


def instr_for(op, width):
    host, flagop, writes, host_flags = ARITH[op]
    if host_flags and width >= 32:
        host += 's'
    return host, flagop, writes


NARROW_COMPARE = {'add': 'cmn', 'sub': 'cmp', 'cmp': 'cmp', 'and': 'tst', 'test': 'tst'}


def flag_form(o, op, width, left, right):
    """Host flags for a narrow operation: shifted to the top of a 32-bit
    register, a compare of the two operands sets NZCV exactly as the
    guest-width operation does, so the next condition can read them. x9 is
    free at every call site."""
    if op not in NARROW_COMPARE or width >= 32:
        return
    shift = 32 - width
    o.emit(f'\tlsl w9, w{left}, #{shift}')
    o.emit(f'\t{NARROW_COMPARE[op]} w9, w{right}, lsl #{shift}')


# ---------------------------------------------------------------- shared pieces
def base_address(o, base):
    """x7 = x14 (displacement) plus the base register, or x14 alone."""
    if base == 'none':
        o.emit('\tmov x7, x14')
    else:
        o.emit(f'\tldr x10, [_cpu, #{base}]')
        o.emit('\tadd x7, x14, x10')


def translate(o, write, size):
    """The ordinary translation with its miss tail shared; x19 carries the continuation."""
    hit, done = o.label('hit'), o.label('done')
    if write:
        o.emit('\tand x23, x23, #~(1 << 60)')
    o.emit('\tand x9, x7, #0xfffffffffffff000')
    o.emit('\tlsr x10, x7, #12')
    o.emit('\teor x10, x10, x10, lsr #15')
    o.emit('\tand w10, w10, #0x7fff')
    o.emit('\tldr x11, [_context, #UML_X86_64_CONTEXT_TLB]')
    o.emit('\tadd x11, x11, x10, lsl #5')
    o.emit(f'\tldp x12, x13, [x11, #{16 if write else 0}]')
    # An exclusive-or and a compare-with-zero branch keep NZCV as the gadget
    # found it, so a fused condition survives a transfer between its producer
    # and its consumer; the miss tails save and restore it around their calls.
    if write:
        o.emit('\tbic x14, x12, #(UML_X86_64_TLB_WRITE_CODE | UML_X86_64_TLB_WRITE_CACHED_CODE)')
        o.emit('\teor x14, x9, x14')
    else:
        o.emit('\teor x14, x9, x12')
    o.emit(f'\tcbz x14, {hit}')
    o.emit(f'\tadr x19, {done}')
    o.emit(f'\tb .Lx86_baked_miss_{1 if write else 0}_{size}')
    o.emit(f'{hit}:')
    if write:
        ready, publishes = o.label('wcready'), o.label('publishes')
        o.emit(f'\ttbz x12, #1, {ready}')
        o.emit('\tldrb w14, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]')
        o.emit('\tsub w14, w14, #UML_X86_64_WRITE_CODE_PUBLISHED')
        o.emit(f'\tcbz w14, {ready}')
        o.emit('\tmov w14, #UML_X86_64_WRITE_CODE_PENDING')
        o.emit('\tstrb w14, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]')
        o.emit(f'{ready}:')
        o.emit(f'\ttbnz x12, #2, {publishes}')
        o.emit('\tldrb w14, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]')
        o.emit('\tsub w14, w14, #UML_X86_64_WRITE_CODE_PENDING')
        o.emit(f'\tcbz w14, {publishes}')
        o.emit('\torr x23, x23, #(1 << 60)')
        o.emit(f'{publishes}:')
    o.emit('\tadd x7, x13, x7')
    o.emit(f'{done}:')


def finish_store(o, size):
    """The ordinary store epilogue with its publication and overlap tails shared.
    x25 is the guest address, x23 the length word, x22 the RIP."""
    check, done, overlap = o.label('check'), o.label('sdone'), o.label('soverlap')
    o.emit(f'\ttbnz x23, #60, {check}')
    o.emit('\tldrb w8, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]')
    o.emit(f'\tcbz w8, {check}')
    o.emit(f'\tadr x19, {check}')
    o.emit(f'\tb .Lx86_baked_publish_{size}')
    o.emit(f'{check}:')
    # Guest addresses and block bounds lie below 2^63, so the sign of a
    # difference is their unsigned order and NZCV stays untouched.
    o.emit('\tldr x8, [_context, #UML_X86_64_CONTEXT_PROGRAM_END]')
    o.emit('\tsub x8, x25, x8')
    o.emit(f'\ttbz x8, #63, {done}')
    o.emit(f'\tadd x9, x25, #{size}')
    o.emit('\tldr x10, [_context, #UML_X86_64_CONTEXT_PROGRAM_BEGIN]')
    o.emit('\tsub x9, x10, x9')
    # A conditional branch reaches 1 MiB and the bank is larger, so the cold
    # exit goes through a local unconditional branch.
    o.emit(f'\ttbnz x9, #63, {overlap}')
    o.emit(f'{done}:')
    o.emit('\tretire_and_next')
    o.emit(f'{overlap}:')
    o.emit(f'\tb .Lx86_baked_store_overlap_{size}')


def page_cross(o, size, label):
    o.emit(f'\tpage_cross_check {size}, {label}')


def record(o, flagop, width, pending=False):
    o.emit(f'\trecord_deferred{"_pending" if pending else ""} {flagop}, {width}')


def normalize(o, width, reg):
    o.emit(f'\tnormalize_result {width}, {reg}')


def memory_prologue(o, base):
    """Stream: displacement, length word, RIP."""
    o.emit('\tldp x14, x23, [_stream], #16')
    o.emit('\tldr x22, [_stream], #8')
    base_address(o, base)
    o.emit('\tmov x25, x7')


def access_prologue(o):
    """The access half of a split address: x7 arrived from the address entry."""
    o.emit('\tldp x23, x22, [_stream], #16')
    o.emit('\tmov x25, x7')


def shared_tails(o):
    for write in (0, 1):
        for size in (1, 2, 4, 8):
            o.emit('\t.p2align 2')
            o.emit(f'.Lx86_baked_miss_{write}_{size}:')
            o.emit('\tmrs x9, nzcv')
            o.emit('\tstr x9, [sp, #-16]!')
            o.emit('\tmov x0, _context')
            o.emit('\tmov x1, x7')
            o.emit(f'\tmov w2, #{write}')
            o.emit(f'\tmov w3, #{size}')
            o.emit('\tbl NAME(uml_x86_64_gadget_tlb_miss)')
            o.emit('\tldr x9, [sp], #16')
            o.emit('\tmsr nzcv, x9')
            o.emit('\tcbz x0, .Lx86_memory_fault')
            o.emit('\tmov x7, x0')
            o.emit('\tbr x19')
    # A crossing load returns the raw bytes zero-extended; the widening forms
    # extend here so the continuation is the same as the in-page path's.
    for name, size, extend in (('8', 1, None), ('16', 2, None), ('32', 4, None), ('64', 8, None),
                               ('sxtb_w', 1, 'sxtb w11, w11'), ('sxtb_x', 1, 'sxtb x11, w11'),
                               ('sxth_w', 2, 'sxth w11, w11'), ('sxth_x', 2, 'sxth x11, w11'),
                               ('sxtw_x', 4, 'sxtw x11, w11')):
        o.emit('\t.p2align 2')
        o.emit(f'.Lx86_baked_cross_load_{name}:')
        o.emit('\tmrs x9, nzcv')
        o.emit('\tstr x9, [sp, #-16]!')
        o.emit('\tmov x0, _context')
        o.emit('\tmov x1, x25')
        o.emit(f'\tmov w2, #{size}')
        o.emit('\tbl NAME(uml_x86_64_gadget_cross_page_load)')
        o.emit('\tldr x9, [sp], #16')
        o.emit('\tmsr nzcv, x9')
        o.emit('\tcbz x1, .Lx86_memory_fault')
        o.emit('\tmov x11, x0')
        if extend:
            o.emit('\t' + extend)
        o.emit('\tbr x19')
    for size in (1, 2, 4, 8):
        o.emit('\t.p2align 2')
        o.emit(f'.Lx86_baked_cross_store_{size}:')
        o.emit('\tmrs x9, nzcv')
        o.emit('\tstr x9, [sp, #-16]!')
        o.emit('\tmov x0, _context')
        o.emit('\tmov x1, x25')
        o.emit('\tmov x2, x24')
        o.emit(f'\tmov w3, #{size}')
        o.emit('\tbl NAME(uml_x86_64_gadget_cross_page_store)')
        o.emit('\tldr x9, [sp], #16')
        o.emit('\tmsr nzcv, x9')
        o.emit('\tcbz w0, .Lx86_memory_fault')
        o.emit('\tbr x19')
        o.emit('\t.p2align 2')
        o.emit(f'.Lx86_baked_publish_{size}:')
        o.emit('\tmrs x9, nzcv')
        o.emit('\tstr x9, [sp, #-16]!')
        o.emit('\tmov x0, _context')
        o.emit('\tmov x1, x25')
        o.emit(f'\tmov w2, #{size}')
        o.emit('\tbl NAME(uml_x86_64_gadget_publish_code_write)')
        o.emit('\tldr x9, [sp], #16')
        o.emit('\tmsr nzcv, x9')
        o.emit('\tbr x19')
        # The store landed inside the running block: leave through the store's
        # own exit as the ordinary epilogue does.
        o.emit('\t.p2align 2')
        o.emit(f'.Lx86_baked_store_overlap_{size}:')
        o.emit('\tldrb w8, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]')
        o.emit(f'\tcbnz w8, .Lx86_baked_store_overlap_exit_{size}')
        o.emit('\tmov x0, _context')
        o.emit('\tmov x1, x25')
        o.emit('\tbl NAME(uml_x86_64_gadget_invalidate_code)')
        o.emit(f'.Lx86_baked_store_overlap_exit_{size}:')
        o.emit('\tubfx x8, x23, #56, #4')
        o.emit('\tadd x8, x22, x8')
        o.emit('\tstr x8, [_cpu, #UML_X86_64_CPU_RIP]')
        o.emit('\tadd _executed, _executed, #1')
        o.emit('\tx86_memory_order_boundary')
        o.emit('\tmov w0, #UML_X86_64_EVENT_CONTINUED')
        o.emit('\tb uml_x86_64_gadget_leave')


# ---------------------------------------------------------------- register forms
def arith_register(o):
    """op reg, reg and op reg, imm. The immediate arrives as the full stream
    word; every width reads the bits it needs and the record's consumer masks
    the operands to the width, so no narrowing is spent here."""
    for op in ARITH:
        for width, (ld, st, hr, sr) in WIDTHS.items():
            host, flagop, writes = instr_for(op, width)
            regs = regs_for(width)
            entries = []
            for d in regs:
                for s in regs:
                    name = f'{op}_{width}_rr_{d}_{s}'
                    o.gadget(name)
                    o.emit(f'\t{ld} {hr}11, [_cpu, #{d}]')
                    o.emit(f'\t{ld} {hr}12, [_cpu, #{s}]')
                    o.emit(f'\t{host} {hr}13, {hr}11, {hr}12')
                    normalize(o, width, f'{hr}13')
                    flag_form(o, op, width, 11, 12)
                    if writes:
                        o.emit(f'\t{st} {sr}13, [_cpu, #{d}]')
                    record(o, flagop, width)
                    o.emit('\tretire_and_next')
                    entries.append(name)
            o.table(f'{op}_{width}_rr', entries)
            entries = []
            for d in regs:
                name = f'{op}_{width}_ri_{d}'
                o.gadget(name)
                o.emit('\tldr x12, [_stream], #8')
                o.emit(f'\t{ld} {hr}11, [_cpu, #{d}]')
                o.emit(f'\t{host} {hr}13, {hr}11, {hr}12')
                normalize(o, width, f'{hr}13')
                flag_form(o, op, width, 11, 12)
                if writes:
                    o.emit(f'\t{st} {sr}13, [_cpu, #{d}]')
                record(o, flagop, width)
                o.emit('\tretire_and_next')
                entries.append(name)
            o.table(f'{op}_{width}_ri', entries)


def move_register(o):
    for width, (ld, st, lr, sr) in WIDTHS.items():
        regs = regs_for(width)
        entries = []
        for d in regs:
            for s in regs:
                name = f'move_{width}_rr_{d}_{s}'
                o.gadget(name)
                o.emit(f'\t{ld} {lr}11, [_cpu, #{s}]')
                o.emit(f'\t{st} {sr}11, [_cpu, #{d}]')
                o.emit('\tretire_and_next')
                entries.append(name)
        o.table(f'move_{width}_rr', entries)
        entries = []
        for d in regs:
            name = f'move_{width}_ri_{d}'
            o.gadget(name)
            o.emit('\tldr x11, [_stream], #8')
            o.emit(f'\t{st} {sr}11, [_cpu, #{d}]')
            o.emit('\tretire_and_next')
            entries.append(name)
        o.table(f'move_{width}_ri', entries)


# name -> (source width, load, load register, cross tail, store, store register)
EXTENDS = {
    'zext_8_16': (8, 'ldrb', 'w', '8', 'strh', 'w'),
    'zext_8_32': (8, 'ldrb', 'w', '8', 'str', 'x'),
    'zext_8_64': (8, 'ldrb', 'w', '8', 'str', 'x'),
    'zext_16_32': (16, 'ldrh', 'w', '16', 'str', 'x'),
    'zext_16_64': (16, 'ldrh', 'w', '16', 'str', 'x'),
    'sext_8_16': (8, 'ldrsb', 'w', 'sxtb_w', 'strh', 'w'),
    'sext_8_32': (8, 'ldrsb', 'w', 'sxtb_w', 'str', 'x'),
    'sext_8_64': (8, 'ldrsb', 'x', 'sxtb_x', 'str', 'x'),
    'sext_16_32': (16, 'ldrsh', 'w', 'sxth_w', 'str', 'x'),
    'sext_16_64': (16, 'ldrsh', 'x', 'sxth_x', 'str', 'x'),
    'sext_32_64': (32, 'ldrsw', 'x', 'sxtw_x', 'str', 'x'),
}


def extend_register(o):
    """MOVZX and MOVSX between registers: the destination is a whole register
    at 16, 32 or 64 bits; an 8-bit source may be a high byte."""
    for name, (source_width, ld, lr, _, st, sr) in EXTENDS.items():
        entries = []
        for d in GPR:
            for s in regs_for(source_width):
                entry = f'{name}_rr_{d}_{s}'
                o.gadget(entry)
                o.emit(f'\t{ld} {lr}11, [_cpu, #{s}]')
                o.emit(f'\t{st} {sr}11, [_cpu, #{d}]')
                o.emit('\tretire_and_next')
                entries.append(entry)
        o.table(f'{name}_rr', entries)


def unary_body(o, name, width, d, ld, st, hr, sr, flagop, pending=False):
    o.emit(f'\t{ld} {hr}11, [_cpu, #{d}]')
    # At 32 and 64 bits the flag-setting host form leaves N, Z and V
    # describing the guest operation; only C differs (INC and DEC preserve it,
    # NEG inverts it), and the builder keeps the carry conditions off these
    # producers.
    s = 's' if width >= 32 else ''
    if name == 'increment':
        o.emit(f'\tadd{s} {hr}13, {hr}11, #1')
        o.emit('\tmov x12, #1')
    elif name == 'decrement':
        o.emit(f'\tsub{s} {hr}13, {hr}11, #1')
        o.emit('\tmov x12, #1')
    else:
        o.emit(f'\tneg{s} {hr}13, {hr}11')
        o.emit('\tmov x12, x11')
        o.emit('\tmov x11, xzr')
    normalize(o, width, f'{hr}13')
    o.emit(f'\t{st} {sr}13, [_cpu, #{d}]')
    record(o, flagop, width, pending)
    o.emit('\tretire_and_next')


# INC and DEC preserve the carry flag, so the flag the previous producer
# defined has to reach RFLAGS before this one records its own -- or, when
# the builder cannot see that producer, the lazy prologue parks its record in
# the carry slot and the INC or DEC record inherits the pending bit. When the
# builder knows what the producer left, the carry is one condition away.
CARRY_PROLOGUE = {
    'settled': [],                  # a previous INC or DEC owns the carry: inherit its pending bit
    'borrow': ['\tcset w16, cc'],   # a subtraction: CF is the inverted borrow
    'carry': ['\tcset w16, cs'],    # an addition: CF is the carry out
    'clear': [],                    # a logical operation defined CF as zero
}


def carry_prologue(o, kind):
    for line in CARRY_PROLOGUE[kind]:
        o.emit(line)
    if kind == 'settled':
        return
    o.emit('\tldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]')
    if kind == 'clear':
        o.emit('\tbic x9, x9, #1')
    else:
        o.emit('\tbfi x9, x16, #0, #1')
    o.emit('\torr x9, x9, #2')
    o.emit('\tstr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]')


def unary_register(o):
    """INC, DEC, NEG and NOT on a register. INC and DEC preserve carry, so the
    previous producer's carry is settled first exactly as the ordinary entry does."""
    for width, (ld, st, hr, sr) in WIDTHS.items():
        regs = regs_for(width)
        for name, flagop, materialize in (('increment', 4, True), ('decrement', 5, True), ('negate', 2, False)):
            if materialize:
                for kind in CARRY_PROLOGUE:
                    entries = []
                    for d in regs:
                        entry = f'{name}_{kind}_{width}_r_{d}'
                        o.gadget(entry)
                        carry_prologue(o, kind)
                        unary_body(o, name, width, d, ld, st, hr, sr, flagop,
                                   pending=kind == 'settled')
                        entries.append(entry)
                    o.table(f'{name}_{kind}_{width}_r', entries)
            entries = []
            for d in regs:
                entry = f'{name}_{width}_r_{d}'
                o.gadget(entry)
                if materialize:
                    o.emit('\tlazy_carry_prologue')
                unary_body(o, name, width, d, ld, st, hr, sr, flagop,
                           pending=materialize)
                entries.append(entry)
            o.table(f'{name}_{width}_r', entries)
        entries = []
        for d in regs:
            entry = f'not_{width}_r_{d}'
            o.gadget(entry)
            o.emit(f'\t{ld} {hr}11, [_cpu, #{d}]')
            o.emit(f'\tmvn {hr}11, {hr}11')
            o.emit(f'\t{st} {sr}11, [_cpu, #{d}]')
            o.emit('\tretire_and_next')
            entries.append(entry)
        o.table(f'not_{width}_r', entries)


# ---------------------------------------------------------------- memory forms
def addresses(o):
    """An indexed address: displacement plus base plus scaled index, left in x7
    for the access entry that follows in the stream."""
    entries = []
    for base in BASES:
        for index in GPR:
            for scale in SCALES:
                name = f'ea_{base}_{index}_{scale}'
                o.gadget(name)
                o.emit('\tldr x14, [_stream], #8')
                o.emit(f'\tldr x13, [_cpu, #{index}]')
                if base == 'none':
                    o.emit(f'\tadd x7, x14, x13, lsl #{scale}')
                else:
                    o.emit(f'\tldr x10, [_cpu, #{base}]')
                    o.emit(f'\tadd x7, x14, x13, lsl #{scale}')
                    o.emit('\tadd x7, x7, x10')
                o.emit('\tnext_gadget')
                entries.append(name)
    o.table('ea', entries)


def load_body(o, size, ld, lr, cross, st, sr, v):
    crossed, write = o.label('lcross'), o.label('lwrite')
    page_cross(o, size, crossed)
    translate(o, False, size)
    o.emit(f'\t{ld} {lr}11, [x7]')
    o.emit(f'{write}:')
    o.emit(f'\t{st} {sr}11, [_cpu, #{v}]')
    o.emit('\tretire_and_next')
    o.emit(f'{crossed}:')
    o.emit(f'\tadr x19, {write}')
    o.emit(f'\tb .Lx86_baked_cross_load_{cross}')


def loads(o):
    """MOV reg, [mem] and the widening loads. `rm` is the fused
    [base + displacement] form with the base baked; `xm` is the access half of
    a split address."""
    families = [(f'move_{width}', width, ld, lr, str(width), st, sr)
                for width, (ld, st, lr, sr) in WIDTHS.items()]
    families += [(name, source_width, ld, lr, cross, st, sr)
                 for name, (source_width, ld, lr, cross, st, sr) in EXTENDS.items()
                 if not name.endswith('_16')]
    for name, source_width, ld, lr, cross, st, sr in families:
        values = regs_for(source_width) if name.startswith('move') else GPR
        entries = []
        for v in values:
            for base in BASES:
                entry = f'{name}_rm_{v}_{base}'
                o.gadget(entry)
                memory_prologue(o, base)
                load_body(o, source_width // 8, ld, lr, cross, st, sr, v)
                entries.append(entry)
        o.table(f'{name}_rm', entries)
        entries = []
        for v in values:
            entry = f'{name}_xm_{v}'
            o.gadget(entry)
            access_prologue(o)
            load_body(o, source_width // 8, ld, lr, cross, st, sr, v)
            entries.append(entry)
        o.table(f'{name}_xm', entries)


def store_body(o, size, st, hr):
    crossed, finish = o.label('scross'), o.label('sfinish')
    page_cross(o, size, crossed)
    translate(o, True, size)
    o.emit(f'\t{st} {hr}24, [x7]')
    o.emit(f'{finish}:')
    finish_store(o, size)
    o.emit(f'{crossed}:')
    o.emit(f'\tadr x19, {finish}')
    o.emit(f'\tb .Lx86_baked_cross_store_{size}')


def stores(o):
    """MOV [mem], reg and MOV [mem], imm. The immediate follows the RIP word."""
    immediates = []
    for width, (ld, st, hr, _) in WIDTHS.items():
        values = regs_for(width)
        entries = []
        for v in values:
            for base in BASES:
                entry = f'move_{width}_mr_{v}_{base}'
                o.gadget(entry)
                memory_prologue(o, base)
                o.emit(f'\t{ld} {hr}24, [_cpu, #{v}]')
                store_body(o, width // 8, st, hr)
                entries.append(entry)
        o.table(f'move_{width}_mr', entries)
        entries = []
        for v in values:
            entry = f'move_{width}_xmr_{v}'
            o.gadget(entry)
            access_prologue(o)
            o.emit(f'\t{ld} {hr}24, [_cpu, #{v}]')
            store_body(o, width // 8, st, hr)
            entries.append(entry)
        o.table(f'move_{width}_xmr', entries)
        entries = []
        for base in BASES:
            entry = f'move_{width}_mi_{base}'
            o.gadget(entry)
            memory_prologue(o, base)
            o.emit('\tldr x24, [_stream], #8')
            store_body(o, width // 8, st, hr)
            entries.append(entry)
        o.table(f'move_{width}_mi', entries)
        entry = f'move_{width}_xmi'
        o.gadget(entry)
        access_prologue(o)
        o.emit('\tldr x24, [_stream], #8')
        store_body(o, width // 8, st, hr)
        immediates.append(entry)
    o.table('move_xmi', immediates)


def arith_source_body(o, op, width, v):
    """op reg, [mem]: the memory operand is the right-hand side."""
    ld, st, hr, sr = WIDTHS[width]
    host, flagop, writes = instr_for(op, width)
    crossed, loaded, from_cross = o.label('rmcross'), o.label('rmloaded'), o.label('rmfromcross')
    page_cross(o, width // 8, crossed)
    translate(o, False, width // 8)
    o.emit(f'\t{ld} {hr}12, [x7]')
    o.emit(f'{loaded}:')
    o.emit(f'\t{ld} {hr}11, [_cpu, #{v}]')
    o.emit(f'\t{host} {hr}13, {hr}11, {hr}12')
    normalize(o, width, f'{hr}13')
    flag_form(o, op, width, 11, 12)
    if writes:
        o.emit(f'\t{st} {sr}13, [_cpu, #{v}]')
    record(o, flagop, width)
    o.emit('\tretire_and_next')
    o.emit(f'{crossed}:')
    o.emit(f'\tadr x19, {from_cross}')
    o.emit(f'\tb .Lx86_baked_cross_load_{width}')
    o.emit(f'{from_cross}:')
    o.emit('\tmov x12, x11')
    o.emit(f'\tb {loaded}')


def arith_destination_body(o, op, width):
    """op [mem], reg and op [mem], imm with the right-hand side already in
    x26. One write translation serves the read and the write: a writable x86
    page is readable, and a read-modify-write to an unwritable one faults as a
    write."""
    ld, st, hr, _ = WIDTHS[width]
    host, flagop, writes = instr_for(op, width)
    size = width // 8
    crossed, computed = o.label('mrcross'), o.label('mrcomputed')
    page_cross(o, size, crossed)
    translate(o, writes, size)
    o.emit(f'\t{ld} {hr}24, [x7]')
    o.emit(f'\t{host} {hr}27, {hr}24, {hr}26')
    normalize(o, width, f'{hr}27')
    if writes:
        o.emit(f'\t{st} {hr}27, [x7]')
    else:
        flag_form(o, op, width, 24, 26)
    o.emit(f'{computed}:')
    o.emit('\tmov x11, x24')
    o.emit('\tmov x12, x26')
    o.emit('\tmov x13, x27')
    record(o, flagop, width)
    if writes:
        finish_store(o, size)
    else:
        o.emit('\tretire_and_next')
    o.emit(f'{crossed}:')
    loaded = o.label('mrloaded')
    o.emit(f'\tadr x19, {loaded}')
    o.emit(f'\tb .Lx86_baked_cross_load_{width}')
    o.emit(f'{loaded}:')
    o.emit('\tmov x24, x11')
    o.emit(f'\t{host} {hr}27, {hr}24, {hr}26')
    normalize(o, width, f'{hr}27')
    if not writes:
        flag_form(o, op, width, 24, 26)
        o.emit(f'\tb {computed}')
        return
    stored = o.label('mrstored')
    o.emit('\tmov x11, x24')
    o.emit('\tmov x12, x26')
    o.emit('\tmov x13, x27')
    record(o, flagop, width)
    o.emit('\tmov x24, x27')
    o.emit(f'\tadr x19, {stored}')
    o.emit(f'\tb .Lx86_baked_cross_store_{size}')
    o.emit(f'{stored}:')
    finish_store(o, size)


def memory_arith(o):
    for op in ARITH:
        immediates = []
        for width, (ld, st, hr, sr) in WIDTHS.items():
            values = regs_for(width)
            entries = []
            for v in values:
                for base in BASES:
                    entry = f'{op}_{width}_rm_{v}_{base}'
                    o.gadget(entry)
                    memory_prologue(o, base)
                    arith_source_body(o, op, width, v)
                    entries.append(entry)
            o.table(f'{op}_{width}_rm', entries)
            entries = []
            for v in values:
                entry = f'{op}_{width}_xrm_{v}'
                o.gadget(entry)
                access_prologue(o)
                arith_source_body(o, op, width, v)
                entries.append(entry)
            o.table(f'{op}_{width}_xrm', entries)
            entries = []
            for v in values:
                for base in BASES:
                    entry = f'{op}_{width}_mr_{v}_{base}'
                    o.gadget(entry)
                    memory_prologue(o, base)
                    o.emit(f'\t{ld} {hr}26, [_cpu, #{v}]')
                    arith_destination_body(o, op, width)
                    entries.append(entry)
            o.table(f'{op}_{width}_mr', entries)
            entries = []
            for v in values:
                entry = f'{op}_{width}_xmr_{v}'
                o.gadget(entry)
                access_prologue(o)
                o.emit(f'\t{ld} {hr}26, [_cpu, #{v}]')
                arith_destination_body(o, op, width)
                entries.append(entry)
            o.table(f'{op}_{width}_xmr', entries)
            entries = []
            for base in BASES:
                entry = f'{op}_{width}_mi_{base}'
                o.gadget(entry)
                memory_prologue(o, base)
                o.emit('\tldr x26, [_stream], #8')
                arith_destination_body(o, op, width)
                entries.append(entry)
            o.table(f'{op}_{width}_mi', entries)
            entry = f'{op}_{width}_xmi'
            o.gadget(entry)
            access_prologue(o)
            o.emit('\tldr x26, [_stream], #8')
            arith_destination_body(o, op, width)
            immediates.append(entry)
        o.table(f'{op}_xmi', immediates)


def lea(o):
    """LEA cannot fault, so its stream is the displacement alone."""
    for width in (32, 64):
        entries = []
        for d in GPR:
            for base in BASES:
                entry = f'lea_{width}_{d}_{base}'
                o.gadget(entry)
                o.emit('\tldr x14, [_stream], #8')
                base_address(o, base)
                if width == 32:
                    o.emit('\tmov w7, w7')
                o.emit(f'\tstr x7, [_cpu, #{d}]')
                o.emit('\tretire_and_next')
                entries.append(entry)
        o.table(f'lea_{width}', entries)
        entries = []
        for d in GPR:
            entry = f'lea_{width}_x_{d}'
            o.gadget(entry)
            if width == 32:
                o.emit('\tmov w7, w7')
            o.emit(f'\tstr x7, [_cpu, #{d}]')
            o.emit('\tretire_and_next')
            entries.append(entry)
        o.table(f'lea_{width}_x', entries)


def push_pop(o):
    """64-bit PUSH and POP of a register. Stream: length word, RIP."""
    entries = []
    for d in GPR:
        entry = f'push_64_r_{d}'
        o.gadget(entry)
        o.emit('\tldp x23, x22, [_stream], #16')
        o.emit(f'\tldr x24, [_cpu, #{d}]')
        o.emit('\tldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]')
        o.emit('\tsub x25, x25, #8')
        o.emit('\tmov x7, x25')
        crossed, done = o.label('pushcross'), o.label('pushdone')
        page_cross(o, 8, crossed)
        translate(o, True, 8)
        o.emit('\tstr x24, [x7]')
        o.emit(f'{done}:')
        o.emit('\tstr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]')
        finish_store(o, 8)
        o.emit(f'{crossed}:')
        o.emit(f'\tadr x19, {done}')
        o.emit('\tb .Lx86_baked_cross_store_8')
        entries.append(entry)
    o.table('push_64_r', entries)
    entries = []
    for d in GPR:
        entry = f'pop_64_r_{d}'
        o.gadget(entry)
        o.emit('\tldp x23, x22, [_stream], #16')
        o.emit('\tldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]')
        o.emit('\tmov x7, x25')
        crossed, done = o.label('popcross'), o.label('popdone')
        page_cross(o, 8, crossed)
        translate(o, False, 8)
        o.emit('\tldr x11, [x7]')
        o.emit(f'{done}:')
        o.emit('\tadd x9, x25, #8')
        o.emit('\tstr x9, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]')
        o.emit(f'\tstr x11, [_cpu, #{d}]')
        o.emit('\tretire_and_next')
        o.emit(f'{crossed}:')
        o.emit(f'\tadr x19, {done}')
        o.emit('\tb .Lx86_baked_cross_load_64')
        entries.append(entry)
    o.table('pop_64_r', entries)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--output', required=True)
    options = parser.parse_args()
    o = Out()
    o.emit('/* SPDX-License-Identifier: GPL-2.0-or-later */')
    o.emit('/* Generated by generate_baked.py; do not edit. */')
    shared_tails(o)
    arith_register(o)
    move_register(o)
    extend_register(o)
    unary_register(o)
    addresses(o)
    loads(o)
    stores(o)
    memory_arith(o)
    lea(o)
    push_pop(o)
    o.emit(f'/* {o.count} baked entries */')
    with open(options.output, 'w') as output:
        output.write('\n'.join(o.lines) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
