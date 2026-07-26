# SPDX-License-Identifier: GPL-2.0-or-later

include(ExternalProject)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy>=1.5.4)

find_program(MESON_EXECUTABLE
    NAMES meson
    HINTS ${PROJECT_SOURCE_DIR}/.build/gpu-tools-venv/bin
    REQUIRED)
find_program(NINJA_EXECUTABLE NAMES ninja REQUIRED)

set(VIRGLRENDERER_BUILD_DIRECTORY
    ${PROJECT_BINARY_DIR}/external/virglrenderer)
set(VIRGLRENDERER_LIBRARY
    ${VIRGLRENDERER_BUILD_DIRECTORY}/src/libvirglrenderer.a)
# CMake validates imported include paths during generation, before Meson has
# had an opportunity to create its generated-header directory.
file(MAKE_DIRECTORY ${VIRGLRENDERER_BUILD_DIRECTORY}/src)
get_filename_component(MESON_PYTHON_DIRECTORY
    ${MESON_EXECUTABLE} DIRECTORY)

# Virgl uses caller-provided CGL contexts, so EGL/GLX and the Vulkan render
# server are excluded from the host renderer.
ExternalProject_Add(virglrenderer_external
    SOURCE_DIR ${PROJECT_SOURCE_DIR}/external/virglrenderer
    BINARY_DIR ${VIRGLRENDERER_BUILD_DIRECTORY}
    CONFIGURE_COMMAND
        ${CMAKE_COMMAND} -E env
            "PATH=${MESON_PYTHON_DIRECTORY}:$ENV{PATH}"
            "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        ${MESON_EXECUTABLE} setup <BINARY_DIR> <SOURCE_DIR>
            --buildtype=release
            --default-library=static
            -Dvenus=false
            -Dplatforms=[]
            -Dtests=false
            -Dfuzzer=false
            -Dvideo=false
            -Dtracing=none
    BUILD_COMMAND
        ${CMAKE_COMMAND} -E env
            "PATH=${MESON_PYTHON_DIRECTORY}:$ENV{PATH}"
            "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        ${MESON_EXECUTABLE} compile -C <BINARY_DIR> virglrenderer
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${VIRGLRENDERER_LIBRARY}
    BUILD_ALWAYS TRUE
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE)

add_library(virglrenderer_host STATIC IMPORTED GLOBAL)
set_target_properties(virglrenderer_host PROPERTIES
    IMPORTED_LOCATION ${VIRGLRENDERER_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/external/virglrenderer/src;${VIRGLRENDERER_BUILD_DIRECTORY}/src")
add_dependencies(virglrenderer_host virglrenderer_external)

add_library(uml_gpu_backend STATIC
    in_process_service.cpp
    virgl_renderer.cpp
    vnc_input.cpp
    vhost_user_server.cpp
    virtio_gpu_device.cpp
    vnc_display.cpp)
add_library(UML::GpuBackend ALIAS uml_gpu_backend)
target_include_directories(uml_gpu_backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/external/libvncserver/include
    ${PROJECT_BINARY_DIR}/external/libvncserver/include)
target_link_libraries(uml_gpu_backend PUBLIC
    vncserver
    virglrenderer_host
    PkgConfig::EPOXY
    z
    "-framework AppKit"
    "-framework Accelerate"
    "-framework CoreFoundation"
    "-framework CoreGraphics"
    "-framework Foundation"
    "-framework IOKit"
    "-framework IOSurface"
    "-framework Metal"
    "-framework OpenGL"
    "-framework QuartzCore")
target_compile_options(uml_gpu_backend PRIVATE
    -Wall -Wextra -Werror -Wno-deprecated-declarations)

# This executable is a host-only protocol/VNC test fixture. Production
# emulation links UML::GpuBackend into the emulator process.
add_executable(uml-virtio-gpu main.cpp)
target_link_libraries(uml-virtio-gpu PRIVATE UML::GpuBackend)
target_compile_options(uml-virtio-gpu PRIVATE
    -Wall -Wextra -Werror -Wno-deprecated-declarations)
set_target_properties(uml-virtio-gpu PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

if(UML_BUILD_TESTS)
    add_executable(uml-vnc-smoke-client
        ${PROJECT_SOURCE_DIR}/tests/gpu/vnc_smoke_client.cpp)
    target_include_directories(uml-vnc-smoke-client PRIVATE
        ${PROJECT_SOURCE_DIR}/external/libvncserver/include
        ${PROJECT_BINARY_DIR}/external/libvncserver/include)
    target_link_libraries(uml-vnc-smoke-client PRIVATE
        vncclient
        z
        "-framework CoreFoundation")
    target_compile_options(uml-vnc-smoke-client PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations)
    set_target_properties(uml-vnc-smoke-client PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

    add_test(NAME virtio-gpu-vnc-smoke
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/gpu/run_vnc_smoke.py
            --server $<TARGET_FILE:uml-virtio-gpu>
            --client $<TARGET_FILE:uml-vnc-smoke-client>)
    set_tests_properties(virtio-gpu-vnc-smoke PROPERTIES
        LABELS "host;gpu;vnc"
        TIMEOUT 30)
endif()
