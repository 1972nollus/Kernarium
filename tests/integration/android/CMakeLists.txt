# SPDX-License-Identifier: GPL-2.0-or-later

# The host's side of an Android guest's disks (android_disk_tool.cpp): the
# runner creates and checks the data disk with the library both launchers use.
add_executable(uml_android_disk_tool android_disk_tool.cpp)
target_link_libraries(uml_android_disk_tool PRIVATE UML::AndroidBoot)
target_compile_options(uml_android_disk_tool PRIVATE -Wall -Wextra -Werror)
target_compile_features(uml_android_disk_tool PRIVATE cxx_std_20)
add_dependencies(integration-test-binaries uml_android_disk_tool)

# A guest boots on the release's read-only system disk and a blank data disk
# the host created, then on the same data disk again.
uml_add_guest_integration_test(
    NAME guest-android-disks
    SCRIPT run_guest_android_disks.py
    ARGS --disk-tool $<TARGET_FILE:uml_android_disk_tool>
        --android-root ${PROJECT_SOURCE_DIR}/android/lineage-24
    LABELS guest;boot;android
    TIMEOUT 600
    CPUS 1)
