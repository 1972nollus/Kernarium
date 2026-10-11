# SPDX-License-Identifier: GPL-2.0-or-later
"""The Advanced SIMD structure loads and stores (VLDn, VSTn) of the gadget
engine's AArch32 part, as gadgets that translate the address and move the
elements with A64 instructions.

Three shapes, as the decoder names them:
  m  multiple structures: VLD1 is a copy of 8..32 bytes onto consecutive D
     registers; VLD2, VLD3 and VLD4 de-interleave with LD2, LD3 and LD4 into
     V0-V3 and store the low halves to the D registers the instruction
     names; VLD2 may name two pairs (32 bytes).
  1  one lane: each of the 1-4 structures' elements goes to its register at
     the instruction's index, with a scalar copy.
  a  all lanes: LD1R to LD4R, one element replicated across a register (VLD1
     may name two registers).

Stream: [key][word0][word1][library operand]. word0 holds Rn's CpuState
offset in bits 0-14, writeback in bit 15, Rm's offset in bits 16-30 and a
register-index writeback in bit 31, and the alignment mask (alignment - 1)
in bits 32-39. word1 holds the CpuState offsets of up to four D register
slots, 16 bits each (the lane's byte for the one-lane shape). A misaligned
hinted access is the semantic library's, run as the whole instruction through
the library operand, so the fault and its order are the library's; so is a
transfer that crosses a page, except a VLD1 or VST1 copy, which the A64 slow
transfer helper moves through q0 and q1 after it has translated both pages.
The writeback is Rn plus the transfer size, or plus Rm.
"""

LOAD, STORE = 0, 1
SHAPES = ("m", "1", "a")
SIZES = (1, 2, 4)
ARRANGEMENT = {1: "8b", 2: "4h", 4: "2s"}


def copy_lines(bytes_: int, load: bool) -> list[str]:
    """A copy between the host pointer x7 and the registers at x13."""
    src, dst = ("x7", "x13") if load else ("x13", "x7")
    if bytes_ == 8:
        return [f"ldr d0, [{src}]", f"str d0, [{dst}]"]
    if bytes_ == 16:
        return [f"ldr q0, [{src}]", f"str q0, [{dst}]"]
    if bytes_ == 24:
        return [f"ldr q0, [{src}]", f"ldr d1, [{src}, #16]",
                f"str q0, [{dst}]", f"str d1, [{dst}, #16]"]
    return [f"ldp q0, q1, [{src}]", f"stp q0, q1, [{dst}]"]


def slot(k: int) -> list[str]:
    """x13 = the CpuState address of slot k."""
    return [f"ubfx x13, x24, #{16 * k}, #16", "add x13, _cpu, x13"]


def body(name: str, load: bool, bytes_: int, transfer: list[str],
         split: list[str] | None = None) -> str:
    """`split`, if given, moves the bytes of a transfer that crosses a page."""
    direction = LOAD if load else STORE
    page = "UML_GADGET_TLB_ENTRY_READ_PAGE" if load else \
        "UML_GADGET_TLB_ENTRY_WRITE_PAGE"
    crossing = f"Lsplit_{name}" if split else f"Lcross_{name}"
    translate = (f"{direction}, {page}, {bytes_}, x7, Lcold_{name},"
                 f" Lresume_{name}, {crossing}, 0,"
                 f" fault=La32_memory_fault")
    lines = ["ldp x21, x19, [_stream], #16", "ldr x24, [_stream], #8",
             "and x9, x19, #0x7fff", "ldr w9, [_cpu, x9]",
             "tbnz x19, #31, 2f", f"mov w10, #{bytes_}", "b 3f",
             "2:", "ubfx x10, x19, #16, #15", "ldr w10, [_cpu, x10]",
             "3:", "add w22, w9, w10", "mov w7, w9",
             "ubfx x11, x19, #32, #8", "tst w7, w11",
             f"b.ne Lcross_{name}",
             "transfer_translate " + translate]
    lines += transfer
    lines += [f"Lfinish_{name}:", "tbz x19, #15, 1f", "and x9, x19, #0x7fff",
              "str w22, [_cpu, x9]", "1:", "add _stream, _stream, #8",
              "next_gadget",
              "transfer_translate_cold " + translate]
    if split:
        lines += [f"Lsplit_{name}:"] + split + [f"b Lfinish_{name}"]
    lines += [f"Lcross_{name}:", "ldr x8, [_stream], #8",
              "a32_library_call", "next_gadget"]
    return "\n".join(lines)


def name_of(load: bool, shape: str, structs: int, size: int,
            variant: int) -> str:
    stem = f"neon_{'vld' if load else 'vst'}{structs}_{shape}"
    if shape == "m" and structs == 1:
        return f"{stem}_{8 * (variant + 1)}"
    return f"{stem}_{size}" + ("_p2" if variant else "")


def vector_lines(bytes_: int, load: bool) -> list[str]:
    """Between q0 (and q1) and the registers at x13: the staging copy of a
    transfer that crosses a page."""
    if bytes_ == 8:
        return ["str d0, [x13]"] if load else ["ldr d0, [x13]"]
    if bytes_ == 16:
        return ["str q0, [x13]"] if load else ["ldr q0, [x13]"]
    if bytes_ == 24:
        return (["str q0, [x13]", "str d1, [x13, #16]"] if load else
                ["ldr q0, [x13]", "ldr d1, [x13, #16]"])
    return ["stp q0, q1, [x13]"] if load else ["ldp q0, q1, [x13]"]


def multiple_copy(asm, load: bool, regs: int) -> None:
    bytes_ = 8 * regs
    name = name_of(load, "m", 1, 1, regs - 1)
    slow = f"transfer_slow {0 if load else 1}, x7, {bytes_}," \
           f" fault=La32_memory_fault"
    split = ([slow] + slot(0) + vector_lines(bytes_, True) if load else
             slot(0) + vector_lines(bytes_, False) + [slow])
    asm.gadget(name, body(name, load, bytes_,
                          slot(0) + copy_lines(bytes_, load), split))


def multiple_interleaved(asm, load: bool, structs: int, size: int,
                         pairs: int) -> None:
    arrangement = ARRANGEMENT[size]
    count = structs * pairs
    name = name_of(load, "m", structs, size, pairs - 1)
    transfer = []
    if load:
        for pair in range(pairs):
            registers = ", ".join(f"v{structs * pair + s}.{arrangement}"
                                  for s in range(structs))
            if pair:
                transfer.append("add x7, x7, #16")
            transfer.append(f"ld{structs} {{{registers}}}, [x7]")
        for k in range(count):
            transfer += slot(k) + [f"str d{k}, [x13]"]
    else:
        for k in range(count):
            transfer += slot(k) + [f"ldr d{k}, [x13]"]
        for pair in range(pairs):
            registers = ", ".join(f"v{structs * pair + s}.{arrangement}"
                                  for s in range(structs))
            if pair:
                transfer.append("add x7, x7, #16")
            transfer.append(f"st{structs} {{{registers}}}, [x7]")
    asm.gadget(name, body(name, load, 8 * count, transfer))


def single_lane(asm, load: bool, structs: int, size: int) -> None:
    name = name_of(load, "1", structs, size, 0)
    suffix = {1: "b", 2: "h", 4: ""}[size]
    reg = "w9"
    transfer = []
    for s in range(structs):
        transfer += slot(s)
        if load:
            transfer += [f"ldr{suffix} {reg}, [x7, #{s * size}]",
                         f"str{suffix} {reg}, [x13]"]
        else:
            transfer += [f"ldr{suffix} {reg}, [x13]",
                         f"str{suffix} {reg}, [x7, #{s * size}]"]
    asm.gadget(name, body(name, load, structs * size, transfer))


def all_lanes(asm, structs: int, size: int, pairs: int) -> None:
    """VLD1 names one or two registers (pairs = 2: the same element in both);
    VLDn names n registers."""
    arrangement = ARRANGEMENT[size]
    name = name_of(True, "a", structs, size, pairs - 1)
    registers = ", ".join(f"v{s}.{arrangement}" for s in range(structs))
    transfer = [f"ld{structs}r {{{registers}}}, [x7]"]
    for k in range(structs):
        transfer += slot(k) + [f"str d{k}, [x13]"]
    if structs == 1 and pairs == 2:
        transfer += slot(1) + ["str d0, [x13]"]
    asm.gadget(name, body(name, True, structs * size, transfer))


def emit(asm) -> None:
    for load in (True, False):
        for regs in (1, 2, 3, 4):
            multiple_copy(asm, load, regs)
        for structs in (2, 3, 4):
            for size in SIZES:
                for pairs in ((1, 2) if structs == 2 else (1,)):
                    multiple_interleaved(asm, load, structs, size, pairs)
        for structs in (1, 2, 3, 4):
            for size in SIZES:
                single_lane(asm, load, structs, size)
    for structs in (1, 2, 3, 4):
        for size in SIZES:
            for pairs in ((1, 2) if structs == 1 else (1,)):
                all_lanes(asm, structs, size, pairs)


def header_tables(names: set[str]) -> list[str]:
    """kNeonMem[load / store][m / 1 / a][structures - 1][log2 size][variant]:
    the gadget, or G::count where the combination does not exist. The
    variant is the register count - 1 for a VLD1/VST1 copy and pairs - 1 for
    a two-pair VLD2/VST2 and for a VLD1 that fills two registers."""
    def g(name: str) -> str:
        return f"G::{name}" if name in names else "G::count"

    out = ["/* Structure loads and stores: [store][shape m, 1, a][structures - 1]"
           "[log2 size][variant]. */",
           "inline constexpr G kNeonMem[2][3][4][3][4] = {"]
    for store in (0, 1):
        load = store == 0
        out.append("\t{")
        for shape in SHAPES:
            out.append("\t\t{")
            for structs in (1, 2, 3, 4):
                out.append("\t\t\t{")
                for log2, size in enumerate(SIZES):
                    cells = []
                    for variant in range(4):
                        if shape == "m" and structs == 1:
                            # A copy: the bytes and the alignment mask do not
                            # depend on the element size, so every size shares
                            # the 8-bit cells.
                            cells.append(g(name_of(load, shape, structs, size,
                                                   variant)))
                        else:
                            cells.append(g(name_of(load, shape, structs, size,
                                                   variant)) if variant < 2
                                         else "G::count")
                    out.append("\t\t\t\t{" + ", ".join(cells) + "},")
                out.append("\t\t\t},")
            out.append("\t\t},")
        out.append("\t},")
    out.append("};")
    return out
