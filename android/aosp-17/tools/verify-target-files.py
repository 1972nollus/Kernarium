#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import io
import stat
import struct
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath


ELF_MAGIC = b"\x7fELF"
ELFCLASS64 = 2
ELFDATA2LSB = 1
ET_EXEC = 2
ET_DYN = 3
EM_AARCH64 = 183
PT_LOAD = 1
PT_DYNAMIC = 2
DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_STRSZ = 10
DT_RUNPATH = 29
PAGE_SIZE = 16384
MAX_PROGRAM_HEADERS_SIZE = 1 << 20
APEX_PAYLOAD_MAGIC_SIZE = 1024 + 0x38 + 2
EXT4_MAGIC_OFFSET = 1024 + 0x38
EXT4_MAGIC = b"\x53\xef"
EROFS_MAGIC_OFFSET = 1024
EROFS_MAGIC = b"\xe2\xe1\xf5\xe0"
VIRGL_ELF_ENTRIES = {
    "VENDOR/lib64/dri/gallium_dri.so",
    "VENDOR/lib64/egl/libEGL_mesa.so",
    "VENDOR/lib64/egl/libGLESv1_CM_mesa.so",
    "VENDOR/lib64/egl/libGLESv2_mesa.so",
}
VIRGL_DRI_SYMLINK = "VENDOR/lib64/dri/virtio_gpu_dri.so"
VIRGL_EGL_ENTRIES = {
    "VENDOR/lib64/egl/libEGL_mesa.so",
    "VENDOR/lib64/egl/libGLESv1_CM_mesa.so",
    "VENDOR/lib64/egl/libGLESv2_mesa.so",
}

REQUIRED_ENTRIES = {
    "BOOT/kernel",
    "INIT_BOOT/RAMDISK/init",
    "IMAGES/init_boot.img",
    "IMAGES/odm.img",
    "IMAGES/product.img",
    "IMAGES/recovery.img",
    "IMAGES/system.img",
    "IMAGES/system_ext.img",
    "IMAGES/userdata.img",
    "IMAGES/vendor.img",
    "META/init_boot_filesystem_config.txt",
    "META/kernel_configs.txt",
    "META/kernel_version.txt",
    "META/misc_info.txt",
    "META/ramdisk_node_list",
    "PRODUCT/app/Jelly/Jelly.apk",
    "PRODUCT/app/LatinIME/LatinIME.apk",
    "PRODUCT/etc/build.prop",
    "RECOVERY/RAMDISK/system/bin/hw/android.hardware.health-service.example_recovery",
    "RECOVERY/RAMDISK/system/bin/recovery",
    "SYSTEM_EXT/priv-app/ThemePicker/ThemePicker.apk",
    "VENDOR/bin/hw/android.hardware.health-service.example",
    "VENDOR/etc/audio_effects_config.xml",
    "VENDOR/etc/audio_policy_configuration.xml",
    "VENDOR/etc/audio_policy_volumes.xml",
    "VENDOR/etc/default_volume_tables.xml",
    "VENDOR/etc/fstab.uml",
    "VENDOR/etc/init/init.kernarium.rc",
    "VENDOR/etc/permissions/bluetooth_excluded_hardware.xml",
    "VENDOR/etc/permissions/handheld_core_hardware.xml",
    "VENDOR/etc/primary_audio_policy_configuration.xml",
    "VENDOR/etc/r_submix_audio_policy_configuration.xml",
    "VENDOR/etc/selinux/vendor_sepolicy.cil",
    "VENDOR/ueventd.rc",
    "VENDOR/etc/vintf/compatibility_matrix.xml",
    "VENDOR/etc/vintf/manifest.xml",
    "VENDOR_BOOT/RAMDISK/first_stage_ramdisk/fstab.uml",
    *VIRGL_ELF_ENTRIES,
    VIRGL_DRI_SYMLINK,
}
REQUIRED_PREOPT_APKS = {
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
}
DLKM_COMPONENTS = {"system_dlkm", "vendor_dlkm", "odm_dlkm"}
DLKM_COMPATIBILITY_ENTRIES = {
    "RECOVERY/RAMDISK/odm_dlkm",
    "RECOVERY/RAMDISK/odm_dlkm/etc",
    "RECOVERY/RAMDISK/system_dlkm",
    "RECOVERY/RAMDISK/vendor_dlkm",
    "RECOVERY/RAMDISK/vendor_dlkm/etc",
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
    "PRODUCT/app/QuickSearchBox/QuickSearchBox.apk",
    "PRODUCT/overlay/cuttlefish_overlay_uwb.apk",
    "SYSTEM_EXT/etc/permissions/privapp-permissions-com.android.threadnetwork.demoapp.xml",
    "SYSTEM_EXT/priv-app/ThreadNetworkDemoApp/ThreadNetworkDemoApp.apk",
    "SYSTEM/bin/wificond",
    "VENDOR/apex/com.android.hardware.boot.apex",
    "VENDOR/apex/com.android.hardware.contexthub.apex",
    "VENDOR/apex/com.android.hardware.neuralnetworks.apex",
    "VENDOR/apex/com.android.hardware.threadnetwork.apex",
    "VENDOR/apex/com.android.hardware.uwb.apex",
    "VENDOR/apex/com.android.hardware.wifi.apex",
    "VENDOR/apex/com.google.cf.bt.apex",
    "VENDOR/apex/com.google.cf.health.apex",
    "VENDOR/apex/com.google.cf.light.apex",
    "VENDOR/apex/com.google.cf.nfc.apex",
    "VENDOR/apex/com.google.cf.oemlock.apex",
    "VENDOR/apex/com.google.cf.rild.apex",
    "VENDOR/apex/com.google.cf.wifi.apex",
    "VENDOR/apex/com.google.cf.wpa_supplicant.apex",
    "VENDOR/bin/hw/android.hardware.sensors-service.multihal",
    "VENDOR/etc/bluetooth_with_le_audio_policy_configuration_7_0.xml",
    "VENDOR/etc/init/seriallogging.rc",
    "VENDOR/etc/permissions/android.hardware.uwb.xml",
    "VENDOR/etc/permissions/android.hardware.wifi.xml",
    "VENDOR/etc/recovery-resource.dat",
    "VENDOR/etc/sensors/hals.conf",
    "VENDOR/etc/vintf/manifest/android.hardware.sensors-multihal.xml",
    "VENDOR/lib64/hw/android.hardware.sensors@2.1-impl.cuttlefish.so",
    "VENDOR/overlay/CuttlefishTetheringOverlay.apk",
    "VENDOR/overlay/CuttlefishWifiOverlay.apk",
    "VENDOR/overlay/cuttlefish_overlay_uwb_gsi.apk",
    "VENDOR/bin/install-recovery.sh",
    "VENDOR/etc/recovery.img",
    "VENDOR/recovery-from-boot.p",
    "SYSTEM/vendor/bin/install-recovery.sh",
    "SYSTEM/vendor/etc/recovery.img",
    "SYSTEM/vendor/etc/recovery-resource.dat",
    "SYSTEM/vendor/recovery-from-boot.p",
}
FORBIDDEN_APP_DIRECTORIES = {
    "BasicDreams",
    "BluetoothMidiService",
    "Calendar",
    "Camera2",
    "Contacts",
    "DeskClock",
    "Dialer",
    "Gallery2",
    "QuickSearchBox",
    "TeleService",
    "Telecom",
    "TelephonyProvider",
    "messaging",
}
FORBIDDEN_FEATURE_DECLARATIONS = {
    "android.hardware.bluetooth.xml",
    "android.hardware.bluetooth_le.xml",
    "android.hardware.camera.concurrent.xml",
    "android.hardware.camera.flash-autofocus.xml",
    "android.hardware.camera.front.xml",
    "android.hardware.camera.full.xml",
    "android.hardware.camera.raw.xml",
    "android.hardware.fingerprint.xml",
    "android.hardware.location.gps.xml",
    "android.hardware.telephony.xml",
    "android.hardware.usb.accessory.xml",
    "android.hardware.usb.host.xml",
    "android.hardware.uwb.xml",
    "android.hardware.wifi.direct.xml",
    "android.hardware.wifi.passpoint.xml",
    "android.hardware.wifi.xml",
    "android.software.verified_boot.xml",
}
FORBIDDEN_FEATURE_NAMES = {
    "android.hardware.bluetooth",
    "android.hardware.bluetooth_le",
    "android.hardware.camera",
    "android.hardware.camera.any",
    "android.hardware.camera.autofocus",
    "android.hardware.camera.flash",
    "android.hardware.camera.front",
    "android.hardware.context_hub",
    "android.hardware.fingerprint",
    "android.hardware.location.gps",
    "android.hardware.microphone",
    "android.hardware.telephony",
    "android.hardware.usb.accessory",
    "android.hardware.usb.host",
    "android.hardware.uwb",
    "android.hardware.wifi.direct",
    "android.hardware.wifi.passpoint",
    "android.hardware.wifi",
}
UML_FSTAB = b"""# Fixed GPT partitions exposed by the Darwin UML UBD device.
/dev/block/by-name/system /system ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/system_ext /system_ext ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/product /product ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/vendor /vendor ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/odm /odm ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/metadata /metadata ext4 nodev,noatime,nosuid wait,check,formattable,first_stage_mount
/dev/block/by-name/metadata /metadata ext4 nodev,noatime,nosuid wait,check,first_stage_mount
# Static non-A/B storage uses neither userdata checkpointing nor BootControl.
/dev/block/by-name/userdata /data ext4 nodev,noatime,nosuid,errors=panic latemount,wait,check,formattable
/dev/block/by-name/userdata /data ext4 nodev,noatime,nosuid,errors=panic latemount,reservedsize=128M,wait,check
/dev/block/by-name/misc /misc emmc defaults defaults
"""
UML_FSTAB_ENTRIES = (
    "VENDOR/etc/fstab.uml",
    "VENDOR_BOOT/RAMDISK/first_stage_ramdisk/fstab.uml",
)
READ_ONLY_FILESYSTEMS = {
    "system_fs_type": "ext4",
    "system_ext_fs_type": "ext4",
    "product_fs_type": "ext4",
    "vendor_fs_type": "ext4",
    "odm_fs_type": "ext4",
}
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
    if path.parts[0] == "SYSTEM_OTHER":
        fail(f"system_other payload is forbidden: {path}")
    if canonical in FORBIDDEN_STATIC_ENTRIES:
        fail(f"nonstatic image or metadata is forbidden: {path}")
    if path.parts[:2] == ("OTA", "bin"):
        fail(f"OTA tool payload is forbidden: {path}")
    if path.parts[0] == "IMAGES" and path.name.endswith("_dlkm.img"):
        fail(f"DLKM image is forbidden: {path}")
    if path.name.startswith("fstab.cf.") and path.parts[0] in {"VENDOR", "VENDOR_BOOT"}:
        fail(f"Cuttlefish fstab is forbidden: {path}")
    if any(part in FORBIDDEN_APP_DIRECTORIES for part in path.parts):
        fail(f"unneeded built-in application payload is forbidden: {path}")
    if path.name in FORBIDDEN_FEATURE_DECLARATIONS and "permissions" in path.parts:
        fail(f"absent hardware feature declaration is forbidden: {path}")


def read_small(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> bytes:
    if info.is_dir() or info.file_size > 1 << 20:
        fail(f"policy file is missing or unexpectedly large: {info.filename}")
    return archive.read(info)


def check_apex_payload_filesystem(
    archive: zipfile.ZipFile, info: zipfile.ZipInfo
) -> None:
    try:
        with zipfile.ZipFile(io.BytesIO(archive.read(info))) as apex:
            payload = apex.getinfo("apex_payload.img")
            with apex.open(payload) as source:
                prefix = source.read(APEX_PAYLOAD_MAGIC_SIZE)
    except (KeyError, OSError, zipfile.BadZipFile) as error:
        fail(f"cannot inspect APEX payload {info.filename}: {error}")
    if prefix[EXT4_MAGIC_OFFSET : EXT4_MAGIC_OFFSET + len(EXT4_MAGIC)] == EXT4_MAGIC:
        return
    if prefix[EROFS_MAGIC_OFFSET : EROFS_MAGIC_OFFSET + len(EROFS_MAGIC)] == EROFS_MAGIC:
        fail(f"EROFS APEX payload is forbidden: {info.filename}")
    fail(f"APEX payload must use ext4: {info.filename}")


def check_boot_policy(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    for entry in UML_FSTAB_ENTRIES:
        if read_small(archive, infos[entry]) != UML_FSTAB:
            fail(f"UML fstab differs from the fixed UBD contract: {entry}")

    misc_info: dict[str, str] = {}
    for line in read_small(archive, infos["META/misc_info.txt"]).decode(
        "utf-8", errors="strict"
    ).splitlines():
        key, separator, value = line.partition("=")
        if separator:
            misc_info[key] = value
    for key, expected in READ_ONLY_FILESYSTEMS.items():
        if misc_info.get(key) != expected:
            fail(f"{key} must select read-only ext4 images")
    if "system_erofs_blocksize" in misc_info:
        fail("system EROFS image policy remains enabled")

    init_payload = read_small(
        archive, infos["VENDOR/etc/init/init.kernarium.rc"]
    ).decode("utf-8", errors="strict")
    for token in UML_INIT_REQUIRED:
        if token not in init_payload:
            fail(f"UML init policy is missing required action: {token}")
    for token in UML_INIT_FORBIDDEN:
        if token in init_payload:
            fail(f"UML init policy retains unsupported Cuttlefish action: {token}")

    ueventd_payload = read_small(
        archive, infos["VENDOR/ueventd.rc"]
    ).decode("utf-8", errors="strict")
    ueventd_entries = {
        line.strip()
        for line in ueventd_payload.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }
    for entry in (
        "/dev/ttyS0 0660 root shell",
        "/dev/hvc0 0660 root shell",
    ):
        if entry not in ueventd_entries:
            fail(f"UML ueventd policy is missing console entry: {entry}")
    for entry in ueventd_entries:
        device = entry.split(maxsplit=1)[0]
        if device.startswith("/dev/hvc") and device != "/dev/hvc0":
            fail(f"UML ueventd policy retains unsupported HVC device: {device}")
        for prefix in ("/dev/ion", "/dev/pmem", "/dev/vhci"):
            if device.startswith(prefix):
                fail(f"UML ueventd policy retains unsupported device: {device}")


def check_hardware_features(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    for name, info in infos.items():
        path = PurePosixPath(name)
        if (
            info.is_dir()
            or path.suffix != ".xml"
            or "permissions" not in path.parts
        ):
            continue
        try:
            root = ET.fromstring(read_small(archive, info))
        except ET.ParseError as error:
            fail(f"cannot parse hardware permission XML {name}: {error}")
        for element in root.iter():
            if element.tag.rsplit("}", 1)[-1] != "feature":
                continue
            feature = element.attrib.get("name")
            if feature in FORBIDDEN_FEATURE_NAMES:
                fail(f"absent hardware feature is advertised in {name}: {feature}")


def app_preopt_paths(apk: str) -> tuple[str, str]:
    apk_path = PurePosixPath(apk)
    artifact_base = apk_path.parent / "oat" / "arm64" / apk_path.stem
    return f"{artifact_base}.odex", f"{artifact_base}.vdex"


def check_app_dexpreopt(seen: set[str]) -> None:
    for apk in sorted(REQUIRED_PREOPT_APKS):
        if apk not in seen:
            fail(f"required preoptimized application is missing: {apk}")
        for artifact in app_preopt_paths(apk):
            if artifact not in seen:
                fail(f"required application dexpreopt artifact is missing: {artifact}")

    for canonical in sorted(seen):
        path = PurePosixPath(canonical)
        if path.suffix not in {".odex", ".vdex"}:
            continue
        if len(path.parts) < 6 or path.parts[1] not in {"app", "priv-app"}:
            continue
        if path.parts[-3] != "oat":
            continue
        if path.parts[-2] != "arm64":
            fail(f"application dexpreopt artifact is not ARM64: {path}")
        companion_suffix = ".vdex" if path.suffix == ".odex" else ".odex"
        companion = str(path.with_suffix(companion_suffix))
        if companion not in seen:
            fail(f"application dexpreopt artifact lacks its pair: {path}")


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


def check_audio_policy(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    main_name = "VENDOR/etc/audio_policy_configuration.xml"
    primary_name = "VENDOR/etc/primary_audio_policy_configuration.xml"
    try:
        main_root = ET.fromstring(read_small(archive, infos[main_name]))
        primary_payload = read_small(archive, infos[primary_name])
        primary_root = ET.fromstring(primary_payload)
    except ET.ParseError as error:
        fail(f"cannot parse UML audio policy: {error}")

    includes = {
        element.attrib["href"]
        for element in main_root.iter()
        if "href" in element.attrib
    }
    expected_includes = {
        "audio_policy_volumes.xml",
        "default_volume_tables.xml",
        "primary_audio_policy_configuration.xml",
        "r_submix_audio_policy_configuration.xml",
    }
    if includes != expected_includes:
        fail("UML audio policy does not select only primary PCM and remote submix")

    def elements(name: str) -> list[ET.Element]:
        return [
            element
            for element in primary_root.iter()
            if element.tag.rsplit("}", 1)[-1] == name
        ]

    mix_ports = elements("mixPort")
    profiles = elements("profile")
    device_ports = elements("devicePort")
    routes = elements("route")
    if len(mix_ports) != 1 or mix_ports[0].attrib != {
        "name": "primary output",
        "role": "source",
        "flags": "AUDIO_OUTPUT_FLAG_PRIMARY",
    }:
        fail("UML primary audio policy must expose one ordinary mixer output")
    if len(profiles) != 1 or profiles[0].attrib != {
        "name": "",
        "format": "AUDIO_FORMAT_PCM_16_BIT",
        "samplingRates": "48000",
        "channelMasks": "AUDIO_CHANNEL_OUT_STEREO",
    }:
        fail("UML primary audio profile must match the 48 kHz stereo Virtio device")
    if len(device_ports) != 1 or device_ports[0].attrib != {
        "tagName": "Speaker",
        "type": "AUDIO_DEVICE_OUT_SPEAKER",
        "role": "sink",
    }:
        fail("UML primary audio policy must expose only its speaker")
    if len(routes) != 1 or routes[0].attrib != {
        "type": "mix",
        "sink": "Speaker",
        "sources": "primary output",
    }:
        fail("UML primary audio route does not connect its mixer to the speaker")

    for forbidden in (
        b"AUDIO_DEVICE_IN_",
        b"AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD",
        b"AUDIO_OUTPUT_FLAG_DIRECT",
        b"AUDIO_OUTPUT_FLAG_MMAP_NOIRQ",
    ):
        if forbidden in primary_payload:
            fail(f"unsupported primary audio path remains enabled: {forbidden.decode()}")

    product_properties = read_small(
        archive, infos["PRODUCT/etc/build.prop"]
    ).decode("utf-8", errors="strict").splitlines()
    for property_name in ("aaudio.mmap_policy", "aaudio.mmap_exclusive_policy"):
        values = [
            line.partition("=")[2].strip()
            for line in product_properties
            if line.partition("=")[0] == property_name
        ]
        if values != ["1"]:
            fail(f"{property_name} must disable unsupported AAudio MMAP streams")


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


def elf_dynamic_strings(
    info: zipfile.ZipInfo, payload: bytes
) -> tuple[set[str], set[str]]:
    program_offset = struct.unpack_from("<Q", payload, 32)[0]
    entry_size, entry_count = struct.unpack_from("<HH", payload, 54)
    loads: list[tuple[int, int, int]] = []
    dynamic: tuple[int, int] | None = None
    for index in range(entry_count):
        offset = program_offset + index * entry_size
        (
            kind,
            _flags,
            file_offset,
            virtual_address,
            _physical_address,
            file_size,
            _memory_size,
            _alignment,
        ) = struct.unpack_from("<IIQQQQQQ", payload, offset)
        if file_offset + file_size > len(payload):
            fail(f"ELF segment extends beyond payload: {info.filename}")
        if kind == PT_LOAD:
            loads.append((file_offset, virtual_address, file_size))
        elif kind == PT_DYNAMIC:
            if dynamic is not None:
                fail(f"ELF has multiple dynamic segments: {info.filename}")
            dynamic = (file_offset, file_size)
    if dynamic is None:
        return set()

    dynamic_offset, dynamic_size = dynamic
    if dynamic_size % 16:
        fail(f"ELF dynamic segment is malformed: {info.filename}")
    string_address: int | None = None
    string_size: int | None = None
    needed_offsets: list[int] = []
    runpath_offsets: list[int] = []
    terminated = False
    for offset in range(dynamic_offset, dynamic_offset + dynamic_size, 16):
        tag, value = struct.unpack_from("<qQ", payload, offset)
        if tag == DT_NULL:
            terminated = True
            break
        if tag == DT_NEEDED:
            needed_offsets.append(value)
        elif tag == DT_STRTAB:
            string_address = value
        elif tag == DT_STRSZ:
            string_size = value
        elif tag == DT_RUNPATH:
            runpath_offsets.append(value)
    if not terminated:
        fail(f"ELF dynamic segment lacks DT_NULL: {info.filename}")
    if not needed_offsets and not runpath_offsets:
        return set(), set()
    if string_address is None or string_size is None:
        fail(f"ELF dynamic string table is incomplete: {info.filename}")

    string_offset: int | None = None
    for file_offset, virtual_address, file_size in loads:
        if virtual_address <= string_address < virtual_address + file_size:
            string_offset = file_offset + string_address - virtual_address
            break
    if string_offset is None or string_offset + string_size > len(payload):
        fail(f"ELF dynamic string table is outside a load segment: {info.filename}")
    string_table = payload[string_offset : string_offset + string_size]
    def decode(offset: int, label: str) -> str:
        if offset >= len(string_table):
            fail(f"ELF {label} offset is out of bounds: {info.filename}")
        end = string_table.find(b"\0", offset)
        if end < 0:
            fail(f"ELF {label} string is unterminated: {info.filename}")
        try:
            return string_table[offset:end].decode("utf-8", errors="strict")
        except UnicodeDecodeError as error:
            fail(f"ELF {label} string is not UTF-8: {info.filename}: {error}")

    needed = {decode(offset, "DT_NEEDED") for offset in needed_offsets}
    runpaths = {decode(offset, "DT_RUNPATH") for offset in runpath_offsets}
    return needed, runpaths


def check_virgl_stack(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    for entry in sorted(VIRGL_ELF_ENTRIES):
        info = infos[entry]
        payload = read_elf_prefix(archive, info)
        if payload is None:
            fail(f"Virgl userspace component is not an ELF file: {entry}")
        elf_type, machine = struct.unpack_from("<HH", payload, 16)
        if elf_type != ET_DYN or machine != EM_AARCH64:
            fail(f"Virgl userspace component is not AArch64 ET_DYN: {entry}")
        check_target_elf(info, payload)
        if entry in VIRGL_EGL_ENTRIES:
            needed, runpaths = elf_dynamic_strings(info, archive.read(info))
            if "gallium_dri.so" not in needed:
                fail(
                    "Virgl EGL component does not bind the packaged DRI driver: "
                    f"{entry}"
                )
            if "$ORIGIN/../dri" not in runpaths:
                fail(f"Virgl EGL component cannot locate its DRI driver: {entry}")

    link = infos[VIRGL_DRI_SYMLINK]
    mode = link.external_attr >> 16
    if not stat.S_ISLNK(mode):
        fail("Virtio-GPU DRI entry is not a symlink")
    if link.file_size > 256 or archive.read(link) != b"gallium_dri.so":
        fail("Virtio-GPU DRI symlink does not select gallium_dri.so")


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
    apex_count = 0
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
            if len(path.parts) >= 3 and path.parts[1] == "apex":
                if path.suffix == ".capex":
                    fail(f"compressed APEX is forbidden: {path}")
                if path.suffix == ".apex":
                    check_apex_payload_filesystem(archive, info)
                    apex_count += 1
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
        if apex_count == 0:
            fail("target files do not contain an APEX payload")
        check_app_dexpreopt(seen)
        check_hardware_features(archive, infos)
        check_audio_policy(archive, infos)
        check_virgl_stack(archive, infos)
        check_boot_policy(archive, infos)
        check_kernel_policy(
            archive, infos, kernel_config, kernel_release_file
        )
        if sha256_member(archive, infos["BOOT/kernel"]) != sha256_path(args.kernel_elf):
            fail("BOOT/kernel does not match the verified ACK UML ELF")

    print(
        f"verified AOSP 17 target files: {len(seen)} entries, "
        f"{elf_count} target executables, {apex_count} ext4 APEX payloads"
    )


if __name__ == "__main__":
    main()
