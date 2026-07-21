# SPDX-License-Identifier: GPL-2.0-or-later

set(UML_KERNEL_IMAGE "${PROJECT_SOURCE_DIR}/.build/kernel/vmlinux" CACHE FILEPATH
    "Linux UML kernel ELF embedded in the transitional macOS CLI")
set(UML_LTP_JOBS 16 CACHE STRING
    "Concurrent isolated guest workers for the host LTP regression suite")

find_program(GMAKE_EXECUTABLE gmake REQUIRED)

set(UML_KERNEL_BUILD_DIR "${PROJECT_SOURCE_DIR}/.build/kernel")
set(UML_KERNEL_DEFCONFIG
    "${PROJECT_SOURCE_DIR}/linux-6.18/arch/um/configs/arm64_defconfig")

add_custom_command(
    OUTPUT "${UML_KERNEL_BUILD_DIR}/.config"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${UML_KERNEL_BUILD_DIR}"
    COMMAND ${GMAKE_EXECUTABLE}
            O=${UML_KERNEL_BUILD_DIR}
            -C ${PROJECT_SOURCE_DIR}/linux-6.18
            ARCH=um SUBARCH=arm64 UML_HOST_OS=Darwin
            LLVM=/opt/homebrew/opt/llvm/bin/
            LD=/opt/homebrew/opt/lld/bin/ld.lld
            CROSS_COMPILE=aarch64-unknown-linux-gnu-
            arm64_defconfig
    DEPENDS "${UML_KERNEL_DEFCONFIG}"
    COMMENT "Refreshing the Darwin UML arm64 kernel configuration"
    VERBATIM)

add_custom_target(uml_kernel_config
    DEPENDS "${UML_KERNEL_BUILD_DIR}/.config")

add_custom_target(uml_kernel
    COMMAND ${GMAKE_EXECUTABLE} -j8
            O=${UML_KERNEL_BUILD_DIR}
            -C ${PROJECT_SOURCE_DIR}/linux-6.18
            ARCH=um SUBARCH=arm64 UML_HOST_OS=Darwin
            LLVM=/opt/homebrew/opt/llvm/bin/
            LD=/opt/homebrew/opt/lld/bin/ld.lld
            CROSS_COMPILE=aarch64-unknown-linux-gnu-
    BYPRODUCTS ${UML_KERNEL_IMAGE}
    USES_TERMINAL
    VERBATIM)
add_dependencies(uml_kernel uml_kernel_config)

find_program(DEBUGFS_EXECUTABLE debugfs
    HINTS /opt/homebrew/opt/e2fsprogs/sbin)
if(DEBUGFS_EXECUTABLE)
    add_custom_target(uml_rootfs_kernel_metadata
        COMMAND ${CMAKE_COMMAND}
            -DROOTFS=${PROJECT_SOURCE_DIR}/rootfs.ext2
            -DKERNEL_BUILD=${PROJECT_SOURCE_DIR}/.build/kernel
            -DDEBUGFS=${DEBUGFS_EXECUTABLE}
            -P ${PROJECT_SOURCE_DIR}/cmake/UpdateRootfsKernelMetadata.cmake
        DEPENDS uml_kernel
        COMMENT "Updating static-kernel metadata in the external rootfs"
        VERBATIM)
endif()

add_executable(uml-darwin main.cpp)
add_dependencies(uml-darwin uml_kernel)
if(TARGET uml_rootfs_kernel_metadata)
    add_dependencies(uml-darwin uml_rootfs_kernel_metadata)
endif()
set_property(TARGET uml-darwin APPEND PROPERTY LINK_DEPENDS
    "${UML_KERNEL_IMAGE}")
target_link_libraries(uml-darwin PRIVATE UML::Backend)
target_compile_options(uml-darwin PRIVATE -Wall -Wextra -Werror)
target_link_options(uml-darwin PRIVATE
    "LINKER:-export_dynamic"
    "LINKER:-force_load,$<TARGET_FILE:uml_backend>"
    "LINKER:-sectcreate,__DATA,__uml_elf,${UML_KERNEL_IMAGE}")

set_target_properties(uml-darwin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

if(UML_BUILD_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME ltp-runner-parser
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/ltp/run_guest_ltp_tests.py)
    set_tests_properties(ltp-runner-parser PROPERTIES LABELS "host;ltp")
    add_test(NAME guest-smp-parallel
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/smp/run_guest_smp.py
            --emulator $<TARGET_FILE:uml-darwin>
            --rootfs ${PROJECT_SOURCE_DIR}/rootfs.ext2)
    set_tests_properties(guest-smp-parallel PROPERTIES
        LABELS "guest;quick;smp"
        RUN_SERIAL TRUE
        TIMEOUT 45)
    add_test(NAME guest-ltp-verified
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/ltp/run_guest_ltp.py
            --emulator $<TARGET_FILE:uml-darwin>
            --rootfs ${PROJECT_SOURCE_DIR}/rootfs.ext2
            --manifest ${PROJECT_SOURCE_DIR}/tests/ltp/verified-tests.tsv
            --jobs ${UML_LTP_JOBS})
    set_tests_properties(guest-ltp-verified PROPERTIES
        LABELS "guest;ltp"
        RUN_SERIAL TRUE
        TIMEOUT 86400)
endif()
