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

# The engine probe: a freestanding armhf program (engine_probe/) that the
# AArch32 execution engines run as the guest's /init. Its C++ and assembly
# objects are compiled with the configured compiler's ARM backend and linked
# by link_elf32.py, because the toolchain has no ARM linker. Groups the engine
# does not yet implement are skipped inside the probe from the HWCAP words it
# sees, so one program serves every engine level. Every run counts SIGILLs.
set(_uml_a32_probe_directory "${CMAKE_CURRENT_SOURCE_DIR}/engine_probe")
set(_uml_a32_probe_objects "${CMAKE_CURRENT_BINARY_DIR}/engine_probe")
set(_uml_a32_probe_header "${_uml_a32_probe_directory}/engine_probe.hpp")
set(_uml_a32_probe_cxx_flags
    --target=armv7-unknown-linux-gnueabihf
    -mcpu=cortex-a15 -mfpu=none -mfloat-abi=soft
    -std=c++20 -O2 -Wall -Wextra -Werror
    -ffreestanding -fno-builtin -fno-exceptions -fno-rtti
    -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
    -fno-pic -fno-pie -fno-vectorize -fno-slp-vectorize
    -fno-threadsafe-statics)
set(_uml_a32_probe_base_asm_flags
    --target=armv7-unknown-linux-gnueabihf
    -mcpu=cortex-a15 -mfpu=none -mfloat-abi=soft -Wa,--fatal-warnings)
set(_uml_a32_probe_simd_asm_flags
    --target=armv7-unknown-linux-gnueabihf
    -mcpu=cortex-a15 -mfpu=neon-vfpv4 -mfloat-abi=hard -Wa,--fatal-warnings)
set(_uml_a32_probe_complete_asm_flags
    --target=armv8.2a-unknown-linux-gnueabihf
    -march=armv8.2-a+dotprod+fp16fml+crypto+crc+fp16+bf16+i8mm
    -mfloat-abi=hard -Wa,--fatal-warnings)

set(_uml_a32_probe_object_files)
foreach(_name IN ITEMS runtime groups basic interwork signals kuser tls
        cacheflush faults ill ptrace atomics absent simd complete)
    set(_source "${_uml_a32_probe_directory}/engine_probe_${_name}.cpp")
    set(_object "${_uml_a32_probe_objects}/${_name}.o")
    add_custom_command(
        OUTPUT "${_object}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_uml_a32_probe_objects}"
        COMMAND "${CMAKE_CXX_COMPILER}" ${_uml_a32_probe_cxx_flags}
            -c "${_source}" -o "${_object}"
        DEPENDS "${_source}" "${_uml_a32_probe_header}"
        COMMENT "Compiling the AArch32 engine probe: ${_name}"
        VERBATIM)
    list(APPEND _uml_a32_probe_object_files "${_object}")
endforeach()
# Assembly: the simd file needs VFP and NEON, the complete ones the Armv8.2
# extensions, and the rest nothing but the integer instruction set.
foreach(_name IN ITEMS start threads basic interwork signals kuser
        cacheflush faults ill ptrace atomics simd absent_complete complete)
    if(_name MATCHES "_complete$|^complete$")
        set(_flags ${_uml_a32_probe_complete_asm_flags})
    elseif(_name STREQUAL "simd")
        set(_flags ${_uml_a32_probe_simd_asm_flags})
    else()
        set(_flags ${_uml_a32_probe_base_asm_flags})
    endif()
    set(_source "${_uml_a32_probe_directory}/engine_probe_${_name}.S")
    set(_object "${_uml_a32_probe_objects}/${_name}-s.o")
    add_custom_command(
        OUTPUT "${_object}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_uml_a32_probe_objects}"
        COMMAND "${CMAKE_CXX_COMPILER}" ${_flags}
            -c "${_source}" -o "${_object}"
        DEPENDS "${_source}"
        COMMENT "Assembling the AArch32 engine probe: ${_name}"
        VERBATIM)
    list(APPEND _uml_a32_probe_object_files "${_object}")
endforeach()

set(UML_GUEST_AARCH32_ENGINE_PROBE
    "${CMAKE_CURRENT_BINARY_DIR}/guest-aarch32-engine-probe")
set(UML_GUEST_AARCH32_ENGINE_PROBE_4K
    "${CMAKE_CURRENT_BINARY_DIR}/guest-aarch32-engine-probe-4k")
add_custom_command(
    OUTPUT "${UML_GUEST_AARCH32_ENGINE_PROBE}"
        "${UML_GUEST_AARCH32_ENGINE_PROBE_4K}"
    COMMAND ${Python3_EXECUTABLE} -I
        "${CMAKE_CURRENT_SOURCE_DIR}/link_elf32.py"
        "${UML_GUEST_AARCH32_ENGINE_PROBE}" ${_uml_a32_probe_object_files}
    COMMAND ${Python3_EXECUTABLE} -I
        "${CMAKE_CURRENT_SOURCE_DIR}/link_elf32.py" --layout 4k
        "${UML_GUEST_AARCH32_ENGINE_PROBE_4K}" ${_uml_a32_probe_object_files}
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${UML_GUEST_AARCH32_ENGINE_PROBE}
        -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGeneratedFile.cmake
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${UML_GUEST_AARCH32_ENGINE_PROBE_4K}
        -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGeneratedFile.cmake
    DEPENDS ${_uml_a32_probe_object_files}
        "${CMAKE_CURRENT_SOURCE_DIR}/link_elf32.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/make_elf32.py"
    COMMENT "Linking the AArch32 engine probe"
    VERBATIM)
add_custom_target(uml-guest-aarch32-engine-probe
    DEPENDS "${UML_GUEST_AARCH32_ENGINE_PROBE}"
        "${UML_GUEST_AARCH32_ENGINE_PROBE_4K}")
add_dependencies(integration-test-binaries uml-guest-aarch32-engine-probe)

# Test support only: reports how much of the AArch32 profile each engine
# implements, as "gadgets=<level> jit=<level>".
add_executable(uml-a32-engine-support a32_engine_support.cpp)
target_include_directories(uml-a32-engine-support PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
target_link_libraries(uml-a32-engine-support PRIVATE UML::CpuContract)
add_dependencies(integration-test-binaries uml-a32-engine-support)

if(UML_GUEST_16K_PAGES)
    set(_uml_a32_page_size 16384)
else()
    set(_uml_a32_page_size 4096)
endif()
# guest-aarch32-engine-<engine> runs the probe under DARWIN_UML_GUEST_AARCH32=1
# on an engine that implements AArch32 and skips, naming the reason, on one
# that does not. The four-vCPU guest gives the exclusives real contention.
foreach(_engine IN ITEMS gadgets jit)
    if(_engine STREQUAL "gadgets")
        set(_timeout 2400)
    else()
        set(_timeout 1200)
    endif()
    uml_add_guest_integration_test(
        NAME guest-aarch32-engine-${_engine}
        SCRIPT run_guest_a32_engine.py
        ARGS --engine ${_engine}
            --probe ${UML_GUEST_AARCH32_ENGINE_PROBE}
            --probe-4k ${UML_GUEST_AARCH32_ENGINE_PROBE_4K}
            --page-size ${_uml_a32_page_size}
            --engine-support-tool $<TARGET_FILE:uml-a32-engine-support>
            --timeout ${_timeout}
        LABELS guest;aarch32;kernel;${_engine}
        TIMEOUT ${_timeout}
        CPUS 4)
    set_tests_properties(guest-aarch32-engine-${_engine}
        PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
