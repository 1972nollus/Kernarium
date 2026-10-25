#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import gzip
import importlib.util
import io
import json
import re
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"android builder lane fixture: {message}")


def run_tool(
    tool: Path, *arguments: object, check: bool = False, **options: object
) -> subprocess.CompletedProcess[str]:
    """Run a Python tool with stdin closed.

    A checked run raises on failure and keeps the test's own output streams.
    Any other run captures stdout and stderr together as text, unless
    `options` name the streams.
    """
    if not check:
        options = {
            "stdout": subprocess.PIPE,
            "stderr": subprocess.STDOUT,
            "text": True,
            **options,
        }
    return subprocess.run(
        [sys.executable, "-B", str(tool), *map(str, arguments)],
        check=check,
        stdin=subprocess.DEVNULL,
        **options,
    )


def with_imports(payload: bytes, elf64: bool, imports: tuple[str, ...]) -> bytes:
    """Append a dynamic symbol table that imports the given functions, and the
    section headers that name it, to a synthetic ELF."""
    data = bytearray(payload)
    strings = bytearray(b"\0")
    symbols = bytearray(24 if elf64 else 16)
    for name in imports:
        offset = len(strings)
        strings.extend(name.encode() + b"\0")
        symbols.extend(
            struct.pack("<IBBHQQ", offset, 0x12, 0, 0, 0, 0)
            if elf64 else struct.pack("<IIIBBH", offset, 0, 0, 0x12, 0, 0)
        )
    data.extend(b"\0" * (-len(data) % 8))
    string_offset = len(data)
    data.extend(strings)
    data.extend(b"\0" * (-len(data) % 8))
    symbol_offset = len(data)
    data.extend(symbols)
    data.extend(b"\0" * (-len(data) % 8))
    section_offset = len(data)
    entry_size = 64 if elf64 else 40
    data.extend(b"\0" * entry_size)  # SHT_NULL
    for kind, offset, size, link, entry in (
        (11, symbol_offset, len(symbols), 2, 24 if elf64 else 16),  # .dynsym
        (3, string_offset, len(strings), 0, 0),                     # .dynstr
    ):
        data.extend(
            struct.pack("<IIQQQQIIQQ", 0, kind, 2, 0, offset, size, link, 1, 8, entry)
            if elf64 else
            struct.pack("<IIIIIIIIII", 0, kind, 2, 0, offset, size, link, 1, 4, entry)
        )
    if elf64:
        struct.pack_into("<Q", data, 40, section_offset)
        struct.pack_into("<HH", data, 58, entry_size, 3)
    else:
        struct.pack_into("<I", data, 32, section_offset)
        struct.pack_into("<HH", data, 46, entry_size, 3)
    return bytes(data)


# What a library built with -D_FILE_OFFSET_BITS=64 imports on a 32-bit target,
# and what the same source imports on a 64-bit one, where Bionic has one off_t.
LARGE_FILE_IMPORTS_32 = ("open", "mmap64", "lseek64", "munmap")
LARGE_FILE_IMPORTS_64 = ("open", "mmap", "lseek", "munmap")


def synthetic_aarch64_elf(
    alignment: int = 16384,
    needed: tuple[str, ...] = (),
    edit: tuple[str, int, int] | None = None,
    imports: tuple[str, ...] | None = None,
) -> bytes:
    """An AArch64 ET_DYN; `edit` is a (struct format, offset, value) overwrite
    and `imports` adds a dynamic symbol table."""
    program_headers = 2 if needed else 1
    dynamic_entries = 3 + len(needed) if needed else 0
    dynamic_offset = 64 + 56 * program_headers
    string_table = bytearray(b"\0")
    string_offsets: dict[str, int] = {}
    for item in needed:
        string_offsets[item] = len(string_table)
        string_table.extend(item.encode())
        string_table.append(0)
    payload_size = dynamic_offset + 16 * dynamic_entries
    string_offset = payload_size
    if needed:
        payload_size += len(string_table)
    payload = bytearray(payload_size)
    payload[:4] = b"\x7fELF"
    payload[4] = 2
    payload[5] = 1
    payload[6] = 1
    struct.pack_into("<HHI", payload, 16, 3, 183, 1)
    struct.pack_into("<Q", payload, 32, 64)
    struct.pack_into("<HHH", payload, 52, 64, 56, program_headers)
    struct.pack_into(
        "<IIQQQQQQ",
        payload,
        64,
        1,
        5,
        0,
        0,
        0,
        len(payload),
        len(payload),
        alignment,
    )
    if needed:
        dynamic_size = 16 * dynamic_entries
        struct.pack_into(
            "<IIQQQQQQ",
            payload,
            120,
            2,
            4,
            dynamic_offset,
            dynamic_offset,
            dynamic_offset,
            dynamic_size,
            dynamic_size,
            8,
        )
        entries = [(5, string_offset), (10, len(string_table))]
        for name in needed:
            entries.append((1, string_offsets[name]))
        entries.append((0, 0))
        for index, (tag, value) in enumerate(entries):
            struct.pack_into("<qQ", payload, dynamic_offset + index * 16, tag, value)
        payload[string_offset:] = string_table
    if edit is not None:
        struct.pack_into(edit[0], payload, edit[1], edit[2])
    return with_imports(bytes(payload), True, imports) if imports is not None else bytes(payload)


def synthetic_arm_elf(
    alignment: int = 4096,
    needed: tuple[str, ...] = (),
    edit: tuple[str, int, int] | None = None,
    flags: int = 0x05000200,
    imports: tuple[str, ...] | None = None,
) -> bytes:
    """An Arm ET_DYN of the softfp EABI; `edit` is a (struct format, offset,
    value) overwrite and `imports` adds a dynamic symbol table."""
    program_headers = 2 if needed else 1
    dynamic_entries = 3 + len(needed) if needed else 0
    dynamic_offset = 52 + 32 * program_headers
    string_table = bytearray(b"\0")
    string_offsets: dict[str, int] = {}
    for item in needed:
        string_offsets[item] = len(string_table)
        string_table.extend(item.encode())
        string_table.append(0)
    payload_size = dynamic_offset + 8 * dynamic_entries
    string_offset = payload_size
    if needed:
        payload_size += len(string_table)
    payload = bytearray(payload_size)
    payload[:4] = b"\x7fELF"
    payload[4] = 1
    payload[5] = 1
    payload[6] = 1
    struct.pack_into("<HHI", payload, 16, 3, 40, 1)
    struct.pack_into("<I", payload, 28, 52)
    struct.pack_into("<I", payload, 36, flags)
    struct.pack_into("<HHH", payload, 40, 52, 32, program_headers)
    struct.pack_into(
        "<IIIIIIII", payload, 52, 1, 0, 0, 0, len(payload), len(payload), 5, alignment
    )
    if needed:
        dynamic_size = 8 * dynamic_entries
        struct.pack_into(
            "<IIIIIIII", payload, 84, 2, dynamic_offset, dynamic_offset,
            dynamic_offset, dynamic_size, dynamic_size, 4, 4,
        )
        entries = [(5, string_offset), (10, len(string_table))]
        for name in needed:
            entries.append((1, string_offsets[name]))
        entries.append((0, 0))
        for index, (tag, value) in enumerate(entries):
            struct.pack_into("<iI", payload, dynamic_offset + index * 8, tag, value)
        payload[string_offset:] = string_table
    if edit is not None:
        struct.pack_into(edit[0], payload, edit[1], edit[2])
    return with_imports(bytes(payload), False, imports) if imports is not None else bytes(payload)


def load_tool(path: Path):
    """Import a tool whose hyphenated file name is not a module name."""
    spec = importlib.util.spec_from_file_location(
        path.stem.replace("-", "_"), path
    )
    if spec is None or spec.loader is None:
        fail(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(path.parent))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(str(path.parent))
    return module


def tracked_fstab(android_root: Path) -> str:
    """The UML fstab the product installs, which the verifiers compare against."""
    return (android_root / "overlay/config/fstab.uml").read_text()


def erofs_system_fstab(fstab: str) -> str:
    """`fstab` with /system declared erofs, which the UBD contract forbids."""
    changed = fstab.replace("/system ext4", "/system erofs", 1)
    if changed == fstab:
        fail("the tracked fstab has no ext4 /system line to corrupt")
    return changed


def synthetic_apex(filesystem: str = "ext4") -> bytes:
    payload = bytearray(1024 + 0x38 + 2)
    if filesystem == "ext4":
        payload[1024 + 0x38 : 1024 + 0x38 + 2] = b"\x53\xef"
    elif filesystem == "erofs":
        payload[1024 : 1024 + 4] = b"\xe2\xe1\xf5\xe0"
    else:
        fail(f"unsupported synthetic APEX filesystem: {filesystem}")
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as apex:
        apex.writestr("apex_payload.img", payload)
    return output.getvalue()


def synthetic_kernel_elf() -> bytearray:
    """Return a 4 KiB AArch64 ET_DYN kernel: R+X and R+W loads and a GNU stack."""
    payload = bytearray(64 + 56 * 3)
    payload[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HHI", payload, 16, 3, 183, 1)
    struct.pack_into("<Q", payload, 32, 64)
    struct.pack_into("<HHH", payload, 52, 64, 56, 3)
    for index, (kind, flags, address, size, alignment) in enumerate(
        (
            (1, 5, 0, len(payload), 0x1000),
            (1, 6, 0x10000, len(payload), 0x1000),
            (0x6474E551, 6, 0, 0, 16),
        )
    ):
        struct.pack_into(
            "<IIQQQQQQ", payload, 64 + 56 * index,
            kind, flags, 0, address, address, size, size, alignment,
        )
    return payload


def verify_kernel_elf_fixtures(verifier: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="android-kernel-elf-") as temporary:
        kernel = Path(temporary) / "vmlinux"

        def verify(payload: bytes) -> subprocess.CompletedProcess:
            kernel.write_bytes(payload)
            return run_tool(verifier, kernel, stderr=subprocess.PIPE)

        accepted = verify(synthetic_kernel_elf())
        if accepted.returncode != 0:
            fail(f"kernel ELF verifier rejected a valid kernel: {accepted.stderr}")
        load, data, stack = 64, 120, 176
        for layout, offset, value, diagnostic in (
            ("<B", 4, 1, "little-endian ELF64 image"),
            ("<B", 5, 2, "little-endian ELF64 image"),
            ("<B", 6, 0, "little-endian ELF64 image"),
            ("<H", 16, 2, "ET_DYN"),
            ("<H", 18, 62, "AArch64"),
            ("<H", 54, 32, "program-header"),
            ("<H", 56, 1000, "program-header"),
            ("<Q", load + 40, 1, "program header 0 has file size above memory size"),
            ("<Q", data + 8, 0x40000, "segment 1 is outside the file"),
            ("<I", stack + 4, 7, "GNU stack is executable"),
            ("<I", data + 4, 7, "load segment 1 is writable and executable"),
            ("<Q", load + 48, 0x800, "load segment 0 has invalid alignment 0x800"),
            ("<Q", load + 48, 0x6000, "load segment 0 has invalid alignment 0x6000"),
            ("<Q", data + 16, 0x10010, "alignment congruence"),
            ("<I", data, 0, "separated executable and writable"),
            ("<I", data + 4, 4, "separated executable and writable"),
        ):
            payload = synthetic_kernel_elf()
            struct.pack_into(layout, payload, offset, value)
            rejected = verify(payload)
            if rejected.returncode == 0 or (
                "Android kernel ELF verifier: " not in rejected.stderr
                or diagnostic not in rejected.stderr
            ):
                fail(
                    f"kernel ELF verifier did not reject with {diagnostic!r}: "
                    f"{rejected.stderr}"
                )


def adjust_archive(path: Path, entries: dict[str, bytes | None]) -> None:
    """Rewrite a fixture archive: drop the entries mapped to None and add or
    replace the others."""
    with zipfile.ZipFile(path) as source:
        kept = [
            (info.filename, source.read(info))
            for info in source.infolist()
            if info.filename not in entries
        ]
    with zipfile.ZipFile(path, "w") as archive:
        for name, payload in kept:
            archive.writestr(name, payload)
        for name, payload in entries.items():
            if payload is not None:
                archive.writestr(name, payload)


def verify_target_files_fixtures(verifier: Path) -> None:
    sys.path.insert(0, str(verifier.parent))
    from release_common import BUILD_VARIANTS, configured_build_variant

    tool = load_tool(verifier)
    preopt_apks = sorted(tool.REQUIRED_PREOPT_APKS)
    plain_apks = sorted(
        entry for entry in tool.REQUIRED_ENTRIES
        if entry.endswith(".apk") and entry not in tool.REQUIRED_PREOPT_APKS
    )
    arm_preopt_apks = sorted(tool.REQUIRED_ARM_PREOPT_APKS)
    permission_files = [
        entry for entry in tool.REQUIRED_ENTRIES
        if entry.startswith("PRODUCT/etc/permissions/")
    ]
    if len(permission_files) != 1:
        fail(f"expected one required PRODUCT permission file: {permission_files}")
    fdroid_permissions = permission_files[0]
    uml_fstab = tracked_fstab(verifier.parents[1])
    tracked_variant = configured_build_variant(verifier.parents[1])
    other_variant = next(name for name in BUILD_VARIANTS if name != tracked_variant)
    with tempfile.TemporaryDirectory(prefix="android-target-files-") as temporary:
        root = Path(temporary)
        (root / "kernel").mkdir()
        kernel = root / "kernel/vmlinux"
        kernel.write_bytes(synthetic_aarch64_elf())
        kernel_config = root / "kernel/config"
        kernel_config.write_text(
            "CONFIG_PAGE_SIZE_4KB=y\n# CONFIG_MODULES is not set\n"
        )
        kernel_release = root / "kernel/kernel.release"
        kernel_release.write_text("6.18.21-uml-fixture\n")
        kernel_options = [
            "--kernel-elf",
            str(kernel),
            "--kernel-config",
            str(kernel_config),
            "--kernel-release-file",
            str(kernel_release),
        ]

        def write_fixture(path: Path, *, module: bool = False,
                          module_loader: bool = False, dlkm_image: bool = False,
                          dynamic_image: bool = False, ota_tool: bool = False,
                          virtual_ab: bool = False,
                          wrong_fstab: bool = False, stock_fstab: bool = False,
                          wrong_filesystem: bool = False,
                          wrong_userdata_inodes: bool = False,
                          missing_userdata_quota: bool = False,
                          wrong_apex_filesystem: bool = False,
                          seriallogging: bool = False,
                          boot_control: bool = False,
                          context_hub: bool = False,
                          unsupported_hal: bool = False,
                          unsupported_feature: bool = False,
                          missing_microphone_feature: bool = False,
                          missing_mic_policy: bool = False,
                          extra_input_device: bool = False,
                          wrong_kernel_config: bool = False,
                          wrong_kernel_release: bool = False,
                          missing_memfd_policy: bool = False,
                          missing_spl_policy: bool = False,
                          blocker_variant: str | None = None,
                          missing_blocker_binary: bool = False,
                          missing_adb_prop_context: bool = False,
                          missing_adb_prop_policy: bool = False,
                          missing_virtio_console: bool = False,
                          unsupported_hvc: bool = False,
                          missing_mesa_gles: bool = False,
                          bad_mesa_gles_elf: bool = False,
                          missing_gallium_dependency: bool = False,
                          retired_egl_library: bool = False,
                          retired_dri_driver: bool = False,
                          vendor_hal_property: str | None = None,
                          system_other: bool = False,
                          missing_sdk_apex: bool = False,
                          removed_payload: bool = False,
                          missing_required_preopt: bool = False,
                          half_preopt_pair: bool = False,
                          wrong_audio_defaults: bool = False,
                          wrong_product_model: bool = False,
                          wide_fdroid_permissions: bool = False,
                          mesa_gles_alignment: int = 16384,
                          elf_edit: tuple[str, int, int] | None = None,
                          abilist32: str = "armeabi-v7a",
                          vendor_abilist32: str = "armeabi-v7a",
                          zygote: str = "zygote64_32",
                          dex2oat64: bool = True,
                          arm_alignment: int = 4096,
                          arm_flags: int = 0x05000200,
                          arm_edit: tuple[str, int, int] | None = None,
                          missing_arm_gallium_dependency: bool = False,
                          entries: dict[str, bytes | None] | None = None,
                          init_variant: str | None = None,
                          extra_entry: str | None = None,
                          build_variant: str = tracked_variant,
                          alignment: int = 16384) -> None:
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("BOOT/kernel", kernel.read_bytes())
                archive.writestr("INIT_BOOT/RAMDISK/init", b"init")
                archive.writestr("META/init_boot_filesystem_config.txt", "")
                archive.writestr(
                    "META/kernel_configs.txt",
                    "CONFIG_PAGE_SIZE_4KB=n\n"
                    if wrong_kernel_config
                    else kernel_config.read_bytes(),
                )
                archive.writestr(
                    "META/kernel_version.txt",
                    "6.18.21-wrong\n"
                    if wrong_kernel_release
                    else kernel_release.read_bytes(),
                )
                archive.writestr(
                    "META/misc_info.txt",
                    "system_fs_type=erofs\n"
                    if wrong_filesystem
                    else (
                        "system_fs_type=ext4\n"
                        "system_ext_fs_type=ext4\n"
                        "product_fs_type=ext4\n"
                        "vendor_fs_type=ext4\n"
                        "odm_fs_type=ext4\n"
                        + (
                            "userdata_extfs_inode_count=32768\n"
                            if wrong_userdata_inodes
                            else "userdata_extfs_inode_count=8192\n"
                        )
                    ),
                )
                archive.writestr("META/ramdisk_node_list", "")
                for image in (
                    "init_boot.img",
                    "odm.img",
                    "product.img",
                    "recovery.img",
                    "system.img",
                    "system_ext.img",
                    "userdata.img",
                    "vendor.img",
                ):
                    archive.writestr(f"IMAGES/{image}", b"image")
                archive.writestr("ROOT/system_dlkm/", b"")
                archive.writestr("SYSTEM/system_dlkm/etc/build.prop", "")
                virtual_ab_property = "ro.virtual_ab.enabled=true\n" if virtual_ab else ""

                def abi_properties(partition: str, thirty_two: str) -> str:
                    abilist = "arm64-v8a" + ("," + thirty_two if thirty_two else "")
                    return (
                        f"ro.{partition}.product.cpu.abilist={abilist}\n"
                        f"ro.{partition}.product.cpu.abilist32={thirty_two}\n"
                        f"ro.{partition}.product.cpu.abilist64=arm64-v8a\n"
                    )

                archive.writestr(
                    "SYSTEM/build.prop",
                    "ro.product.cpu.abi=arm64-v8a\n"
                    + abi_properties("system", abilist32)
                    + "".join(
                        f"{name}={value}\n"
                        for name, value in BUILD_VARIANTS[build_variant].items()
                    )
                    + f"{virtual_ab_property}",
                )
                archive.writestr(
                    "PRODUCT/etc/build.prop",
                    "aaudio.mmap_policy=1\n"
                    "aaudio.mmap_exclusive_policy=1\n"
                    "ro.config.vc_call_vol_steps=15\n"
                    "ro.config.vc_call_vol_default=15\n"
                    f"ro.config.media_vol_default="
                    f"{'5' if wrong_audio_defaults else '15'}\n"
                    "ro.config.alarm_vol_default=7\n"
                    "ro.config.system_vol_default=7\n"
                    "ro.config.assistant_vol_default=15\n"
                    "persist.sys.usb.config=adb\n"
                    f"ro.product.product.model="
                    f"{'Android 17 on Darwin UML' if wrong_product_model else 'Darwin UML'}\n",
                )
                archive.writestr(
                    "SYSTEM/bin/android-fixture",
                    synthetic_aarch64_elf(alignment, edit=elf_edit),
                )
                for tool in ("e2fsck", "resize2fs", "sgdisk"):
                    archive.writestr(
                        f"SYSTEM/bin/{tool}",
                        synthetic_aarch64_elf(alignment),
                    )
                # The 32-bit runtime: the second zygote's process, the
                # bootstrap linker, the BoringSSL self test and the ART boot
                # image of the arm ISA.
                archive.writestr(
                    "SYSTEM/bin/app_process32",
                    synthetic_arm_elf(arm_alignment, edit=arm_edit, flags=arm_flags),
                )
                for entry in (
                    "SYSTEM/bin/bootstrap/linker",
                    "SYSTEM/bin/boringssl_self_test32",
                    "SYSTEM/framework/arm/boot.oat",
                    "SYSTEM/framework/arm/boot-framework.oat",
                ):
                    archive.writestr(entry, synthetic_arm_elf())
                archive.writestr(
                    "SYSTEM/etc/init/hw/init.zygote64_32.rc", "service zygote_secondary\n"
                )
                archive.writestr(
                    "SYSTEM/apex/com.android.fixture.apex",
                    synthetic_apex(
                        "erofs" if wrong_apex_filesystem else "ext4"
                    ),
                )
                required_sdk_apexes = (
                    "com.android.bt",
                    "com.android.devicelock",
                    "com.android.nfcservices",
                    "com.android.rkpd",
                    "com.android.telephonycore",
                    "com.android.tethering",
                    "com.android.uwb",
                    "com.android.virt",
                    "com.android.wifi",
                )
                for index, apex in enumerate(required_sdk_apexes):
                    if missing_sdk_apex and index == 0:
                        continue
                    archive.writestr(
                        f"SYSTEM/apex/{apex}.apex", synthetic_apex("ext4")
                    )
                fstab = erofs_system_fstab(uml_fstab) if wrong_fstab else uml_fstab
                if missing_userdata_quota:
                    fstab = fstab.replace(",quota", "")
                archive.writestr("VENDOR/etc/fstab.uml", fstab)
                archive.writestr(
                    "VENDOR_BOOT/RAMDISK/first_stage_ramdisk/fstab.uml",
                    fstab,
                )
                graphics = "".join(
                    f"    setprop ro.hardware.{name} ${{ro.boot.hardware.{name}}}\n"
                    for name in ("egl", "gralloc", "vulkan")
                    if not (init_variant == "missing-hal-copy" and name == "vulkan")
                )
                if init_variant == "angle-copy":
                    graphics += (
                        "    setprop debug.angle.feature_overrides_enabled "
                        "${ro.boot.hardware.angle_feature_overrides_enabled}\n"
                    )
                density = (
                    "on early-init\n"
                    if init_variant == "ungated-density"
                    else "on early-init && property:ro.boot.lcd_density=*\n"
                ) + "    setprop ro.sf.lcd_density ${ro.boot.lcd_density}\n"
                # The restore of normal RCU grace periods, which belongs to
                # boot completion, follows the boot action's commands.
                rcu_restore = "    write /sys/kernel/rcu_expedited 0\n"
                if init_variant == "missing-rcu-restore":
                    rcu_restore = ""
                elif init_variant != "rcu-restore-at-boot":
                    rcu_restore = "on property:sys.boot_completed=1\n" + rcu_restore
                # The wakelock that keeps the guest awake while an adb host is connected.
                blocker_rules = ""
                if blocker_variant != "missing-service":
                    blocker_rules += (
                        "service suspend_blocker /vendor/bin/suspend_blocker\n"
                        "    user root\n    group system\n"
                        + ("    class early_hal\n" if blocker_variant == "class-started"
                           else "    disabled\n")
                    )
                blocker_rules += (
                    "on property:vendor.kernarium.adb.host_connected=1\n"
                    "    start suspend_blocker\n"
                )
                if blocker_variant != "no-stop":
                    blocker_rules += (
                        "on property:vendor.kernarium.adb.host_connected=0\n"
                        "    stop suspend_blocker\n"
                    )
                if blocker_variant == "started-at-boot":
                    blocker_rules += "on boot\n    start suspend_blocker\n"
                archive.writestr(
                    "VENDOR/etc/init/init.kernarium.rc",
                    "on early-init\n" + graphics + density +
                    "on fs\n    mount_all --early\n"
                    "on late-fs\n"
                    "    exec -- /system/bin/resize2fs "
                    "/dev/block/by-name/userdata\n"
                    "    mount_all --late\n"
                    "on boot\n"
                    "    setprop ro.hardware.audio.primary goldfish\n"
                    "    setprop service.adb.tcp.port 5555\n"
                    "    start adbd\n" + rcu_restore + blocker_rules,
                )
                if extra_entry is not None:
                    archive.writestr(extra_entry, "on early-init\n")
                archive.writestr(
                    "VENDOR/etc/audio_effects_config.xml",
                    '<audio_effects_conf version="2.0"/>\n',
                )
                archive.writestr(
                    "VENDOR/etc/audio_policy_configuration.xml",
                    '<?xml version="1.0"?><audioPolicyConfiguration '
                    'xmlns:xi="http://www.w3.org/2001/XInclude">'
                    '<xi:include href="primary_audio_policy_configuration.xml"/>'
                    '<xi:include href="r_submix_audio_policy_configuration.xml"/>'
                    '<xi:include href="audio_policy_volumes.xml"/>'
                    '<xi:include href="default_volume_tables.xml"/>'
                    "</audioPolicyConfiguration>",
                )
                archive.writestr(
                    "VENDOR/etc/primary_audio_policy_configuration.xml",
                    '<module name="primary" halVersion="3.0">'
                    '<attachedDevices><item>Speaker</item>'
                    + ("" if missing_mic_policy else "<item>Built-In Mic</item>")
                    + '</attachedDevices>'
                    '<defaultOutputDevice>Speaker</defaultOutputDevice>'
                    '<mixPorts><mixPort name="primary output" role="source" '
                    'flags="AUDIO_OUTPUT_FLAG_PRIMARY"><profile name="" '
                    'format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="48000" '
                    'channelMasks="AUDIO_CHANNEL_OUT_STEREO"/></mixPort>'
                    + (
                        ""
                        if missing_mic_policy
                        else '<mixPort name="primary input" role="sink"><profile '
                        'name="" format="AUDIO_FORMAT_PCM_16_BIT" '
                        'samplingRates="48000" channelMasks="AUDIO_CHANNEL_IN_STEREO '
                        'AUDIO_CHANNEL_IN_MONO"/></mixPort>'
                    )
                    + '</mixPorts>'
                    '<devicePorts><devicePort tagName="Speaker" '
                    'type="AUDIO_DEVICE_OUT_SPEAKER" role="sink"/>'
                    + (
                        ""
                        if missing_mic_policy
                        else '<devicePort tagName="Built-In Mic" '
                        'type="AUDIO_DEVICE_IN_BUILTIN_MIC" address="bottom" '
                        'role="source"/>'
                    )
                    + (
                        '<devicePort tagName="Back Mic" '
                        'type="AUDIO_DEVICE_IN_BACK_MIC" address="back" '
                        'role="source"/>'
                        if extra_input_device
                        else ""
                    )
                    + '</devicePorts>'
                    '<routes><route type="mix" sink="Speaker" '
                    'sources="primary output"/>'
                    + (
                        ""
                        if missing_mic_policy
                        else '<route type="mix" sink="primary input" '
                        'sources="Built-In Mic"/>'
                    )
                    + '</routes></module>',
                )
                archive.writestr(
                    "VENDOR/etc/audio_policy_volumes.xml",
                    "<volumes/>\n",
                )
                archive.writestr(
                    "VENDOR/etc/default_volume_tables.xml",
                    "<volumes/>\n",
                )
                archive.writestr(
                    "VENDOR/etc/r_submix_audio_policy_configuration.xml",
                    '<module name="r_submix" halVersion="3.0"/>\n',
                )
                archive.writestr(
                    "VENDOR/bin/hw/android.hardware.health-service.example",
                    b"health",
                )
                if not missing_blocker_binary:
                    archive.writestr("VENDOR/bin/suspend_blocker", b"blocker")
                archive.writestr(
                    "VENDOR/etc/selinux/vendor_property_contexts",
                    "" if missing_adb_prop_context else
                    "vendor.kernarium.adb.host_connected    "
                    "u:object_r:vendor_kernarium_adb_prop:s0 exact bool\n",
                )
                archive.writestr(
                    "VENDOR/etc/selinux/vendor_sepolicy.cil",
                    "(policycap open_perms)\n"
                    if missing_memfd_policy
                    else "(policycap memfd_class)\n"
                    + (
                        ""
                        if missing_spl_policy
                        else "(allow appdomain vendor_security_patch_level_prop_202604 "
                        "(file (read getattr map open)))\n"
                    )
                    + (
                        ""
                        if missing_adb_prop_policy
                        else "(allow adbd_202604 vendor_kernarium_adb_prop "
                        "(property_service (set)))\n"
                    ),
                )
                ueventd = "/dev/ttyS0 0660 root shell\n"
                if not missing_virtio_console:
                    ueventd += "/dev/hvc0 0660 root shell\n"
                if unsupported_hvc:
                    ueventd += "/dev/hvc1 0660 root shell\n"
                archive.writestr("VENDOR/etc/ueventd.rc", ueventd)
                for apk in preopt_apks:
                    archive.writestr(apk, b"apk")
                    apk_path = Path(apk)
                    artifact = (
                        apk_path.parent / "oat" / "arm64" / apk_path.stem
                    )
                    if not (
                        missing_required_preopt
                        and apk.endswith("/CredentialManager.apk")
                    ):
                        archive.writestr(f"{artifact}.odex", b"odex")
                        if not (
                            half_preopt_pair and apk.endswith("/Jelly.apk")
                        ):
                            archive.writestr(f"{artifact}.vdex", b"vdex")
                for apk in plain_apks:
                    archive.writestr(apk, b"apk")
                for apk in arm_preopt_apks:
                    apk_path = Path(apk)
                    for isa in ("arm64", "arm"):
                        artifact = apk_path.parent / "oat" / isa / apk_path.stem
                        archive.writestr(f"{artifact}.odex", b"odex")
                        archive.writestr(f"{artifact}.vdex", b"vdex")
                archive.writestr(
                    fdroid_permissions,
                    '<permissions><privapp-permissions '
                    'package="org.fdroid.fdroid.privileged">'
                    '<permission name="android.permission.INSTALL_PACKAGES"/>'
                    '<permission name="android.permission.DELETE_PACKAGES"/>'
                    + (
                        '<permission name="android.permission.WRITE_SECURE_SETTINGS"/>'
                        if wide_fdroid_permissions
                        else ""
                    )
                    + '</privapp-permissions></permissions>',
                )
                advertised_features = ["android.hardware.touchscreen"]
                if not missing_microphone_feature:
                    advertised_features.append("android.hardware.microphone")
                if unsupported_feature:
                    advertised_features.append("android.hardware.camera")
                archive.writestr(
                    "VENDOR/etc/permissions/handheld_core_hardware.xml",
                    "<permissions>"
                    + "".join(f'<feature name="{name}"/>' for name in advertised_features)
                    + "</permissions>",
                )
                archive.writestr(
                    "VENDOR/etc/permissions/bluetooth_excluded_hardware.xml",
                    '<permissions><unavailable-feature '
                    'name="android.hardware.bluetooth"/></permissions>',
                )
                archive.writestr(
                    "RECOVERY/RAMDISK/system/bin/recovery", b"recovery"
                )
                archive.writestr(
                    "RECOVERY/RAMDISK/system/bin/hw/"
                    "android.hardware.health-service.example_recovery",
                    b"health-recovery",
                )
                archive.writestr(
                    "VENDOR/etc/vintf/compatibility_matrix.xml",
                    "<compatibility-matrix/>\n",
                )
                archive.writestr("VENDOR/etc/vintf/manifest.xml", "<manifest/>\n")
                mesa_gles_entries = (
                    "VENDOR/lib64/venus_gallium_dri.so",
                    "VENDOR/lib64/egl/libEGL_kernarium.so",
                    "VENDOR/lib64/egl/libGLESv1_CM_kernarium.so",
                    "VENDOR/lib64/egl/libGLESv2_kernarium.so",
                )
                for index, entry in enumerate(mesa_gles_entries):
                    if missing_mesa_gles and index == 0:
                        continue
                    payload = (
                        b"not an ELF"
                        if bad_mesa_gles_elf and index == 0
                        else synthetic_aarch64_elf(
                            mesa_gles_alignment,
                            ()
                            if missing_gallium_dependency or index == 0
                            else ("venus_gallium_dri.so",),
                            imports=LARGE_FILE_IMPORTS_64,
                        )
                    )
                    archive.writestr(entry, payload)
                for index, entry in enumerate((
                    "VENDOR/lib/venus_gallium_dri.so",
                    "VENDOR/lib/egl/libEGL_kernarium.so",
                    "VENDOR/lib/egl/libGLESv1_CM_kernarium.so",
                    "VENDOR/lib/egl/libGLESv2_kernarium.so",
                )):
                    archive.writestr(
                        entry,
                        synthetic_arm_elf(
                            needed=()
                            if missing_arm_gallium_dependency or index == 0
                            else ("venus_gallium_dri.so",),
                            imports=LARGE_FILE_IMPORTS_32,
                        ),
                    )
                for hal in ("vulkan.virtio", "gralloc.minigbm", "mapper.minigbm"):
                    archive.writestr(
                        f"VENDOR/lib64/hw/{hal}.so",
                        synthetic_aarch64_elf(imports=LARGE_FILE_IMPORTS_64),
                    )
                    archive.writestr(
                        f"VENDOR/lib/hw/{hal}.so", synthetic_arm_elf(imports=LARGE_FILE_IMPORTS_32)
                    )
                if retired_egl_library:
                    archive.writestr(
                        "VENDOR/lib64/egl/libGLES_mesa.so", synthetic_aarch64_elf()
                    )
                if retired_dri_driver:
                    archive.writestr(
                        "VENDOR/lib64/dri/gallium_dri.so", synthetic_aarch64_elf()
                    )
                archive.writestr(
                    "VENDOR/build.prop",
                    "ro.vendor.build.fingerprint=kernarium\n"
                    + abi_properties("vendor", vendor_abilist32)
                    + f"ro.zygote={zygote}\n"
                    "# Removed by post_process_props.py because overridden by "
                    "ro.zygote=zygote64_32\n#ro.zygote?=zygote32\n"
                    + ("dalvik.vm.dex2oat64.enabled=true\n" if dex2oat64 else "")
                    + "ro.bionic.2nd_arch=arm\n"
                    "ro.bionic.2nd_cpu_variant=cortex-a53\n"
                    "dalvik.vm.isa.arm.variant=cortex-a53\n"
                    + (
                        f"ro.hardware.{vendor_hal_property}=selected\n"
                        if vendor_hal_property
                        else ""
                    ),
                )
                archive.writestr("ODM/etc/build.prop", abi_properties("odm", "armeabi-v7a"))
                if module:
                    archive.writestr("VENDOR/lib/modules/forbidden.ko", b"module")
                if module_loader:
                    archive.writestr("VENDOR/bin/dlkm_loader", b"loader")
                if dlkm_image:
                    archive.writestr("SYSTEM_DLKM/etc/build.prop", "")
                if dynamic_image:
                    archive.writestr("IMAGES/super_empty.img", b"dynamic")
                if ota_tool:
                    archive.writestr("OTA/bin/updater", b"updater")
                if stock_fstab:
                    archive.writestr("VENDOR/etc/fstab.cf.ext4.hctr2", b"stock")
                if seriallogging:
                    archive.writestr("VENDOR/etc/init/seriallogging.rc", b"stock")
                if boot_control:
                    archive.writestr(
                        "VENDOR/apex/com.android.hardware.boot.apex", b"apex"
                    )
                if context_hub:
                    archive.writestr(
                        "VENDOR/apex/com.android.hardware.contexthub.apex", b"apex"
                    )
                if unsupported_hal:
                    archive.writestr("VENDOR/apex/com.google.cf.light.apex", b"apex")
                if system_other:
                    archive.writestr(
                        "SYSTEM_OTHER/app/Fixture/Fixture.apk", b"apk"
                    )
                if removed_payload:
                    archive.writestr(
                        "SYSTEM/apex/com.android.hardware.rebootescrow.apex",
                        synthetic_apex("ext4"),
                    )
            if entries:
                adjust_archive(path, entries)

        good = root / "good.zip"
        write_fixture(good)
        run_tool(verifier, "--target-files", good, *kernel_options, check=True)
        # Every supported variant verifies as itself and is rejected as any
        # other: the build-type properties follow the requested variant.
        for variant in BUILD_VARIANTS:
            for requested in BUILD_VARIANTS:
                fixture = root / f"{variant}-as-{requested}.zip"
                write_fixture(fixture, build_variant=variant)
                result = run_tool(
                    verifier, "--target-files", fixture,
                    "--build-variant", requested, *kernel_options,
                )
                if (result.returncode == 0) != (variant == requested):
                    fail(
                        f"a {variant} build verified as {requested}: "
                        f"{result.returncode} {result.stdout}"
                    )

        for label, options in (
            ("module", {"module": True}),
            ("module-loader", {"module_loader": True}),
            ("dlkm-image", {"dlkm_image": True}),
            ("dynamic-image", {"dynamic_image": True}),
            ("ota-tool", {"ota_tool": True}),
            ("virtual-ab", {"virtual_ab": True}),
            ("no-32-bit-abi", {"abilist32": ""}),
            ("no-32-bit-vendor-abi", {"vendor_abilist32": ""}),
            ("single-zygote", {"zygote": "zygote64"}),
            ("no-64-bit-compiler", {"dex2oat64": False}),
            ("missing-32-bit-runtime",
             {"entries": {"SYSTEM/bin/app_process32": None}}),
            ("missing-sgdisk",
             {"entries": {"SYSTEM/bin/sgdisk": None}}),
            ("missing-32-bit-boot-image",
             {"entries": {"SYSTEM/framework/arm/boot-framework.oat": None}}),
            ("missing-32-bit-gallium-driver",
             {"entries": {"VENDOR/lib/venus_gallium_dri.so": None}}),
            ("missing-32-bit-egl",
             {"entries": {"VENDOR/lib/egl/libGLESv2_kernarium.so": None}}),
            ("missing-32-bit-vulkan-hal",
             {"entries": {"VENDOR/lib/hw/vulkan.virtio.so": None}}),
            ("missing-32-bit-gralloc",
             {"entries": {"VENDOR/lib/hw/gralloc.minigbm.so": None}}),
            ("missing-32-bit-mapper",
             {"entries": {"VENDOR/lib/hw/mapper.minigbm.so": None}}),
            ("32-bit-gallium-dependency", {"missing_arm_gallium_dependency": True}),
            ("32-bit-egl-is-aarch64",
             {"entries": {"VENDOR/lib/egl/libEGL_kernarium.so": synthetic_aarch64_elf()}}),
            ("64-bit-egl-is-arm",
             {"entries": {"VENDOR/lib64/egl/libEGL_kernarium.so": synthetic_arm_elf()}}),
            ("missing-webview-32-bit-odex",
             {"entries": {"PRODUCT/app/webview/oat/arm/webview.odex": None}}),
            ("missing-webview-32-bit-vdex",
             {"entries": {"PRODUCT/app/webview/oat/arm/webview.vdex": None}}),
            ("32-bit-odex-without-arm64",
             {"entries": {"PRODUCT/app/Orphan/oat/arm/Orphan.odex": b"odex",
                          "PRODUCT/app/Orphan/oat/arm/Orphan.vdex": b"vdex"}}),
            ("unknown-isa",
             {"entries": {"PRODUCT/app/Jelly/oat/x86_64/Jelly.odex": b"odex"}}),
            ("32-bit-elf-in-lib64",
             {"entries": {"VENDOR/lib64/libfixture.so": synthetic_arm_elf()}}),
            ("64-bit-elf-in-lib",
             {"entries": {"SYSTEM/lib/libfixture.so": synthetic_aarch64_elf()}}),
            ("32-bit-daemon",
             {"entries": {"SYSTEM/bin/mediaserver": synthetic_arm_elf()}}),
            ("32-bit-vendor-daemon",
             {"entries": {"VENDOR/bin/hw/android.hardware.fixture-service": synthetic_arm_elf()}}),
            ("32-bit-alignment", {"arm_alignment": 2048}),
            ("32-bit-hard-float", {"arm_flags": 0x05000400}),
            ("alignment", {"alignment": 4096}),
            ("untracked-variant", {"build_variant": other_variant}),
            ("wrong-fstab", {"wrong_fstab": True}),
            ("wrong-filesystem", {"wrong_filesystem": True}),
            ("wrong-userdata-inodes", {"wrong_userdata_inodes": True}),
            ("missing-userdata-quota", {"missing_userdata_quota": True}),
            ("wrong-apex-filesystem", {"wrong_apex_filesystem": True}),
            ("stock-fstab", {"stock_fstab": True}),
            ("seriallogging", {"seriallogging": True}),
            ("boot-control", {"boot_control": True}),
            ("context-hub", {"context_hub": True}),
            ("unsupported-hal", {"unsupported_hal": True}),
            ("unsupported-feature", {"unsupported_feature": True}),
            ("missing-microphone-feature", {"missing_microphone_feature": True}),
            ("missing-mic-policy", {"missing_mic_policy": True}),
            ("extra-input-device", {"extra_input_device": True}),
            ("kernel-config", {"wrong_kernel_config": True}),
            ("kernel-release", {"wrong_kernel_release": True}),
            ("memfd-policy", {"missing_memfd_policy": True}),
            ("spl-policy", {"missing_spl_policy": True}),
            ("blocker-missing-service", {"blocker_variant": "missing-service"}),
            ("blocker-class-started", {"blocker_variant": "class-started"}),
            ("blocker-started-at-boot", {"blocker_variant": "started-at-boot"}),
            ("blocker-no-stop", {"blocker_variant": "no-stop"}),
            ("blocker-binary", {"missing_blocker_binary": True}),
            ("adb-prop-context", {"missing_adb_prop_context": True}),
            ("adb-prop-policy", {"missing_adb_prop_policy": True}),
            ("missing-virtio-console", {"missing_virtio_console": True}),
            ("unsupported-hvc", {"unsupported_hvc": True}),
            ("missing-mesa-gles", {"missing_mesa_gles": True}),
            ("mesa-gles-not-elf", {"bad_mesa_gles_elf": True}),
            ("gallium-dependency", {"missing_gallium_dependency": True}),
            ("retired-egl-library", {"retired_egl_library": True}),
            ("retired-dri-driver", {"retired_dri_driver": True}),
            ("vendor-egl-property", {"vendor_hal_property": "egl"}),
            ("vendor-vulkan-property", {"vendor_hal_property": "vulkan"}),
            ("vendor-gralloc-property", {"vendor_hal_property": "gralloc"}),
            ("system-other", {"system_other": True}),
            ("missing-sdk-apex", {"missing_sdk_apex": True}),
            ("removed-payload", {"removed_payload": True}),
            ("missing-preopt", {"missing_required_preopt": True}),
            ("half-preopt-pair", {"half_preopt_pair": True}),
            ("audio-defaults", {"wrong_audio_defaults": True}),
            ("product-model", {"wrong_product_model": True}),
            ("mesa-gles-alignment", {"mesa_gles_alignment": 4096}),
            ("missing-hal-copy", {"init_variant": "missing-hal-copy"}),
            ("angle-copy", {"init_variant": "angle-copy"}),
            ("ungated-density", {"init_variant": "ungated-density"}),
            ("missing-rcu-restore", {"init_variant": "missing-rcu-restore"}),
            ("rcu-restore-at-boot", {"init_variant": "rcu-restore-at-boot"}),
            ("cuttlefish-graphics-rc",
             {"extra_entry": "VENDOR/etc/init/init_graphics.vendor.rc"}),
            ("product-init-rc", {"extra_entry": "PRODUCT/etc/init/init.rc"}),
            ("legacy-ueventd", {"extra_entry": "VENDOR/ueventd.rc"}),
            ("curated-out-canvas",
             {"extra_entry": "PRODUCT/app/Canvas/Canvas.apk"}),
            ("curated-out-updater",
             {"extra_entry": "SYSTEM_EXT/priv-app/Updater/Updater.apk"}),
            ("fastbootd", {"extra_entry": "SYSTEM/bin/fastbootd"}),
            ("curated-out-profiles",
             {"extra_entry": "SYSTEM_EXT/app/Profiles/Profiles.apk"}),
            ("fdroid-wide-permissions", {"wide_fdroid_permissions": True}),
        ):
            fixture = root / f"bad-{label}.zip"
            write_fixture(fixture, **options)
            result = run_tool(verifier, "--target-files", fixture, *kernel_options)
            if result.returncode == 0:
                fail(f"target-files verifier accepted the {label} fixture")
            if result.returncode != 1 or "Android target-files verifier: " not in result.stdout:
                fail(f"target-files verifier did not reject the {label} fixture: {result.stdout}")

        # Rejections that must name their cause: kernel module files and
        # metadata in any case, and a target ELF whose header is not an
        # AArch64 little-endian ELF64 with whole program-header entries.
        for label, options, reason in (
            ("module-object-suffix", {"extra_entry": "VENDOR/etc/fixture.ko.xz"}, "kernel module"),
            ("module-metadata", {"extra_entry": "VENDOR/etc/modules.dep"}, "kernel module"),
            ("module-metadata-case", {"extra_entry": "VENDOR/etc/MODULES.Dep"}, "kernel module"),
            ("module-blocklist", {"extra_entry": "VENDOR/etc/modules.blocklist"}, "kernel module"),
            ("elf-class", {"elf_edit": ("<B", 4, 3)}, "ELF32 or ELF64"),
            ("elf-big-endian", {"elf_edit": ("<B", 5, 2)}, "little-endian ELF32 or ELF64"),
            ("elf-machine", {"elf_edit": ("<H", 18, 62)}, "not AArch64"),
            ("elf-program-header-size", {"elf_edit": ("<H", 54, 32)}, "program.header"),
            ("armeabi", {"abilist32": "armeabi-v7a,armeabi"}, "armeabi is forbidden|legacy armeabi"),
            ("no-32-bit-abi-cause", {"abilist32": ""}, "unexpected ABI"),
            ("single-zygote-cause", {"zygote": "zygote64"}, "zygote"),
            ("blocker-missing-service-cause", {"blocker_variant": "missing-service"},
             "does not declare the suspend_blocker service"),
            ("blocker-class-started-cause", {"blocker_variant": "class-started"},
             "starts suspend_blocker with its class"),
            ("blocker-started-at-boot-cause", {"blocker_variant": "started-at-boot"},
             "starts suspend_blocker under 'boot'"),
            ("blocker-no-stop-cause", {"blocker_variant": "no-stop"},
             "does not stop suspend_blocker when the last adb host leaves"),
            ("blocker-binary-cause", {"missing_blocker_binary": True},
             "VENDOR/bin/suspend_blocker"),
            ("adb-prop-context-cause", {"missing_adb_prop_context": True},
             "do not label vendor.kernarium.adb.host_connected"),
            ("adb-prop-policy-cause", {"missing_adb_prop_policy": True},
             "does not let adbd set vendor.kernarium.adb.host_connected"),
            ("missing-userdata-quota-cause", {"missing_userdata_quota": True},
             "userdata entry lacks the fs_mgr quota flag"),
            ("missing-microphone-feature-cause", {"missing_microphone_feature": True},
             "feature is not declared: android.hardware.microphone"),
            ("camera-feature-cause", {"unsupported_feature": True},
             "absent hardware feature is advertised.*android.hardware.camera"),
            ("missing-mic-policy-cause", {"missing_mic_policy": True},
             "ordinary mixer input"),
            ("extra-input-device-cause", {"extra_input_device": True},
             "only its speaker and built-in microphone"),
            ("no-64-bit-compiler-cause", {"dex2oat64": False}, "dalvik.vm.dex2oat64.enabled"),
            ("missing-32-bit-runtime-cause",
             {"entries": {"SYSTEM/bin/app_process32": None}}, "required entries are missing"),
            ("missing-sgdisk-cause",
             {"entries": {"SYSTEM/bin/sgdisk": None}}, "SYSTEM/bin/sgdisk"),
            ("32-bit-gallium-dependency-cause",
             {"missing_arm_gallium_dependency": True}, "does not link the gallium driver"),
            ("32-bit-egl-is-aarch64-cause",
             {"entries": {"VENDOR/lib/egl/libEGL_kernarium.so": synthetic_aarch64_elf()}},
             "64-bit ELF in a 32-bit library directory"),
            ("64-bit-egl-is-arm-cause",
             {"entries": {"VENDOR/lib64/egl/libEGL_kernarium.so": synthetic_arm_elf()}},
             "32-bit ELF outside"),
            ("missing-webview-32-bit-odex-cause",
             {"entries": {"PRODUCT/app/webview/oat/arm/webview.odex": None}},
             "32-bit application dexpreopt artifact is missing"),
            ("32-bit-odex-without-arm64-cause",
             {"entries": {"PRODUCT/app/Orphan/oat/arm/Orphan.odex": b"odex",
                          "PRODUCT/app/Orphan/oat/arm/Orphan.vdex": b"vdex"}},
             "has no ARM64 one"),
            ("unknown-isa-cause",
             {"entries": {"PRODUCT/app/Jelly/oat/x86_64/Jelly.odex": b"odex"}},
             "not for an ISA of this product"),
            ("32-bit-elf-in-lib64-cause",
             {"entries": {"VENDOR/lib64/libfixture.so": synthetic_arm_elf()}},
             "32-bit ELF outside"),
            ("64-bit-elf-in-lib-cause",
             {"entries": {"SYSTEM/lib/libfixture.so": synthetic_aarch64_elf()}},
             "64-bit ELF in a 32-bit library directory"),
            ("32-bit-daemon-cause",
             {"entries": {"SYSTEM/bin/mediaserver": synthetic_arm_elf()}},
             "32-bit ELF outside"),
            ("32-bit-alignment-cause", {"arm_alignment": 2048}, "below 4 KiB"),
            ("32-bit-hard-float-cause", {"arm_flags": 0x05000400}, "hard-float"),
            ("32-bit-eabi-version", {"arm_flags": 0x04000000}, "EABI version 5"),
            ("32-bit-machine", {"arm_edit": ("<H", 18, 62)}, "not Arm"),
            ("32-bit-big-endian", {"arm_edit": ("<B", 5, 2)}, "little-endian ELF32 or ELF64"),
            ("32-bit-program-header-size", {"arm_edit": ("<H", 42, 16)}, "program.header"),
            # The Venus ICD built without -D_FILE_OFFSET_BITS=64 imports the
            # 32-bit-offset mmap and lseek, and fails to map a DRM offset.
            ("32-bit-vulkan-icd-plain-mmap",
             {"entries": {"VENDOR/lib/hw/vulkan.virtio.so": synthetic_arm_elf(
                 imports=("open", "mmap", "lseek64", "munmap"))}},
             r"32-bit VENDOR/lib/hw/vulkan.virtio.so imports mmap instead of mmap64"),
            ("32-bit-gallium-plain-lseek",
             {"entries": {"VENDOR/lib/venus_gallium_dri.so": synthetic_arm_elf(
                 imports=("open", "mmap64", "lseek"))}},
             r"venus_gallium_dri.so imports lseek instead of lseek64"),
            ("32-bit-gralloc-both-plain",
             {"entries": {"VENDOR/lib/hw/gralloc.minigbm.so": synthetic_arm_elf(
                 imports=("mmap", "lseek"))}},
             r"gralloc.minigbm.so imports lseek, mmap instead of lseek64, mmap64"),
            ("32-bit-stack-without-symbols",
             {"entries": {"VENDOR/lib/egl/libEGL_kernarium.so": synthetic_arm_elf(
                 needed=("venus_gallium_dri.so",))}},
             r"no readable section headers"),
        ):
            fixture = root / f"bad-{label}.zip"
            write_fixture(fixture, **options)
            result = run_tool(verifier, "--target-files", fixture, *kernel_options)
            if result.returncode != 1 or not re.search(reason, result.stdout):
                fail(f"target-files verifier did not reject the {label} fixture for {reason!r}: {result.stdout}")


def verify_hal_selections(android_root: Path) -> None:
    # The boot command line is the one writer of each graphics HAL selection,
    # so it must name the modules the target-files verifier expects.
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    command_line = (
        android_root / "support/boot/kernel-command-line.txt"
    ).read_text().split()
    for name, expected in verifier.BOOT_HAL_SELECTIONS.items():
        values = [
            token.partition("=")[2]
            for token in command_line
            if token.partition("=")[0] == f"androidboot.hardware.{name}"
        ]
        if values != [expected]:
            fail(f"boot command line selects {name} HAL {values}, not {expected}")
    metadata = (android_root / "third_party/mesa-venus/metadata.env").read_text()
    module = f"vulkan.{verifier.BOOT_HAL_SELECTIONS['vulkan']}"
    if f"MESA_VENUS_HAL_MODULE='{module}'" not in metadata.splitlines():
        fail(f"the Vulkan HAL selection does not name the Venus module {module}")

    # A setprop that expands a missing ro.boot property fails at every boot.
    # The product init script may therefore expand only properties the command
    # line sets, or a host-supplied one inside an action gated on its presence.
    provided = {token.partition("=")[0] for token in command_line}
    script = (android_root / "overlay/config/init.vendor.rc").read_text()
    for trigger, commands in verifier.init_actions(script).items():
        gated = set(re.findall(r"property:ro\.boot\.([\w.]+)=\*", trigger))
        for command in commands:
            for name in re.findall(r"\$\{ro\.boot\.([\w.]+)\}", command):
                if name not in gated and f"androidboot.{name}" not in provided:
                    fail(
                        f"init action '{trigger}' expands ro.boot.{name}, "
                        "which the boot command line does not set"
                    )

    # Boot runs with expedited RCU grace periods, and the product init script
    # restores normal ones once boot completes.
    if command_line.count("rcupdate.rcu_expedited=1") != 1:
        fail("boot command line does not expedite RCU grace periods once")
    actions = verifier.init_actions(script)
    if verifier.UML_INIT_RCU_RESTORE not in actions.get(verifier.UML_INIT_RCU_TRIGGER, []):
        fail("product init script does not restore normal RCU grace periods after boot")


def newc_entry(name: str, mode: int, payload: bytes, inode: int) -> bytes:
    fields = (
        inode,
        mode,
        0,
        0,
        1,
        0,
        len(payload),
        0,
        0,
        0,
        0,
        len(name.encode()) + 1,
        0,
    )
    result = bytearray(b"070701" + b"".join(f"{value:08x}".encode() for value in fields))
    result.extend(name.encode() + b"\0")
    result.extend(b"\0" * (-len(result) % 4))
    result.extend(payload)
    result.extend(b"\0" * (-len(result) % 4))
    return bytes(result)


def synthetic_initramfs(fstab: str, *, wrong_fstab: bool = False,
                        module_loader: bool = False,
                        duplicate_file: bool = False,
                        module_file: str | None = None) -> bytes:
    if wrong_fstab:
        fstab = erofs_system_fstab(fstab)
    entries = [
        newc_entry("dev", 0o40755, b"", 1),
        newc_entry("dev", 0o40755, b"", 2),
        newc_entry("init", 0o100750, b"init", 3),
        newc_entry(
            "first_stage_ramdisk/system/etc/fstab.uml",
            0o100644,
            fstab.encode(),
            4,
        ),
    ]
    if module_loader:
        entries.append(newc_entry("system/bin/modprobe", 0o120777, b"toybox", 5))
    if duplicate_file:
        entries.append(newc_entry("init", 0o100750, b"init", 6))
    if module_file is not None:
        entries.append(newc_entry(f"lib/modules/{module_file}", 0o100644, b"module", 7))
    entries.append(newc_entry("TRAILER!!!", 0, b"", len(entries) + 1))
    return b"".join(entries)


def verify_initramfs_fixtures(verifier: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="android-initramfs-") as temporary:
        root = Path(temporary)
        good = root / "ramdisk.img"
        fstab = tracked_fstab(verifier.parents[1])
        good.write_bytes(synthetic_initramfs(fstab))
        run_tool(verifier, good, check=True)
        for label, options in (
            ("wrong-fstab", {"wrong_fstab": True}),
            ("module-loader", {"module_loader": True}),
            ("duplicate-file", {"duplicate_file": True}),
        ):
            fixture = root / f"bad-{label}.img"
            fixture.write_bytes(synthetic_initramfs(fstab, **options))
            result = run_tool(verifier, fixture)
            if result.returncode == 0:
                fail(f"initramfs verifier accepted the {label} fixture")
        # Kernel module files and metadata, in any case, name their cause.
        for module_file in (
            "fixture.ko", "fixture.ko.xz", "modules.dep", "MODULES.Dep",
            "modules.devname", "modules.symbols", "modules.blocklist",
        ):
            fixture = root / "bad-module.img"
            fixture.write_bytes(synthetic_initramfs(fstab, module_file=module_file))
            result = run_tool(verifier, fixture)
            if result.returncode == 0 or "loadable-module payload" not in result.stdout:
                fail(f"initramfs verifier did not reject {module_file} as a module: {result.stdout}")


def synthetic_boot_image(
    ramdisk: bytes = b"fixture-initramfs",
    *,
    kernel_size: int = 0,
    header_version: int = 4,
) -> bytes:
    header = bytearray(4096)
    header[:8] = b"ANDROID!"
    struct.pack_into("<IIII", header, 8, kernel_size, len(ramdisk), 0, 1584)
    struct.pack_into("<I", header, 40, header_version)
    struct.pack_into("<I", header, 1580, 0)
    return bytes(header) + ramdisk


def verify_boot_image_fixtures(verifier: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="android-boot-image-") as temporary:
        root = Path(temporary)
        ramdisk = root / "ramdisk.img"
        ramdisk.write_bytes(b"fixture-initramfs")
        good = root / "init_boot.img"
        good.write_bytes(synthetic_boot_image(ramdisk.read_bytes()))
        run_tool(verifier, "verify", "--image", good, "--ramdisk", ramdisk, check=True)
        selected_range = run_tool(
            verifier, "range", "--image", good,
            check=True, stdout=subprocess.PIPE, text=True,
        )
        parsed = json.loads(selected_range.stdout)
        if parsed["ramdisk_offset"] != 4096 or parsed["ramdisk_size"] != ramdisk.stat().st_size:
            fail("boot-image verifier reported the wrong ramdisk range")

        for label, payload in (
            ("embedded-kernel", synthetic_boot_image(kernel_size=1)),
            ("old-header", synthetic_boot_image(header_version=3)),
        ):
            fixture = root / f"bad-{label}.img"
            fixture.write_bytes(payload)
            result = run_tool(verifier, "verify", "--image", fixture)
            if result.returncode == 0:
                fail(f"boot-image verifier accepted {label}")


def lz4_literal_block(payload: bytes) -> bytes:
    """One LZ4 block that carries `payload` as a single literal run."""
    length = len(payload)
    token = min(length, 15) << 4
    extension = bytearray()
    if length >= 15:
        remaining = length - 15
        while remaining >= 255:
            extension.append(255)
            remaining -= 255
        extension.append(remaining)
    return bytes([token]) + bytes(extension) + payload


def lz4_legacy_frame(payload: bytes, block_size: int = 1 << 20) -> bytes:
    """An LZ4 legacy frame, the format of Android's compressed recovery ramdisk."""
    frame = bytearray(b"\x02\x21\x4c\x18")
    for offset in range(0, len(payload), block_size):
        block = lz4_literal_block(payload[offset:offset + block_size])
        frame += struct.pack("<I", len(block)) + block
    return bytes(frame)


def synthetic_lineage_recovery_ramdisk(
    fstab: bytes, *, alignment: int = 16384, omit_dependency: bool = False,
    interface: bytes = b"eth0\0", compression: str = "lz4",
    module: str | None = None, no_memfd: bool = False,
    elf_edit: tuple[str, int, int] | None = None,
) -> bytes:
    entries = [
        newc_entry(
            "init", 0o100750, synthetic_aarch64_elf(alignment, edit=elf_edit), 1
        ),
        newc_entry(
            "prop.default",
            0o100644,
            b"ro.product.system.device=darwin\n"
            b"persist.adb.tcp.port=5555\n"
            + (b"" if no_memfd else b"sys.use_memfd=true\n"),
            2,
        ),
        newc_entry(
            "system/bin/adbd", 0o100755,
            synthetic_aarch64_elf(alignment, needed=("libfixture.so",)), 3,
        ),
        newc_entry("system/bin/e2fsck", 0o100755, b"tool", 4),
        newc_entry("system/bin/mke2fs", 0o100755, b"tool", 5),
        newc_entry(
            "system/bin/recovery", 0o100755,
            synthetic_aarch64_elf(alignment, needed=("librecovery_ui.so",)), 6,
        ),
        newc_entry("system/bin/resize2fs", 0o100755, b"tool", 7),
        newc_entry("system/bin/tune2fs", 0o100755, b"tool", 8),
        newc_entry("system/etc/recovery.fstab", 0o100644, fstab, 9),
        newc_entry(
            "system/lib64/librecovery_ui.so", 0o100644,
            synthetic_aarch64_elf(alignment), 10,
        ),
        newc_entry(
            "system/lib64/librecovery_ui_ext.so", 0o100644,
            synthetic_aarch64_elf(alignment)
            + interface + b"10.0.2.15\0" + b"255.255.255.0\0", 11,
        ),
        *([] if omit_dependency else [
            newc_entry(
                "system/lib64/libfixture.so", 0o100644,
                synthetic_aarch64_elf(alignment), 12,
            )
        ]),
        *([newc_entry(f"lib/modules/{module}", 0o100644, b"module", 13)] if module else []),
    ]
    entries.append(newc_entry("TRAILER!!!", 0, b"", len(entries) + 1))
    archive = b"".join(entries)
    if compression == "gzip":
        return gzip.compress(archive, mtime=0)
    if compression == "lz4":
        return lz4_legacy_frame(archive)
    return archive


def verify_lineage_recovery_fixtures(verifier: Path, android_root: Path) -> None:
    fstab = (android_root / "overlay/config/recovery.fstab").read_bytes()
    module = load_tool(verifier)
    # A match whose offset is shorter than its length repeats the pattern.
    if module.lz4_block(b"\x35abc\x03\x00\x20de", 1 << 20) != b"abcabcabcabcde":
        fail("LZ4 block decoder mis-decoded an overlapping match")
    for label, block in (
        ("offset past the output", b"\x15a\x02\x00\x10b"),
        ("zero offset", b"\x15a\x00\x00\x10b"),
        ("literal run past the block", b"\x50ab"),
        ("truncated length", b"\xf0"),
    ):
        try:
            module.lz4_block(block, 1 << 20)
        except module.VerificationError:
            continue
        fail(f"LZ4 block decoder accepted {label}")

    def run(image: Path) -> subprocess.CompletedProcess[str]:
        return run_tool(
            verifier, "--android-root", android_root, "--image", image
        )

    with tempfile.TemporaryDirectory(prefix="android-lineage-recovery-") as temporary:
        root = Path(temporary)
        for compression in ("lz4", "gzip"):
            good = root / f"recovery-{compression}.img"
            good.write_bytes(
                synthetic_boot_image(
                    synthetic_lineage_recovery_ramdisk(fstab, compression=compression)
                )
            )
            result = run(good)
            if result.returncode != 0:
                fail(f"Lineage recovery verifier rejected a {compression} fixture: {result.stdout}")
        for label, ramdisk in (
            ("a 4 KiB-only ELF", synthetic_lineage_recovery_ramdisk(fstab, alignment=4096)),
            (
                "a missing ELF dependency",
                synthetic_lineage_recovery_ramdisk(fstab, omit_dependency=True),
            ),
            (
                "the Cuttlefish interface",
                synthetic_lineage_recovery_ramdisk(fstab, interface=b"eth1\0"),
            ),
            (
                "a loadable module",
                synthetic_lineage_recovery_ramdisk(fstab, module="fixture.ko"),
            ),
            (
                "properties without sys.use_memfd",
                synthetic_lineage_recovery_ramdisk(fstab, no_memfd=True),
            ),
            (
                "a changed fstab",
                synthetic_lineage_recovery_ramdisk(fstab + b"# changed\n"),
            ),
            (
                "an uncompressed ramdisk",
                synthetic_lineage_recovery_ramdisk(fstab, compression="none"),
            ),
        ):
            bad = root / "recovery-bad.img"
            bad.write_bytes(synthetic_boot_image(ramdisk))
            if run(bad).returncode == 0:
                fail(f"Lineage recovery verifier accepted {label}")
        # Rejections that must name their cause: kernel module files and
        # metadata in any case, and an ELF whose header is not an AArch64
        # little-endian ELF64 with whole program-header entries.
        for label, options, reason in (
            ("a module object suffix", {"module": "fixture.ko.xz"}, "loadable-module payload"),
            ("module metadata", {"module": "modules.dep"}, "loadable-module payload"),
            ("module metadata in capitals", {"module": "MODULES.Dep"}, "loadable-module payload"),
            ("a module blocklist", {"module": "modules.blocklist"}, "loadable-module payload"),
            ("a module device-name index", {"module": "modules.devname"}, "loadable-module payload"),
            ("a module symbol index", {"module": "modules.symbols.bin"}, "loadable-module payload"),
            (
                "a built-in alias index",
                {"module": "modules.builtin.alias.bin"},
                "loadable-module payload",
            ),
            ("a 32-bit ELF", {"elf_edit": ("<B", 4, 1)}, "little-endian ELF64"),
            ("a big-endian ELF", {"elf_edit": ("<B", 5, 2)}, "little-endian ELF64"),
            ("a foreign machine", {"elf_edit": ("<H", 18, 62)}, "not AArch64"),
            ("a short program-header entry", {"elf_edit": ("<H", 54, 32)}, "program.header"),
        ):
            bad = root / "recovery-bad.img"
            bad.write_bytes(
                synthetic_boot_image(synthetic_lineage_recovery_ramdisk(fstab, **options))
            )
            result = run(bad)
            if result.returncode == 0 or not re.search(reason, result.stdout):
                fail(f"Lineage recovery verifier did not reject {label} for {reason!r}: {result.stdout}")


def synthetic_sparse_image() -> bytes:
    block_size = 4096
    raw = b"A" * block_size
    return b"".join(
        (
            struct.pack(
                "<IHHHHIIII",
                0xED26FF3A,
                1,
                0,
                28,
                12,
                block_size,
                3,
                3,
                0,
            ),
            struct.pack("<HHII", 0xCAC1, 0, 1, 12 + block_size),
            raw,
            struct.pack("<HHII", 0xCAC2, 0, 1, 16),
            b"\x12\x34\x56\x78",
            struct.pack("<HHII", 0xCAC3, 0, 1, 12),
        )
    )


def verify_ubd_disk_fixtures(tool: Path, production_layout: Path) -> None:
    run_tool(tool, "validate", "--layout", production_layout, check=True)

    with tempfile.TemporaryDirectory(prefix="android-ubd-disk-") as temporary:
        root = Path(temporary)
        images = root / "images"
        images.mkdir()
        (images / "system.img").write_bytes(b"raw-system")
        userdata_payload = synthetic_sparse_image()
        (images / "userdata.img").write_bytes(userdata_payload)
        fixture_layout = root / "layout.json"
        fixture_layout.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "sector_size": 512,
                    "alignment_bytes": 1024 * 1024,
                    "disk_size_bytes": 16 * 1024 * 1024,
                    "partition_entry_count": 128,
                    "disk_guid": "253a21bd-933f-5273-8cd3-586ec7288d68",
                    "partitions": [
                        {
                            "name": "system",
                            "start_bytes": 1024 * 1024,
                            "size_bytes": 2 * 1024 * 1024,
                            "image": "system.img",
                            "type_guid": "0fc63daf-8483-4772-8e79-3d69d8477de4",
                            "unique_guid": "e2c6424a-9fda-551e-b5d2-c1ba3b29ae33",
                            "writable": False,
                        },
                        {
                            "name": "userdata",
                            "start_bytes": 3 * 1024 * 1024,
                            "size_bytes": 4 * 1024 * 1024,
                            "image": "userdata.img",
                            "type_guid": "0fc63daf-8483-4772-8e79-3d69d8477de4",
                            "unique_guid": "f07a1ac4-5c95-5c37-86dd-70eaa487a193",
                            "writable": True,
                        },
                    ],
                },
                indent=2,
            )
            + "\n"
        )
        disk = root / "android.img"
        run_tool(
            tool, "assemble", "--layout", fixture_layout,
            "--images-dir", images, "--output", disk, check=True,
        )
        run_tool(tool, "verify", "--layout", fixture_layout, "--disk", disk, check=True)
        if disk.stat().st_size != 16 * 1024 * 1024:
            fail("assembled UBD fixture has the wrong logical size")
        with disk.open("rb") as image:
            image.seek(1024 * 1024)
            if image.read(len(b"raw-system")) != b"raw-system":
                fail("raw partition payload was not staged at its fixed offset")
            image.seek(3 * 1024 * 1024)
            if image.read(4096) != b"A" * 4096:
                fail("Android sparse RAW chunk was not expanded")
            if image.read(8) != b"\x12\x34\x56\x78" * 2:
                fail("Android sparse FILL chunk was not expanded")
            image.seek(3 * 1024 * 1024 + 2 * 4096)
            if image.read(32) != b"\0" * 32:
                fail("Android sparse DONT_CARE chunk is not a disk hole")

        existing = run_tool(
            tool, "assemble", "--layout", fixture_layout,
            "--images-dir", images, "--output", disk,
        )
        if existing.returncode == 0:
            fail("UBD assembler replaced an existing disk without permission")

        (images / "userdata.img").unlink()
        missing = run_tool(
            tool, "assemble", "--layout", fixture_layout,
            "--images-dir", images, "--output", root / "missing.img",
        )
        if missing.returncode == 0:
            fail("UBD assembler silently accepted a missing writable image")

        blank = root / "blank.img"
        run_tool(
            tool, "assemble", "--layout", fixture_layout, "--images-dir", images,
            "--blank-partition", "userdata", "--output", blank, check=True,
        )
        with blank.open("rb") as image:
            image.seek(3 * 1024 * 1024)
            if image.read(4096) != b"\0" * 4096:
                fail("explicit blank UBD partition is not zero initialized")

        for label, partition in (("immutable", "system"), ("unknown", "missing")):
            rejected = run_tool(
                tool, "assemble", "--layout", fixture_layout,
                "--images-dir", images, "--blank-partition", partition,
                "--output", root / f"bad-blank-{label}.img",
            )
            if rejected.returncode == 0:
                fail(f"UBD assembler accepted an {label} blank partition")
        (images / "userdata.img").write_bytes(userdata_payload)

        with disk.open("r+b") as image:
            image.seek(512 + 20)
            byte = image.read(1)
            image.seek(512 + 20)
            image.write(bytes((byte[0] ^ 0x80,)))
        corrupted = run_tool(
            tool, "verify", "--layout", fixture_layout, "--disk", disk
        )
        if corrupted.returncode == 0:
            fail("UBD verifier accepted a corrupted primary GPT")

        (images / "system.img").write_bytes(b"X" * (2 * 1024 * 1024 + 1))
        oversized = run_tool(
            tool, "assemble", "--layout", fixture_layout,
            "--images-dir", images, "--output", root / "oversized.img",
        )
        if oversized.returncode == 0:
            fail("UBD assembler accepted a partition payload that exceeds its capacity")


def verify_pin_manifest(tool: Path) -> None:
    """Only a project the manifest reads at a branch needs pinning."""
    commit = "1" * 40
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        base = root / "base-manifest.xml"
        base.write_text(
            '<manifest><remote name="r" fetch=".."/><default remote="r"/>'
            f'<project name="tag" path="a/tag" revision="{commit}" '
            'upstream="refs/tags/fixture"/>'
            f'<project name="branch" path="a/branch" revision="{commit}" '
            'upstream="refs/heads/fixture"/>'
            f'<project name="branch" path="b/branch" revision="{commit}" '
            'upstream="refs/heads/other"/></manifest>'
        )
        pin = root / "local_manifests/pin.xml"
        run_tool(tool, "--base-manifest", base, "--output", pin, check=True)
        expected = (
            '<?xml version="1.0" encoding="UTF-8"?>\n<manifest>\n'
            f'  <extend-project name="branch" path="a/branch" revision="{commit}" '
            'upstream="refs/heads/fixture" />\n'
            f'  <extend-project name="branch" path="b/branch" revision="{commit}" '
            'upstream="refs/heads/other" />\n</manifest>\n'
        )
        if pin.read_text() != expected:
            fail(f"pin manifest differs from the expected local manifest: {pin.read_text()}")
        base.write_text(base.read_text().replace(commit, "refs/heads/moving", 1))
        refused = run_tool(
            tool, "--base-manifest", base, "--output", root / "refused.xml"
        )
        if refused.returncode == 0 or "not pinned to a commit" not in refused.stdout:
            fail("pin manifest rendering accepted a project not pinned to a commit")


def verify_layout_consumers(root: Path, android_root: Path) -> None:
    """The fixed GPT geometry is compiled into the emulator and the launcher."""
    layout = json.loads(
        (android_root / "support/image/partition-layout.json").read_text()
    )
    partitions = layout["partitions"]
    emulator = (root / "darwin/device/block/android_boot.cpp").read_text()
    seed_mib = re.search(
        r"seedDiskSize = ([0-9']+)ULL \* mebibyte;", emulator
    )
    if seed_mib is None or (
        int(seed_mib.group(1).replace("'", "")) << 20 != layout["disk_size_bytes"]
    ):
        fail("the emulator's seed disk size differs from partition-layout.json")
    compiled = re.findall(
        r'ExpectedPartition\{"([a-z_]+)", ([0-9\']+)ULL, ([0-9\']+)ULL\}',
        emulator,
    )
    expected = [
        (p["name"], p["start_bytes"], p["size_bytes"]) for p in partitions
    ]
    if [
        (name, int(offset.replace("'", "")), int(size.replace("'", "")))
        for name, offset, size in compiled
    ] != expected:
        fail("the emulator's fixed partitions differ from partition-layout.json")
    launcher = (root / "tools/boot_android.py").read_text()
    first_lba = re.search(r"USERDATA_FIRST_LBA = ([0-9_]+)", launcher)
    userdata = partitions[-1]
    if userdata["name"] != "userdata" or first_lba is None or (
        int(first_lba.group(1).replace("_", ""))
        != userdata["start_bytes"] // layout["sector_size"]
    ):
        fail("the launcher's userdata start differs from partition-layout.json")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    root = args.source_root.resolve()

    android_root = root / "android/lineage-24"
    kernel_root = root / "kernel/mainline-6.18"

    subprocess.run(
        [
            str(android_root / "tools/verify-aosp-patches.py"),
            "--android-root",
            str(android_root),
        ],
        check=True,
    )
    verify_kernel_elf_fixtures(android_root / "build/verify-kernel-elf.py")
    verify_target_files_fixtures(android_root / "tools/verify-target-files.py")
    verify_hal_selections(android_root)
    verify_pin_manifest(android_root / "tools/render-pin-manifest.py")
    verify_initramfs_fixtures(android_root / "tools/verify-initramfs.py")
    verify_boot_image_fixtures(android_root / "tools/boot-image.py")
    verify_lineage_recovery_fixtures(
        android_root / "tools/verify-lineage-recovery.py", android_root
    )
    verify_ubd_disk_fixtures(
        android_root / "tools/ubd-disk.py",
        android_root / "support/image/partition-layout.json",
    )
    verify_layout_consumers(root, android_root)

    scripts = list((android_root / "build").rglob("*.sh"))
    scripts += list((android_root / "tools").rglob("*.py"))
    forbidden_commands = re.compile(
        r"(^|[;&|()\s])(ssh|scp|sftp|rsync|rsh)([;&|()\s]|$)", re.MULTILINE
    )
    forbidden_endpoint = re.compile(
        r"(BUILDER_(HOST|USER|PORT)|SSH_|CREDENTIAL|PRIVATE_KEY|REMOTE_URI)"
    )
    for script in scripts:
        text = script.read_text()
        if forbidden_commands.search(text):
            fail(f"tracked producer contains a transport command: {script}")
        if forbidden_endpoint.search(text):
            fail(f"tracked producer contains endpoint or credential state: {script}")
        if "/home/build/" in text:
            fail(f"tracked producer stores a builder-specific path: {script}")

    tracked_policy_text = "\n".join(
        path.read_text(errors="surrogateescape")
        for path in (
            android_root / "README.md",
            android_root / "metadata/lineage.env",
            kernel_root / "README.md",
            root / "kernel/common/config/darwin-uml.fragment",
            kernel_root / "config/android.fragment",
            kernel_root / "patches/metadata.env",
        )
    )
    for token in ("BUILDER_ALIAS", "/home/build/", "BUILDER_HOST", "PRIVATE_KEY"):
        if token in tracked_policy_text:
            fail(f"tracked Android policy stores operator state: {token}")

    print("Android 17 builder lane verifiers passed")


if __name__ == "__main__":
    main()
