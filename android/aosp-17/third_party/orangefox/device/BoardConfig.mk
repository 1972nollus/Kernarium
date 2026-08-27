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

TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_ABI := arm64-v8a
TARGET_CPU_VARIANT := generic
TARGET_SUPPORTS_64_BIT_APPS := true
TARGET_IS_64_BIT := true

TARGET_BOARD_PLATFORM := uml
TARGET_BOOTLOADER_BOARD_NAME := uml
TARGET_NO_BOOTLOADER := true

# The ACK UML ELF is selected by Kernarium and never enters an Android boot
# container. OrangeFox therefore produces the same kernelless boot-image-v4
# contract as the stock recovery it replaces.
TARGET_KERNEL_ARCH := arm64
BOARD_KERNEL_IMAGE_NAME := Image
BOARD_KERNEL_PAGESIZE := 16384
BOARD_BOOT_HEADER_VERSION := 4
BOARD_EXCLUDE_KERNEL_FROM_RECOVERY_IMAGE := true
BOARD_MKBOOTIMG_ARGS += --header_version $(BOARD_BOOT_HEADER_VERSION)
TARGET_PREBUILT_KERNEL := $(DEVICE_PATH)/prebuilt/kernel

# OrangeFox injects two upstream arm64 prebuilts linked for 4 KiB pages. Boot
# image repacking is inapplicable because Kernarium keeps its signed ACK ELF
# outside Android partitions; toybox supplies ps. Remove only those payloads
# after OrangeFox stages its ramdisk so every executable can run on 16 KiB.
BOARD_RECOVERY_IMAGE_PREPARE = \
    $(DEVICE_PATH)/recovery/prepare-kernarium.sh $(TARGET_RECOVERY_ROOT_OUT)

BOARD_RECOVERYIMAGE_PARTITION_SIZE := 268435456
BOARD_FLASH_BLOCK_SIZE := 16384
BOARD_HAS_LARGE_FILESYSTEM := true
BOARD_SUPPRESS_SECURE_ERASE := true
BOARD_USES_RECOVERY_AS_BOOT := false
TARGET_NO_RECOVERY := false

TARGET_USERIMAGES_USE_EXT4 := true
BOARD_USES_METADATA_PARTITION := true

BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true
BUILD_BROKEN_DUP_RULES := true
BUILD_BROKEN_MISSING_REQUIRED_MODULES := true
