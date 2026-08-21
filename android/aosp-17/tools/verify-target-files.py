#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import struct
import zipfile
from pathlib import Path, PurePosixPath


ELF_MAGIC = b"\x7fELF"
ELFCLASS64 = 2
ELFDATA2LSB = 1
ET_EXEC = 2
ET_DYN = 3
EM_AARCH64 = 183
PT_LOAD = 1
PAGE_SIZE = 16384
MAX_PROGRAM_HEADERS_SIZE = 1 << 20

REQUIRED_ENTRIES = {
    "BOOT/kernel",
    "INIT_BOOT/RAMDISK/init",
    "IMAGES/odm.img",
    "IMAGES/product.img",
    "IMAGES/system.img",
    "IMAGES/system_ext.img",
    "IMAGES/userdata.img",
    "IMAGES/vendor.img",
    "META/init_boot_filesystem_config.txt",
    "META/kernel_configs.txt",
    "META/kernel_version.txt",
    "META/misc_info.txt",
    "META/ramdisk_node_list",
    "VENDOR/etc/fstab.uml",
    "VENDOR/etc/init/init.cutf_cvm.rc",
    "VENDOR/etc/selinux/vendor_sepolicy.cil",
    "VENDOR/etc/ueventd.rc",
    "VENDOR/etc/vintf/compatibility_matrix.xml",
    "VENDOR/etc/vintf/manifest.xml",
    "VENDOR_BOOT/RAMDISK/first_stage_ramdisk/system/etc/fstab.uml",
}
DLKM_COMPONENTS = {"system_dlkm", "vendor_dlkm", "odm_dlkm"}
DLKM_COMPATIBILITY_ENTRIES = {
    "ROOT/odm_dlkm",
    "ROOT/odm_dlkm/etc",
    "ROOT/system_dlkm",
    "ROOT/vendor_dlkm",
    "ROOT/vendor_dlkm/etc",
    "SYSTEM/system_dlkm",
    "SYSTEM/system_dlkm/etc",
    "SYSTEM/system_dlkm/etc/build.prop",
    "VENDOR/odm_dlkm",
    "VENDOR/odm_dlkm/etc",
    "VENDOR/odm_dlkm/etc/build.prop",
    "VENDOR/vendor_dlkm",
    "VENDOR/vendor_dlkm/etc",
    "VENDOR/vendor_dlkm/etc/build.prop",
    "VENDOR_BOOT/RAMDISK/odm_dlkm",
    "VENDOR_BOOT/RAMDISK/odm_dlkm/etc",
    "VENDOR_BOOT/RAMDISK/system_dlkm",
    "VENDOR_BOOT/RAMDISK/vendor_dlkm",
    "VENDOR_BOOT/RAMDISK/vendor_dlkm/etc",
}
MODULE_METADATA = {
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
}
TARGET_PARTITIONS = {"SYSTEM", "SYSTEM_EXT", "PRODUCT", "VENDOR", "ODM"}
FORBIDDEN_STATIC_ENTRIES = {
    "IMAGES/super.img",
    "IMAGES/super_empty.img",
    "IMAGES/system_other.img",
    "IMAGES/vbmeta.img",
    "IMAGES/vbmeta_system.img",
    "IMAGES/vbmeta_vendor.img",
    "META/ab_partitions.txt",
    "VENDOR/apex/com.android.hardware.boot.apex",
    "VENDOR/etc/init/seriallogging.rc",
}
UML_FSTAB = b"""# Fixed GPT partitions exposed by the Darwin UML UBD device.
/dev/block/by-name/system /system erofs ro wait,first_stage_mount
/dev/block/by-name/system_ext /system_ext ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/product /product erofs ro wait,first_stage_mount
/dev/block/by-name/vendor /vendor ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/odm /odm ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/metadata /metadata ext4 nodev,noatime,nosuid wait,check,formattable,first_stage_mount
/dev/block/by-name/metadata /metadata ext4 nodev,noatime,nosuid wait,check,first_stage_mount
# Static non-A/B storage uses neither userdata checkpointing nor BootControl.
/dev/block/by-name/userdata /data ext4 nodev,noatime,nosuid,errors=panic latemount,wait,check,formattable,keydirectory=/metadata/vold/metadata_encryption,fileencryption=aes-256-xts:aes-256-hctr2
/dev/block/by-name/userdata /data ext4 nodev,noatime,nosuid,errors=panic latemount,reservedsize=128M,wait,check,keydirectory=/metadata/vold/metadata_encryption,fileencryption=aes-256-xts:aes-256-hctr2
"""
UML_FSTAB_ENTRIES = (
    "VENDOR/etc/fstab.uml",
    "VENDOR_BOOT/RAMDISK/first_stage_ramdisk/system/etc/fstab.uml",
)
UML_INIT_REQUIRED = (
    "mount_all --early",
    "mount_all --late",
    "ro.hardware.audio.primary",
)
UML_INIT_FORBIDDEN = (
    "metrics_helper",
    "rename_eth0",
    "setup_wifi",
    "socket_vsock_proxy",
    "/dev/hvc",
    "suspend_blocker",
    "/dev/block/by-name/custom",
)


def fail(message: str) -> None:
    raise SystemExit(f"Android target-files verifier: {message}")


def sha256_path(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_member(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> str:
    digest = hashlib.sha256()
    with archive.open(info) as source:
        while chunk := source.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def safe_name(name: str) -> PurePosixPath:
    if not name or "\\" in name or "\0" in name:
        fail(f"unsafe archive path: {name!r}")
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        fail(f"unsafe archive path: {name!r}")
    if str(path) != name:
        fail(f"non-normalized archive path: {name!r}")
    return path


def reject_module_payload(path: PurePosixPath) -> None:
    lowered = tuple(part.lower() for part in path.parts)
    if (any(part in DLKM_COMPONENTS for part in lowered)
            and str(path) not in DLKM_COMPATIBILITY_ENTRIES):
        fail(f"DLKM payload is forbidden: {path}")
    basename = lowered[-1]
    if basename == "dlkm_loader":
        fail(f"kernel module loader is forbidden: {path}")
    if basename.endswith(".ko") or ".ko." in basename:
        fail(f"loadable kernel module is forbidden: {path}")
    if basename in MODULE_METADATA:
        fail(f"kernel module metadata is forbidden: {path}")
    if any(
        lowered[index : index + 2] == ("lib", "modules")
        for index in range(len(lowered) - 1)
    ):
        fail(f"kernel module directory is forbidden: {path}")


def reject_nonstatic_payload(path: PurePosixPath) -> None:
    canonical = str(path)
    if canonical in FORBIDDEN_STATIC_ENTRIES:
        fail(f"nonstatic image or metadata is forbidden: {path}")
    if path.parts[:2] == ("OTA", "bin"):
        fail(f"OTA tool payload is forbidden: {path}")
    if path.parts[0] == "IMAGES" and path.name.endswith("_dlkm.img"):
        fail(f"DLKM image is forbidden: {path}")
    if path.name.startswith("fstab.cf.") and path.parts[0] in {"VENDOR", "VENDOR_BOOT"}:
        fail(f"Cuttlefish fstab is forbidden: {path}")


def read_small(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> bytes:
    if info.is_dir() or info.file_size > 1 << 20:
        fail(f"policy file is missing or unexpectedly large: {info.filename}")
    return archive.read(info)


def check_boot_policy(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    for entry in UML_FSTAB_ENTRIES:
        if read_small(archive, infos[entry]) != UML_FSTAB:
            fail(f"UML fstab differs from the fixed UBD contract: {entry}")

    init_payload = read_small(
        archive, infos["VENDOR/etc/init/init.cutf_cvm.rc"]
    ).decode("utf-8", errors="strict")
    for token in UML_INIT_REQUIRED:
        if token not in init_payload:
            fail(f"UML init policy is missing required action: {token}")
    for token in UML_INIT_FORBIDDEN:
        if token in init_payload:
            fail(f"UML init policy retains unsupported Cuttlefish action: {token}")

    ueventd_payload = read_small(
        archive, infos["VENDOR/etc/ueventd.rc"]
    ).decode("utf-8", errors="strict")
    if "/dev/ttyS0 0660 root shell" not in ueventd_payload:
        fail("UML ueventd policy does not expose the serial console")
    for token in ("/dev/hvc", "/dev/ion", "/dev/pmem", "/dev/vhci"):
        if token in ueventd_payload:
            fail(f"UML ueventd policy retains unsupported device: {token}")


def check_kernel_policy(
    archive: zipfile.ZipFile,
    infos: dict[str, zipfile.ZipInfo],
    kernel_config: Path,
    kernel_release_file: Path,
) -> None:
    packaged_config = read_small(archive, infos["META/kernel_configs.txt"])
    if packaged_config != kernel_config.read_bytes():
        fail("packaged kernel configuration differs from the ACK UML build")

    packaged_release = read_small(
        archive, infos["META/kernel_version.txt"]
    ).decode("utf-8", errors="strict").strip()
    kernel_release = kernel_release_file.read_text(
        encoding="utf-8", errors="strict"
    ).strip()
    if not kernel_release or packaged_release != kernel_release:
        fail("packaged kernel release differs from the ACK UML build")

    vendor_policy = read_small(
        archive, infos["VENDOR/etc/selinux/vendor_sepolicy.cil"]
    )
    if b"(policycap memfd_class)" not in vendor_policy:
        fail("Android 17 vendor policy does not enable policycap memfd_class")


def check_property_file(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> None:
    if info.file_size > 1 << 20:
        fail(f"property file is unexpectedly large: {info.filename}")
    payload = archive.read(info).decode("utf-8", errors="strict")
    for line in payload.splitlines():
        key, separator, value = line.partition("=")
        if not separator:
            continue
        value = value.strip()
        if key.startswith("ro.virtual_ab."):
            fail(f"Virtual A/B property is forbidden in {info.filename}: {key}")
        if key == "ro.build.ab_update" and value == "true":
            fail(f"A/B update property is enabled in {info.filename}")
        if key == "ro.product.cpu.abilist32" and value:
            fail(f"32-bit ABI list is not empty in {info.filename}: {value}")
        if key in {"ro.product.cpu.abi", "ro.product.cpu.abilist"}:
            abis = {item.strip() for item in value.split(",") if item.strip()}
            if abis - {"arm64-v8a"}:
                fail(f"non-arm64 ABI in {info.filename}: {value}")


def read_elf_prefix(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> bytes | None:
    if info.file_size < 64:
        return None
    with archive.open(info) as source:
        header = source.read(64)
        if header[:4] != ELF_MAGIC:
            return None
        if header[4] != ELFCLASS64 or header[5] != ELFDATA2LSB:
            fail(f"target ELF is not little-endian ELF64: {info.filename}")
        program_offset = struct.unpack_from("<Q", header, 32)[0]
        entry_size, entry_count = struct.unpack_from("<HH", header, 54)
        program_end = program_offset + entry_size * entry_count
        if program_end > info.file_size or program_end > MAX_PROGRAM_HEADERS_SIZE:
            fail(f"malformed ELF program headers: {info.filename}")
        prefix = header
        if program_end > len(prefix):
            prefix += source.read(program_end - len(prefix))
        if len(prefix) < program_end:
            fail(f"truncated ELF program headers: {info.filename}")
        return prefix


def check_target_elf(info: zipfile.ZipInfo, payload: bytes) -> None:
    elf_type, machine = struct.unpack_from("<HH", payload, 16)
    if elf_type not in (ET_EXEC, ET_DYN):
        return
    if machine != EM_AARCH64:
        fail(f"target executable is not AArch64: {info.filename}")

    program_offset = struct.unpack_from("<Q", payload, 32)[0]
    entry_size, entry_count = struct.unpack_from("<HH", payload, 54)
    if entry_size < 56:
        fail(f"malformed ELF program-header size: {info.filename}")
    for index in range(entry_count):
        offset = program_offset + index * entry_size
        (
            kind,
            _flags,
            file_offset,
            virtual_address,
            _physical_address,
            _file_size,
            _memory_size,
            alignment,
        ) = struct.unpack_from("<IIQQQQQQ", payload, offset)
        if kind != PT_LOAD:
            continue
        if alignment < PAGE_SIZE or alignment & (alignment - 1):
            fail(
                f"target ELF load alignment is below 16 KiB: {info.filename} "
                f"({alignment:#x})"
            )
        if virtual_address % alignment != file_offset % alignment:
            fail(f"target ELF load alignment is incongruent: {info.filename}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target-files", type=Path, required=True)
    parser.add_argument("--kernel-elf", type=Path, required=True)
    parser.add_argument("--kernel-config", type=Path)
    parser.add_argument("--kernel-release-file", type=Path)
    args = parser.parse_args()

    kernel_config = args.kernel_config or args.kernel_elf.parent / ".config"
    kernel_release_file = (
        args.kernel_release_file
        or args.kernel_elf.parent / "include/config/kernel.release"
    )

    if not args.target_files.is_file():
        fail(f"target-files archive does not exist: {args.target_files}")
    if not args.kernel_elf.is_file():
        fail(f"kernel ELF does not exist: {args.kernel_elf}")
    if not kernel_config.is_file():
        fail(f"kernel configuration does not exist: {kernel_config}")
    if not kernel_release_file.is_file():
        fail(f"kernel release file does not exist: {kernel_release_file}")

    seen: set[str] = set()
    infos: dict[str, zipfile.ZipInfo] = {}
    elf_count = 0
    try:
        archive = zipfile.ZipFile(args.target_files)
    except (OSError, zipfile.BadZipFile) as error:
        fail(f"cannot open target-files archive: {error}")

    with archive:
        for info in archive.infolist():
            path = safe_name(info.filename.rstrip("/"))
            canonical = str(path)
            if canonical in seen:
                fail(f"duplicate archive entry: {canonical}")
            seen.add(canonical)
            infos[canonical] = info
            reject_module_payload(path)
            reject_nonstatic_payload(path)
            if info.is_dir():
                continue
            if path.name in {"build.prop", "default.prop"}:
                check_property_file(archive, info)
            if path.parts[0] not in TARGET_PARTITIONS:
                continue
            if len(path.parts) > 1 and path.parts[1] == "lib":
                fail(f"32-bit target library directory is forbidden: {path}")
            payload = read_elf_prefix(archive, info)
            if payload is not None:
                elf_count += 1
                check_target_elf(info, payload)

        missing = sorted(REQUIRED_ENTRIES - seen)
        if missing:
            fail(f"required entries are missing: {', '.join(missing)}")
        check_boot_policy(archive, infos)
        check_kernel_policy(
            archive, infos, kernel_config, kernel_release_file
        )
        if sha256_member(archive, infos["BOOT/kernel"]) != sha256_path(args.kernel_elf):
            fail("BOOT/kernel does not match the verified ACK UML ELF")

    print(
        f"verified AOSP 17 target files: {len(seen)} entries, "
        f"{elf_count} target executables"
    )


if __name__ == "__main__":
    main()
