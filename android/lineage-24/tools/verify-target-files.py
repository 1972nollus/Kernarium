#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import io
import re
import struct
import sys
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parent))

import android_image  # noqa: E402
from release_common import BUILD_VARIANTS, configured_build_variant, sha256_path  # noqa: E402


ET_EXEC = 2
ET_DYN = 3
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
# Both userspace ABIs load the stack: a 64-bit application finds it in lib64,
# and an armeabi-v7a one in lib. The ABI directory decides the ELF class
# check_mesa_gles_stack requires.
MESA_ABI_DIRECTORIES = {"lib64": android_image.EM_AARCH64, "lib": android_image.EM_ARM}
# The hardware video codecs: the vendor Codec2 service of external/v4l2_codec2
# and the components it serves, two encoders and two decoders. The codec list
# names exactly those, beside the updates to the software decoders' limits. The
# properties enable those components, admit one instance per stream the host
# serves, and state the pixel rate the host sustains, which the service reports
# as its capacity and which no published performance point may exceed.
VIDEO_ENCODER_SERVICE = "android.hardware.media.c2-service-v4l2"
VIDEO_ENCODER_CODECS = {
    "c2.v4l2.avc.encoder": "video/avc",
    "c2.v4l2.hevc.encoder": "video/hevc",
}
VIDEO_ENCODER_PROPERTIES = {
    "ro.vendor.v4l2_codec2.encoder.supported.h264": "true",
    "ro.vendor.v4l2_codec2.encoder.supported.hevc": "true",
    "ro.vendor.v4l2_codec2.encode_concurrent_instances": "8",
    "ro.vendor.v4l2_codec2.video_pixel_rate": "108000000",
}
VIDEO_DECODER_CODECS = {
    "c2.v4l2.avc.decoder": "video/avc",
    "c2.v4l2.hevc.decoder": "video/hevc",
}
VIDEO_DECODER_PROPERTY_PREFIX = "ro.vendor.v4l2_codec2.decoder.supported."
VIDEO_DECODER_PROPERTIES = {
    "ro.vendor.v4l2_codec2.decoder.supported.h264": "true",
    "ro.vendor.v4l2_codec2.decoder.supported.hevc": "true",
    "ro.vendor.v4l2_codec2.decode_concurrent_instances": "4",
    "ro.vendor.v4l2_codec2.video_decode_pixel_rate": "456192000",
}
# CTS demands that the use of the largest published performance point be at least this share of
# the capacity (VideoEncoderAvailabilityTest and VideoDecoderAvailabilityTest, resource
# consumption for performance points), so a capacity cannot be far above what the codec publishes.
VIDEO_MIN_CAPACITY_SHARE = 0.6
VIDEO_ENCODER_UEVENTD = "/dev/video* 0660 system camera"
# The service reads this policy beside its default one. The poller of the V4L2
# device wakes its thread through an eventfd, which the default lacks.
VIDEO_ENCODER_SECCOMP_CALLS = ("eventfd2",)
# The crash handler of debuggerd runs in the service when it faults.
VIDEO_CRASH_HANDLER_SECCOMP_CALLS = ("getppid", "recvfrom", "setsockopt", "uname")
VIDEO_ENCODER_SECCOMP_DEFAULT = "android.hardware.media.c2-default-seccomp_policy"
VIDEO_ENCODER_SECCOMP_EXTENDED = "android.hardware.media.c2-extended-seccomp_policy"
MESA_EGL_ENTRIES = {
    f"VENDOR/{abi}/egl/lib{name}_{MESA_EGL_SUFFIX}.so"
    for abi in MESA_ABI_DIRECTORIES
    for name in ("EGL", "GLESv1_CM", "GLESv2")
}
MESA_ELF_ENTRIES = {
    *(f"VENDOR/{abi}/{MESA_GALLIUM_DRIVER}" for abi in MESA_ABI_DIRECTORIES),
    *MESA_EGL_ENTRIES,
}
# The Vulkan HAL and the buffer allocator and mapper Mesa queries through
# gralloc: same-process HALs of each ABI.
MESA_HAL_ENTRIES = {
    f"VENDOR/{abi}/hw/{module}.so"
    for abi in MESA_ABI_DIRECTORIES
    for module in ("vulkan.virtio", "gralloc.minigbm", "mapper.minigbm")
}
# A 32-bit library of the stack must be built with -D_FILE_OFFSET_BITS=64, or
# its mmap of a DRM offset above 4 GiB (the Venus renderer's, the buffer
# allocator's) passes a 32-bit off_t and fails with EINVAL. Bionic's headers
# then rename these calls to their 64 suffix. The rule covers this stack alone:
# the platform's own 32-bit libraries import plain mmap and lseek for
# offsets that never leave 32 bits.
LARGE_FILE_FUNCTIONS = ("mmap", "lseek")
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
    f"VENDOR/{abi}/egl/{name}.so"
    for abi in MESA_ABI_DIRECTORIES
    for name in ("libGLES_mesa", "libEGL_mesa", "libGLESv1_CM_mesa", "libGLESv2_mesa")
}
RETIRED_DRI_DIRECTORIES = tuple(f"VENDOR/{abi}/dri/" for abi in MESA_ABI_DIRECTORIES)
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

# The product runs 64-bit applications natively and armeabi-v7a ones beside
# them, never the older armeabi. The build writes each ABI list per partition
# (ro.<partition>.product.cpu.abilist*) and the runtime derives the unprefixed
# ones from them.
ABI_PROPERTY = re.compile(
    r"ro\.(?:(?:system|vendor|odm)\.product|product)\.cpu\.(abi|abi2|abilist|abilist32|abilist64)"
)
EXPECTED_ABIS = {
    "abi": "arm64-v8a",
    "abi2": "",
    "abilist": "arm64-v8a,armeabi-v7a",
    "abilist32": "armeabi-v7a",
    "abilist64": "arm64-v8a",
}
# Partitions whose build properties list the ABIs, with the properties a
# multilib product must carry in them.
ABI_LIST_PROPERTY_FILES = {
    "SYSTEM/build.prop": "system",
    "VENDOR/build.prop": "vendor",
    "ODM/etc/build.prop": "odm",
}
# What only the vendor properties describe: the second zygote that starts the
# 32-bit applications, the 64-bit compiler ART Service runs for both ISAs, and
# the second architecture's variant for dex2oat and bionic.
VENDOR_MULTILIB_PROPERTIES = {
    "ro.zygote": "zygote64_32",
    "dalvik.vm.dex2oat64.enabled": "true",
    "ro.bionic.2nd_arch": "arm",
    "ro.bionic.2nd_cpu_variant": "cortex-a53",
    "dalvik.vm.isa.arm.variant": "cortex-a53",
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
    "SYSTEM_EXT/bin/kernarium_wifi_network.sh",
    "SYSTEM_EXT/etc/init/kernarium_wifi_network.rc",
    "SYSTEM_EXT/etc/selinux/system_ext_file_contexts",
    "SYSTEM_EXT/etc/selinux/system_ext_property_contexts",
    "SYSTEM_EXT/etc/selinux/system_ext_sepolicy.cil",
    "SYSTEM_EXT/priv-app/ThemePicker/ThemePicker.apk",
    "SYSTEM/etc/selinux/plat_property_contexts",
    "SYSTEM/etc/selinux/plat_sepolicy.cil",
    "VENDOR/bin/hw/android.hardware.health-service.example",
    "VENDOR/bin/kernarium_net_setup",
    "VENDOR/bin/suspend_blocker",
    "VENDOR/etc/audio_effects_config.xml",
    "VENDOR/etc/audio_policy_configuration.xml",
    "VENDOR/etc/audio_policy_volumes.xml",
    "VENDOR/etc/default_volume_tables.xml",
    "VENDOR/etc/fstab.uml",
    "VENDOR/etc/init/init.kernarium.rc",
    f"VENDOR/bin/hw/{VIDEO_ENCODER_SERVICE}",
    f"VENDOR/etc/init/{VIDEO_ENCODER_SERVICE}.rc",
    "VENDOR/etc/media_codecs_c2.xml",
    f"VENDOR/etc/seccomp_policy/{VIDEO_ENCODER_SECCOMP_DEFAULT}",
    f"VENDOR/etc/seccomp_policy/{VIDEO_ENCODER_SECCOMP_EXTENDED}",
    f"VENDOR/etc/vintf/manifest/{VIDEO_ENCODER_SERVICE}.xml",
    "VENDOR/lib64/libc2plugin_store.so",
    "VENDOR/etc/permissions/bluetooth_excluded_hardware.xml",
    "VENDOR/etc/permissions/handheld_core_hardware.xml",
    "VENDOR/etc/primary_audio_policy_configuration.xml",
    "VENDOR/etc/r_submix_audio_policy_configuration.xml",
    "VENDOR/etc/selinux/vendor_file_contexts",
    "VENDOR/etc/selinux/vendor_property_contexts",
    "VENDOR/etc/selinux/vendor_sepolicy.cil",
    "VENDOR/etc/ueventd.rc",
    "VENDOR/etc/vintf/compatibility_matrix.xml",
    "VENDOR/etc/vintf/manifest.xml",
    "VENDOR_BOOT/RAMDISK/first_stage_ramdisk/fstab.uml",
    "VENDOR/build.prop",
    # The network setup tool's netlink library (Cuttlefish's).
    "VENDOR/lib64/cuttlefish_net.so",
    # The Wi-Fi module's overlay: virt_wifi cannot randomize its MAC.
    "VENDOR/overlay/KernariumWifiOverlay.apk",
    *ABI_LIST_PROPERTY_FILES,
    *MESA_ELF_ENTRIES,
    *MESA_HAL_ENTRIES,
    *REQUIRED_SDK_APEX_ENTRIES,
    # vold executes sgdisk to scan and partition any disk it manages; without
    # it every such operation fails with ENOENT.
    "SYSTEM/bin/sgdisk",
    # The 32-bit runtime the product adds beside the 64-bit one: the second
    # zygote's process, the ART boot image of its ISA, and the bootstrap
    # linker that starts it.
    "SYSTEM/bin/app_process32",
    "SYSTEM/bin/bootstrap/linker",
    "SYSTEM/bin/boringssl_self_test32",
    "SYSTEM/framework/arm/boot.oat",
    "SYSTEM/framework/arm/boot-framework.oat",
    "SYSTEM/etc/init/hw/init.zygote64_32.rc",
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
# The one application whose WebView renderer and in-process library run under
# either zygote: it carries libraries for both ABIs and declares multiArch.
REQUIRED_ARM_PREOPT_APKS = {
    "PRODUCT/app/webview/webview.apk",
}
APP_ISAS = {"arm64", "arm"}
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
TARGET_PARTITIONS = {"SYSTEM", "SYSTEM_EXT", "PRODUCT", "VENDOR", "ODM"}
# The one 32-bit executable that does not carry the 32 stem suffix of a binary
# built for both ABIs: the bootstrap linker that starts the first processes.
ELF32_BIN_EXCEPTIONS = {"SYSTEM/bin/bootstrap/linker"}
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
    "VENDOR/apex/com.google.cf.bt.apex",
    "VENDOR/apex/com.google.cf.health.apex",
    "VENDOR/apex/com.google.cf.light.apex",
    "VENDOR/apex/com.google.cf.nfc.apex",
    "VENDOR/apex/com.google.cf.oemlock.apex",
    "VENDOR/apex/com.google.cf.rild.apex",
    "VENDOR/apex/com.google.cf.wifi.apex",
    "VENDOR/bin/hw/android.hardware.sensors-service.multihal",
    "VENDOR/etc/bluetooth_with_le_audio_policy_configuration_7_0.xml",
    "VENDOR/etc/init/init_graphics.vendor.rc",
    "VENDOR/etc/init/seriallogging.rc",
    "VENDOR/etc/permissions/android.hardware.ethernet.xml",
    "VENDOR/etc/permissions/android.hardware.uwb.xml",
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
    "com.android.hardware.graphics.composer.ranchu.apex",
    "com.android.hardware.input.processor.apex",
    "com.android.hardware.net.nlinterceptor.apex",
    "com.android.hardware.rebootescrow.apex",
    "com.android.hardware.security.authgraph.apex",
    "com.android.hardware.security.secretkeeper.apex",
    "com.android.hardware.tetheroffload.apex",
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
    "snapshotctl",
    "snapuserd",
    "socket_vsock_proxy",
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
    "android.hardware.ethernet.xml",
    "android.hardware.fingerprint.xml",
    "android.hardware.location.gps.xml",
    "android.hardware.telephony.xml",
    "android.hardware.usb.accessory.xml",
    "android.hardware.usb.host.xml",
    "android.hardware.uwb.xml",
    "android.hardware.wifi.aware.xml",
    "android.hardware.wifi.direct.xml",
    "android.hardware.wifi.passpoint.xml",
    "android.hardware.wifi.rtt.xml",
    "android.software.verified_boot.xml",
}
# The Virtio sound capture stream is the product's microphone. A build
# fingerprint cannot report it only on some boots, so the declaration is
# permanent (CDD 7.8.1 C-0-7) and the audio policy carries its input.
# system_server starts the CompanionDeviceManager service only with
# android.software.companion_device_setup, which the handheld declaration the
# product replaces used to carry; the service runs without Bluetooth.
# The kernel's virt_wifi is a station, which is android.hardware.wifi and no more:
# there is no peer-to-peer, Aware or RTT, and Passpoint waits for the first Wi-Fi
# CTS run. android.hardware.ethernet is not declared, because eth0 is the management
# NIC and the framework must not manage it.
REQUIRED_FEATURE_NAMES = {
    "android.hardware.microphone",
    "android.hardware.wifi",
    "android.software.companion_device_setup",
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
    "android.hardware.ethernet",
    "android.hardware.fingerprint",
    "android.hardware.location.gps",
    "android.hardware.telephony",
    "android.hardware.usb.accessory",
    "android.hardware.usb.host",
    "android.hardware.uwb",
    "android.hardware.wifi.aware",
    "android.hardware.wifi.direct",
    "android.hardware.wifi.passpoint",
    "android.hardware.wifi.rtt",
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
VENDOR_SPL_APP_READ = re.compile(
    rb"^\(allow appdomain(?:_[0-9]+)? vendor_security_patch_level_prop(?:_[0-9]+)? "
    rb"\(file \([^)]*\bread\b[^)]*\)\)\)$",
    re.MULTILINE,
)
# The image's inode count, which the first-boot resize keeps per block group
# (BOARD_USERDATAIMAGE_EXTFS_INODE_COUNT in overlay/BoardConfig.mk).
USERDATA_EXTFS_INODE_COUNT = "8192"
# 3840x2160 at 30 frames per second, in the blocks that the codec limits count: HEVC counts
# 8x8 blocks (480 x 270 = 129,600, 3,888,000 per second), VP9 16x16 blocks (240 x 135 = 32,400).
MEDIA_C2_HEVC_BLOCKS_PER_SECOND = 129_600 * 30
MEDIA_C2_VP9_BLOCK_COUNT = 32_400
MEDIA_C2_SIZE = (3840, 2160)
# The ClearKey DRM HAL maps the shared buffers of applications, which are memfd files labelled with
# the application's domain on a kernel without ashmem (overlay/sepolicy/vendor/hal_drm_clearkey.te).
# The compiled rule names the application domains less the isolated ones through a generated
# attribute, a set expression of the form (and (appdomain) (not (isolated_app_all))).
CLEARKEY_MEMFD_RULE = re.compile(
    rb"^\(allow hal_drm_clearkey_aidl(?:_[0-9]+)? (\S+) \(memfd_file \(([^)]*)\)\)\)$",
    re.MULTILINE,
)
CLEARKEY_MEMFD_PERMISSIONS = frozenset({"getattr", "map", "read", "write"})
# The two example audio HAL processes share one domain and call each other's binder
# (overlay/sepolicy/vendor/hal_audio_default.te).
AUDIO_HAL_SELF_BINDER = re.compile(
    rb"^\(allow hal_audio_default(?:_[0-9]+)? hal_audio_default(?:_[0-9]+)? "
    rb"\(binder \([^)]*\bcall\b[^)]*\)\)\)$",
    re.MULTILINE,
)
# adbd reports its host in this property and init holds a userspace wakelock while
# it is 1 (adbd's p0011 topic, overlay/config/init.vendor.rc,
# overlay/sepolicy/system_ext/private). The property is system-owned: adbd is a core
# domain, which the platform neverallow keeps from setting a vendor-owned property.
ADB_HOST_PROPERTY = "sys.kernarium.adb.host_connected"
ADB_HOST_PROPERTY_TYPE = "kernarium_adb_host_prop"
CIL_PLATFORM_VERSION_SUFFIX = re.compile(r"_[0-9]{6}$")
CIL_PROPERTY_SET_RULE = re.compile(
    rb"^\(allow (\S+) (\S+) \(property_service \(([^)]*)\)\)\)$", re.MULTILINE
)
CIL_FILE_ALLOW_RULE = re.compile(
    rb"^\(allow (\S+) (\S+) \(file \(([^)]*)\)\)\)$", re.MULTILINE
)
CIL_TYPE_ATTRIBUTE_SET = re.compile(rb"^\(typeattributeset (\S+) \(([^()]*)\)\)$", re.MULTILINE)
INIT_PROPERTY_TRIGGER = re.compile(r"property:([^=\s&]+)=")
# The property_contexts of every partition, in the order libselinux merges them.
PROPERTY_CONTEXT_FILES = (
    "SYSTEM/etc/selinux/plat_property_contexts",
    "SYSTEM_EXT/etc/selinux/system_ext_property_contexts",
    "PRODUCT/etc/selinux/product_property_contexts",
    "VENDOR/etc/selinux/vendor_property_contexts",
    "ODM/etc/selinux/odm_property_contexts",
)
CIL_DONTAUDIT_RULE = re.compile(rb"^\(dontaudit (\S+) (\S+) \((\w+) \(([^)]*)\)\)\)$", re.MULTILINE)
CIL_ALLOW_RULE = re.compile(rb"^\(allow (\S+) (\S+) \((\w+) \(([^)]*)\)\)\)$", re.MULTILINE)
CIL_PERMISSIVE_TYPE = re.compile(rb"^\(typepermissive (\S+)\)$", re.MULTILINE)
# The only domain that may be permissive: the userdebug root shell.
PERMISSIVE_DOMAINS = frozenset({"su"})
SUSPEND_BLOCKER_SERVICE = ("suspend_blocker", "/vendor/bin/suspend_blocker")
# eth0 is the management NIC adb is reached on and the framework sees only the
# virtual Wi-Fi built on it. The tool that sets this up runs at post-fs-data, after
# init has limited every interface to answering ARP for its own addresses
# (overlay/config/init.vendor.rc, tools/kernarium_net_setup.cpp).
NET_SETUP_SERVICE = ("kernarium_net_setup", "/vendor/bin/kernarium_net_setup")
NET_SETUP_TRIGGER = "post-fs-data"
NET_SETUP_SYSCTLS = (
    "write /proc/sys/net/ipv4/conf/all/arp_ignore 1",
    "write /proc/sys/net/ipv4/conf/all/arp_announce 2",
)
# The Wi-Fi HAL and the supplicant keep their sockets in these directories, which Cuttlefish's
# own init script creates and this product's replaces.
WIFI_DATA_DIRECTORIES = (
    "mkdir /data/vendor/wifi 0770 wifi wifi",
    "mkdir /data/vendor/wifi/wpa 0770 wifi wifi",
    "mkdir /data/vendor/wifi/wpa/sockets 0770 wifi wifi",
)
NET_SETUP_TRANSITION = re.compile(
    rb"^\(typetransition init(?:_[0-9]+)? kernarium_net_setup_exec process kernarium_net_setup\)$",
    re.MULTILINE,
)
# After boot completes a script saves the virtual Wi-Fi network (VirtWifi, open) the
# first time only, in a core domain of its own on system_ext
# (overlay/tools/kernarium_wifi_network.sh, overlay/sepolicy/system_ext/private).
# It never turns Wi-Fi on or connects: the user's switch decides whether the saved
# network is joined, and a network the user forgets stays forgotten.
WIFI_NETWORK_SERVICE = ("kernarium_wifi_network", "/system_ext/bin/kernarium_wifi_network.sh")
WIFI_NETWORK_TRIGGER = "property:sys.boot_completed=1"
WIFI_NETWORK_MARKER = "persist.sys.kernarium.virtwifi_saved"
WIFI_NETWORK_MARKER_TYPE = "kernarium_wifi_network_prop"
WIFI_NETWORK_SCRIPT_REQUIRED = ("cmd wifi add-network VirtWifi open", WIFI_NETWORK_MARKER, "setprop")
WIFI_NETWORK_SCRIPT_FORBIDDEN = (
    "set-wifi-enabled",
    "connect-network",
    "settings put",
    "wifi_on",
    "svc wifi",
)
WIFI_NETWORK_TRANSITION = re.compile(
    rb"^\(typetransition init(?:_[0-9]+)? kernarium_wifi_network_exec process "
    rb"kernarium_wifi_network\)$",
    re.MULTILINE,
)
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
# The boot command line expedites RCU grace periods until boot completes; the
# product init script then restores normal ones.
UML_INIT_RCU_TRIGGER = "property:sys.boot_completed=1"
UML_INIT_RCU_RESTORE = "write /sys/kernel/rcu_expedited 0"
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
    if android_image.is_module_payload(basename):
        fail(f"kernel module or module metadata is forbidden: {path}")
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


# The platform's compiled policy (plat_sepolicy.cil) is 3.4 MB; the bound leaves
# room for it to grow about fivefold. The vendor and system_ext policies are
# a few hundred kilobytes and keep the default bound.
PLATFORM_POLICY_LIMIT = 16 << 20


def read_small(
    archive: zipfile.ZipFile, info: zipfile.ZipInfo, limit: int = 1 << 20
) -> bytes:
    if info.is_dir() or info.file_size > limit:
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


def init_services(payload: str) -> dict[str, tuple[str, list[str]]]:
    """Map each service of an init script to its executable and its option lines."""
    services: dict[str, tuple[str, list[str]]] = {}
    options: list[str] | None = None
    for raw in payload.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if fields[0] == "service" and len(fields) >= 3:
            options = []
            services[fields[1]] = (fields[2], options)
        elif fields[0] in ("on", "import"):
            options = None
        elif options is not None and raw[:1].isspace():
            options.append(" ".join(fields))
    return services


def check_suspend_blocker(init_payload: str, actions: dict[str, list[str]]) -> None:
    """The wakelock that keeps the guest awake runs exactly while an adb host is connected."""
    name, executable = SUSPEND_BLOCKER_SERVICE
    service = init_services(init_payload).get(name)
    if service is None or service[0] != executable:
        fail(f"UML init policy does not declare the {name} service at {executable}")
    if "disabled" not in service[1]:
        fail(f"UML init policy starts {name} with its class instead of on an adb host")
    for trigger, commands in actions.items():
        if f"start {name}" in commands and trigger != f"property:{ADB_HOST_PROPERTY}=1":
            fail(f"UML init policy starts {name} under '{trigger}'")
    if f"start {name}" not in actions.get(f"property:{ADB_HOST_PROPERTY}=1", []):
        fail(f"UML init policy does not start {name} while an adb host is connected")
    if f"stop {name}" not in actions.get(f"property:{ADB_HOST_PROPERTY}=0", []):
        fail(f"UML init policy does not stop {name} when the last adb host leaves")


def check_network_setup(init_payload: str, actions: dict[str, list[str]]) -> None:
    """init limits ARP, then waits for the tool that addresses eth0 and builds the Wi-Fi."""
    name, executable = NET_SETUP_SERVICE
    service = init_services(init_payload).get(name)
    if service is None or service[0] != executable:
        fail(f"UML init policy does not declare the {name} service at {executable}")
    for option in ("oneshot", "disabled"):
        if option not in service[1]:
            fail(f"UML init policy does not declare {name} as {option}")
    start = f"exec_start {name}"
    for trigger, commands in actions.items():
        if trigger != NET_SETUP_TRIGGER and (start in commands or f"start {name}" in commands):
            fail(f"UML init policy starts {name} under '{trigger}'")
    commands = actions.get(NET_SETUP_TRIGGER, [])
    if start not in commands:
        fail(f"UML init policy does not wait for {name} at {NET_SETUP_TRIGGER}")
    for sysctl in NET_SETUP_SYSCTLS:
        if sysctl not in commands or commands.index(sysctl) > commands.index(start):
            fail(f"UML init policy does not run '{sysctl}' before {name}")
    boot = actions.get("boot", [])
    for directory in WIFI_DATA_DIRECTORIES:
        if directory not in boot:
            fail(f"UML init policy does not run '{directory}' at boot")


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
        check_userdata_quota(read_small(archive, infos[entry]), entry)
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
    if misc_info.get("userdata_extfs_inode_count") != USERDATA_EXTFS_INODE_COUNT:
        fail(
            "userdata_extfs_inode_count must be "
            f"{USERDATA_EXTFS_INODE_COUNT}: {misc_info.get('userdata_extfs_inode_count')!r}"
        )
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
    if UML_INIT_RCU_RESTORE not in actions.get(UML_INIT_RCU_TRIGGER, []):
        fail("UML init policy does not restore normal RCU grace periods after boot")
    check_suspend_blocker(init_payload, actions)
    check_network_setup(init_payload, actions)
    check_wifi_network(
        read_small(archive, infos["SYSTEM_EXT/etc/init/kernarium_wifi_network.rc"]).decode(
            "utf-8", errors="strict"
        ),
        read_small(archive, infos["SYSTEM_EXT/bin/kernarium_wifi_network.sh"]),
    )
    check_media_codecs_c2(read_small(archive, infos["VENDOR/etc/media_codecs_c2.xml"]))

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


# The runtime resource overlay Soong generates from the product's SimpleDeviceConfig
# overlays, and the DeviceConfig key whose value lists the app ops recorded into the discrete
# history. The default ops are DiscreteOpsRegistry's LEGACY_OPS and IMPORTANT_OPS_FOR_SECURITY:
# LineageOS's list predates most of them, and an op that is not listed never reaches the history.
DEVICE_CONFIG_OVERLAY = (
    "PRODUCT/overlay/SimpleDeviceConfig__lineage_darwin__auto_generated_rro_product.apk"
)
DISCRETE_OPS_KEY = b"privacy/discrete_history_ops_cslist="
DISCRETE_OPS_DEFAULT = frozenset({
    0, 1, 2, 25, 26, 27, 41, 42, 63, 73, 88, 89, 100, 101,
    120, 136, 141, 147, 149, 150, 152, 170, 171, 172, 173,
})


def check_discrete_ops(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The discrete app-op history list must keep AOSP's default ops."""
    info = infos.get(DEVICE_CONFIG_OVERLAY)
    if info is None:
        fail(f"{DEVICE_CONFIG_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info))) as overlay:
            table = overlay.read("resources.arsc")
    except (KeyError, OSError, zipfile.BadZipFile) as error:
        fail(f"{DEVICE_CONFIG_OVERLAY} has no readable resource table: {error}")
    lists = [
        {int(op) for op in match.group(1).split(b",") if op}
        for match in re.finditer(re.escape(DISCRETE_OPS_KEY) + rb"([0-9,]+)", table)
    ]
    if not lists:
        fail(f"{DEVICE_CONFIG_OVERLAY} does not set {DISCRETE_OPS_KEY.decode()}")
    missing = min((sorted(DISCRETE_OPS_DEFAULT - ops) for ops in lists), key=len)
    if missing:
        fail(
            f"no discrete app-op list in {DEVICE_CONFIG_OVERLAY} keeps AOSP's default ops; "
            f"the closest lacks {', '.join(map(str, missing))}"
        )


# The AOSP example HALs the product keeps, each packaged as the vendor APEX that Cuttlefish
# installs. The APEX payload holds the service binary (a regular-file directory entry: name
# length, file type 1, name), the VINTF fragment that declares the AIDL interface, and the init
# script that starts the service. The init script names the binary too, so the entry is told
# apart from that.
APEX_PAYLOAD_READ_LIMIT = 64 << 20
# system_server's thermal manager and CTS NativeThermalTest need a thermal HAL.
THERMAL_HAL = {
    "apex": "VENDOR/apex/com.android.hardware.thermal.apex",
    "binary": b"android.hardware.thermal-service.example",
    "fragment": (
        b"<name>android.hardware.thermal</name>",
        b"<fqname>IThermal/default</fqname>",
    ),
    "interface": "IThermal/default",
    "service": "vendor.thermal-example",
    "need": "a thermal HAL",
}
# MediaCas and CTS MediaCasTest need the default CAS HAL, which loads the ClearKey CAS plugin
# (CAS system id 63192) from /vendor/lib*/mediacas.
CAS_HAL = {
    "apex": "VENDOR/apex/com.android.hardware.cas.apex",
    "binary": b"android.hardware.cas-service.example",
    "fragment": (
        b"<name>android.hardware.cas</name>",
        b"<fqname>IMediaCasService/default</fqname>",
    ),
    "interface": "IMediaCasService/default",
    "service": "vendor.cas-default",
    "need": "the default CAS HAL",
}
# The virtual Wi-Fi needs the Wi-Fi HAL and the supplicant, which Cuttlefish packages as vendor
# APEXes (the supplicant one holds hostapd too). The HAL APEX's init script names the binary
# below /apex/com.android.hardware.wifi.
WIFI_HAL = {
    "apex": "VENDOR/apex/com.android.hardware.wifi.apex",
    "binary": b"android.hardware.wifi-service",
    "fragment": (
        b"<name>android.hardware.wifi</name>",
        b"<fqname>IWifi/default</fqname>",
    ),
    "interface": "IWifi/default",
    "service": "vendor.wifi_hal_legacy",
    "need": "the Wi-Fi HAL",
}
WIFI_SUPPLICANT = {
    "apex": "VENDOR/apex/com.google.cf.wpa_supplicant.apex",
    "binary": b"wpa_supplicant",
    "fragment": (
        b"<name>android.hardware.wifi.supplicant</name>",
        b"<fqname>ISupplicant/default</fqname>",
    ),
    "interface": "ISupplicant/default",
    "service": "wpa_supplicant",
    "need": "the Wi-Fi supplicant",
}
CAS_PLUGINS = (
    "VENDOR/lib64/mediacas/libclearkeycasplugin.so",
    "VENDOR/lib/mediacas/libclearkeycasplugin.so",
)


def check_example_hal(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo], hal: dict
) -> None:
    """The vendor APEX must hold the example service, its VINTF fragment and its init script."""
    apex_name = hal["apex"]
    binary = hal["binary"]
    info = infos.get(apex_name)
    if info is None:
        fail(f"{apex_name} is missing: the product needs {hal['need']}")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, 16 << 20))) as apex:
            payload_info = apex.getinfo("apex_payload.img")
            if payload_info.file_size > APEX_PAYLOAD_READ_LIMIT:
                fail(f"{apex_name} has an unexpectedly large payload")
            payload = apex.read(payload_info)
    except (KeyError, OSError, zipfile.BadZipFile) as error:
        fail(f"cannot inspect {apex_name}: {error}")
    if bytes([len(binary), 1]) + binary not in payload:
        fail(f"{apex_name} does not hold the service binary bin/hw/{binary.decode()}")
    if not all(marker in payload for marker in hal["fragment"]):
        fail(f"{apex_name} does not declare {hal['interface']} in a VINTF fragment")
    apex_dir = PurePosixPath(apex_name).stem
    service = f"service {hal['service']} /apex/{apex_dir}/bin/hw/".encode() + binary
    if service not in payload:
        fail(f"{apex_name} does not start the {hal['service']} service")


def check_thermal_hal(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    check_example_hal(archive, infos, THERMAL_HAL)


def check_wifi_hals(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    check_example_hal(archive, infos, WIFI_HAL)
    check_example_hal(archive, infos, WIFI_SUPPLICANT)


def check_cas_hal(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    check_example_hal(archive, infos, CAS_HAL)
    for plugin in CAS_PLUGINS:
        if plugin not in infos:
            fail(f"the ClearKey CAS plugin is missing: {plugin}")


def check_hardware_features(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    declared: set[str] = set()
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
            if feature:
                declared.add(feature)
    for feature in sorted(REQUIRED_FEATURE_NAMES - declared):
        fail(f"required feature is not declared: {feature}")


def app_preopt_paths(apk: str, isa: str = "arm64") -> tuple[str, str]:
    apk_path = PurePosixPath(apk)
    artifact_base = apk_path.parent / "oat" / isa / apk_path.stem
    return f"{artifact_base}.odex", f"{artifact_base}.vdex"


def check_app_dexpreopt(seen: set[str]) -> None:
    for apk in sorted(REQUIRED_PREOPT_APKS):
        if apk not in seen:
            fail(f"required preoptimized application is missing: {apk}")
        for artifact in app_preopt_paths(apk):
            if artifact not in seen:
                fail(f"required application dexpreopt artifact is missing: {artifact}")
    # An application that carries libraries for both ABIs runs under either
    # zygote, so ART Service would compile its secondary ISA on first boot.
    for apk in sorted(REQUIRED_ARM_PREOPT_APKS):
        if apk not in seen:
            fail(f"required multi-ABI application is missing: {apk}")
        for artifact in app_preopt_paths(apk, "arm"):
            if artifact not in seen:
                fail(f"required 32-bit application dexpreopt artifact is missing: {artifact}")

    for canonical in sorted(seen):
        path = PurePosixPath(canonical)
        if path.suffix not in {".odex", ".vdex"}:
            continue
        if len(path.parts) < 6 or path.parts[1] not in {"app", "priv-app"}:
            continue
        if path.parts[-3] != "oat":
            continue
        if path.parts[-2] not in APP_ISAS:
            fail(f"application dexpreopt artifact is not for an ISA of this product: {path}")
        companion_suffix = ".vdex" if path.suffix == ".odex" else ".odex"
        companion = str(path.with_suffix(companion_suffix))
        if companion not in seen:
            fail(f"application dexpreopt artifact lacks its pair: {path}")
        if path.parts[-2] == "arm":
            # The secondary ISA only ever adds to the primary one.
            primary = path.parent.parent / "arm64" / path.name
            if str(primary) not in seen:
                fail(f"32-bit application dexpreopt artifact has no ARM64 one: {path}")


# The virtual Wi-Fi needs these built in: kernarium_net_setup stacks a macvlan on eth0 and a
# virt_wifi device on that, and a kernel without them leaves apps no network at all.
WIFI_KERNEL_OPTIONS = ("CONFIG_MACVLAN=y", "CONFIG_CFG80211=y", "CONFIG_VIRT_WIFI=y")


def check_kernel_policy(
    archive: zipfile.ZipFile,
    infos: dict[str, zipfile.ZipInfo],
    kernel_config: Path,
    kernel_release_file: Path,
) -> None:
    packaged_config = read_small(archive, infos["META/kernel_configs.txt"])
    if packaged_config != kernel_config.read_bytes():
        fail("packaged kernel configuration differs from the UML build")
    packaged_options = {
        line.strip() for line in packaged_config.decode("utf-8", errors="replace").splitlines()
    }
    for option in WIFI_KERNEL_OPTIONS:
        if option not in packaged_options:
            fail(
                f"packaged kernel configuration lacks {option}: the virtual Wi-Fi needs it, "
                "and without it apps have no network"
            )

    packaged_release = read_small(
        archive, infos["META/kernel_version.txt"]
    ).decode("utf-8", errors="strict").strip()
    kernel_release = kernel_release_file.read_text(
        encoding="utf-8", errors="strict"
    ).strip()
    if not kernel_release or packaged_release != kernel_release:
        fail("packaged kernel release differs from the UML build")

    vendor_policy = read_small(
        archive, infos["VENDOR/etc/selinux/vendor_sepolicy.cil"]
    )
    if b"(policycap memfd_class)" not in vendor_policy:
        fail("Android 17 vendor policy does not enable policycap memfd_class")
    # Applications must read the vendor patch level that LineageOS lets
    # system_server report to them (overlay/sepolicy/vendor/appdomain.te).
    if not VENDOR_SPL_APP_READ.search(vendor_policy):
        fail("vendor policy does not let applications read vendor_security_patch_level_prop")
    if not AUDIO_HAL_SELF_BINDER.search(vendor_policy):
        fail("vendor policy does not let the audio HAL processes call each other's binder")
    check_clearkey_memfd_policy(vendor_policy)
    check_network_setup_policy(
        vendor_policy, read_small(archive, infos["VENDOR/etc/selinux/vendor_file_contexts"])
    )
    platform_policy = read_small(
        archive, infos["SYSTEM/etc/selinux/plat_sepolicy.cil"], PLATFORM_POLICY_LIMIT
    )
    system_ext_policy = read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_sepolicy.cil"])
    check_wifi_network_policy(
        system_ext_policy,
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_file_contexts"]),
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_property_contexts"]).decode(
            "utf-8", errors="strict"
        ),
    )
    check_property_writers(
        platform_policy,
        vendor_policy,
        system_ext_policy,
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_property_contexts"]).decode(
            "utf-8", errors="strict"
        ),
    )
    check_policy_relaxations((platform_policy, vendor_policy, system_ext_policy))
    check_mediaserver_property_audit((platform_policy, vendor_policy, system_ext_policy))
    contexts = [
        line
        for name in PROPERTY_CONTEXT_FILES
        if name in infos
        for line in read_small(archive, infos[name], PLATFORM_POLICY_LIMIT)
        .decode("utf-8", errors="strict")
        .splitlines()
    ]
    check_vendor_init_triggers(
        init_actions(
            read_small(archive, infos["VENDOR/etc/init/init.kernarium.rc"]).decode(
                "utf-8", errors="strict"
            )
        ),
        contexts,
        (platform_policy, vendor_policy, system_ext_policy),
    )


def media_codec_limit(codec: ET.Element, name: str) -> ET.Element:
    limits = [limit for limit in codec.findall("Limit") if limit.get("name") == name]
    if len(limits) != 1:
        fail(f"media_codecs_c2.xml: {codec.get('name')} needs exactly one {name} limit")
    return limits[0]


def check_media_codecs_c2(payload: bytes) -> None:
    """The software HEVC and VP9 decoders must be allowed 3840x2160."""
    try:
        root = ET.fromstring(payload)
    except ET.ParseError as error:
        fail(f"media_codecs_c2.xml is not well-formed: {error}")
    decoders = {
        codec.get("name"): codec for codec in root.findall("./Decoders/MediaCodec")
    }
    for name in ("c2.android.hevc.decoder", "c2.android.vp9.decoder"):
        codec = decoders.get(name)
        if codec is None or codec.get("update") != "true":
            fail(f"media_codecs_c2.xml does not update {name}")

    def upper(limit: ET.Element) -> int:
        return int(limit.get("range", "0-0").split("-")[1])

    hevc_rate = upper(
        media_codec_limit(decoders["c2.android.hevc.decoder"], "blocks-per-second")
    )
    if hevc_rate < MEDIA_C2_HEVC_BLOCKS_PER_SECOND:
        fail(
            f"media_codecs_c2.xml allows the HEVC decoder only {hevc_rate} blocks per second; "
            f"3840x2160 at 30 frames per second needs {MEDIA_C2_HEVC_BLOCKS_PER_SECOND}"
        )
    vp9 = decoders["c2.android.vp9.decoder"]
    width, height = (int(side) for side in media_codec_limit(vp9, "size").get("max", "0x0").split("x"))
    if width < MEDIA_C2_SIZE[0] or height < MEDIA_C2_SIZE[1]:
        fail(f"media_codecs_c2.xml limits the VP9 decoder to {width}x{height}, below 3840x2160")
    vp9_blocks = upper(media_codec_limit(vp9, "block-count"))
    if vp9_blocks < MEDIA_C2_VP9_BLOCK_COUNT:
        fail(
            f"media_codecs_c2.xml allows the VP9 decoder {vp9_blocks} blocks, below "
            f"{MEDIA_C2_VP9_BLOCK_COUNT} for 3840x2160"
        )


def cil_attribute_members(policy: bytes, attribute: str) -> set[str]:
    """Types of a CIL attribute, platform version suffixes removed."""
    pattern = re.compile(
        rb"^\(typeattributeset " + re.escape(attribute.encode()) + rb" \(([^)]*)\)\)$",
        re.MULTILINE,
    )
    return {
        CIL_PLATFORM_VERSION_SUFFIX.sub("", name.decode())
        for match in pattern.finditer(policy)
        for name in match.group(1).split()
    }


def check_property_writers(
    platform_policy: bytes,
    vendor_policy: bytes,
    system_ext_policy: bytes,
    system_ext_contexts: str,
) -> None:
    """A core domain may set no vendor-owned property, and adbd reports its host.

    The first rule repeats the platform neverallow of private/property.te so a
    product rule that breaks it fails here and not only in the policy build.
    """
    core_domains = cil_attribute_members(platform_policy, "coredomain") - {"init"}
    vendor_properties = cil_attribute_members(
        vendor_policy, "vendor_property_type"
    ) | cil_attribute_members(system_ext_policy, "vendor_property_type")
    for label, policy in (("vendor", vendor_policy), ("system_ext", system_ext_policy)):
        for match in CIL_PROPERTY_SET_RULE.finditer(policy):
            source = CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode())
            target = CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode())
            if b"set" not in match.group(3).split():
                continue
            if source in core_domains and target in vendor_properties:
                fail(
                    f"{label} policy lets the core domain {source} set the vendor "
                    f"property type {target}; a core domain may set only a "
                    "system-owned property"
                )
    system_properties = cil_attribute_members(system_ext_policy, "system_property_type")
    if ADB_HOST_PROPERTY_TYPE not in system_properties:
        fail(f"system_ext policy does not define the system-owned {ADB_HOST_PROPERTY_TYPE}")
    allowed = False
    for match in CIL_PROPERTY_SET_RULE.finditer(system_ext_policy):
        if (
            match.group(1) == b"adbd"
            and match.group(2).decode() == ADB_HOST_PROPERTY_TYPE
            and b"set" in match.group(3).split()
        ):
            allowed = True
    if not allowed:
        fail(f"system_ext policy does not let adbd set {ADB_HOST_PROPERTY}")
    if not any(
        line.split()[:2] == [ADB_HOST_PROPERTY, f"u:object_r:{ADB_HOST_PROPERTY_TYPE}:s0"]
        for line in system_ext_contexts.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ):
        fail(f"system_ext property contexts do not label {ADB_HOST_PROPERTY}")


def check_clearkey_memfd_policy(vendor_policy: bytes) -> None:
    """The ClearKey HAL may use the memfd files of application domains, not of isolated apps."""
    for match in CLEARKEY_MEMFD_RULE.finditer(vendor_policy):
        permissions = set(match.group(2).decode().split())
        if not CLEARKEY_MEMFD_PERMISSIONS <= permissions:
            continue
        target = match.group(1).decode()
        expression = re.search(
            rb"^\(typeattributeset " + re.escape(target.encode())
            + rb" \(and \(appdomain(?:_[0-9]+)? ?\) \(not \(([^)]*)\) ?\)\)\)$",
            vendor_policy,
            re.MULTILINE,
        )
        if expression is not None and "isolated_app_all" in {
            CIL_PLATFORM_VERSION_SUFFIX.sub("", name)
            for name in expression.group(1).decode().split()
        }:
            return
    fail(
        "vendor policy does not let the ClearKey DRM HAL read, write and map the memfd files "
        "of application domains other than isolated apps"
    )


def file_context_label(contexts: bytes, path: str) -> str | None:
    """The context of the last file_contexts line whose expression matches |path|."""
    label = None
    for line in contexts.decode("utf-8", errors="strict").splitlines():
        fields = line.split()
        if len(fields) < 2 or fields[0].startswith("#"):
            continue
        try:
            matched = re.fullmatch(fields[0], path)
        except re.error:
            continue
        if matched:
            label = fields[-1]
    return label


def check_network_setup_policy(vendor_policy: bytes, vendor_file_contexts: bytes) -> None:
    """The network setup tool runs in its own domain, not in init's."""
    if file_context_label(vendor_file_contexts, NET_SETUP_SERVICE[1]) != (
        "u:object_r:kernarium_net_setup_exec:s0"
    ):
        fail("vendor file contexts do not label /vendor/bin/kernarium_net_setup")
    if not NET_SETUP_TRANSITION.search(vendor_policy):
        fail("vendor policy does not move init into the network setup domain")


def check_wifi_network(rc_payload: str, script: bytes) -> None:
    """The script that saves VirtWifi runs once, after boot, and leaves the Wi-Fi switch alone."""
    name, executable = WIFI_NETWORK_SERVICE
    service = init_services(rc_payload).get(name)
    if service is None or service[0] != executable:
        fail(f"system_ext init policy does not declare the {name} service at {executable}")
    for option in ("oneshot", "disabled"):
        if option not in service[1]:
            fail(f"system_ext init policy does not declare {name} as {option}")
    actions = init_actions(rc_payload)
    for trigger, commands in actions.items():
        if f"start {name}" in commands and trigger != WIFI_NETWORK_TRIGGER:
            fail(f"system_ext init policy starts {name} under '{trigger}'")
    if f"start {name}" not in actions.get(WIFI_NETWORK_TRIGGER, []):
        fail(f"system_ext init policy does not start {name} when boot completes")
    code = "\n".join(
        line
        for line in script.decode("utf-8", errors="strict").splitlines()
        if not line.lstrip().startswith("#")
    )
    for token in WIFI_NETWORK_SCRIPT_REQUIRED:
        if token not in code:
            fail(f"{executable} does not contain '{token}'")
    for token in WIFI_NETWORK_SCRIPT_FORBIDDEN:
        if token in code:
            fail(f"{executable} contains '{token}': the user's Wi-Fi switch is not the product's")


def check_wifi_network_policy(
    system_ext_policy: bytes, system_ext_file_contexts: bytes, system_ext_property_contexts: str
) -> None:
    """The script runs in its own core domain and may set only its own marker."""
    if file_context_label(system_ext_file_contexts, WIFI_NETWORK_SERVICE[1]) != (
        "u:object_r:kernarium_wifi_network_exec:s0"
    ):
        fail(f"system_ext file contexts do not label {WIFI_NETWORK_SERVICE[1]}")
    if not WIFI_NETWORK_TRANSITION.search(system_ext_policy):
        fail("system_ext policy does not move init into the Wi-Fi network domain")
    if WIFI_NETWORK_MARKER_TYPE not in cil_attribute_members(
        system_ext_policy, "system_property_type"
    ):
        fail(f"system_ext policy does not define the system-owned {WIFI_NETWORK_MARKER_TYPE}")
    if not any(
        match.group(1) == b"kernarium_wifi_network"
        and match.group(2).decode() == WIFI_NETWORK_MARKER_TYPE
        and b"set" in match.group(3).split()
        for match in CIL_PROPERTY_SET_RULE.finditer(system_ext_policy)
    ):
        fail(f"system_ext policy does not let the Wi-Fi network domain set {WIFI_NETWORK_MARKER}")
    if not any(
        CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) == "kernarium_wifi_network"
        and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()) == "system_server"
        and match.group(3) == b"binder"
        and b"transfer" in match.group(4).split()
        for match in CIL_ALLOW_RULE.finditer(system_ext_policy)
    ):
        fail(
            "system_ext policy does not let the Wi-Fi network domain transfer binders to "
            "system_server: cmd hands the Wi-Fi service the receiver of its result, and "
            "without the rule the network is never saved"
        )
    if not any(
        CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) == "system_server"
        and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()) == "kernarium_wifi_network"
        and match.group(3) == b"binder"
        and b"call" in match.group(4).split()
        for match in CIL_ALLOW_RULE.finditer(system_ext_policy)
    ):
        fail(
            "system_ext policy does not let system_server call the Wi-Fi network domain's "
            "binder: cmd waits for the receiver of its result, so without the rule it never "
            "returns and the script never sets its marker"
        )
    if not any(
        line.split()[:2] == [WIFI_NETWORK_MARKER, f"u:object_r:{WIFI_NETWORK_MARKER_TYPE}:s0"]
        for line in system_ext_property_contexts.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ):
        fail(f"system_ext property contexts do not label {WIFI_NETWORK_MARKER}")


def check_policy_relaxations(policies: tuple[bytes, ...]) -> None:
    """The policy holds none of the LineageOS grants that Android's neverallow tests refuse.

    LineageOS serves an ext4 SD card and addon.d updates with rules that the
    platform build tolerates only because its fork relaxed the matching
    neverallows; the CTS carries AOSP's. This product has neither feature, and
    its tree restores the neverallows (the system/sepolicy route), so the rules
    are gone. This repeats the checks on the built policy.
    """
    mls_trusted = set()
    for policy in policies:
        mls_trusted |= cil_attribute_members(policy, "mlstrustedsubject")
    if "mediaprovider_app" in mls_trusted:
        fail(
            "policy makes mediaprovider_app an mlstrustedsubject; Android's neverallow "
            "tests refuse it to fork and to modify privapp_data_file"
        )
    groups: dict[str, set[str]] = {}
    for policy in policies:
        for match in CIL_ALLOW_RULE.finditer(policy):
            source, target, object_class, permissions = (
                CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(index).decode())
                for index in (1, 2, 3, 4)
            )
            perms = set(permissions.split())
            if source == "mediaprovider_app" and target == "privapp_data_file":
                fail("policy lets mediaprovider_app use privapp_data_file beyond the app rules")
            if (
                source == "fsck_untrusted"
                and object_class in ("capability", "cap_userns")
                and "sys_admin" in perms
            ):
                fail("policy gives fsck_untrusted sys_admin; Android's neverallow tests refuse it")
            if object_class == "filesystem" and "relabelto" in perms:
                if target not in groups:
                    groups[target] = cil_types_and_attributes(policies, target)
                if "fs_type" in groups[target] and "contextmount_type" not in groups[target]:
                    fail(
                        f"policy lets {source} relabel a filesystem to {target}; only a "
                        "contextmount_type may be a relabelto target"
                    )
        for match in CIL_PERMISSIVE_TYPE.finditer(policy):
            domain = CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode())
            if domain not in PERMISSIVE_DOMAINS:
                fail(f"policy makes the domain {domain} permissive; only the userdebug su may be")


def check_mediaserver_property_audit(policies: tuple[bytes, ...]) -> None:
    """mediaserver's read of a vendor property is silenced as it is for the other graphics clients.

    The vendor's gralloc mapper, which mediaserver loads to allocate decoder
    buffers, reads vendor.minigbm.debug when it initialises. That name is a
    vendor_default_prop, which a core domain may not read, and the lookup
    carries on without a value. Without this rule every first use logs an
    audit denial that nothing needs to act on.
    """
    for policy in policies:
        for match in CIL_DONTAUDIT_RULE.finditer(policy):
            source, target, object_class, permissions = (
                CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(index).decode())
                for index in (1, 2, 3, 4)
            )
            if (
                source == "mediaserver"
                and target == "vendor_default_prop"
                and object_class == "file"
                and "read" in permissions.split()
            ):
                return
    fail(
        "policy does not silence the denial of mediaserver reading vendor_default_prop "
        "(overlay/sepolicy/system_ext/private/mediaserver.te)"
    )


def property_label(name: str, contexts: list[str]) -> str | None:
    """The SELinux type that property_contexts give a property: an exact entry, else the longest prefix."""
    best: tuple[int, str] | None = None
    for line in contexts:
        fields = line.split()
        if not fields or fields[0].startswith("#") or len(fields) < 2:
            continue
        label = fields[1].split(":")[2] if fields[1].count(":") >= 3 else None
        if label is None:
            continue
        if "exact" in fields[2:]:
            if fields[0] == name:
                return label
        elif name.startswith(fields[0]) and (best is None or len(fields[0]) > best[0]):
            best = (len(fields[0]), label)
    return best[1] if best else None


def cil_types_and_attributes(policies: tuple[bytes, ...], type_name: str) -> set[str]:
    """A type and every attribute that includes it."""
    names = {type_name}
    for policy in policies:
        for match in CIL_TYPE_ATTRIBUTE_SET.finditer(policy):
            members = {
                CIL_PLATFORM_VERSION_SUFFIX.sub("", member.decode())
                for member in match.group(2).split()
            }
            if type_name in members:
                names.add(CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()))
    return names


def check_vendor_init_triggers(
    actions: dict[str, list[str]], contexts: list[str], policies: tuple[bytes, ...]
) -> None:
    """Every property that a product init trigger names is readable by vendor_init.

    Init parses a vendor script in the vendor_init subcontext and drops a trigger
    whose property that domain may not read ("unexported property trigger"), so a
    service that only such a trigger starts never runs. The platform's
    neverallow keeps vendor_init off system_internal_prop types.
    """
    readers_of = cil_types_and_attributes(policies, "vendor_init")
    for trigger in actions:
        for name in INIT_PROPERTY_TRIGGER.findall(trigger):
            label = property_label(name, contexts) or "default_prop"
            readable = cil_types_and_attributes(policies, label)
            if not any(
                CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) in readers_of
                and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()) in readable
                and b"read" in match.group(3).split()
                for policy in policies
                for match in CIL_FILE_ALLOW_RULE.finditer(policy)
            ):
                fail(
                    f"the trigger 'on {trigger}' names {name}, labelled {label}, which "
                    "vendor_init may not read; init drops the trigger"
                )


# The virtio-gpu device paces the display with a vblank timer, so the present fences of
# drm_hwcomposer are reliable. The inherited Cuttlefish property marks them unreliable, which
# keeps SurfaceFlinger from taking present timestamps from them (service.sf.present_timestamp)
# and the Vulkan loader from exposing VK_GOOGLE_display_timing.
PRESENT_FENCE_PROPERTY = "ro.vendor.hwc.drm.present_fence_not_reliable"
FALSE_PROPERTY_VALUES = {"0", "n", "no", "off", "false"}
# Incremental installs (adb install --incremental, the Incremental FS service) need this
# vendor property and the kernel's incfs.
INCREMENTAL_PROPERTY = "ro.incremental.enable"


def parse_properties(payload: str) -> dict[str, str]:
    properties: dict[str, str] = {}
    for line in payload.splitlines():
        key, separator, value = line.partition("=")
        if separator and not key.startswith("#"):
            properties[key] = value.strip()
    return properties


def check_property_file(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> None:
    if info.file_size > 1 << 20:
        fail(f"property file is unexpectedly large: {info.filename}")
    payload = archive.read(info).decode("utf-8", errors="strict")
    for key, value in parse_properties(payload).items():
        if key.startswith("ro.virtual_ab."):
            fail(f"Virtual A/B property is forbidden in {info.filename}: {key}")
        if key == "ro.build.ab_update" and value == "true":
            fail(f"A/B update property is enabled in {info.filename}")
        abi_property = ABI_PROPERTY.fullmatch(key)
        if abi_property is not None:
            if "armeabi" in {item.strip() for item in value.split(",")}:
                fail(f"the legacy armeabi ABI is forbidden in {info.filename}: {key}={value}")
            expected = EXPECTED_ABIS[abi_property.group(1)]
            if value != expected:
                fail(f"unexpected ABI in {info.filename}: {key}={value!r}, not {expected!r}")
        if key == "ro.zygote" and value != VENDOR_MULTILIB_PROPERTIES["ro.zygote"]:
            fail(f"unexpected zygote in {info.filename}: {value}")
        if key == PRESENT_FENCE_PROPERTY and value.lower() not in FALSE_PROPERTY_VALUES:
            fail(
                f"{info.filename} marks the present fences unreliable: {key}={value}; "
                "the virtio-gpu display is paced by a vblank timer"
            )


def check_multilib_properties(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The partitions state both ABIs, and the vendor describes the second."""
    for name, partition in ABI_LIST_PROPERTY_FILES.items():
        properties = parse_properties(
            read_small(archive, infos[name]).decode("utf-8", errors="strict")
        )
        for suffix in ("abilist", "abilist32", "abilist64"):
            key = f"ro.{partition}.product.cpu.{suffix}"
            if properties.get(key) != EXPECTED_ABIS[suffix]:
                fail(f"{name} lacks {key}={EXPECTED_ABIS[suffix]}: {properties.get(key)!r}")
    vendor = parse_properties(
        read_small(archive, infos["VENDOR/build.prop"]).decode("utf-8", errors="strict")
    )
    for key, expected in VENDOR_MULTILIB_PROPERTIES.items():
        if vendor.get(key) != expected:
            fail(f"VENDOR/build.prop lacks {key}={expected}: {vendor.get(key)!r}")
    if vendor.get(INCREMENTAL_PROPERTY) != "1":
        fail(f"VENDOR/build.prop lacks {INCREMENTAL_PROPERTY}=1: {vendor.get(INCREMENTAL_PROPERTY)!r}")
    system = parse_properties(
        read_small(archive, infos["SYSTEM/build.prop"]).decode("utf-8", errors="strict")
    )
    if system.get("ro.product.cpu.abi") != EXPECTED_ABIS["abi"]:
        fail(f"SYSTEM/build.prop lacks ro.product.cpu.abi=arm64-v8a: {system.get('ro.product.cpu.abi')!r}")


def check_userdata_quota(fstab: bytes, label: str) -> None:
    """Every userdata line carries the fs_mgr `quota` flag.

    installd answers StorageStatsManager only from a /data device whose user quota
    is on, and fs_mgr's tune2fs step enables it only for an entry with the flag.
    """
    lines = [
        fields
        for fields in (line.split() for line in fstab.decode("utf-8", errors="strict").splitlines())
        if fields and not fields[0].startswith("#") and fields[0] == "/dev/block/by-name/userdata"
    ]
    if not lines:
        fail(f"UML fstab has no userdata entry: {label}")
    for fields in lines:
        flags = fields[4].split(",") if len(fields) > 4 else []
        if "quota" not in flags:
            fail(f"UML fstab userdata entry lacks the fs_mgr quota flag: {label}")


def check_video_encoders(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The codec list names the hardware encoders and decoders, and only they are enabled."""
    try:
        codecs = ET.fromstring(read_small(archive, infos["VENDOR/etc/media_codecs_c2.xml"]))
    except ET.ParseError as error:
        fail(f"VENDOR/etc/media_codecs_c2.xml is not valid XML: {error}")
    encoders = {
        codec.get("name"): codec.get("type")
        for codec in codecs.iterfind("./Encoders/MediaCodec")
    }
    if encoders != VIDEO_ENCODER_CODECS:
        fail(f"the Codec2 list must name exactly the hardware encoders: {encoders}")
    # A decoder is one of the hardware decoders, or an update to a software decoder's limits.
    hardware_decoders = {
        codec.get("name"): codec.get("type")
        for codec in codecs.iterfind("./Decoders/MediaCodec")
        if (codec.get("name") or "").startswith("c2.v4l2.")
    }
    if hardware_decoders != VIDEO_DECODER_CODECS:
        fail(f"the Codec2 list must name the hardware decoders: {hardware_decoders}")
    decoders = list(codecs.iterfind("./Decoders/MediaCodec"))
    if any(
        (codec.get("name") or "") not in VIDEO_DECODER_CODECS
        and (
            not (codec.get("name") or "").startswith("c2.android.") or codec.get("update") != "true"
        )
        for codec in decoders
    ) or len(list(codecs.iter("MediaCodec"))) != len(encoders) + len(decoders):
        fail("the Codec2 list names a codec outside the hardware codecs and the software updates")
    for codec in decoders:
        if codec.get("name") not in VIDEO_DECODER_CODECS:
            continue
        # CTS wants a decoder's width and height ranges to match, and the adaptive playback and
        # dynamic colour aspects features it implements to be published.
        limits = {limit.get("name"): limit for limit in codec.iterfind("./Limit")}
        size = limits.get("size")
        if size is None or "x" not in (size.get("min") or "") or "x" not in (size.get("max") or ""):
            fail(f"{codec.get('name')} publishes no size limit")
        features = {feature.get("name") for feature in codec.iterfind("./Feature")}
        for feature in ("adaptive-playback", "dynamic-color-aspects"):
            if feature not in features:
                fail(f"{codec.get('name')} does not publish {feature}")
    # CTS requires a hardware codec to publish what it sustains: the performance points that
    # the claims test reads and the measured frame rates that the performance tests check
    # against what they measure.
    hardware_codecs = list(codecs.iterfind("./Encoders/MediaCodec")) + [
        codec for codec in decoders if codec.get("name") in VIDEO_DECODER_CODECS
    ]
    for codec in hardware_codecs:
        limits = [limit.get("name", "") for limit in codec.iterfind("./Limit")]
        for prefix in ("performance-point-", "measured-frame-rate-"):
            if not any(name.startswith(prefix) for name in limits):
                fail(f"{codec.get('name')} publishes no {prefix}* limit: "
                     "a hardware codec states its performance")

    vendor = parse_properties(
        read_small(archive, infos["VENDOR/build.prop"]).decode("utf-8", errors="strict")
    )
    for key, expected in VIDEO_ENCODER_PROPERTIES.items():
        if vendor.get(key) != expected:
            fail(f"VENDOR/build.prop lacks {key}={expected}: {vendor.get(key)!r}")
    for key, expected in VIDEO_DECODER_PROPERTIES.items():
        if vendor.get(key) != expected:
            fail(f"VENDOR/build.prop lacks {key}={expected}: {vendor.get(key)!r}")
    # A codec cannot publish more than the device sustains: the service reports the pixel rate
    # as the capacity that real-time streams of its direction are counted against, and the
    # largest point must use a good share of it.
    for direction, group, key in (
        ("pixel rate", list(codecs.iterfind("./Encoders/MediaCodec")),
         "ro.vendor.v4l2_codec2.video_pixel_rate"),
        ("decode pixel rate",
         [codec for codec in decoders if codec.get("name") in VIDEO_DECODER_CODECS],
         "ro.vendor.v4l2_codec2.video_decode_pixel_rate"),
    ):
        capacity = int(vendor[key])
        largest = 0
        for codec in group:
            for limit in codec.iterfind("./Limit"):
                name = limit.get("name", "")
                if not name.startswith("performance-point-"):
                    continue
                try:
                    width, height = (int(side) for side in name.rpartition("-")[2].split("x"))
                    frame_rate = int((limit.get("range") or "").partition("-")[2])
                except ValueError:
                    fail(f"{codec.get('name')} publishes a malformed {name}: "
                         f"{limit.get('range')!r}")
                if width * height * frame_rate > capacity:
                    fail(f"{codec.get('name')} publishes {name} above the {direction} capacity "
                         f"{capacity}: {width * height * frame_rate}")
                largest = max(largest, width * height * frame_rate)
        if largest < capacity * VIDEO_MIN_CAPACITY_SHARE:
            fail(f"the largest performance point of the {direction} group uses {largest}, "
                 f"below {VIDEO_MIN_CAPACITY_SHARE:.0%} of the capacity {capacity}")
    for key, value in vendor.items():
        if (
            key.startswith(VIDEO_DECODER_PROPERTY_PREFIX)
            and key not in VIDEO_DECODER_PROPERTIES
            and value != "false"
        ):
            fail(f"VENDOR/build.prop enables a hardware decoder outside the approved set: {key}")

    ueventd = {
        line.strip()
        for line in read_small(archive, infos["VENDOR/etc/ueventd.rc"])
        .decode("utf-8", errors="strict")
        .splitlines()
    }
    if VIDEO_ENCODER_UEVENTD not in ueventd:
        fail(f"UML ueventd policy lacks the video nodes' group: {VIDEO_ENCODER_UEVENTD}")

    service_rc = read_small(archive, infos[f"VENDOR/etc/init/{VIDEO_ENCODER_SERVICE}.rc"])
    groups = {
        group
        for line in service_rc.decode("utf-8", errors="strict").splitlines()
        if line.split()[:1] == ["group"]
        for group in line.split()[1:]
    }
    if "camera" not in groups:
        fail("the video encoder service does not join the group that owns the video nodes")

    policy = read_small(
        archive, infos[f"VENDOR/etc/seccomp_policy/{VIDEO_ENCODER_SECCOMP_EXTENDED}"]
    ).decode("utf-8", errors="strict")
    allowed = {
        line.partition(":")[0].strip()
        for line in policy.splitlines()
        if not line.lstrip().startswith("#")
    }
    for call in VIDEO_ENCODER_SECCOMP_CALLS + VIDEO_CRASH_HANDLER_SECCOMP_CALLS:
        if call not in allowed:
            fail(f"the video codec seccomp policy does not allow {call}")

    contexts = read_small(archive, infos["VENDOR/etc/selinux/vendor_file_contexts"]).decode(
        "utf-8", errors="strict"
    )
    if not any(
        "c2-service-v4l2" in line and line.split()[-1:] == ["u:object_r:mediacodec_exec:s0"]
        for line in contexts.splitlines()
        if not line.lstrip().startswith("#")
    ):
        fail("the video encoder service is not labeled mediacodec_exec")


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
    if [port.attrib for port in mix_ports] != [
        {
            "name": "primary output",
            "role": "source",
            "flags": "AUDIO_OUTPUT_FLAG_PRIMARY",
        },
        {"name": "primary input", "role": "sink"},
    ]:
        fail("UML primary audio policy must expose one ordinary mixer output "
             "and one ordinary mixer input")
    if [profile.attrib for profile in profiles] != [
        {
            "name": "",
            "format": "AUDIO_FORMAT_PCM_16_BIT",
            "samplingRates": "48000",
            "channelMasks": "AUDIO_CHANNEL_OUT_STEREO",
        },
        {
            "name": "",
            "format": "AUDIO_FORMAT_PCM_16_BIT",
            "samplingRates": "48000",
            "channelMasks": "AUDIO_CHANNEL_IN_STEREO AUDIO_CHANNEL_IN_MONO",
        },
    ]:
        fail("UML primary audio profiles must match the 48 kHz Virtio playback "
             "and capture streams")
    if [port.attrib for port in device_ports] != [
        {
            "tagName": "Speaker",
            "type": "AUDIO_DEVICE_OUT_SPEAKER",
            "role": "sink",
        },
        {
            "tagName": "Built-In Mic",
            "type": "AUDIO_DEVICE_IN_BUILTIN_MIC",
            "address": "bottom",
            "role": "source",
        },
    ]:
        fail("UML primary audio policy must expose only its speaker and built-in microphone")
    if [route.attrib for route in routes] != [
        {"type": "mix", "sink": "Speaker", "sources": "primary output"},
        {"type": "mix", "sink": "primary input", "sources": "Built-In Mic"},
    ]:
        fail("UML primary audio routes do not connect the mixer to the speaker "
             "and the built-in microphone to the input mixer")
    if [item.text for item in elements("item")] != ["Speaker", "Built-In Mic"]:
        fail("UML primary audio policy must attach only its speaker and built-in microphone")
    if set(re.findall(rb"AUDIO_DEVICE_IN_[A-Z0-9_]+", primary_payload)) != {
        b"AUDIO_DEVICE_IN_BUILTIN_MIC"
    }:
        fail("UML primary audio policy must name no input device but its built-in microphone")

    for forbidden in (
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
        # The guest boots without a lock screen, and `locksettings set-disabled` can turn
        # it on: LockSettingsService stores this default when it creates its database.
        "ro.lockscreen.disable.default": "true",
        # Userdata is a plain ext4 mount of its partition, which the mount holds exclusively:
        # apexd's pinned APEX images (a dm-linear table over that partition) fail with EBUSY,
        # so every staged APEX install is rejected unless apexd stages through /data/app-staging.
        "apexd.config.use_fiemap": "false",
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
    """Return an ELF file's header and program-header table, or None for any
    other file."""
    if info.file_size < android_image.ELF32_HEADER_SIZE:
        return None
    with archive.open(info) as source:
        header = source.read(android_image.ELF64_HEADER_SIZE)
        if header[:4] != android_image.ELF_MAGIC:
            return None
        elf = android_image.elf_format(info.filename, header, error)
        program_offset, entry_size, entry_count = android_image.program_table(elf, header)
        program_end = program_offset + entry_size * entry_count
        if program_end > info.file_size or program_end > MAX_PROGRAM_HEADERS_SIZE:
            fail(f"malformed ELF program headers: {info.filename}")
        prefix = header
        if program_end > len(prefix):
            prefix += source.read(program_end - len(prefix))
        if len(prefix) < program_end:
            fail(f"truncated ELF program headers: {info.filename}")
        return prefix


def elf_directories(path: PurePosixPath) -> set[tuple[str, str]]:
    return set(zip(path.parts, path.parts[1:]))


def elf32_placement_ok(path: PurePosixPath) -> bool:
    """Whether a 32-bit ELF may sit at this target-files path.

    The ABI directories hold the libraries: lib in a partition, and lib/arm
    beside an application's own. ART writes its images and compiled code under
    arm directories. An executable built for both ABIs takes the stem suffix
    32, and the bootstrap linker is the one 32-bit executable without it.
    Anything else is a 32-bit daemon, which this product does not have.
    """
    parts = path.parts
    directories = elf_directories(path)
    if len(parts) > 1 and parts[1] == "lib":
        return True
    if ("lib", "arm") in directories or ("oat", "arm") in directories:
        return True
    if parts[1:3] == ("framework", "arm"):
        return True
    if len(parts) > 2 and parts[1] == "bin":
        return path.name.endswith("32") or str(path) in ELF32_BIN_EXCEPTIONS
    return False


def elf64_placement_ok(path: PurePosixPath) -> bool:
    """Whether a 64-bit ELF may sit at this target-files path: not where the
    32-bit libraries live."""
    return not (
        (len(path.parts) > 1 and path.parts[1] == "lib")
        or ("lib", "arm") in elf_directories(path)
    )


def check_target_elf(info: zipfile.ZipInfo, payload: bytes) -> None:
    elf = android_image.elf_format(info.filename, payload, error)
    if struct.unpack_from("<H", payload, 16)[0] not in (ET_EXEC, ET_DYN):
        return
    path = PurePosixPath(info.filename)
    if elf is android_image.ELF32:
        android_image.check_arm_elf_header(info.filename, payload, error)
        if not elf32_placement_ok(path):
            fail(f"32-bit ELF outside the 32-bit library and runtime paths: {info.filename}")
    else:
        android_image.check_aarch64_elf_header(info.filename, payload, error)
        if not elf64_placement_ok(path):
            fail(f"64-bit ELF in a 32-bit library directory: {info.filename}")
    android_image.check_load_alignment(info.filename, payload, error)


def check_large_file_imports(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo], entry: str
) -> None:
    """Refuse a 32-bit stack library that imports mmap or lseek without the 64
    suffix when the same library has a 64-bit build."""
    parts = PurePosixPath(entry).parts
    if parts[1] != "lib":
        return
    twin = str(PurePosixPath(parts[0], "lib64", *parts[2:]))
    if twin not in infos:
        return
    info = infos[entry]
    plain = sorted(
        set(LARGE_FILE_FUNCTIONS)
        & android_image.elf_imports(info.filename, archive.read(info), error)
    )
    if plain:
        fail(
            f"32-bit {entry} imports {', '.join(plain)} instead of "
            f"{', '.join(name + '64' for name in plain)}: it was built without "
            "-D_FILE_OFFSET_BITS=64"
        )


def check_mesa_gles_stack(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    for entry in sorted(MESA_ELF_ENTRIES | MESA_HAL_ENTRIES):
        info = infos[entry]
        payload = read_elf_prefix(archive, info)
        if payload is None:
            fail(f"Mesa component is not an ELF file: {entry}")
        elf_type, machine = struct.unpack_from("<HH", payload, 16)
        expected_machine = MESA_ABI_DIRECTORIES[PurePosixPath(entry).parts[1]]
        if elf_type != ET_DYN or machine != expected_machine:
            fail(
                "Mesa component is not "
                f"{'AArch64' if expected_machine == android_image.EM_AARCH64 else 'Arm'}"
                f" ET_DYN: {entry}"
            )
        check_target_elf(info, payload)
        check_large_file_imports(archive, infos, entry)
        if entry in MESA_EGL_ENTRIES:
            needed = android_image.elf_needed(info.filename, archive.read(info), error)
            if MESA_GALLIUM_DRIVER not in needed:
                fail(f"Mesa GLES component does not link the gallium driver: {entry}")

    for name in infos:
        if name in RETIRED_EGL_ENTRIES:
            fail(f"retired AOSP Mesa EGL or GLES library is forbidden: {name}")
        if name.startswith(RETIRED_DRI_DIRECTORIES):
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
        check_discrete_ops(archive, infos)
        check_thermal_hal(archive, infos)
        check_cas_hal(archive, infos)
        check_wifi_hals(archive, infos)
        check_audio_policy(archive, infos)
        check_multilib_properties(archive, infos)
        check_video_encoders(archive, infos)
        check_mesa_gles_stack(archive, infos)
        check_boot_policy(archive, infos, build_variant)
        check_kernel_policy(
            archive, infos, kernel_config, kernel_release_file
        )
        if sha256_member(archive, infos["BOOT/kernel"]) != sha256_path(args.kernel_elf):
            fail("BOOT/kernel does not match the verified UML ELF")

    print(
        f"verified AOSP 17 target files: {len(seen)} entries, "
        f"{elf_count} target executables, {apex_count} ext4 APEX payloads"
    )


if __name__ == "__main__":
    main()
