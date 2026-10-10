# SPDX-License-Identifier: GPL-2.0-or-later

# The guest kernel's AArch32 task support (run_guest_aarch32.py). The armhf
# program is built with the configured compiler's ARM backend and wrapped
# into an ELF32 executable by make_elf32.py, because the toolchain has no
# ARM linker. It is generated here and never committed.
#
# Until a host engine runs AArch32 the bridge reports no compat HWCAPs and the
# test expects the kernel to behave like a CPU without 32-bit EL0: ENOEXEC for
# the programs and EINVAL for personality(PER_LINUX32). A lane that lands an
# engine switches UML_AARCH32_EXPECT to run, which executes the program and
# checks that a program laid out for 4 KiB pages only runs on 4 KiB pages and
# fails to exec, cleanly, on 16 KiB pages.
set(UML_AARCH32_EXPECT "unsupported" CACHE STRING
    "What guest-aarch32 expects of the host: unsupported or run")
set_property(CACHE UML_AARCH32_EXPECT PROPERTY STRINGS unsupported run)

set(_uml_aarch32_object "${CMAKE_CURRENT_BINARY_DIR}/aarch32_probe.o")
set(UML_GUEST_AARCH32_PROBE "${CMAKE_CURRENT_BINARY_DIR}/guest-aarch32-probe")
set(UML_GUEST_AARCH32_PROBE_4K
    "${CMAKE_CURRENT_BINARY_DIR}/guest-aarch32-probe-4k")
# The same program twice: laid out for any page size, and for 4 KiB pages
# only, as 11 files of a stock Debian armhf image are.
add_custom_command(
    OUTPUT "${UML_GUEST_AARCH32_PROBE}" "${UML_GUEST_AARCH32_PROBE_4K}"
    COMMAND "${CMAKE_CXX_COMPILER}"
        --target=armv7-unknown-linux-gnueabihf
        -mfpu=neon-vfpv4
        -mfloat-abi=hard
        -c "${CMAKE_CURRENT_SOURCE_DIR}/aarch32_probe.S"
        -o "${_uml_aarch32_object}"
    COMMAND ${Python3_EXECUTABLE} -I
        "${CMAKE_CURRENT_SOURCE_DIR}/make_elf32.py"
        "${_uml_aarch32_object}" "${UML_GUEST_AARCH32_PROBE}"
    COMMAND ${Python3_EXECUTABLE} -I
        "${CMAKE_CURRENT_SOURCE_DIR}/make_elf32.py" --layout 4k
        "${_uml_aarch32_object}" "${UML_GUEST_AARCH32_PROBE_4K}"
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${UML_GUEST_AARCH32_PROBE}
        -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGeneratedFile.cmake
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${UML_GUEST_AARCH32_PROBE_4K}
        -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGeneratedFile.cmake
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/aarch32_probe.S"
        "${CMAKE_CURRENT_SOURCE_DIR}/make_elf32.py"
    COMMENT "Building the armhf AArch32 guest test programs"
    VERBATIM)
add_custom_target(uml-guest-aarch32-probe
    DEPENDS "${UML_GUEST_AARCH32_PROBE}" "${UML_GUEST_AARCH32_PROBE_4K}")
add_dependencies(integration-test-binaries uml-guest-aarch32-probe)

uml_add_guest_integration_test(
    NAME guest-aarch32-kernel
    SCRIPT run_guest_aarch32.py
    ARGS --probe ${CMAKE_CURRENT_SOURCE_DIR}/aarch32_guest_probe.py
        --elf ${UML_GUEST_AARCH32_PROBE}
        --elf-4k ${UML_GUEST_AARCH32_PROBE_4K}
        --expect ${UML_AARCH32_EXPECT}
    LABELS guest;kernel;aarch32;smoke
    TIMEOUT 420
    CPUS 2)
