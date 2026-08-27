#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import gzip
import io
import json
import re
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"android builder lane fixture: {message}")


def parse_env(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    assignment = re.compile(r"^([A-Z][A-Z0-9_]*)=(?:'([^']*)'|([0-9]+))$")
    for line_number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = assignment.fullmatch(line)
        if match is None:
            fail(f"{path}:{line_number}: malformed metadata assignment")
        key, quoted, numeric = match.groups()
        if key in values:
            fail(f"{path}:{line_number}: duplicate metadata key {key}")
        values[key] = quoted if quoted is not None else numeric
    return values


def synthetic_aarch64_elf(
    alignment: int = 16384,
    needed: tuple[str, ...] = (),
    runpath: str | None = None,
) -> bytes:
    has_dynamic = bool(needed or runpath)
    program_headers = 2 if has_dynamic else 1
    dynamic_entries = 3 + len(needed) + (1 if runpath else 0) if has_dynamic else 0
    dynamic_offset = 64 + 56 * program_headers
    string_table = bytearray(b"\0")
    string_offsets: dict[str, int] = {}
    for item in needed + ((runpath,) if runpath else ()):
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
        if runpath:
            entries.append((29, string_offsets[runpath]))
        entries.append((0, 0))
        for index, (tag, value) in enumerate(entries):
            struct.pack_into("<qQ", payload, dynamic_offset + index * 16, tag, value)
        payload[string_offset:] = string_table
    return bytes(payload)


UML_FSTAB = """# Fixed GPT partitions exposed by the Darwin UML UBD device.
/dev/block/by-name/system /system ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/system_ext /system_ext ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/product /product ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/vendor /vendor ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/odm /odm ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/metadata /metadata ext4 nodev,noatime,nosuid wait,check,formattable,first_stage_mount
/dev/block/by-name/metadata /metadata ext4 nodev,noatime,nosuid wait,check,first_stage_mount
# Static non-A/B storage uses neither userdata checkpointing nor BootControl.
/dev/block/by-name/userdata /data ext4 nodev,noatime,nosuid,errors=panic latemount,wait,check,formattable,keydirectory=/metadata/vold/metadata_encryption,fileencryption=aes-256-xts:aes-256-hctr2
/dev/block/by-name/userdata /data ext4 nodev,noatime,nosuid,errors=panic latemount,reservedsize=128M,wait,check,keydirectory=/metadata/vold/metadata_encryption,fileencryption=aes-256-xts:aes-256-hctr2
/dev/block/by-name/misc /misc emmc defaults defaults
"""
REQUIRED_PREOPT_APKS = (
    "PRODUCT/app/Jelly/Jelly.apk",
    "PRODUCT/app/LatinIME/LatinIME.apk",
    "SYSTEM/app/CaptivePortalLogin/CaptivePortalLogin.apk",
    "SYSTEM/app/PartnerBookmarksProvider/PartnerBookmarksProvider.apk",
    "SYSTEM/app/PrintRecommendationService/PrintRecommendationService.apk",
    "SYSTEM/app/Traceur/Traceur.apk",
    "SYSTEM/priv-app/BlockedNumberProvider/BlockedNumberProvider.apk",
    "SYSTEM/priv-app/ContactsProvider/ContactsProvider.apk",
    "SYSTEM/priv-app/CredentialManager/CredentialManager.apk",
    "SYSTEM/priv-app/DeviceDiagnostics/DeviceDiagnostics.apk",
    "SYSTEM/priv-app/E2eeContactKeysProvider/E2eeContactKeysProvider.apk",
    "SYSTEM/priv-app/IntentResolver/IntentResolver.apk",
    "SYSTEM/priv-app/LiveWallpapersPicker/LiveWallpapersPicker.apk",
    "SYSTEM/priv-app/LocalTransport/LocalTransport.apk",
    "SYSTEM/priv-app/ManagedProvisioning/ManagedProvisioning.apk",
    "SYSTEM/priv-app/PackageInstaller/PackageInstaller.apk",
    "SYSTEM/priv-app/ProxyHandler/ProxyHandler.apk",
    "SYSTEM/priv-app/Shell/Shell.apk",
    "SYSTEM/priv-app/SoundPicker/SoundPicker.apk",
    "SYSTEM/priv-app/Tag/Tag.apk",
    "SYSTEM/priv-app/UserDictionaryProvider/UserDictionaryProvider.apk",
    "SYSTEM/priv-app/VpnDialogs/VpnDialogs.apk",
    "SYSTEM_EXT/priv-app/Settings/Settings.apk",
    "SYSTEM_EXT/priv-app/StorageManager/StorageManager.apk",
    "SYSTEM_EXT/priv-app/SystemUI/SystemUI.apk",
    "SYSTEM_EXT/priv-app/ThemePicker/ThemePicker.apk",
)


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


def verify_target_files_fixtures(verifier: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="android-target-files-") as temporary:
        root = Path(temporary)
        kernel = root / "vmlinux"
        kernel.write_bytes(synthetic_aarch64_elf())
        kernel_config = root / ".config"
        kernel_config.write_text(
            "CONFIG_ARM64_16K_PAGES=y\n# CONFIG_MODULES is not set\n"
        )
        kernel_release = root / "include/config/kernel.release"
        kernel_release.parent.mkdir(parents=True)
        kernel_release.write_text("6.18.21-uml-fixture\n")

        def write_fixture(path: Path, *, module: bool = False,
                          module_loader: bool = False, dlkm_image: bool = False,
                          dynamic_image: bool = False, ota_tool: bool = False,
                          virtual_ab: bool = False, abi32: bool = False,
                          wrong_fstab: bool = False, stock_fstab: bool = False,
                          wrong_filesystem: bool = False,
                          wrong_apex_filesystem: bool = False,
                          seriallogging: bool = False,
                          boot_control: bool = False,
                          context_hub: bool = False,
                          unsupported_hal: bool = False,
                          unsupported_feature: bool = False,
                          wrong_kernel_config: bool = False,
                          wrong_kernel_release: bool = False,
                          missing_memfd_policy: bool = False,
                          missing_virtio_console: bool = False,
                          unsupported_hvc: bool = False,
                          missing_virgl: bool = False,
                          bad_virgl_elf: bool = False,
                          bad_virgl_link: bool = False,
                          missing_virgl_dependency: bool = False,
                          missing_virgl_runpath: bool = False,
                          system_other: bool = False,
                          missing_required_preopt: bool = False,
                          half_preopt_pair: bool = False,
                          virgl_alignment: int = 16384,
                          alignment: int = 16384) -> None:
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("BOOT/kernel", kernel.read_bytes())
                archive.writestr("INIT_BOOT/RAMDISK/init", b"init")
                archive.writestr("META/init_boot_filesystem_config.txt", "")
                archive.writestr(
                    "META/kernel_configs.txt",
                    "CONFIG_ARM64_16K_PAGES=n\n"
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
                abilist32 = "armeabi-v7a" if abi32 else ""
                virtual_ab_property = "ro.virtual_ab.enabled=true\n" if virtual_ab else ""
                archive.writestr(
                    "SYSTEM/build.prop",
                    "ro.product.cpu.abi=arm64-v8a\n"
                    "ro.product.cpu.abilist=arm64-v8a\n"
                    f"ro.product.cpu.abilist32={abilist32}\n"
                    f"{virtual_ab_property}",
                )
                archive.writestr(
                    "PRODUCT/etc/build.prop",
                    "aaudio.mmap_policy=1\n"
                    "aaudio.mmap_exclusive_policy=1\n",
                )
                archive.writestr(
                    "SYSTEM/bin/android-fixture", synthetic_aarch64_elf(alignment)
                )
                archive.writestr(
                    "SYSTEM/apex/com.android.fixture.apex",
                    synthetic_apex(
                        "erofs" if wrong_apex_filesystem else "ext4"
                    ),
                )
                fstab = (
                    UML_FSTAB.replace("/system ext4", "/system erofs", 1)
                    if wrong_fstab
                    else UML_FSTAB
                )
                archive.writestr("VENDOR/etc/fstab.uml", fstab)
                archive.writestr(
                    "VENDOR_BOOT/RAMDISK/first_stage_ramdisk/fstab.uml",
                    fstab,
                )
                archive.writestr(
                    "VENDOR/etc/init/init.kernarium.rc",
                    "on fs\n    mount_all --early\n"
                    "on late-fs\n    mount_all --late\n"
                    "on boot\n    setprop ro.hardware.audio.primary goldfish\n",
                )
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
                    '<attachedDevices><item>Speaker</item></attachedDevices>'
                    '<defaultOutputDevice>Speaker</defaultOutputDevice>'
                    '<mixPorts><mixPort name="primary output" role="source" '
                    'flags="AUDIO_OUTPUT_FLAG_PRIMARY"><profile name="" '
                    'format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="48000" '
                    'channelMasks="AUDIO_CHANNEL_OUT_STEREO"/></mixPort></mixPorts>'
                    '<devicePorts><devicePort tagName="Speaker" '
                    'type="AUDIO_DEVICE_OUT_SPEAKER" role="sink"/></devicePorts>'
                    '<routes><route type="mix" sink="Speaker" '
                    'sources="primary output"/></routes></module>',
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
                archive.writestr(
                    "VENDOR/etc/selinux/vendor_sepolicy.cil",
                    "(policycap open_perms)\n"
                    if missing_memfd_policy
                    else "(policycap memfd_class)\n",
                )
                ueventd = "/dev/ttyS0 0660 root shell\n"
                if not missing_virtio_console:
                    ueventd += "/dev/hvc0 0660 root shell\n"
                if unsupported_hvc:
                    ueventd += "/dev/hvc1 0660 root shell\n"
                archive.writestr("VENDOR/ueventd.rc", ueventd)
                for apk in REQUIRED_PREOPT_APKS:
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
                advertised_feature = (
                    "android.hardware.microphone"
                    if unsupported_feature
                    else "android.hardware.touchscreen"
                )
                archive.writestr(
                    "VENDOR/etc/permissions/handheld_core_hardware.xml",
                    f'<permissions><feature name="{advertised_feature}"/></permissions>',
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
                virgl_entries = (
                    "VENDOR/lib64/dri/gallium_dri.so",
                    "VENDOR/lib64/egl/libEGL_mesa.so",
                    "VENDOR/lib64/egl/libGLESv1_CM_mesa.so",
                    "VENDOR/lib64/egl/libGLESv2_mesa.so",
                )
                for index, entry in enumerate(virgl_entries):
                    if missing_virgl and index == 0:
                        continue
                    payload = (
                        b"not an ELF"
                        if bad_virgl_elf and index == 0
                        else synthetic_aarch64_elf(
                            virgl_alignment,
                            ()
                            if missing_virgl_dependency or index == 0
                            else ("gallium_dri.so",),
                            None
                            if missing_virgl_runpath or index == 0
                            else "$ORIGIN/../dri",
                        )
                    )
                    archive.writestr(entry, payload)
                link = zipfile.ZipInfo("VENDOR/lib64/dri/virtio_gpu_dri.so")
                link.create_system = 3
                link.external_attr = (stat.S_IFLNK | 0o777) << 16
                archive.writestr(
                    link,
                    b"wrong_dri.so" if bad_virgl_link else b"gallium_dri.so",
                )
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

        good = root / "good.zip"
        write_fixture(good)
        subprocess.run(
            [
                sys.executable,
                "-B",
                str(verifier),
                "--target-files",
                str(good),
                "--kernel-elf",
                str(kernel),
            ],
            check=True,
        )

        for label, options in (
            ("module", {"module": True}),
            ("module-loader", {"module_loader": True}),
            ("dlkm-image", {"dlkm_image": True}),
            ("dynamic-image", {"dynamic_image": True}),
            ("ota-tool", {"ota_tool": True}),
            ("virtual-ab", {"virtual_ab": True}),
            ("abi32", {"abi32": True}),
            ("alignment", {"alignment": 4096}),
            ("wrong-fstab", {"wrong_fstab": True}),
            ("wrong-filesystem", {"wrong_filesystem": True}),
            ("wrong-apex-filesystem", {"wrong_apex_filesystem": True}),
            ("stock-fstab", {"stock_fstab": True}),
            ("seriallogging", {"seriallogging": True}),
            ("boot-control", {"boot_control": True}),
            ("context-hub", {"context_hub": True}),
            ("unsupported-hal", {"unsupported_hal": True}),
            ("unsupported-feature", {"unsupported_feature": True}),
            ("kernel-config", {"wrong_kernel_config": True}),
            ("kernel-release", {"wrong_kernel_release": True}),
            ("memfd-policy", {"missing_memfd_policy": True}),
            ("missing-virtio-console", {"missing_virtio_console": True}),
            ("unsupported-hvc", {"unsupported_hvc": True}),
            ("missing-virgl", {"missing_virgl": True}),
            ("virgl-not-elf", {"bad_virgl_elf": True}),
            ("virgl-link", {"bad_virgl_link": True}),
            ("virgl-dependency", {"missing_virgl_dependency": True}),
            ("virgl-runpath", {"missing_virgl_runpath": True}),
            ("system-other", {"system_other": True}),
            ("missing-preopt", {"missing_required_preopt": True}),
            ("half-preopt-pair", {"half_preopt_pair": True}),
            ("virgl-alignment", {"virgl_alignment": 4096}),
        ):
            fixture = root / f"bad-{label}.zip"
            write_fixture(fixture, **options)
            result = subprocess.run(
                [
                    sys.executable,
                    "-B",
                    str(verifier),
                    "--target-files",
                    str(fixture),
                    "--kernel-elf",
                    str(kernel),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            if result.returncode == 0:
                fail(f"target-files verifier accepted the {label} fixture")


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


def synthetic_initramfs(*, wrong_fstab: bool = False, module_loader: bool = False,
                        duplicate_file: bool = False) -> bytes:
    fstab = (
        UML_FSTAB.replace("/system ext4", "/system erofs", 1)
        if wrong_fstab
        else UML_FSTAB
    )
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
    entries.append(newc_entry("TRAILER!!!", 0, b"", len(entries) + 1))
    return b"".join(entries)


def verify_initramfs_fixtures(verifier: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="android-initramfs-") as temporary:
        root = Path(temporary)
        good = root / "ramdisk.img"
        good.write_bytes(synthetic_initramfs())
        subprocess.run([sys.executable, "-B", str(verifier), str(good)], check=True)
        for label, options in (
            ("wrong-fstab", {"wrong_fstab": True}),
            ("module-loader", {"module_loader": True}),
            ("duplicate-file", {"duplicate_file": True}),
        ):
            fixture = root / f"bad-{label}.img"
            fixture.write_bytes(synthetic_initramfs(**options))
            result = subprocess.run(
                [sys.executable, "-B", str(verifier), str(fixture)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            if result.returncode == 0:
                fail(f"initramfs verifier accepted the {label} fixture")


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
        subprocess.run(
            [
                sys.executable,
                "-B",
                str(verifier),
                "verify",
                "--image",
                str(good),
                "--ramdisk",
                str(ramdisk),
            ],
            check=True,
        )
        selected_range = subprocess.run(
            [
                sys.executable,
                "-B",
                str(verifier),
                "range",
                "--image",
                str(good),
            ],
            check=True,
            stdout=subprocess.PIPE,
            text=True,
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
            result = subprocess.run(
                [
                    sys.executable,
                    "-B",
                    str(verifier),
                    "verify",
                    "--image",
                    str(fixture),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            if result.returncode == 0:
                fail(f"boot-image verifier accepted {label}")


def synthetic_orangefox_ramdisk(
    fstab: bytes, *, alignment: int = 16384, omit_dependency: bool = False
) -> bytes:
    entries = [
        newc_entry("init", 0o100750, b"init", 1),
        newc_entry(
            "prop.default",
            0o100644,
            b"ro.product.system.device=darwin\n"
            b"ro.product.system.model=OrangeFox on Darwin UML\n"
            b"service.adb.tcp.port=5555\n",
            2,
        ),
        newc_entry(
            "init.recovery.cutf_cvm.rc",
            0o100644,
            b"service recovery_ethernet /system/bin/ifconfig eth0 10.0.2.15 "
            b"netmask 255.255.255.0 up\n"
            b"    seclabel u:r:su:s0\n",
            15,
        ),
        newc_entry(
            "system/bin/recovery", 0o100755,
            synthetic_aarch64_elf(alignment, needed=("libfixture.so",)), 3,
        ),
        *([] if omit_dependency else [
            newc_entry(
                "system/lib64/libfixture.so", 0o100644,
                synthetic_aarch64_elf(alignment), 12,
            )
        ]),
        newc_entry("system/etc/fox.cfg", 0o100644, b"FOX_CODE_BASE=fixture\n", 4),
        newc_entry("system/etc/recovery.fstab", 0o100644, fstab, 5),
        newc_entry(
            "system/etc/cgroups.json", 0o100644,
            b'{"Cgroups":[{"Controller":"cpuset"}]}\n', 13,
        ),
        newc_entry(
            "system/etc/task_profiles.json", 0o100644,
            b'{"Profiles":[{"Name":"ServiceCapacityLow"}]}\n', 14,
        ),
    ]
    for inode, name in enumerate(
        ("recovery", "backup", "restore", "files", "mount", "wipe"), 6
    ):
        entries.append(newc_entry(f"twres/pages/{name}.xml", 0o100644, b"<recovery/>\n", inode))
    entries.append(newc_entry("TRAILER!!!", 0, b"", len(entries) + 1))
    return gzip.compress(b"".join(entries), mtime=0)


def verify_orangefox_recovery_fixtures(verifier: Path, android_root: Path) -> None:
    fstab = (
        android_root
        / "third_party/orangefox/device/recovery/root/system/etc/recovery.fstab"
    ).read_bytes()
    with tempfile.TemporaryDirectory(prefix="android-orangefox-") as temporary:
        root = Path(temporary)
        good = root / "recovery.img"
        good.write_bytes(synthetic_boot_image(synthetic_orangefox_ramdisk(fstab)))
        subprocess.run(
            [
                sys.executable, "-B", str(verifier),
                "--android-root", str(android_root), "--image", str(good),
            ],
            check=True,
        )
        bad = root / "recovery-4k.img"
        bad.write_bytes(
            synthetic_boot_image(synthetic_orangefox_ramdisk(fstab, alignment=4096))
        )
        result = subprocess.run(
            [
                sys.executable, "-B", str(verifier),
                "--android-root", str(android_root), "--image", str(bad),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if result.returncode == 0:
            fail("OrangeFox verifier accepted a 4 KiB-only recovery ELF")
        missing_dependency = root / "recovery-missing-dependency.img"
        missing_dependency.write_bytes(
            synthetic_boot_image(
                synthetic_orangefox_ramdisk(fstab, omit_dependency=True)
            )
        )
        result = subprocess.run(
            [
                sys.executable, "-B", str(verifier),
                "--android-root", str(android_root),
                "--image", str(missing_dependency),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if result.returncode == 0:
            fail("OrangeFox verifier accepted a missing ELF dependency")


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
    subprocess.run(
        [
            sys.executable,
            "-B",
            str(tool),
            "validate",
            "--layout",
            str(production_layout),
        ],
        check=True,
    )
    layout = json.loads(production_layout.read_text())
    names = [partition["name"] for partition in layout["partitions"]]
    if names != [
        "system",
        "system_ext",
        "product",
        "vendor",
        "odm",
        "metadata",
        "userdata",
        "init_boot",
        "misc",
        "recovery",
    ]:
        fail("production UBD layout has the wrong fixed partition order")
    if "super" in names or any("dlkm" in name for name in names):
        fail("production UBD layout contains a dynamic or DLKM partition")
    if layout["sector_size"] != 512 or layout["alignment_bytes"] != 1024 * 1024:
        fail("production UBD layout has the wrong sector geometry")
    for partition in layout["partitions"]:
        if partition["start_bytes"] % layout["alignment_bytes"]:
            fail(f"production UBD partition is misaligned: {partition['name']}")
        if partition["size_bytes"] % layout["alignment_bytes"]:
            fail(f"production UBD partition has a partial alignment unit: {partition['name']}")

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
        subprocess.run(
            [
                sys.executable,
                "-B",
                str(tool),
                "assemble",
                "--layout",
                str(fixture_layout),
                "--images-dir",
                str(images),
                "--output",
                str(disk),
            ],
            check=True,
        )
        subprocess.run(
            [
                sys.executable,
                "-B",
                str(tool),
                "verify",
                "--layout",
                str(fixture_layout),
                "--disk",
                str(disk),
            ],
            check=True,
        )
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

        existing = subprocess.run(
            [
                sys.executable,
                "-B",
                str(tool),
                "assemble",
                "--layout",
                str(fixture_layout),
                "--images-dir",
                str(images),
                "--output",
                str(disk),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if existing.returncode == 0:
            fail("UBD assembler replaced an existing disk without permission")

        (images / "userdata.img").unlink()
        missing = subprocess.run(
            [
                sys.executable,
                "-B",
                str(tool),
                "assemble",
                "--layout",
                str(fixture_layout),
                "--images-dir",
                str(images),
                "--output",
                str(root / "missing.img"),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if missing.returncode == 0:
            fail("UBD assembler silently accepted a missing writable image")

        blank = root / "blank.img"
        subprocess.run(
            [
                sys.executable,
                "-B",
                str(tool),
                "assemble",
                "--layout",
                str(fixture_layout),
                "--images-dir",
                str(images),
                "--blank-partition",
                "userdata",
                "--output",
                str(blank),
            ],
            check=True,
        )
        with blank.open("rb") as image:
            image.seek(3 * 1024 * 1024)
            if image.read(4096) != b"\0" * 4096:
                fail("explicit blank UBD partition is not zero initialized")

        for label, partition in (("immutable", "system"), ("unknown", "missing")):
            rejected = subprocess.run(
                [
                    sys.executable,
                    "-B",
                    str(tool),
                    "assemble",
                    "--layout",
                    str(fixture_layout),
                    "--images-dir",
                    str(images),
                    "--blank-partition",
                    partition,
                    "--output",
                    str(root / f"bad-blank-{label}.img"),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            if rejected.returncode == 0:
                fail(f"UBD assembler accepted an {label} blank partition")
        (images / "userdata.img").write_bytes(userdata_payload)

        with disk.open("r+b") as image:
            image.seek(512 + 20)
            byte = image.read(1)
            image.seek(512 + 20)
            image.write(bytes((byte[0] ^ 0x80,)))
        corrupted = subprocess.run(
            [
                sys.executable,
                "-B",
                str(tool),
                "verify",
                "--layout",
                str(fixture_layout),
                "--disk",
                str(disk),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if corrupted.returncode == 0:
            fail("UBD verifier accepted a corrupted primary GPT")

        (images / "system.img").write_bytes(b"X" * (2 * 1024 * 1024 + 1))
        oversized = subprocess.run(
            [
                sys.executable,
                "-B",
                str(tool),
                "assemble",
                "--layout",
                str(fixture_layout),
                "--images-dir",
                str(images),
                "--output",
                str(root / "oversized.img"),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if oversized.returncode == 0:
            fail("UBD assembler accepted a partition payload that exceeds its capacity")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    root = args.source_root.resolve()

    android_root = root / "android/aosp-17"
    kernel_root = root / "kernel/android-6.18"
    required = [
        root / "android/README.md",
        android_root / "README.md",
        android_root / "metadata/aosp.env",
        android_root / "metadata/base-manifest.xml",
        android_root / "build/prepare-sources.sh",
        android_root / "build/prepare-orangefox-source.sh",
        android_root / "build/preflight.sh",
        android_root / "build/apply-aosp-patches.sh",
        android_root / "build/apply-orangefox-patches.sh",
        android_root / "build/build-aosp.sh",
        android_root / "build/build-orangefox.sh",
        android_root / "build/assemble-image.sh",
        android_root / "build/assemble-ramdisk.sh",
        android_root / "build/apply-kernel-patches.sh",
        android_root / "build/build-kernel.sh",
        android_root / "build/verify-kernel-elf.py",
        android_root / "build/toolchain/aarch64-linux-clang.sh",
        android_root / "build/toolchain/linux-host-clang.sh",
        android_root / "build/toolchain/linux-host-compat.h",
        android_root / "support/verification/required-kernel-builtins.txt",
        android_root / "support/verification/forbidden-kernel-options.txt",
        android_root / "patches/projects.tsv",
        android_root / "patches/projects/p0001/metadata.env",
        android_root / "patches/projects/p0001/series/manifest.tsv",
        android_root / "patches/projects/p0002/metadata.env",
        android_root / "patches/projects/p0002/series/manifest.tsv",
        android_root / "patches/projects/p0003/metadata.env",
        android_root / "patches/projects/p0003/series/manifest.tsv",
        android_root / "third_party/jelly/metadata.env",
        android_root / "third_party/jelly/series/manifest.tsv",
        android_root / "third_party/orangefox/metadata.env",
        android_root / "third_party/orangefox/base-manifest.xml",
        android_root / "third_party/orangefox/patches/projects.tsv",
        android_root / "third_party/orangefox/patches/projects/p0001/metadata.env",
        android_root / "third_party/orangefox/patches/projects/p0001/series/manifest.tsv",
        android_root / "third_party/orangefox/patches/projects/p0002/metadata.env",
        android_root / "third_party/orangefox/patches/projects/p0002/series/manifest.tsv",
        android_root / "third_party/orangefox/device/AndroidProducts.mk",
        android_root / "third_party/orangefox/device/BoardConfig.mk",
        android_root / "third_party/orangefox/device/device.mk",
        android_root / "third_party/orangefox/device/twrp_darwin.mk",
        android_root / "third_party/orangefox/device/recovery/prepare-kernarium.sh",
        android_root / "third_party/orangefox/device/recovery/root/system/etc/recovery.fstab",
        android_root / "tools/verify-aosp-patches.py",
        android_root / "tools/regenerate-aosp-patches.py",
        android_root / "tools/verify-jelly-source.py",
        android_root / "tools/verify-orangefox-source.py",
        android_root / "tools/verify-orangefox-recovery.py",
        android_root / "tools/sync-orangefox-device.py",
        android_root / "tools/regenerate-jelly-patches.py",
        android_root / "tools/verify-target-files.py",
        android_root / "tools/verify-initramfs.py",
        android_root / "tools/boot-image.py",
        android_root / "tools/sync-device-overlay.py",
        android_root / "tools/ubd-disk.py",
        android_root / "overlay/AndroidProducts.mk",
        android_root / "overlay/BoardConfig.mk",
        android_root / "overlay/aosp_kernarium_darwin.mk",
        android_root / "overlay/config/Android.bp",
        android_root / "overlay/config/recovery.fstab",
        android_root / "overlay/config/kernarium_core_hardware.xml",
        android_root / "overlay/overlay/frameworks/base/core/res/res/values/config.xml",
        android_root / "overlay/overlay/frameworks/base/packages/SettingsProvider/res/values/defaults.xml",
        android_root / "support/boot/kernel-command-line.txt",
        android_root / "support/image/partition-layout.json",
        kernel_root / "README.md",
        kernel_root / "config/android.fragment",
        kernel_root / "patches/metadata.env",
        kernel_root / "patches/topic-path-overlaps.tsv",
        kernel_root / "patches/series/manifest.tsv",
        android_root / "tools/regenerate-kernel-patches.py",
    ]
    for path in required:
        if not path.is_file():
            fail(f"required owner is missing: {path.relative_to(root)}")

    subprocess.run(
        [
            sys.executable,
            "-B",
            str(android_root / "tools/verify-jelly-source.py"),
            "--android-root",
            str(android_root),
        ],
        check=True,
    )
    subprocess.run(
        [
            sys.executable,
            "-B",
            str(android_root / "tools/verify-orangefox-source.py"),
            "--android-root",
            str(android_root),
        ],
        check=True,
    )
    if (android_root / "overlay/apps/Jelly").exists():
        fail("the device overlay vendors the full Jelly source tree")

    aosp = parse_env(android_root / "metadata/aosp.env")
    ack = parse_env(kernel_root / "patches/metadata.env")
    expected_aosp = {
        "ANDROID17_MANIFEST_TAG": "android-17.0.0_r1",
        "ANDROID17_MANIFEST_COMMIT": "5bc9a7ce1cd78dd53613bbfd0ebf506e1e4adb0f",
        "ANDROID17_MANIFEST_TREE": "1541b7154f1532032baf7c73f222256cc29e8cfb",
        "ANDROID17_BASE_MANIFEST_SHA256": "3aa2cfa6bf79012561b33755177e094cddd9f6488ba2f7fca583f12cbba698c1",
        "ANDROID17_MAJOR": "17",
        "ANDROID17_PRODUCT": "aosp_kernarium_darwin",
        "ANDROID17_PAGE_SIZE": "16384",
        "ANDROID17_RELEASE_CONFIG": "aosp_current",
    }
    expected_ack = {
        "UML_KERNEL_FAMILY": "android-6.18",
        "UML_KERNEL_UPSTREAM_TAG": "android17-6.18-2026-06_r35",
        "UML_KERNEL_UPSTREAM_COMMIT": "7942b4b6552dc2c5932ed2bd9a6f3981f6a04333",
        "UML_KERNEL_UPSTREAM_TREE": "24e0546bc6c78e8e829f0a3657c8ba7c68faabda",
        "UML_KERNEL_FINAL_TREE": "0b665061a2b4edc1459d788e0cc0339f15140f21",
        "UML_KERNEL_PATCH_COUNT": "37",
        "UML_KERNEL_ABI_VERSION": "8",
        "UML_KERNEL_ABI_FINGERPRINT": "0x3871ad612337ea76",
        "UML_KERNEL_CLANG_VERSION": "r584948c",
        "UML_KERNEL_RUST_VERSION": "1.91.1.p3",
        "UML_KERNEL_BINDGEN_VERSION": "0.72.1",
        "UML_KERNEL_HOST_GLIBC_TOOLCHAIN": "x86_64-linux-glibc2.17-4.8",
    }
    for key, value in expected_aosp.items():
        if aosp.get(key) != value:
            fail(f"AOSP metadata {key} is not pinned to {value}")
    for key, value in expected_ack.items():
        if ack.get(key) != value:
            fail(f"ACK metadata {key} is not pinned to {value}")

    verifier = root / "kernel/common/tooling/verify-series-policy.py"
    subprocess.run(
        [str(verifier), "--line-root", str(kernel_root), "manifest"], check=True
    )
    subprocess.run(
        [
            str(android_root / "tools/verify-aosp-patches.py"),
            "--android-root",
            str(android_root),
        ],
        check=True,
    )
    verify_target_files_fixtures(android_root / "tools/verify-target-files.py")
    verify_initramfs_fixtures(android_root / "tools/verify-initramfs.py")
    verify_boot_image_fixtures(android_root / "tools/boot-image.py")
    verify_orangefox_recovery_fixtures(
        android_root / "tools/verify-orangefox-recovery.py", android_root
    )
    verify_ubd_disk_fixtures(
        android_root / "tools/ubd-disk.py",
        android_root / "support/image/partition-layout.json",
    )

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
        if script.name not in {
            "prepare-sources.sh", "prepare-orangefox-source.sh"
        } and re.search(
            r"\bgit(?:\s+-[^\s]+|\s+-C\s+[^\s]+)*\s+(?:clone|fetch)\b", text
        ):
            fail(f"producer fetches source outside preparation: {script}")

    tracked_policy_text = "\n".join(
        path.read_text(errors="surrogateescape")
        for path in (
            android_root / "README.md",
            android_root / "metadata/aosp.env",
            android_root / "third_party/orangefox/metadata.env",
            kernel_root / "README.md",
            kernel_root / "config/android.fragment",
            kernel_root / "patches/metadata.env",
        )
    )
    for token in ("BUILDER_ALIAS", "/home/build/", "BUILDER_HOST", "PRIVATE_KEY"):
        if token in tracked_policy_text:
            fail(f"tracked Android policy stores operator state: {token}")

    prepare = (android_root / "build/prepare-sources.sh").read_text()
    prepare_orangefox = (
        android_root / "build/prepare-orangefox-source.sh"
    ).read_text()
    preflight = (android_root / "build/preflight.sh").read_text()
    if "--aosp-source" not in prepare or "--ack-workspace" not in prepare:
        fail("source preparation does not require explicit local workspaces")
    for token in (
        "JELLY_UPSTREAM_REPOSITORY",
        "JELLY_UPSTREAM_COMMIT",
        "JELLY_UPSTREAM_TREE",
        "JELLY_FINAL_COMMIT",
        "JELLY_FINAL_TREE",
        "JELLY_DESTINATION",
        "git -C \"$JELLY_STAGING\" fetch",
        "am --committer-date-is-author-date",
        "verify-jelly-source.py",
    ):
        if token not in prepare:
            fail(f"source preparation is missing Jelly policy: {token}")
    for token in (
        "builder host must be Linux",
        "builder host must be x86_64",
        "ANDROID17_PAGE_SIZE\" -eq 16384",
        "UML_KERNEL_FAMILY\" = android-6.18",
        "ACK common",
    ):
        if token not in preflight:
            fail(f"builder preflight is missing policy: {token}")
    for script_name, text in (("prepare", prepare), ("preflight", preflight)):
        if "repo status" in text:
            fail(f"{script_name} mistakes Repo's clean status prose for a delta")
        if "repo forall" not in text or "git status --porcelain" not in text:
            fail(f"{script_name} does not inspect each project porcelain status")

    apply_kernel = (android_root / "build/apply-kernel-patches.sh").read_text()
    regenerate_kernel = (
        android_root / "tools/regenerate-kernel-patches.py"
    ).read_text()
    apply_aosp = (android_root / "build/apply-aosp-patches.sh").read_text()
    apply_orangefox = (
        android_root / "build/apply-orangefox-patches.sh"
    ).read_text()
    build_aosp = (android_root / "build/build-aosp.sh").read_text()
    build_orangefox = (android_root / "build/build-orangefox.sh").read_text()
    assemble_image = (android_root / "build/assemble-image.sh").read_text()
    assemble_ramdisk = (android_root / "build/assemble-ramdisk.sh").read_text()
    publish_release = (android_root / "tools/publish-release.py").read_text()
    build_kernel = (android_root / "build/build-kernel.sh").read_text()
    target_files_verifier = (
        android_root / "tools/verify-target-files.py"
    ).read_text()
    orangefox_image_verifier = (
        android_root / "tools/verify-orangefox-recovery.py"
    ).read_text()
    kernel_config = (kernel_root / "config/android.fragment").read_text()
    required_kernel_builtins = (
        android_root / "support/verification/required-kernel-builtins.txt"
    ).read_text()
    rcu_spin_patch = (
        kernel_root
        / "patches/series/0024-UML-DARWIN-LOCAL-service-timers-from-kernel-spin-che.patch"
    ).read_text()
    rcu_quiescent_patch = (
        kernel_root
        / "patches/series/0025-UML-DARWIN-LOCAL-add-quiescent-boundaries-to-kernel-.patch"
    ).read_text()
    rcu_timer_boundary_patch = (
        kernel_root
        / "patches/series/0026-UML-DARWIN-LOCAL-service-timers-at-kernel-quiescent-.patch"
    ).read_text()
    rcu_cond_resched_patch = (
        kernel_root
        / "patches/series/0027-UML-DARWIN-LOCAL-poll-timers-before-cond_resched.patch"
    ).read_text()
    ubd_cow_offset_patch = (
        kernel_root
        / "patches/series/0033-UML-DARWIN-LOCAL-um-advance-COW-I-O-by-the-full-desc.patch"
    ).read_text()
    initrd_range_patch = (
        kernel_root
        / "patches/series/0034-UML-DARWIN-LOCAL-um-load-an-initrd-from-a-host-file-.patch"
    ).read_text()
    battery_patch = (
        kernel_root
        / "patches/series/0036-UML-DARWIN-LOCAL-power-add-Kernarium-host-battery.patch"
    ).read_text()
    build_make_patch = "\n".join(
        path.read_text()
        for path in sorted(
            (android_root / "patches/projects/p0002/series").glob("*.patch")
        )
    )
    connectivity_patch = next(
        (android_root / "patches/projects/p0003/series").glob("*.patch")
    ).read_text()
    cuttlefish_patches = "\n".join(
        path.read_text()
        for path in sorted(
            (android_root / "patches/projects/p0001/series").glob("*.patch")
        )
    )
    virtualization_patch = next(
        (android_root / "patches/projects/p0008/series").glob("*.patch")
    ).read_text()
    settings_patch = "\n".join(
        path.read_text()
        for path in sorted(
            (android_root / "patches/projects/p0009/series").glob("*.patch")
        )
    )
    launcher_patch = "\n".join(
        path.read_text()
        for path in sorted(
            (android_root / "patches/projects/p0010/series").glob("*.patch")
        )
    )
    if "worktree add --detach" not in apply_kernel or "git -C \"$KERNEL_SOURCE\" am" not in apply_kernel:
        fail("ACK materializer does not replay generated topics in a worktree")
    for token in (
        'expected_line_root = outer_root / "kernel/android-6.18"',
        'metadata.get("UML_KERNEL_FAMILY") != "android-6.18"',
        '"format-patch"',
        '"--full-index"',
        '"--binary"',
        "series_dir.rename(series_backup)",
        '"UML_KERNEL_FINAL_TREE"',
        '"UML_KERNEL_PATCH_COUNT"',
        '"kernel/common/tooling/verify-series-policy.py"',
    ):
        if token not in regenerate_kernel:
            fail(f"ACK patch regenerator is missing isolation policy: {token}")
    for token in (
        "darwin_timer_spin_checkpoint",
        "darwin_timer_poll(&current_pt_regs()->regs)",
        "#include <linux/ptrace.h>",
        "spin_relax_counts[CONFIG_NR_CPUS]",
        "if (!um_get_signals())",
        "darwin_ipi_poll();",
        "(++spin_relax_counts[cpu] & 0xffU) == 0",
    ):
        if token not in rcu_spin_patch:
            fail(f"ACK RCU spin-timer topic is missing fixed policy: {token}")
    if len(re.findall(r"\+\s*cpu = uml_curr_cpu\(\);", rcu_spin_patch)) != 2:
        fail("ACK spin checkpoint does not recompute CPU identity after IPI service")
    if re.search(
        r"if \(!um_get_signals\(\)\).*"
        r"return;.*"
        r"if \(__atomic_load_n\(&pending_ipis\[cpu\].*"
        r"darwin_ipi_poll\(\);.*"
        r"cpu = uml_curr_cpu\(\);.*"
        r"darwin_timer_spin_checkpoint\(\);",
        rcu_spin_patch,
        re.DOTALL,
    ) is None:
        fail("ACK spin checkpoint no longer preserves IRQ/IPI/timer ordering")
    for token in (
        "darwin_uaccess_checkpoint",
        "!faulthandler_disabled()",
        "cond_resched();",
        "darwin_tlb_invalidate_range(mm, vma->vm_start, vma->vm_end);",
    ):
        if token not in rcu_quiescent_patch:
            fail(f"ACK RCU quiescent-boundary topic is missing policy: {token}")
    for token in (
        "darwin_timer_kernel_checkpoint",
        "!may_resched || !um_get_signals()",
        "darwin_timer_poll(&current_pt_regs()->regs)",
        "darwin_tlb_invalidate_range(mm, vma->vm_start, vma->vm_end);",
    ):
        if token not in rcu_timer_boundary_patch:
            fail(f"ACK RCU timer-boundary topic is missing policy: {token}")
    if re.search(
        r"if \(!may_resched \|\| !um_get_signals\(\)\).*"
        r"darwin_timer_kernel_checkpoint\(\);.*"
        r"cond_resched\(\);",
        rcu_timer_boundary_patch,
        re.DOTALL,
    ) is None:
        fail("ACK uaccess boundary no longer polls the timer before rescheduling")
    if re.search(
        r"darwin_tlb_invalidate_range\(mm, vma->vm_start, vma->vm_end\);.*"
        r"if \(um_get_signals\(\)\).*"
        r"darwin_timer_kernel_checkpoint\(\);.*"
        r"cond_resched\(\);",
        rcu_timer_boundary_patch,
        re.DOTALL,
    ) is None:
        fail("ACK TLB boundary no longer preserves invalidate/IRQ/timer/reschedule ordering")
    for token in (
        "#ifdef UML_HOST_DARWIN",
        "#include <kern_util.h>",
        "int __sched __cond_resched(void)",
        "if (!irqs_disabled())",
        "darwin_timer_kernel_checkpoint();",
        "if (should_resched(0) && !irqs_disabled())",
    ):
        if token not in rcu_cond_resched_patch:
            fail(f"ACK cond_resched timer topic is missing fixed policy: {token}")
    if re.search(
        r"int __sched __cond_resched\(void\).*"
        r"if \(!irqs_disabled\(\)\).*"
        r"darwin_timer_kernel_checkpoint\(\);.*"
        r"if \(should_resched\(0\) && !irqs_disabled\(\)\)",
        rcu_cond_resched_patch,
        re.DOTALL,
    ) is None:
        fail("ACK cond_resched boundary no longer polls the timer before rescheduling")
    if re.search(
        r"^\+\s*req->offset \+= desc->length;$",
        ubd_cow_offset_patch,
        re.MULTILINE,
    ) is None:
        fail("ACK UBD COW topic does not advance by the complete descriptor")
    if re.search(
        r"^\+\s*req->offset \+= len;$", ubd_cow_offset_patch, re.MULTILINE
    ) is not None:
        fail("ACK UBD COW topic advances by only the final backing/COW run")
    for token in (
        '__uml_setup("initrd_offset=", uml_initrd_offset_setup',
        '__uml_setup("initrd_size=", uml_initrd_size_setup',
        "initrd_offset and initrd_size are invalid or incomplete",
        "uml_load_file_range",
        "os_pread_file",
        "os_pread_file",
    ):
        if token not in initrd_range_patch:
            fail(f"ACK initrd-range topic is missing fixed policy: {token}")
    if "patches/projects.tsv" not in apply_aosp or 'git -C "$checkout" am' not in apply_aosp:
        fail("AOSP materializer does not replay the routed per-project series")
    if "$ANDROID_ROOT/support" in apply_aosp or "cp " in apply_aosp:
        fail("AOSP materializer stages support files outside the exact overlay sync")
    for token in (
        "sync-device-overlay.py",
        '"$OVERLAY_SYNC" --android-root "$ANDROID_ROOT" --aosp-source "$AOSP_SOURCE"',
    ):
        if token not in apply_aosp:
            fail(f"AOSP materializer is missing device-overlay policy: {token}")
    if "TARGET_SKIP_OTA_PACKAGE" not in build_make_patch or "built_ota_tools" not in build_make_patch:
        fail("AOSP build route does not remove the skipped-OTA updater dependency")
    for token in (
        "$(TARGET_SKIP_OTA_PACKAGE) $(BOARD_USES_FULL_RECOVERY_IMAGE)",
        "$(MAKE_RECOVERY_PATCH) $(zip_root) $(zip_root)",
    ):
        if token not in build_make_patch:
            fail(f"AOSP build route is missing skipped-OTA recovery policy: {token}")
    if re.search(
        r"ifeq \(\$\(TARGET_OTA_ALLOW_NON_AB\),true\).*"
        r"ifneq \(\$\(TARGET_SKIP_OTA_PACKAGE\),true\).*"
        r"\$\(MAKE_RECOVERY_PATCH\) \$\(zip_root\) \$\(zip_root\)",
        build_make_patch,
        re.DOTALL,
    ) is None:
        fail("AOSP build route does not gate recovery reconstruction on skipped OTA")
    for token in (
        "PRODUCT_PACKAGES_EXCLUDE",
        "PRODUCT_COPY_FILES_EXCLUDE",
        "$(eval _pif_modules := $(filter-out "
        "$(call get-product-var,$(1),PRODUCT_PACKAGES_EXCLUDE)",
        "PRODUCT_COPY_FILES := $(filter-out $(PRODUCT_COPY_FILES_EXCLUDE)",
    ):
        if token not in build_make_patch:
            fail(f"AOSP build route is missing inherited-payload exclusion: {token}")
    if build_make_patch.count(
        "$(call get-product-var,$(1),PRODUCT_PACKAGES_EXCLUDE)"
    ) != 2:
        fail("AOSP build route does not exclude packages before and after dependencies")
    for token in (
        'payload_fs_type: "ext4"',
        '-    payload_fs_type: "erofs"',
        "-        pcluster_size: 32768",
    ):
        if token not in virtualization_patch:
            fail(f"Virtualization APEX route is missing ext4 policy: {token}")

    android_products = (android_root / "overlay/AndroidProducts.mk").read_text()
    device_product = (android_root / "overlay/aosp_kernarium_darwin.mk").read_text()
    board_config = (android_root / "overlay/BoardConfig.mk").read_text()
    if (
        "aosp_kernarium_darwin-aosp_current-userdebug" not in android_products
        or "trunk_staging" in android_products
    ):
        fail("Android product registration does not select aosp_current")
    for token in (
        "include device/google/cuttlefish/vsoc_arm64_pgagnostic/BoardConfig.mk",
        "BOARD_USES_FULL_RECOVERY_IMAGE := true",
        "BOARD_USES_SYSTEM_OTHER_ODEX :=\n",
        "BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE := ext4",
        "BOARD_SYSTEMIMAGE_EROFS_BLOCKSIZE :=",
        "BOARD_PRODUCTIMAGE_FILE_SYSTEM_TYPE := ext4",
    ):
        if token not in board_config:
            fail(f"Android board configuration is missing recovery policy: {token}")
    inherited_board = (
        "include device/google/cuttlefish/vsoc_arm64_pgagnostic/BoardConfig.mk"
    )
    system_other_override = "BOARD_USES_SYSTEM_OTHER_ODEX :=\n"
    if board_config.index(system_other_override) < board_config.index(inherited_board):
        fail("Android board clears system_other odex before inherited policy")
    settings_defaults = (
        android_root
        / "overlay/overlay/frameworks/base/packages/SettingsProvider/res/values/defaults.xml"
    ).read_text()
    framework_defaults = (
        android_root / "overlay/overlay/frameworks/base/core/res/res/values/config.xml"
    ).read_text()
    settings_config = (
        android_root
        / "overlay/overlay/packages/apps/Settings/res/values/config.xml"
    ).read_text()
    settings_strings = (
        android_root
        / "overlay/overlay/packages/apps/Settings/res/values/strings.xml"
    ).read_text()
    jelly_metadata = parse_env(android_root / "third_party/jelly/metadata.env")
    orangefox_metadata = parse_env(
        android_root / "third_party/orangefox/metadata.env"
    )
    orangefox_device = (
        android_root / "third_party/orangefox/device/BoardConfig.mk"
    ).read_text() + (
        android_root / "third_party/orangefox/device/device.mk"
    ).read_text()
    orangefox_prepare = (
        android_root
        / "third_party/orangefox/device/recovery/prepare-kernarium.sh"
    ).read_text()
    jelly_patches = "\n".join(
        path.read_text()
        for path in sorted((android_root / "third_party/jelly/series").glob("*.patch"))
    )
    core_hardware = (
        android_root / "overlay/config/kernarium_core_hardware.xml"
    ).read_text()
    vendor_genfs_contexts = (
        android_root / "overlay/sepolicy/vendor/genfs_contexts"
    ).read_text()
    for token in (
        "PRODUCT_NAME := aosp_kernarium_darwin",
        "PRODUCT_DEFAULT_APEX_PAYLOAD_TYPE := ext4",
        "PRODUCT_PACKAGES_EXCLUDE",
        "PRODUCT_COPY_FILES_EXCLUDE",
        "Jelly",
        "LatinIME",
        "ThemePicker",
        "BOARD_HAVE_BLUETOOTH := false",
        "TARGET_NO_TELEPHONY := true",
        "PRODUCT_BUILD_RECOVERY_IMAGE := true",
        "android.hardware.health-service.example",
        "android.hardware.health-service.example_recovery",
        "com.android.hardware.contexthub",
        "hardware/interfaces/audio/aidl/default/audio_effects.mk",
        "aaudio.mmap_policy=1",
        "aaudio.mmap_exclusive_policy=1",
    ):
        if token not in device_product:
            fail(f"device overlay product is missing fixed policy: {token}")
    for removed in (
        "Calendar",
        "Camera2",
        "Contacts",
        "DeskClock",
        "Dialer",
        "Gallery2",
        "messaging",
    ):
        if removed not in device_product:
            fail(f"device overlay does not remove obsolete app: {removed}")
    removed_packages = device_product.split(
        "KERNARIUM_REMOVED_PACKAGES :=", 1
    )[1].split("PRODUCT_PACKAGES_EXCLUDE", 1)[0]
    if "QuickSearchBox" not in removed_packages:
        fail("device overlay retains Launcher3's unwanted home-screen search provider")
    for removed in (
        "com.android.hardware.wifi",
        "com.google.cf.bt",
        "com.google.cf.wifi",
        "com.google.cf.wpa_supplicant",
        "CuttlefishTetheringOverlay",
        "CuttlefishWifiOverlay",
        "wificond",
        "wificond_compat_symlink_module",
    ):
        if removed not in removed_packages:
            fail(f"device overlay retains unsupported radio package: {removed}")
    added_packages = device_product.split("PRODUCT_PACKAGES +=", 1)[1].split(
        "# Replace the handheld feature declaration", 1
    )[0]
    for removed in (
        "com.android.hardware.wifi",
        "com.google.cf.bt",
        "com.google.cf.wifi",
        "com.google.cf.wpa_supplicant",
    ):
        if removed in added_packages:
            fail(f"device overlay explicitly restores radio package: {removed}")
    if (android_root / "overlay/apps/SystemUpdater").exists():
        fail("device overlay replaces the stock System Update Settings route")
    for token in (
        "platform/packages/apps/Settings",
        "packages/apps/Settings",
        "platform/packages/apps/Launcher3",
        "packages/apps/Launcher3",
    ):
        if token not in (android_root / "patches/projects.tsv").read_text():
            fail(f"AOSP app patch route is missing identity: {token}")
    for token in (
        '-            android:key="bluetooth_development_settings"',
        '-            android:key="wifi_display_certification"',
        '-            android:key="media_output"',
        "-        controllers.add(new WifiVerboseLoggingPreferenceController(context));",
    ):
        if token not in settings_patch:
            fail(f"Settings topic does not remove radio-only control: {token}")
    for token in (
        '+        android:title="@string/ethernet"',
        "+    private boolean useEthernetSettings() {",
        "+        if (hasWifiManager()) {",
        "+            mWifiCategory.getPreferenceCategory().setVisible(false);",
        "+            mWifiEntryPreferenceCategory.setVisible(false);",
        "+            findPreference(PREF_KEY_WIFI_EXT_CATEGORY).setVisible(false);",
        "+        return (Flags.ethernetSettings() || !hasWifiManager) &&",
    ):
        if token not in settings_patch:
            fail(f"Settings topic is missing Ethernet-only routing policy: {token}")
    for path in (
        "overlay/overlay/packages/apps/Settings/res/xml/connected_devices.xml",
        "overlay/overlay/packages/apps/Settings/res/xml/connected_devices_advanced.xml",
    ):
        if (android_root / path).exists():
            fail(f"device overlay needlessly replaces stock Settings XML: {path}")
    for path in (
        "res/xml/connected_devices.xml",
        "res/xml/connected_devices_advanced.xml",
        "src/com/android/settings/connecteddevice/ConnectedDeviceDashboardFragment.java",
    ):
        if f"diff --git a/{path}" in settings_patch:
            fail(f"Settings topic sweeps preferences instead of gating its route: {path}")
    if "diff --git a/AndroidManifest.xml" not in settings_patch:
        fail("Settings topic does not gate unavailable radio activities")
    if "diff --git a/src/com/android/settings/core/gateway/SettingsGateway.java" in settings_patch:
        fail("Settings topic needlessly changes the stock fragment gateway")
    expected_activity_gates = {
        '+            android:enabled="false"': 8,
        '+                  android:enabled="false"': 2,
    }
    for token, count in expected_activity_gates.items():
        if settings_patch.count(token) != count:
            fail(f"Settings topic has the wrong radio activity gate count: {token}")
    if 'android:enabled="@bool/config_show_' in settings_patch:
        fail("Settings manifest gates incorrectly depend on runtime resource overlays")
    for token in (
        '<bool name="config_show_system_update_settings">false</bool>',
        '<bool name="config_show_top_level_connected_devices">false</bool>',
        '<bool name="config_show_wifi_settings">false</bool>',
        '<bool name="config_show_wifi_hotspot_settings">false</bool>',
        '<bool name="config_show_wifi_ip_address">false</bool>',
        '<bool name="config_show_wifi_mac_address">false</bool>',
    ):
        if token not in settings_config:
            fail(f"Settings resource overlay retains Wi-Fi surface: {token}")
    for token in (
        '-        android:key="tether_settings"',
        '-        android:key="airplane_mode_on"',
        '-        android:key="mobile_network_list"',
        '-        android:key="calls_and_sms"',
        '-        android:key="adaptive_connectivity"',
        '-        android:key="cellular_security_network_internet"',
    ):
        if token not in settings_patch:
            fail(f"Settings topic retains unavailable Network route: {token}")
    for token in (
        "-        use(NetworkProviderCallsSmsController.class).init(this);",
        "-            use(AirplaneModePreferenceController.class).setFragment(this);",
        "-                        Settings.ConnectedDeviceDashboardActivity.class.getName()),",
        "-                !UserManager.isDeviceInDemoMode(this) /* enabled */",
    ):
        if token not in settings_patch:
            fail(f"Settings topic leaves an omitted Network controller attached: {token}")
    for token in (
        "-        return NetworkDashboardScreen.KEY;",
        "+        return null;",
    ):
        if token not in settings_patch:
            fail(f"Settings topic retains incompatible Network metadata binding: {token}")
    for token in (
        '<string name="network_dashboard_summary_mobile">VPN, private DNS</string>',
        '<string name="network_dashboard_summary_no_mobile">VPN, private DNS</string>',
    ):
        if token not in settings_strings:
            fail(f"Network summary still advertises unavailable radios: {token}")
    for token in (
        '+    cmd: " $(location launcher-build-config-tool) '
        '-d QSB_ON_FIRST_SCREEN > $(out)",',
        "+        if (!BuildConfig.QSB_ON_FIRST_SCREEN) {",
        "+                visibility = View.GONE",
        "+        if (cueBarAceMigration() && mControllers != null",
    ):
        if token not in launcher_patch:
            fail(f"Launcher3 topic retains the first-screen search surface: {token}")
    for token in (
        '<bool name="def_lockscreen_disabled">true</bool>',
        '<integer name="def_screen_off_timeout">2147483647</integer>',
        '<bool name="def_show_ime_with_hard_keyboard">true</bool>',
        '<bool name="def_stay_on_while_plugged_in">true</bool>',
    ):
        if token not in settings_defaults:
            fail(f"SettingsProvider overlay is missing emulator default: {token}")
    for token in (
        '<bool name="config_disableLockscreenByDefault">true</bool>',
        "org.lineageos.jelly",
    ):
        if token not in framework_defaults:
            fail(f"framework overlay is missing emulator default: {token}")
    expected_jelly = {
        "JELLY_UPSTREAM_REPOSITORY": (
            "https://github.com/LineageOS/android_packages_apps_Jelly"
        ),
        "JELLY_UPSTREAM_BRANCH": "lineage-23.2",
        "JELLY_UPSTREAM_COMMIT": "767833067da7bbbb639782bbddec33f602215c3d",
        "JELLY_UPSTREAM_TREE": "82fe9a20385cd4546a0f5bc48c472e11ed7ad0fc",
        "JELLY_FINAL_COMMIT": "607d28aece407ccc5e245539d2e1b76d3ea81a37",
        "JELLY_FINAL_TREE": "ce63d63703fbfd2e616e94513ea1e5c63ccae4ef",
        "JELLY_PATCH_COUNT": "2",
        "JELLY_DESTINATION": "packages/apps/Jelly",
    }
    for key, expected in expected_jelly.items():
        if jelly_metadata.get(key) != expected:
            fail(f"Jelly metadata differs for {key}")
    if '+    sdk_version: "37"' not in jelly_patches:
        fail("Jelly topic is not bound to the AOSP 17 product SDK")
    if "app/src/main/AndroidManifest.xml" in jelly_patches:
        fail("Jelly topic unnecessarily changes upstream browser permissions")
    if "+        private const val LOCATION_ENABLED_DEFAULT = false" not in jelly_patches:
        fail("Jelly topic does not disable WebView geolocation by default")
    if "android.hardware.microphone" in core_hardware:
        fail("device overlay advertises unsupported audio capture hardware")
    battery_sysfs = (
        "genfscon sysfs /devices/virtio-uml-cmdline/virtio-uml.4/"
        "virtio4/power_supply u:object_r:sysfs_batteryinfo:s0"
    )
    if battery_sysfs not in vendor_genfs_contexts:
        fail("battery sysfs is not labeled for the stock Health HAL")
    if "OrangeFox" in device_product:
        fail("OrangeFox recovery configuration leaked into the AOSP product overlay")
    expected_orangefox = {
        "ORANGEFOX_SYNC_COMMIT": "14eca5f7ef82c9dacfe353db8b93329383c7293f",
        "ORANGEFOX_MANIFEST_COMMIT": "49f2d6950edd2dc87c213636a8a680ca5c97e52c",
        "ORANGEFOX_BOOTABLE_RECOVERY_COMMIT": "9860b87c823039b7950a4dee4e945d587ec0a9b0",
        "ORANGEFOX_VENDOR_RECOVERY_COMMIT": "af3d99b83adedf88fa9992d9320c5ce9811c6b08",
        "ORANGEFOX_SE_OMAPI_COMMIT": "9ea6e4a9ecfe04ffb82767d7cbcab3e8dc6295af",
        "ORANGEFOX_PRODUCT": "twrp_darwin",
        "ORANGEFOX_RELEASE_CONFIG": "ap2a",
        "ORANGEFOX_PATCH_COUNT": "3",
    }
    for key, expected in expected_orangefox.items():
        if orangefox_metadata.get(key) != expected:
            fail(f"OrangeFox metadata differs for {key}")
    for token in (
        "BOARD_EXCLUDE_KERNEL_FROM_RECOVERY_IMAGE := true",
        "BOARD_BOOT_HEADER_VERSION := 4",
        "PRODUCT_MAX_PAGE_SIZE_SUPPORTED := 65536",
        "TW_INCLUDE_CRYPTO_FBE := true",
        "TW_INCLUDE_FBE_METADATA_DECRYPT := true",
        "TW_NO_SCREEN_BLANK := true",
        "TW_NO_SCREEN_TIMEOUT := true",
        "BOARD_RECOVERY_IMAGE_PREPARE",
    ):
        if token not in orangefox_device:
            fail(f"OrangeFox device tree is missing fixed policy: {token}")
    for token in (
        '"$root/FFiles/ps"',
        '"$root/sbin/magiskboot"',
        'ln -s -- /system/bin/toybox "$root/FFiles/ps"',
    ):
        if token not in orangefox_prepare:
            fail(f"OrangeFox 16 KiB prepare hook is missing policy: {token}")
    for token in (
        "platform/packages/modules/Connectivity",
        "ro.boot.uml_bpf_interpreter",
        "api_level_full >= NETBPFLOAD_25Q2_VER && !umlInterpreter",
        "UML kernel has no eBPF JIT; using its built-in interpreter.",
        "isAtLeastKernelVersion(6, 6, 118) && !umlInterpreter",
        "UML BPF interpreter cannot execute callbacks; using inline BPF objects.",
    ):
        if token not in connectivity_patch and token not in (
            android_root / "patches/projects.tsv"
        ).read_text():
            fail(f"Connectivity patch route is missing UML BPF policy: {token}")
    for token in (
        "UML_ANDROID_DISABLE_PROJECT_QUOTAS := true",
        "ifeq ($(UML_ANDROID_DISABLE_PROJECT_QUOTAS),true)",
        "external_storage.casefold.enabled=1",
        "external_storage.sdcardfs.enabled=0",
        "latemount,wait,check,formattable,keydirectory=",
        "BOARD_HAVE_BLUETOOTH := false",
        "CF_VENDOR_NO_UWB := true",
        "CF_VENDOR_NO_THREADNETWORK := true",
        "LOCAL_ENABLE_LIGHT := false",
        "LOCAL_ENABLE_OEMLOCK := false",
        "LOCAL_ENABLE_NEURALNETWORKS := false",
        "LOCAL_ENABLE_NEURALNETWORKS ?= true",
        "LOCAL_ENABLE_SENSORS := false",
        "LOCAL_ENABLE_SENSORS ?= true",
    ):
        if token not in cuttlefish_patches:
            fail(f"Cuttlefish patch route is missing userdata quota policy: {token}")
    for token in (
        "verify-target-files.py",
        "verify-initramfs.py",
        "verify-orangefox-recovery.py",
        "ubd-disk.py",
        "--recovery-image",
        '--image "recovery=$RECOVERY_IMAGE"',
        "--blank-partition metadata",
        'TARGET_FILES_DIR=${TARGET_FILES%.zip}',
    ):
        if token not in assemble_image:
            fail(f"Android disk assembler is missing fixed policy: {token}")
    for token in (
        "ORANGEFOX_MANIFEST_REPOSITORY",
        "ORANGEFOX_MANIFEST_COMMIT",
        "ORANGEFOX_BOOTABLE_RECOVERY_REPOSITORY",
        "ORANGEFOX_VENDOR_RECOVERY_REPOSITORY",
        "ORANGEFOX_SE_OMAPI_REPOSITORY",
        "repo sync -c --detach",
        "resolved OrangeFox Repo manifest differs",
    ):
        if token not in prepare_orangefox:
            fail(f"OrangeFox source preparation is missing policy: {token}")
    for token in (
        "verify-orangefox-source.py",
        "sync-orangefox-device.py",
        "am --committer-date-is-author-date",
    ):
        if token not in apply_orangefox:
            fail(f"OrangeFox patch replay is missing policy: {token}")
    for token in (
        "verify-orangefox-source.py",
        "verify-orangefox-recovery.py",
        "verify-kernel-elf.py",
        "BOARD_EXCLUDE_KERNEL_FROM_RECOVERY_IMAGE",
        "TARGET_MAX_PAGE_SIZE_SUPPORTED",
        "recoveryimage",
    ):
        if token not in build_orangefox:
            fail(f"OrangeFox builder is missing fixed policy: {token}")
    for token in (
        "kernel_size != 0",
        "gzip.GzipFile",
        "system/bin/recovery",
        "system/etc/recovery.fstab",
        "target ELF load alignment is below 16 KiB",
        "loadable-module payload remains",
    ):
        if token not in orangefox_image_verifier:
            fail(f"OrangeFox recovery verifier is missing policy: {token}")
    for token in (
        "verify-target-files.py",
        "verify-initramfs.py",
        "--mkbootfs",
        'TARGET_FILES_DIR=${TARGET_FILES%.zip}',
        "first_stage_ramdisk/system/etc/fstab.uml",
        "system/bin/modprobe",
        "system/bin/snapuserd",
    ):
        if token not in assemble_ramdisk:
            fail(f"Android ramdisk assembler is missing fixed policy: {token}")
    for token in (
        "command -v zip",
        "--kernel-elf",
        "verify-kernel-elf.py",
        "verify-target-files.py",
        "check_target_files_vintf",
        "target_files_intermediates",
        'install -m 0755 -- "$KERNEL_ELF" "$PRODUCT_OUT/kernel"',
        "SOONG_NINJA=ninja",
        "SOONG_BUILD_GRAPH",
        "reusing incremental AOSP output",
        "PARTITION_LAYOUT",
        "partition_size_from_layout",
        "TARGET_2ND_ARCH",
        "TARGET_CHECK_PREBUILT_MAX_PAGE_SIZE",
        "BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE",
        "BOARD_SYSTEM_EXTIMAGE_FILE_SYSTEM_TYPE",
        "BOARD_PRODUCTIMAGE_FILE_SYSTEM_TYPE",
        "BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE",
        "BOARD_ODMIMAGE_FILE_SYSTEM_TYPE",
        "BOARD_SYSTEMIMAGE_EROFS_BLOCKSIZE",
        "PRODUCT_DEFAULT_APEX_PAYLOAD_TYPE",
        "PRODUCT_COMPRESSED_APEX",
        "PRODUCT_USE_DYNAMIC_PARTITIONS",
        "PRODUCT_QUOTA_PROJID",
        "external_storage.projid.enabled=*",
        "PRODUCT_BUILD_SUPER_PARTITION",
        "PRODUCT_BUILD_SYSTEM_OTHER_IMAGE",
        "PRODUCT_VIRTUAL_AB_OTA",
        "AB_OTA_UPDATER",
        "TARGET_SKIP_OTA_PACKAGE",
        "BOARD_AVB_ENABLE",
        "BOARD_SUPER_PARTITION_SIZE",
        "AB_OTA_POSTINSTALL_CONFIG",
        "BOARD_SYSTEMIMAGE_PARTITION_SIZE",
        "BOARD_USERDATAIMAGE_PARTITION_SIZE",
        "ro.virtual_ab.*",
        "PRODUCT_BUILD_SYSTEM_DLKM_IMAGE",
        "PRODUCT_BUILD_VENDOR_DLKM_IMAGE",
        "PRODUCT_BUILD_ODM_DLKM_IMAGE",
        "BOARD_USES_SYSTEM_DLKMIMAGE",
        "BOARD_VENDOR_DLKMIMAGE_FILE_SYSTEM_TYPE",
        "AB_OTA_PARTITIONS",
        "BOARD_VENDOR_RAMDISK_KERNEL_MODULES",
        "BOARD_SYSTEM_KERNEL_MODULES",
        "TRUSTY_SYSTEM_VM",
        "com.android.hardware.gatekeeper.nonsecure",
        "com.android.hardware.keymint.rust_nonsecure",
        "dlkm_loader",
        "checkpoint_gc",
        "com.android.hardware.boot",
        "android.hardware.boot-service.default_recovery",
        "update_engine_sideload",
        "trusty_security_vm.elf",
        "android.hardware.sensors-service.multihal",
        "BOARD_USES_FULL_RECOVERY_IMAGE",
        "com.android.hardware.neuralnetworks",
        "com.android.hardware.threadnetwork",
        "com.android.hardware.uwb",
        "com.google.cf.bt",
        "com.google.cf.light",
        "com.google.cf.oemlock",
        "cuttlefish_sensor_injection",
        "target-files-package",
        "BOARD_INIT_BOOT_IMAGE_PARTITION_SIZE",
        "BOARD_RECOVERYIMAGE_PARTITION_SIZE",
        "PRODUCT_BUILD_RECOVERY_IMAGE",
        "BOARD_EXCLUDE_KERNEL_FROM_RECOVERY_IMAGE",
        "BOARD_INIT_BOOT_HEADER_VERSION",
        "BOARD_BOOT_HEADER_VERSION",
        "Jelly",
        "LatinIME",
        "QuickSearchBox",
        "ThemePicker",
        "BOARD_USES_SYSTEM_OTHER_ODEX",
        "DisablePreopt",
        "DisablePreoptBootImages",
        "OnlyPreoptArtBootImage",
        "HasSystemOther",
    ):
        if token not in build_aosp:
            fail(f"AOSP builder is missing fixed product policy: {token}")
    if re.search(
        r"^\s*(?:export\s+)?SOONG_INCREMENTAL_ANALYSIS=", build_aosp, re.MULTILINE
    ):
        fail("AOSP builder forces a Soong analysis mode instead of preserving reuse")
    for token in (
        "META/kernel_configs.txt",
        "META/kernel_version.txt",
        "policycap memfd_class",
        "VENDOR/etc/vintf/manifest.xml",
        "VENDOR/etc/vintf/compatibility_matrix.xml",
        "VENDOR/etc/audio_effects_config.xml",
        "VENDOR/apex/com.android.hardware.boot.apex",
        "VENDOR/apex/com.android.hardware.neuralnetworks.apex",
        "VENDOR/apex/com.android.hardware.threadnetwork.apex",
        "VENDOR/apex/com.android.hardware.uwb.apex",
        "VENDOR/apex/com.google.cf.bt.apex",
        "VENDOR/apex/com.google.cf.light.apex",
        "VENDOR/apex/com.google.cf.oemlock.apex",
        "VENDOR/apex/com.google.cf.wifi.apex",
        "VENDOR/apex/com.google.cf.wpa_supplicant.apex",
        "VENDOR/etc/permissions/android.hardware.wifi.xml",
        "VENDOR/overlay/CuttlefishWifiOverlay.apk",
        "VENDOR/bin/hw/android.hardware.sensors-service.multihal",
        "RECOVERY/RAMDISK/odm_dlkm",
        "RECOVERY/RAMDISK/vendor_dlkm/etc",
        "VENDOR/etc/recovery.img",
        "SYSTEM/vendor/etc/recovery.img",
        "apex_payload.img",
        "EROFS APEX payload is forbidden",
        "compressed APEX is forbidden",
        "SYSTEM_OTHER",
        "REQUIRED_PREOPT_APKS",
        "application dexpreopt artifact lacks its pair",
    ):
        if token not in target_files_verifier:
            fail(f"target-files verifier is missing fixed policy: {token}")
    for token in (
        "verify-aosp-patches.py",
        "verify-jelly-source.py",
        "verify-orangefox-source.py",
        "verify-series-policy.py",
        "verify-kernel-elf.py",
        "verify-target-files.py",
        "ubd-disk.py",
        "verify_disk_boot_payloads",
        "check_no_module_output",
        "verify_kernel_config",
        'parser.add_argument("--clang"',
        'parser.add_argument("--linker"',
        "enforce_tracked_policy=True",
        "release-manifest.json",
        "android-17-uml-arm64.img.zst",
        "jelly_patch_manifest_sha256",
        "orangefox_projects_manifest_sha256",
        "orangefox-repo-manifest.xml",
        '"jelly"',
        "repo",
        "manifest",
        '"-r"',
        "os.replace",
    ):
        if token not in publish_release:
            fail(f"Android release publisher is missing fixed policy: {token}")

    command_line = (android_root / "support/boot/kernel-command-line.txt").read_text()
    for token in (
        "init=/init",
        "console=ttyS0",
        "androidboot.console=hvc0",
        "androidboot.fstab_suffix=uml",
        "androidboot.boot_devices=uml-blkdev.0",
        "androidboot.selinux=enforcing",
        "androidboot.serialconsole=1",
        "androidboot.hardware.gralloc=minigbm",
        "androidboot.hardware.hwcomposer=drm_hwcomposer",
        "androidboot.hardware.hwcomposer.display_finder_mode=drm",
        "androidboot.hardware.hwcomposer.display_framebuffer_format=rgba",
        "androidboot.hardware.egl=mesa",
        "androidboot.opengles.version=196608",
        "androidboot.cpuvulkan.version=0",
        "androidboot.hardware.guest_hwui_renderer=skiagl",
        "androidboot.hardware.guest_disable_renderer_preload=false",
        "androidboot.uml_bpf_interpreter=1",
    ):
        if token not in command_line.split():
            fail(f"Android kernel command line is missing fixed token: {token}")
    kernel_consoles = [
        token for token in command_line.split() if token.startswith("console=")
    ]
    if kernel_consoles != ["console=ttyS0"]:
        fail("Android kernel logs must be restricted to ttyS0")
    if "ubd0=" in command_line or "/home/" in command_line:
        fail("Android kernel command line stores a machine-specific disk path")
    if "androidboot.force_normal_boot" in command_line:
        fail("Android kernel command line overrides the selected ramdisk boot mode")
    if any(
        token.startswith("androidboot.lcd_density=")
        for token in command_line.split()
    ):
        fail("Android display density must remain a per-launch host argument")
    if any(
        token.startswith("androidboot.hardware.vulkan=")
        for token in command_line.split()
    ):
        fail("Android UML must not select or advertise a Vulkan implementation")
    if "androidboot.hw_timeout_multiplier=5" not in command_line.split():
        fail("Android hardware timeout multiplier is not calibrated to five")
    for token in (
        "config KERNARIUM_VIRTIO_BATTERY",
        "VIRTIO_ID_KERNARIUM_BATTERY",
        "65535",
        "devm_power_supply_register",
        "POWER_SUPPLY_PROP_CAPACITY",
        "KERNARIUM_BATTERY_STATUS_FULL",
    ):
        if token not in battery_patch:
            fail(f"ACK battery topic is missing fixed transport policy: {token}")
    for token in (
        "CONFIG_MODULES=n",
        "CONFIG_[A-Za-z0-9_]+=m$",
        "-name '*.ko'",
        "--page-size 16k",
        "verify-kernel-elf.py",
    ):
        if token not in build_kernel:
            fail(f"Android kernel build is missing fixed built-in policy: {token}")
    if "# CONFIG_MODULES is not set" not in kernel_config:
        fail("Android kernel fragment does not disable loadable modules")
    if "CONFIG_MSEAL_SYSTEM_MAPPINGS=y" not in kernel_config:
        fail("Android kernel fragment does not enable system mapping sealing")
    for symbol in (
        "CONFIG_BPF=y",
        "CONFIG_BPF_SYSCALL=y",
        "CONFIG_BLK_INLINE_ENCRYPTION=y",
        "CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK=y",
        "CONFIG_CGROUP_BPF=y",
        "CONFIG_CRYPTO_HCTR2=y",
        "CONFIG_FS_ENCRYPTION_INLINE_CRYPT=y",
        "CONFIG_DM_DEFAULT_KEY=y",
        "CONFIG_KERNARIUM_VIRTIO_BATTERY=y",
        "CONFIG_POWER_SUPPLY=y",
    ):
        if symbol not in kernel_config:
            fail(f"Android kernel fragment does not build in {symbol}")
    for symbol in (
        "CONFIG_KERNARIUM_VIRTIO_BATTERY",
        "CONFIG_POWER_SUPPLY",
    ):
        if symbol not in required_kernel_builtins:
            fail(f"Android kernel verifier does not require {symbol}")

    print("Android 17 builder lane ownership passed")


if __name__ == "__main__":
    main()
