# SPDX-License-Identifier: Apache-2.0
"""What the Android image verifiers share.

boot-image.py, verify-initramfs.py, verify-target-files.py and
verify-lineage-recovery.py read the same formats: the kernelless boot-image
v4 header, a partition's range in partition-layout.json, uncompressed newc
archives and the program headers of AArch64 and Arm ELF payloads. They also compare
against the same tracked first-stage fstab and reject the same kernel
module files. Each caller passes its own error type, so diagnostics keep
their per-tool prefixes.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import json
import stat
import struct
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import BinaryIO, Callable


Error = Callable[[str], Exception]

ANDROID_ROOT = Path(__file__).resolve().parents[1]
BOOT_MAGIC = b"ANDROID!"
BOOT_HEADER_VERSION = 4
BOOT_HEADER_SIZE = 1584
BOOT_PAGE_SIZE = 4096
NEWC_HEADER_SIZE = 110
MAX_NEWC_ENTRY_SIZE = 128 * 1024 * 1024
MAX_PAGE_SIZE_SUPPORTED = 16384
ELF_MAGIC = b"\x7fELF"
ELFCLASS32 = 1
ELFCLASS64 = 2
ELFDATA2LSB = 1
EM_ARM = 40
EM_AARCH64 = 183
ELF32_HEADER_SIZE = 52
ELF32_PROGRAM_HEADER_SIZE = 32
ELF64_HEADER_SIZE = 64
ELF64_PROGRAM_HEADER_SIZE = 56
# The 32-bit userspace is armeabi-v7a: the Arm EABI, version 5, in its softfp
# form (floating-point arguments in integer registers), never hard-float.
EF_ARM_EABIMASK = 0xFF000000
EF_ARM_EABI_VER5 = 0x05000000
EF_ARM_ABI_FLOAT_HARD = 0x00000400
# Arm's 32-bit userspace is linked for 4 KiB pages. The kernel's pages are
# 4 KiB, and no 32-bit library is mapped on a larger page.
MIN_ARM_PAGE_SIZE = 4096
MODULE_METADATA = frozenset({
    "modules.alias",
    "modules.alias.bin",
    "modules.blocklist",
    "modules.builtin",
    "modules.builtin.alias.bin",
    "modules.builtin.modinfo",
    "modules.dep",
    "modules.dep.bin",
    "modules.devname",
    "modules.load",
    "modules.options",
    "modules.order",
    "modules.softdep",
    "modules.symbols",
    "modules.symbols.bin",
})
PT_LOAD = 1
PT_DYNAMIC = 2
SHT_DYNSYM = 11
SHN_UNDEF = 0
DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_STRSZ = 10


def tracked_fstab(error: Error) -> bytes:
    """Return the UML fstab the product installs in vendor and first stage."""
    path = ANDROID_ROOT / "overlay/config/fstab.uml"
    if path.is_symlink() or not path.is_file():
        raise error(f"tracked UML fstab is missing: {path}")
    return path.read_bytes()


def is_module_payload(basename: str) -> bool:
    """Whether a file name is a kernel module or a module-tool index.

    The product's kernel is built in only, so no image carries either.
    """
    lowered = basename.lower()
    return lowered.endswith(".ko") or ".ko." in lowered or lowered in MODULE_METADATA


def read_exact(source: BinaryIO, count: int, label: str, error: Error) -> bytes:
    payload = source.read(count)
    if len(payload) != count:
        raise error(f"truncated {label}")
    return payload


@dataclass(frozen=True)
class BootImage:
    container_offset: int
    container_size: int
    header_version: int
    header_size: int
    kernel_size: int
    ramdisk_offset: int
    ramdisk_size: int
    signature_size: int


def parse_boot_image(
    source: BinaryIO, container_offset: int, container_size: int, error: Error
) -> BootImage:
    """Parse an unsigned boot-image v4 header that embeds no kernel."""
    if container_offset < 0 or container_size < BOOT_PAGE_SIZE:
        raise error("boot image range is invalid")
    source.seek(container_offset)
    header = read_exact(source, BOOT_PAGE_SIZE, "boot image header", error)
    if header[:8] != BOOT_MAGIC:
        raise error("Android boot magic is missing")
    kernel_size, ramdisk_size, _os_version, header_size = struct.unpack_from(
        "<IIII", header, 8
    )
    reserved = struct.unpack_from("<4I", header, 24)
    header_version = struct.unpack_from("<I", header, 40)[0]
    signature_size = struct.unpack_from("<I", header, 1580)[0]
    if header_version != BOOT_HEADER_VERSION or header_size != BOOT_HEADER_SIZE:
        raise error("boot image must use the exact version-4 header")
    if any(reserved):
        raise error("boot image reserved header words are not zero")
    if any(header[BOOT_HEADER_SIZE:BOOT_PAGE_SIZE]):
        raise error("boot image header padding is not zero")
    if kernel_size != 0:
        raise error("boot image must not embed the independently signed kernel")
    if ramdisk_size <= 0:
        raise error("boot image ramdisk is empty")
    if signature_size != 0:
        raise error("boot image must not carry a separate boot signature")

    ramdisk_offset = container_offset + BOOT_PAGE_SIZE
    if ramdisk_offset + ramdisk_size > container_offset + container_size:
        raise error("boot image ramdisk exceeds its container")
    return BootImage(
        container_offset, container_size, header_version, header_size,
        kernel_size, ramdisk_offset, ramdisk_size, signature_size,
    )


def partition_range(
    layout_path: Path, disk_path: Path, name: str, error: Error
) -> tuple[int, int]:
    """Return a partition's byte offset and size from the checked layout."""
    try:
        document = json.loads(layout_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as failure:
        raise error(f"cannot read partition layout: {failure}") from failure
    if not isinstance(document, dict) or document.get("schema_version") != 1:
        raise error("partition layout schema is invalid")
    disk_size = document.get("disk_size_bytes")
    partitions = document.get("partitions")
    if isinstance(disk_size, bool) or not isinstance(disk_size, int):
        raise error("partition layout disk size is invalid")
    if not isinstance(partitions, list):
        raise error("partition layout inventory is invalid")
    matches = [
        entry for entry in partitions
        if isinstance(entry, dict) and entry.get("name") == name
    ]
    if len(matches) != 1:
        raise error(f"partition is absent or repeated: {name}")
    start = matches[0].get("start_bytes")
    size = matches[0].get("size_bytes")
    if (
        isinstance(start, bool) or not isinstance(start, int) or start < 0
        or isinstance(size, bool) or not isinstance(size, int) or size <= 0
        or start + size > disk_size
    ):
        raise error(f"partition range is invalid: {name}")
    if disk_path.stat().st_size != disk_size:
        raise error("disk size differs from the checked partition layout")
    return start, size


class RangeReader:
    """A seekable view of `size` bytes of `source`, starting at `offset`."""

    def __init__(self, source: BinaryIO, offset: int, size: int, error: Error) -> None:
        self.source = source
        self.offset = offset
        self.size = size
        self.position = 0
        self.error = error
        source.seek(offset)

    def tell(self) -> int:
        return self.position

    def read(self, count: int = -1) -> bytes:
        remaining = self.size - self.position
        amount = remaining if count < 0 else min(count, remaining)
        payload = self.source.read(amount)
        self.position += len(payload)
        return payload

    def seek(self, offset: int, whence: int = 0) -> int:
        if whence == 0:
            position = offset
        elif whence == 1:
            position = self.position + offset
        elif whence == 2:
            position = self.size + offset
        else:
            raise self.error("archive reader received an invalid seek origin")
        if position < 0 or position > self.size:
            raise self.error("archive entry exceeds the selected archive range")
        self.source.seek(self.offset + position)
        self.position = position
        return position


@dataclass(frozen=True)
class NewcEntry:
    mode: int
    size: int
    payload: bytes | None


def align4(value: int) -> int:
    return (value + 3) & ~3


def safe_name(raw: bytes, error: Error) -> str:
    try:
        name = raw.decode("utf-8", errors="strict")
    except UnicodeError as failure:
        raise error(f"archive path is not UTF-8: {failure}") from failure
    if not name or "\\" in name or "\0" in name:
        raise error(f"unsafe archive path: {name!r}")
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        raise error(f"unsafe archive path: {name!r}")
    if str(path) != name:
        raise error(f"non-normalized archive path: {name!r}")
    return name


def parse_newc(
    source: RangeReader, retain: Callable[[str, int], bool], error: Error
) -> dict[str, NewcEntry]:
    """Map each path of an uncompressed newc archive to its entry.

    retain(name, mode) selects the payloads kept in memory; the others are
    skipped. Only an identical empty directory may repeat, and only zero
    bytes may follow the trailer.
    """
    entries: dict[str, NewcEntry] = {}
    while True:
        header = read_exact(source, NEWC_HEADER_SIZE, "newc header", error)
        if header[:6] not in (b"070701", b"070702"):
            raise error("archive is not an uncompressed newc archive")
        try:
            fields = [
                int(header[offset : offset + 8], 16)
                for offset in range(6, NEWC_HEADER_SIZE, 8)
            ]
        except ValueError as failure:
            raise error(f"malformed newc header: {failure}") from failure
        mode = fields[1]
        file_size = fields[6]
        name_size = fields[11]
        if file_size > MAX_NEWC_ENTRY_SIZE or name_size < 2 or name_size > 4096:
            raise error("newc entry has an invalid size")
        raw_name = read_exact(source, name_size, "newc path", error)
        if raw_name[-1:] != b"\0":
            raise error("newc path is not NUL terminated")
        source.seek(align4(source.tell()))

        if raw_name[:-1] == b"TRAILER!!!":
            if file_size != 0:
                raise error("newc trailer has data")
            if any(source.read()):
                raise error("newc archive has nonzero data after its trailer")
            return entries

        name = safe_name(raw_name[:-1], error)
        payload = None
        if retain(name, mode):
            payload = read_exact(source, file_size, f"payload for {name}", error)
        else:
            source.seek(file_size, 1)
        source.seek(align4(source.tell()))
        previous = entries.get(name)
        if previous is not None and not (
            stat.S_ISDIR(previous.mode) and stat.S_ISDIR(mode)
            and stat.S_IMODE(previous.mode) == stat.S_IMODE(mode)
            and previous.size == file_size == 0
        ):
            raise error(f"ambiguous duplicate archive entry: {name}")
        entries[name] = NewcEntry(mode, file_size, payload)


@dataclass(frozen=True)
class Segment:
    kind: int
    offset: int
    address: int
    file_size: int
    alignment: int


@dataclass(frozen=True)
class ElfFormat:
    """Where one little-endian ELF class keeps what the verifiers read."""

    elf_class: int
    header_size: int
    program_offset_at: int
    program_offset_format: str
    entry_size_at: int
    program_header_size: int
    program_header_format: str
    dynamic_format: str
    min_page_size: int
    section_offset_at: int
    section_entry_at: int
    section_header_format: str
    symbol_format: str


# The section header is (name, type, flags, address, offset, size, link, info,
# alignment, entry size) in both classes, and a symbol is (name, info, other,
# section index, value, size) in the class's own order.
ELF32 = ElfFormat(
    ELFCLASS32, ELF32_HEADER_SIZE, 28, "<I", 42, ELF32_PROGRAM_HEADER_SIZE,
    "<IIIIIIII", "<iI", MIN_ARM_PAGE_SIZE,
    32, 46, "<IIIIIIIIII", "<IIIBBH",
)
ELF64 = ElfFormat(
    ELFCLASS64, ELF64_HEADER_SIZE, 32, "<Q", 54, ELF64_PROGRAM_HEADER_SIZE,
    "<IIQQQQQQ", "<qQ", MAX_PAGE_SIZE_SUPPORTED,
    40, 58, "<IIQQQQIIQQ", "<IBBHQQ",
)
ELF_FORMATS = {ELFCLASS32: ELF32, ELFCLASS64: ELF64}


def elf_format(name: str, header: bytes, error: Error) -> ElfFormat:
    """Classify a little-endian ELF header; refuse every other image."""
    if (
        len(header) < ELF32_HEADER_SIZE
        or header[:4] != ELF_MAGIC
        or header[5] != ELFDATA2LSB
        or header[4] not in ELF_FORMATS
    ):
        raise error(f"target ELF is not a little-endian ELF32 or ELF64: {name}")
    elf = ELF_FORMATS[header[4]]
    if len(header) < elf.header_size:
        raise error(f"truncated ELF header: {name}")
    return elf


def program_table(elf: ElfFormat, header: bytes) -> tuple[int, int, int]:
    """Return the program-header table's offset, entry size and entry count."""
    offset = struct.unpack_from(elf.program_offset_format, header, elf.program_offset_at)[0]
    entry_size, entry_count = struct.unpack_from("<HH", header, elf.entry_size_at)
    return offset, entry_size, entry_count


def check_elf64_little_endian(name: str, header: bytes, error: Error) -> None:
    if (
        len(header) < ELF64_HEADER_SIZE
        or header[4] != ELFCLASS64
        or header[5] != ELFDATA2LSB
    ):
        raise error(f"target ELF is not little-endian ELF64: {name}")


def check_aarch64_elf_header(name: str, header: bytes, error: Error) -> None:
    """Require a little-endian AArch64 ELF64 header whose program-header
    entries are whole, which program_headers relies on."""
    check_elf64_little_endian(name, header, error)
    if struct.unpack_from("<H", header, 18)[0] != EM_AARCH64:
        raise error(f"target ELF is not AArch64: {name}")
    if struct.unpack_from("<H", header, 54)[0] < ELF64_PROGRAM_HEADER_SIZE:
        raise error(f"target ELF program-header size is malformed: {name}")


def check_arm_elf_header(name: str, header: bytes, error: Error) -> None:
    """Require a little-endian Arm ELF32 header of the softfp EABI, version 5,
    whose program-header entries are whole, which program_headers relies on."""
    if (
        len(header) < ELF32_HEADER_SIZE
        or header[:4] != ELF_MAGIC
        or header[4] != ELFCLASS32
        or header[5] != ELFDATA2LSB
    ):
        raise error(f"target ELF is not little-endian ELF32: {name}")
    if struct.unpack_from("<H", header, 18)[0] != EM_ARM:
        raise error(f"target ELF is not Arm: {name}")
    if struct.unpack_from("<H", header, 42)[0] < ELF32_PROGRAM_HEADER_SIZE:
        raise error(f"target ELF program-header size is malformed: {name}")
    flags = struct.unpack_from("<I", header, 36)[0]
    if flags & EF_ARM_EABIMASK != EF_ARM_EABI_VER5:
        raise error(f"target ELF is not Arm EABI version 5: {name}")
    if flags & EF_ARM_ABI_FLOAT_HARD:
        raise error(f"target ELF is hard-float, but armeabi-v7a is softfp: {name}")


def program_headers(payload: bytes) -> list[Segment]:
    """Return an ELF payload's program headers; the caller bounds the table
    and has classified the payload with elf_format."""
    elf = ELF_FORMATS[payload[4]]
    program_offset, entry_size, entry_count = program_table(elf, payload)
    segments = []
    for index in range(entry_count):
        fields = struct.unpack_from(
            elf.program_header_format, payload, program_offset + index * entry_size
        )
        if elf.elf_class == ELFCLASS64:
            kind, _, offset, address, _, file_size, _, alignment = fields
        else:
            kind, offset, address, _, file_size, _, _, alignment = fields
        segments.append(Segment(kind, offset, address, file_size, alignment))
    return segments


def check_load_alignment(name: str, payload: bytes, error: Error) -> int:
    """Require load segments aligned to the page size the class promises: the
    16 KiB maximum of the page-agnostic 64-bit userspace, and the 4 KiB of the
    32-bit one. Return how many load segments there are."""
    minimum = ELF_FORMATS[payload[4]].min_page_size
    loads = 0
    for segment in program_headers(payload):
        if segment.kind != PT_LOAD:
            continue
        loads += 1
        alignment = segment.alignment
        if alignment < minimum or alignment & (alignment - 1):
            raise error(
                f"target ELF load alignment is below {minimum // 1024} KiB: "
                f"{name} ({alignment:#x})"
            )
        if segment.address % alignment != segment.offset % alignment:
            raise error(f"target ELF load alignment is incongruent: {name}")
    return loads


def elf_needed(name: str, payload: bytes, error: Error) -> set[str]:
    """Return the DT_NEEDED names of a whole ELF32 or ELF64 payload."""
    elf = ELF_FORMATS[payload[4]]
    entry_size = struct.calcsize(elf.dynamic_format)
    loads: list[Segment] = []
    dynamic: Segment | None = None
    for segment in program_headers(payload):
        if segment.offset + segment.file_size > len(payload):
            raise error(f"ELF segment extends beyond payload: {name}")
        if segment.kind == PT_LOAD:
            loads.append(segment)
        elif segment.kind == PT_DYNAMIC:
            if dynamic is not None:
                raise error(f"ELF has multiple dynamic segments: {name}")
            dynamic = segment
    if dynamic is None:
        return set()

    if dynamic.file_size % entry_size:
        raise error(f"ELF dynamic segment is malformed: {name}")
    string_address: int | None = None
    string_size: int | None = None
    needed_offsets: list[int] = []
    terminated = False
    for offset in range(dynamic.offset, dynamic.offset + dynamic.file_size, entry_size):
        tag, value = struct.unpack_from(elf.dynamic_format, payload, offset)
        if tag == DT_NULL:
            terminated = True
            break
        if tag == DT_NEEDED:
            needed_offsets.append(value)
        elif tag == DT_STRTAB:
            string_address = value
        elif tag == DT_STRSZ:
            string_size = value
    if not terminated:
        raise error(f"ELF dynamic segment lacks DT_NULL: {name}")
    if not needed_offsets:
        return set()
    if string_address is None or string_size is None:
        raise error(f"ELF dynamic string table is incomplete: {name}")

    string_offset: int | None = None
    for load in loads:
        if load.address <= string_address < load.address + load.file_size:
            string_offset = load.offset + string_address - load.address
            break
    if string_offset is None or string_offset + string_size > len(payload):
        raise error(f"ELF dynamic string table is outside a load segment: {name}")
    string_table = payload[string_offset : string_offset + string_size]
    needed: set[str] = set()
    for offset in needed_offsets:
        if offset >= len(string_table):
            raise error(f"ELF DT_NEEDED offset is out of bounds: {name}")
        end = string_table.find(b"\0", offset)
        if end < 0:
            raise error(f"ELF DT_NEEDED string is unterminated: {name}")
        try:
            needed.add(string_table[offset:end].decode("utf-8", errors="strict"))
        except UnicodeDecodeError as failure:
            raise error(f"ELF DT_NEEDED string is not UTF-8: {name}: {failure}") from failure
    return needed


def elf_imports(name: str, payload: bytes, error: Error) -> set[str]:
    """Return the undefined dynamic symbols, the imports, of a whole ELF32 or
    ELF64 payload that elf_format has classified. They come from the section
    headers, so a payload without a dynamic symbol table is refused."""
    elf = ELF_FORMATS[payload[4]]
    table = struct.unpack_from(elf.program_offset_format, payload, elf.section_offset_at)[0]
    entry_size, count = struct.unpack_from("<HH", payload, elf.section_entry_at)
    if (
        not table
        or not count
        or entry_size < struct.calcsize(elf.section_header_format)
        or table + entry_size * count > len(payload)
    ):
        raise error(f"ELF has no readable section headers, so its imports cannot be read: {name}")
    sections = [
        struct.unpack_from(elf.section_header_format, payload, table + index * entry_size)
        for index in range(count)
    ]
    imports: set[str] = set()
    found = False
    symbol_size = struct.calcsize(elf.symbol_format)
    for _, kind, _, _, offset, size, link, _, _, entry in sections:
        if kind != SHT_DYNSYM:
            continue
        found = True
        if link >= count or entry != symbol_size or size % symbol_size or offset + size > len(payload):
            raise error(f"ELF dynamic symbol table is malformed: {name}")
        _, _, _, _, string_offset, string_size, _, _, _, _ = sections[link]
        if string_offset + string_size > len(payload):
            raise error(f"ELF dynamic string table is outside the payload: {name}")
        strings = payload[string_offset : string_offset + string_size]
        for position in range(offset, offset + size, symbol_size):
            fields = struct.unpack_from(elf.symbol_format, payload, position)
            if elf.elf_class == ELFCLASS32:
                symbol, _, _, _, _, section = fields
            else:
                symbol, _, _, section, _, _ = fields
            if section != SHN_UNDEF or symbol == 0:
                continue
            end = strings.find(b"\0", symbol)
            if symbol >= len(strings) or end < 0:
                raise error(f"ELF dynamic symbol name is out of bounds: {name}")
            imports.add(strings[symbol:end].decode("utf-8", errors="replace"))
    if not found:
        raise error(f"ELF has no dynamic symbol table: {name}")
    return imports
