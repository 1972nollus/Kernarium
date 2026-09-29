# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_android_boot_tests android_boot_tests.cpp)
target_link_libraries(uml_android_boot_tests PRIVATE UML::AndroidBoot)
target_compile_options(uml_android_boot_tests PRIVATE -Wall -Wextra -Werror)
uml_add_unit_test(NAME android-partition-boot-selection
    COMMAND uml_android_boot_tests
    LABELS android;device)

add_executable(uml_boot_initramfs_tests boot_initramfs_tests.cpp)
target_link_libraries(uml_boot_initramfs_tests PRIVATE UML::BootInitramfs)
target_compile_options(uml_boot_initramfs_tests PRIVATE -Wall -Wextra -Werror)
uml_add_unit_test(NAME boot-initramfs-archive
    COMMAND uml_boot_initramfs_tests
    LABELS boot;device)
