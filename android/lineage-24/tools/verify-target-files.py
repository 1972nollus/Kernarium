#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import io
import struct
import sys
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parent))

import android_image  # noqa: E402
from release_common import BUILD_VARIANTS, configured_build_variant, sha256_path  # noqa: E402


ELF_MAGIC = b"\x7fELF"
ELFCLASS64 = 2
ELFDATA2LSB = 1
ET_EXEC = 2
ET_DYN = 3
EM_AARCH64 = 183
MAX_PROGRAM_HEADERS_SIZE = 1 << 20
APEX_PAYLOAD_MAGIC_SIZE = 1024 + 0x38 + 2
EXT4_MAGIC_OFFSET = 1024 + 0x38
EXT4_MAGIC = b"\x53\xef"
EROFS_MAGIC_OFFSET = 1024
EROFS_MAGIC = b"\xe2\xe1\xf5\xe0"
# The Mesa Virgl GLES stack: the EGL and GLES libraries the platform loader
# finds through ro.hardware.egl, and the gallium driver they link. The boot
# command line's androidboot.hardware.egl must name the same suffix.
MESA_EGL_SUFFIX = "kernarium"
MESA_GALLIUM_DRIVER = "venus_gallium_dri.so"
MESA_EGL_DIRECTORY = "VENDOR/lib64/egl/"
MESA_EGL_ENTRIES = {
    f"{MESA_EGL_DIRECTORY}libEGL_{MESA_EGL_SUFFIX}.so",
    f"{MESA_EGL_DIRECTORY}libGLESv1_CM_{MESA_EGL_SUFFIX}.so",
    f"{MESA_EGL_DIRECTORY}libGLESv2_{MESA_EGL_SUFFIX}.so",
}
MESA_ELF_ENTRIES = {f"VENDOR/lib64/{MESA_GALLIUM_DRIVER}", *MESA_EGL_ENTRIES}
# The graphics HAL selections. The product init script copies each
# androidboot.hardware.<name> into ro.hardware.<name> at early-init, so the
# boot command line is each property's one writer and must name these values;
# a vendor property would make that setprop fail.
BOOT_HAL_SELECTIONS = {
    "egl": MESA_EGL_SUFFIX,
    "vulkan": "virtio",
    "gralloc": "minigbm",
}
# The retired AOSP external/mesa3d stack: its EGL and GLES libraries, and the
# directory it loaded its driver from.
RETIRED_EGL_ENTRIES = {
    f"{MESA_EGL_DIRECTORY}libGLES_mesa.so",
    f"{MESA_EGL_DIRECTORY}libEGL_mesa.so",
    f"{MESA_EGL_DIRECTORY}libGLESv1_CM_mesa.so",
    f"{MESA_EGL_DIRECTORY}libGLESv2_mesa.so",
}
RETIRED_DRI_DIRECTORY = "VENDOR/lib64/dri/"
REQUIRED_SDK_APEX_ENTRIES = {
    "SYSTEM/apex/com.android.bt.apex",
    "SYSTEM/apex/com.android.devicelock.apex",
    "SYSTEM/apex/com.android.nfcservices.apex",
    "SYSTEM/apex/com.android.rkpd.apex",
    "SYSTEM/apex/com.android.telephonycore.apex",
    "SYSTEM/apex/com.android.tethering.apex",
    "SYSTEM/apex/com.android.uwb.apex",
    "SYSTEM/apex/com.android.virt.apex",
    "SYSTEM/apex/com.android.wifi.apex",
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
    "PRODUCT/app/Camelot/Camelot.apk",
    "PRODUCT/app/Etar/Etar.apk",
    "PRODUCT/app/ExactCalculator/ExactCalculator.apk",
    "PRODUCT/app/F-Droid/F-Droid.apk",
    "PRODUCT/app/Glimpse/Glimpse.apk",
    "PRODUCT/app/Jelly/Jelly.apk",
    "PRODUCT/app/LatinIME/LatinIME.apk",
    "PRODUCT/app/Twelve/Twelve.apk",
    "PRODUCT/app/webview/webview.apk",
    "PRODUCT/etc/permissions/privapp-permissions-org.fdroid.fdroid.privileged.xml",
    "PRODUCT/priv-app/F-DroidPrivilegedExtension/F-DroidPrivilegedExtension.apk",
    "PRODUCT/etc/build.prop",
    "SYSTEM/build.prop",
    "RECOVERY/RAMDISK/system/bin/hw/android.hardware.health-service.example_recovery",
    "RECOVERY/RAMDISK/system/bin/recovery",
    "SYSTEM/bin/e2fsck",
    "SYSTEM/bin/resize2fs",
    "SYSTEM_EXT/priv-app/Launcher3QuickStep/Launcher3QuickStep.apk",
    "SYSTEM_EXT/priv-app/LineageParts/LineageParts.apk",
    "SYSTEM_EXT/priv-app/LineageSettingsProvider/LineageSettingsProvider.apk",
    "SYSTEM_EXT/priv-app/Seedvault/Seedvault.apk",
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
    "VENDOR/etc/ueventd.rc",
    "VENDOR/etc/vintf/compatibility_matrix.xml",
    "VENDOR/etc/vintf/manifest.xml",
    "VENDOR_BOOT/RAMDISK/first_stage_ramdisk/fstab.uml",
    "VENDOR/build.prop",
    *MESA_ELF_ENTRIES,
    *REQUIRED_SDK_APEX_ENTRIES,
}
REQUIRED_PREOPT_APKS = {
    "PRODUCT/app/Jelly/Jelly.apk",
    "PRODUCT/app/LatinIME/LatinIME.apk",
    "SYSTEM/app/CaptivePortalLogin/CaptivePortalLogin.apk",
    "SYSTEM/app/PartnerBookmarksProvider/PartnerBookmarksProvider.apk",
    "SYSTEM/app/PrintRecommendationService/PrintRecommendationService.apk",
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
    "SYSTEM_EXT/priv-app/Launcher3QuickStep/Launcher3QuickStep.apk",
    "SYSTEM_EXT/priv-app/LineageParts/LineageParts.apk",
    "SYSTEM_EXT/priv-app/Seedvault/Seedvault.apk",
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
    "PRODUCT/etc/init/init.rc",
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
    "VENDOR/etc/init/init_graphics.vendor.rc",
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
    "VENDOR/ueventd.rc",
    "SYSTEM/vendor/bin/install-recovery.sh",
    "SYSTEM/vendor/etc/recovery.img",
    "SYSTEM/vendor/etc/recovery-resource.dat",
    "SYSTEM/vendor/recovery-from-boot.p",
}
FORBIDDEN_APEX_FILENAMES = {
    "com.android.hardware.authsecret.apex",
    "com.android.hardware.cas.apex",
    "com.android.hardware.graphics.composer.ranchu.apex",
    "com.android.hardware.input.processor.apex",
    "com.android.hardware.net.nlinterceptor.apex",
    "com.android.hardware.rebootescrow.apex",
    "com.android.hardware.security.authgraph.apex",
    "com.android.hardware.security.secretkeeper.apex",
    "com.android.hardware.tetheroffload.apex",
    "com.android.hardware.thermal.apex",
    "com.android.hardware.usb.apex",
    "com.android.vndk.v31.apex",
    "com.android.vndk.v32.apex",
    "com.android.vndk.v33.apex",
    "com.android.vndk.v34.apex",
    "com.google.cf.confirmationui.apex",
    "com.google.cf.identity.apex",
    "com.google.cf.ir.apex",
}
FORBIDDEN_EXECUTABLE_FILENAMES = {
    "fastbootd",
    "gsi_tool",
    "gsid",
    "pmgd",
    "sgdisk",
    "snapshotctl",
    "snapuserd",
    "socket_vsock_proxy",
    "suspend_blocker",
    "tombstone_transmit",
    "update_engine",
    "update_engine_client",
    "update_verifier",
}
FORBIDDEN_APP_DIRECTORIES = {
    "Aperture",
    "AudioFX",
    "BasicDreams",
    "BluetoothMidiService",
    "Calendar",
    "Camera2",
    "Canvas",
    "Contacts",
    "CuttlefishService",
    "DeskClock",
    "Dialer",
    "DynamicSystemInstallationService",
    "EmergencyInfo",
    "Gallery2",
    "LineageSetupWizard",
    "Profiles",
    "QuickAccessWallet",
    "QuickSearchBox",
    "Recorder",
    "TeleService",
    "Telecom",
    "TelephonyProvider",
    "Updater",
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
    "exec -- /system/bin/resize2fs /dev/block/by-name/userdata",
    "mount_all --late",
    "ro.hardware.audio.primary",
    "setprop service.adb.tcp.port 5555",
    "start adbd",
)
# Only Kernarium passes a display density, so the product init script copies
# it inside an action gated on its presence; an unconditional copy fails on
# every standalone macOS boot.
UML_INIT_DENSITY_TRIGGER = "early-init && property:ro.boot.lcd_density=*"
UML_INIT_DENSITY_COPY = "setprop ro.sf.lcd_density ${ro.boot.lcd_density}"
# The inherited Cuttlefish graphics script also copied the goldfish composer
# mode and the ANGLE feature overrides; this product has neither, and their
# androidboot sources never exist.
UML_INIT_FORBIDDEN = (
    "ro.vendor.hwcomposer.mode",
    "debug.angle.",
    "metrics_helper",
    "rename_eth0",
    "setup_wifi",
    "socket_vsock_proxy",
    "/dev/hvc",
    "suspend_blocker",
    "/dev/block/by-name/custom",
)


def error(message: str) -> SystemExit:
    return SystemExit(f"Android target-files verifier: {message}")


def fail(message: str) -> None:
    raise error(message)


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
    if path.name in FORBIDDEN_APEX_FILENAMES and "apex" in path.parts:
        fail(f"unneeded APEX payload is forbidden: {path}")
    if path.name in FORBIDDEN_EXECUTABLE_FILENAMES:
        fail(f"unneeded static-layout executable is forbidden: {path}")
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


def init_actions(payload: str) -> dict[str, list[str]]:
    """Map each action trigger of an init script to its commands."""
    actions: dict[str, list[str]] = {}
    commands: list[str] | None = None
    for raw in payload.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        keyword, _, rest = line.partition(" ")
        if keyword == "on":
            commands = actions.setdefault(" ".join(rest.split()), [])
        elif keyword in ("service", "import"):
            commands = None
        elif commands is not None and raw[:1].isspace():
            commands.append(" ".join(line.split()))
    return actions


def check_boot_policy(
    archive: zipfile.ZipFile,
    infos: dict[str, zipfile.ZipInfo],
    build_variant: str,
) -> None:
    system_properties = read_small(
        archive, infos["SYSTEM/build.prop"]
    ).decode("utf-8", errors="strict").splitlines()
    system_property_map = {
        line.partition("=")[0]: line.partition("=")[2].strip()
        for line in system_properties
        if line.partition("=")[1]
    }
    for property_name, expected in BUILD_VARIANTS[build_variant].items():
        if system_property_map.get(property_name) != expected:
            fail(
                f"unexpected {build_variant}-build property {property_name}: "
                f"{system_property_map.get(property_name)!r}"
            )

    fstab = android_image.tracked_fstab(error)
    for entry in UML_FSTAB_ENTRIES:
        if read_small(archive, infos[entry]) != fstab:
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
    actions = init_actions(init_payload)
    early_init = actions.get("early-init", [])
    for name in sorted(BOOT_HAL_SELECTIONS):
        copy = f"setprop ro.hardware.{name} ${{ro.boot.hardware.{name}}}"
        if copy not in early_init:
            fail(f"UML init policy does not copy the {name} HAL selection at early-init")
    for trigger, commands in actions.items():
        if UML_INIT_DENSITY_COPY in commands and trigger != UML_INIT_DENSITY_TRIGGER:
            fail(f"UML init policy copies the display density under '{trigger}'")
    if UML_INIT_DENSITY_COPY not in actions.get(UML_INIT_DENSITY_TRIGGER, []):
        fail("UML init policy does not copy a host-supplied display density")

    ueventd_payload = read_small(
        archive, infos["VENDOR/etc/ueventd.rc"]
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


FDROID_PRIVILEGED_PERMISSIONS = [
    "android.permission.DELETE_PACKAGES",
    "android.permission.INSTALL_PACKAGES",
]


def check_fdroid(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The F-Droid Privileged Extension may install and delete packages only."""
    permissions = ET.fromstring(
        read_small(
            archive,
            infos[
                "PRODUCT/etc/permissions/"
                "privapp-permissions-org.fdroid.fdroid.privileged.xml"
            ],
        )
    )
    grants = {
        element.get("package"): sorted(
            permission.get("name") for permission in element.findall("permission")
        )
        for element in permissions.findall("privapp-permissions")
    }
    if grants != {"org.fdroid.fdroid.privileged": FDROID_PRIVILEGED_PERMISSIONS}:
        fail("the F-Droid Privileged Extension allowlist must be exactly "
             "INSTALL_PACKAGES and DELETE_PACKAGES")


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
    expected_properties = {
        "aaudio.mmap_policy": "1",
        "aaudio.mmap_exclusive_policy": "1",
        "ro.config.vc_call_vol_steps": "15",
        "ro.config.vc_call_vol_default": "15",
        "ro.config.media_vol_default": "15",
        "ro.config.alarm_vol_default": "7",
        "ro.config.system_vol_default": "7",
        "ro.config.assistant_vol_default": "15",
        "ro.product.product.model": "Darwin UML",
        "persist.sys.usb.config": "adb",
    }
    for property_name, expected in expected_properties.items():
        values = [
            line.partition("=")[2].strip()
            for line in product_properties
            if line.partition("=")[0] == property_name
        ]
        if values != [expected]:
            fail(f"unexpected product property {property_name}: {values}")


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

    entry_size = struct.unpack_from("<H", payload, 54)[0]
    if entry_size < 56:
        fail(f"malformed ELF program-header size: {info.filename}")
    android_image.check_load_alignment(info.filename, payload, error)


def check_mesa_gles_stack(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    for entry in sorted(MESA_ELF_ENTRIES):
        info = infos[entry]
        payload = read_elf_prefix(archive, info)
        if payload is None:
            fail(f"Mesa GLES component is not an ELF file: {entry}")
        elf_type, machine = struct.unpack_from("<HH", payload, 16)
        if elf_type != ET_DYN or machine != EM_AARCH64:
            fail(f"Mesa GLES component is not AArch64 ET_DYN: {entry}")
        check_target_elf(info, payload)
        if entry in MESA_EGL_ENTRIES:
            needed = android_image.elf_needed(info.filename, archive.read(info), error)
            if MESA_GALLIUM_DRIVER not in needed:
                fail(f"Mesa GLES component does not link the gallium driver: {entry}")

    for name in infos:
        if name in RETIRED_EGL_ENTRIES:
            fail(f"retired AOSP Mesa EGL or GLES library is forbidden: {name}")
        if name.startswith(RETIRED_DRI_DIRECTORY):
            fail(f"retired DRI driver payload is forbidden: {name}")

    vendor_properties = {
        line.partition("=")[0].strip()
        for line in read_small(archive, infos["VENDOR/build.prop"])
        .decode("utf-8", errors="strict")
        .splitlines()
    }
    for name in sorted(BOOT_HAL_SELECTIONS):
        if f"ro.hardware.{name}" in vendor_properties:
            fail(
                f"VENDOR/build.prop sets ro.hardware.{name}, which the boot "
                "command line owns"
            )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target-files", type=Path, required=True)
    parser.add_argument("--kernel-elf", type=Path, required=True)
    parser.add_argument("--kernel-config", type=Path, required=True)
    parser.add_argument("--kernel-release-file", type=Path, required=True)
    parser.add_argument(
        "--build-variant",
        choices=sorted(BUILD_VARIANTS),
        help="build variant the target files must carry (default: tracked metadata)",
    )
    args = parser.parse_args()
    build_variant = args.build_variant or configured_build_variant(
        Path(__file__).resolve().parents[1]
    )

    kernel_config = args.kernel_config
    kernel_release_file = args.kernel_release_file

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
        check_fdroid(archive, infos)
        check_audio_policy(archive, infos)
        check_mesa_gles_stack(archive, infos)
        check_boot_policy(archive, infos, build_variant)
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
