#
# Copyright 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

PRODUCT_SHIPPING_API_LEVEL := 34

# Android 14's page-size-agnostic recovery build uses 64 KiB maximum ELF
# alignment. Those binaries run unchanged on the guest's 16 KiB ACK kernel.
PRODUCT_MAX_PAGE_SIZE_SUPPORTED := 65536
PRODUCT_NO_BIONIC_PAGE_SIZE_MACRO := true

# Virtio-GPU allocates legacy dumb buffers as host XRGB8888 resources before
# drmModeAddFB2 labels the framebuffer. OrangeFox's BGRA path writes the BGRX
# byte order expected by that fixed resource format.
TARGET_RECOVERY_PIXEL_FORMAT := BGRA_8888
TARGET_RECOVERY_FSTAB := device/kernarium/darwin/recovery/root/system/etc/recovery.fstab

TW_THEME := portrait_hdpi
# The emulator keeps userdata unencrypted, and early recovery cannot depend on
# the normal system's software KeyMint service. Keep the unused recovery crypto
# stack out of this image.
TW_NO_SCREEN_BLANK := true
TW_NO_SCREEN_TIMEOUT := true
TW_MAX_BRIGHTNESS := 255
TW_NO_HAPTICS := true
# The Darwin emulator has no USB gadget controller. OrangeFox's default MTP
# startup otherwise tears down the working TCP adbd transport before its
# FunctionFS open fails.
TW_EXCLUDE_MTP := true
TW_EXCLUDE_DEFAULT_USB_INIT := true
TW_EXCLUDE_APEX := true
TW_INCLUDE_NTFS_3G :=
TW_EXTRA_LANGUAGES := true
TW_USE_TOOLBOX := true
TW_INCLUDE_RESETPROP := true
TW_INCLUDE_LIBRESETPROP := true
TWRP_INCLUDE_LOGCAT := true
TARGET_USES_LOGD := true
TARGET_USES_MKE2FS := true
RECOVERY_SDCARD_ON_DATA := true

# OrangeFox 14.1's logd service requests ServiceCapacityLow during recovery.
# Supply the pinned platform process-group maps instead of relying on a
# device-specific ROM copy; without task_profiles.json logcat never starts.
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/recovery/root/init.recovery.cutf_cvm.rc:$(TARGET_COPY_OUT_RECOVERY)/root/init.recovery.cutf_cvm.rc \
    system/core/libprocessgroup/profiles/cgroups.recovery.json:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/cgroups.json \
    system/core/libprocessgroup/profiles/task_profiles.json:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/task_profiles.json

PRODUCT_SYSTEM_DEFAULT_PROPERTIES += \
    ro.adb.secure=0 \
    ro.debuggable=1 \
    service.adb.tcp.port=5555
