# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_arm64_dispatch OBJECT
    cpu/arm64/dispatcher.cpp
    cpu/arm64/instructions/control_flow.cpp
    cpu/arm64/instructions/crc32.cpp
    cpu/arm64/instructions/floating_point.cpp
    cpu/arm64/instructions/integer.cpp
    cpu/arm64/instructions/integer/arithmetic.cpp
    cpu/arm64/instructions/load_store.cpp
    cpu/arm64/instructions/simd.cpp
    cpu/arm64/instructions/simd/complete.cpp
    cpu/arm64/instructions/simd/conversion.cpp
    cpu/arm64/instructions/simd/crypto.cpp
    cpu/arm64/instructions/simd/floating_point.cpp
    cpu/arm64/instructions/simd/narrowing.cpp
    cpu/arm64/instructions/simd/rearrange.cpp
    cpu/arm64/instructions/simd/shift.cpp)

target_include_directories(uml_arm64_dispatch PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_arm64_dispatch PUBLIC cxx_std_20)
target_compile_options(uml_arm64_dispatch PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_arm64_dispatch PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_arm64_gadgets OBJECT
    cpu/arm64/gadgets/decoded.cpp
    cpu/arm64/gadgets/program.cpp
    cpu/arm64/gadgets/tlb.cpp
    cpu/arm64/gadgets/engine.S)
target_include_directories(uml_arm64_gadgets PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_arm64_gadgets PUBLIC cxx_std_20)
target_compile_options(uml_arm64_gadgets PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>)
set_target_properties(uml_arm64_gadgets PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_static_mmu OBJECT
    ${PROJECT_SOURCE_DIR}/darwin/runtime/mmu/address_space.cpp)
target_include_directories(uml_static_mmu PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include)
target_compile_features(uml_static_mmu PUBLIC cxx_std_20)
target_compile_options(uml_static_mmu PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_static_mmu PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_static_runtime OBJECT
    runtime/static_guest.cpp
    runtime/static/gadget_executor.cpp
    runtime/static/guest_memory.cpp)

target_include_directories(uml_static_runtime PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/include
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(uml_static_runtime PUBLIC cxx_std_20)
target_compile_options(uml_static_runtime PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_static_runtime PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_native_mmu OBJECT
    ${PROJECT_SOURCE_DIR}/darwin/runtime/mmu/native_address_space.cpp)
target_include_directories(uml_native_mmu PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/runtime/mmu)
target_compile_features(uml_native_mmu PUBLIC cxx_std_20)
target_compile_options(uml_native_mmu PRIVATE -Wall -Wextra -Werror)
set_target_properties(uml_native_mmu PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(uml_backend STATIC
    src/backend.cpp
    runtime/guest_selector.cpp
    ${PROJECT_SOURCE_DIR}/darwin/network/device.cpp
    ${PROJECT_SOURCE_DIR}/darwin/runtime/bridge.cpp
    ${PROJECT_SOURCE_DIR}/darwin/runtime/mmu/static_api.cpp
    $<TARGET_OBJECTS:uml_arm64_dispatch>
    $<TARGET_OBJECTS:uml_arm64_gadgets>
    $<TARGET_OBJECTS:uml_static_mmu>
    $<TARGET_OBJECTS:uml_static_runtime>
    $<TARGET_OBJECTS:uml_native_mmu>)

add_library(UML::Backend ALIAS uml_backend)

target_include_directories(uml_backend
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${PROJECT_SOURCE_DIR}/darwin/network
        ${PROJECT_SOURCE_DIR}/darwin/runtime/mmu
        ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(uml_backend PUBLIC cxx_std_20)
target_compile_options(uml_backend PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(uml_backend PRIVATE
    UML_APPLE_PLATFORM_NAME="${UML_APPLE_PLATFORM}"
    UML_BACKEND_HAS_KERNEL_RUNNER=$<BOOL:${UML_BUILD_KERNEL_RUNNER}>
    UML_BACKEND_HAS_SLIRP=$<BOOL:${UML_BUILD_KERNEL_RUNNER}>)

set_target_properties(uml_backend PROPERTIES
    OUTPUT_NAME uml_backend
    POSITION_INDEPENDENT_CODE ON
    PUBLIC_HEADER include/uml/backend.h)

if(UML_BUILD_KERNEL_RUNNER)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SLIRP REQUIRED slirp)
    pkg_check_modules(GLIB REQUIRED glib-2.0)
    pkg_check_modules(PCRE2 REQUIRED libpcre2-8)

    pkg_get_variable(SLIRP_LIBDIR slirp libdir)
    pkg_get_variable(GLIB_LIBDIR glib-2.0 libdir)
    pkg_get_variable(PCRE2_LIBDIR libpcre2-8 libdir)

    target_sources(uml_backend PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/loader/elf_loader.cpp)

    add_library(uml_slirp_network OBJECT
        ${PROJECT_SOURCE_DIR}/darwin/network/slirp/backend.cpp
        ${PROJECT_SOURCE_DIR}/darwin/network/slirp/event_loop.cpp
        ${PROJECT_SOURCE_DIR}/darwin/network/slirp/frame_queue.cpp)
    target_include_directories(uml_slirp_network PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/include
        ${PROJECT_SOURCE_DIR}/darwin/network/slirp
        ${SLIRP_INCLUDE_DIRS}
        ${GLIB_INCLUDE_DIRS})
    target_compile_features(uml_slirp_network PUBLIC cxx_std_20)
    target_compile_options(uml_slirp_network PRIVATE -Wall -Wextra -Werror)
    set_target_properties(uml_slirp_network PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_sources(uml_backend PRIVATE $<TARGET_OBJECTS:uml_slirp_network>)

    target_include_directories(uml_backend PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/loader
        ${SLIRP_INCLUDE_DIRS}
        ${GLIB_INCLUDE_DIRS}
        ${PCRE2_INCLUDE_DIRS})

    set(GETTEXT_STATIC /opt/homebrew/opt/gettext/lib/libintl.a)
    foreach(STATIC_LIBRARY
            ${SLIRP_LIBDIR}/libslirp.a
            ${GLIB_LIBDIR}/libglib-2.0.a
            ${PCRE2_LIBDIR}/libpcre2-8.a
            ${GETTEXT_STATIC})
        if(NOT EXISTS ${STATIC_LIBRARY})
            message(FATAL_ERROR "Required static dependency is missing: ${STATIC_LIBRARY}")
        endif()
    endforeach()

    target_link_libraries(uml_backend PRIVATE
        ${SLIRP_LIBDIR}/libslirp.a
        ${GLIB_LIBDIR}/libglib-2.0.a
        ${PCRE2_LIBDIR}/libpcre2-8.a
        ${GETTEXT_STATIC}
        resolv iconv m
        "-framework Foundation"
        "-framework CoreFoundation")

    if(UML_BUILD_TESTS)
        add_executable(uml_slirp_network_tests
            ${PROJECT_SOURCE_DIR}/darwin/probes/network.cpp
            $<TARGET_OBJECTS:uml_slirp_network>)
        target_include_directories(uml_slirp_network_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin/include)
        target_compile_features(uml_slirp_network_tests PRIVATE cxx_std_20)
        target_compile_options(uml_slirp_network_tests PRIVATE
            -Wall -Wextra -Werror)
        target_link_libraries(uml_slirp_network_tests PRIVATE
            ${SLIRP_LIBDIR}/libslirp.a
            ${GLIB_LIBDIR}/libglib-2.0.a
            ${PCRE2_LIBDIR}/libpcre2-8.a
            ${GETTEXT_STATIC}
            resolv iconv m
            "-framework Foundation"
            "-framework CoreFoundation")
        add_test(NAME slirp-network-arp COMMAND uml_slirp_network_tests)
    endif()
endif()

if(UML_BUILD_TESTS)
    add_executable(uml_native_mmu_tests
        tests/native_mmu_tests.cpp
        $<TARGET_OBJECTS:uml_native_mmu>)
    target_include_directories(uml_native_mmu_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/runtime/mmu)
    target_compile_features(uml_native_mmu_tests PRIVATE cxx_std_20)
    target_compile_options(uml_native_mmu_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME native-4k-shadow-mmu COMMAND uml_native_mmu_tests)

    add_executable(uml_backend_api_tests tests/backend_api_tests.cpp)
    target_link_libraries(uml_backend_api_tests PRIVATE UML::Backend)
    target_compile_features(uml_backend_api_tests PRIVATE cxx_std_20)
    target_compile_options(uml_backend_api_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME backend-native-entry-api COMMAND uml_backend_api_tests)

    add_executable(uml_arm64_dispatch_tests
        tests/arm64_dispatch_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/arm64-neon-inventory.s
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_static_runtime>)
    target_include_directories(uml_arm64_dispatch_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_dispatch_tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME arm64-static-dispatch COMMAND uml_arm64_dispatch_tests)

    add_executable(uml_arm64_gadget_tests
        tests/arm64_gadget_tests.cpp
        $<TARGET_OBJECTS:uml_arm64_dispatch>
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_arm64_gadgets>)
    target_include_directories(uml_arm64_gadget_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_options(uml_arm64_gadget_tests PRIVATE
        -Wall -Wextra -Werror)
    add_test(NAME arm64-static-gadgets COMMAND uml_arm64_gadget_tests)
endif()

install(TARGETS uml_backend
    ARCHIVE DESTINATION lib
    PUBLIC_HEADER DESTINATION include/uml)
