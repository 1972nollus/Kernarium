# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT UML_INTEGRATION_ROOTFS)
    message(FATAL_ERROR
        "UML_BUILD_INTEGRATION_TESTS requires UML_INTEGRATION_ROOTFS")
endif()
if(NOT EXISTS "${UML_INTEGRATION_ROOTFS}")
    message(FATAL_ERROR
        "UML_INTEGRATION_ROOTFS does not exist: ${UML_INTEGRATION_ROOTFS}")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(UML_LTP_JOBS 16 CACHE STRING
    "Concurrent isolated guest workers for each LTP engine campaign")
if(NOT UML_LTP_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "UML_LTP_JOBS must be a positive integer")
endif()

# These are explicit guest campaigns, deliberately outside ordinary CTest.
add_custom_target(guest-smp-parallel
    COMMAND ${PROJECT_SOURCE_DIR}/tools/rootfs/with-verified-rootfs.sh
        --rootfs ${UML_INTEGRATION_ROOTFS}
        --
        ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/smp/run_guest_smp.py
        --emulator $<TARGET_FILE:uml-darwin>
        --rootfs ${UML_INTEGRATION_ROOTFS}
        --root-device /dev/ubda
        --overlay-shell
    DEPENDS uml-darwin
    COMMENT "Running standalone guest SMP integration suite"
    USES_TERMINAL
    VERBATIM)

function(uml_add_ltp_engine_campaign ENGINE)
    set(_results
        "${PROJECT_BINARY_DIR}/results/refactor/ltp/${ENGINE}")
    add_custom_target(guest-ltp-${ENGINE}
        COMMAND ${PROJECT_SOURCE_DIR}/tools/rootfs/with-verified-rootfs.sh
            --rootfs ${UML_INTEGRATION_ROOTFS}
            --
            ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/integration/ltp/run_guest_ltp.py
            --emulator $<TARGET_FILE:uml-darwin>
            --rootfs ${UML_INTEGRATION_ROOTFS}
            --manifest ${PROJECT_SOURCE_DIR}/tests/integration/ltp/verified-tests.tsv
            --modules-builtin ${UML_KERNEL_BUILD_DIR}/modules.builtin
            --engine ${ENGINE}
            --results-dir ${_results}
            --jobs ${UML_LTP_JOBS}
        DEPENDS uml-darwin
        COMMENT "Running standalone ${ENGINE} guest LTP integration suite"
        USES_TERMINAL
        VERBATIM)
endfunction()

uml_add_ltp_engine_campaign(gadgets)
uml_add_ltp_engine_campaign(jit)
add_custom_target(guest-ltp-verified)
add_dependencies(guest-ltp-verified guest-ltp-gadgets guest-ltp-jit)

set(UML_DOCKER_HELLO_WORLD
    "${CMAKE_CURRENT_BINARY_DIR}/docker-hello-world")
add_custom_command(
    OUTPUT "${UML_DOCKER_HELLO_WORLD}"
    COMMAND "${CMAKE_CXX_COMPILER}"
        --target=aarch64-unknown-linux-gnu
        -nostdlib
        -static
        -fuse-ld=lld
        -Wl,-e,_start
        "${CMAKE_CURRENT_SOURCE_DIR}/docker/hello_world.S"
        -o "${UML_DOCKER_HELLO_WORLD}"
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${UML_DOCKER_HELLO_WORLD}
        -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGeneratedFile.cmake
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/docker/hello_world.S"
    COMMENT "Building the offline arm64 Docker hello-world payload"
    VERBATIM)
add_custom_target(uml-docker-hello-world
    DEPENDS "${UML_DOCKER_HELLO_WORLD}")
add_dependencies(integration-test-binaries uml-docker-hello-world)

# Every guest campaign uses the EROFS image wrapper, which validates the raw
# filesystem contract and digest before launch and after cleanup.  The fast
# suite contains short real boots; full adds topology, MMU, and rendering work.
uml_add_guest_integration_test(
    NAME guest-overlay-systemd-smoke
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/gpu/run_gpu_guest.py
        --emulator $<TARGET_FILE:uml-darwin>
        --rootfs ${UML_INTEGRATION_ROOTFS}
        --timeout 240
        --docker-hello-world-binary ${UML_DOCKER_HELLO_WORLD}
        --command "test -d /run/systemd/system && grep -qx systemd /proc/1/comm"
        --command "getent passwd uml >/dev/null"
        --command "test \"$(id -u uml)\" -ne 0"
        --command "test \"$(getent passwd uml | cut -d: -f6)\" = /home/uml"
        --command "runuser -u uml -- sudo -n true"
        --command "systemctl is-enabled --quiet getty@tty0.service"
        --command "timeout 30 sh -c 'until systemctl is-active --quiet getty@tty0.service
do
    sleep 1
done'"
        --command "timeout 60 sh -c 'until systemctl is-active --quiet docker.service
do
    sleep 1
done'"
        --command "docker info --format '{{.Driver}}' | grep -Eq '^overlay(fs|2)$'"
        --command "docker info --format '{{.DockerRootDir}}' | grep -qx /var/lib/docker"
        --command "test \"$(findmnt -n -o FSTYPE -T /var/lib/containerd)\" = tmpfs"
        --command "test -x /usr/bin/docker"
        --command "test -x /opt/ltp/testcases/bin/getpid01"
        --command "test -x /opt/node/bin/node"
        --command "/opt/geekbench/release/geekbench6 --no-upload --version >/dev/null"
        --command "test -x /usr/sbin/mkfs.ext2"
        --command "test -x /usr/sbin/mke2fs"
        --command "test -x /usr/sbin/mkfs.ext3"
        --command "test -x /usr/sbin/mkfs.ext4"
        --command "test -x /usr/sbin/debugfs"
        --command "test -x /usr/sbin/ip"
        --command "/usr/sbin/ip -Version 2>&1 | grep -q '^ip utility, iproute2-'"
        --command "test -x /usr/bin/bc"
        --command "test -x /usr/bin/quota"
        --command "test -x /usr/sbin/quotacheck"
        --command "test -x /usr/sbin/e4crypt"
    LABELS guest;overlay;systemd;smoke
    TIMEOUT 300
    PROCESSORS 2
    RESOURCE_GROUPS cpus:2)

function(uml_add_guest_engine_smoke ENGINE)
    set(_labels guest cpu ${ENGINE} smoke)
    if(NOT ENGINE STREQUAL "interpreter")
        list(APPEND _labels dynamic-code)
    endif()
    uml_add_guest_integration_test(
        NAME guest-${ENGINE}-smoke
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/integration/jit/run_guest_jit_smoke.py
            --emulator $<TARGET_FILE:uml-darwin>
            --rootfs ${UML_INTEGRATION_ROOTFS}
            --engine ${ENGINE}
        LABELS ${_labels}
        TIMEOUT 420
        PROCESSORS 2
        RESOURCE_GROUPS cpus:2)
endfunction()

uml_add_guest_engine_smoke(interpreter)
uml_add_guest_engine_smoke(gadgets)
uml_add_guest_engine_smoke(jit)

uml_add_guest_integration_test(
    NAME guest-slirp-network
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/network/run_guest_network_smoke.py
        --emulator $<TARGET_FILE:uml-darwin>
        --rootfs ${UML_INTEGRATION_ROOTFS}
    LABELS guest;network;slirp;smoke
    TIMEOUT 240
    PROCESSORS 2
    RESOURCE_GROUPS cpus:2)

set(UML_GUEST_SHARED_MMAP_STRESS
    "${CMAKE_CURRENT_BINARY_DIR}/guest-shared-mmap-stress")
add_custom_command(
    OUTPUT "${UML_GUEST_SHARED_MMAP_STRESS}"
    COMMAND "${CMAKE_CXX_COMPILER}"
        --target=aarch64-unknown-linux-gnu
        -nostdlib
        -static
        -fuse-ld=lld
        -Wl,-e,_start
        "${CMAKE_CURRENT_SOURCE_DIR}/smp/guest_shared_mmap_stress.S"
        -o "${UML_GUEST_SHARED_MMAP_STRESS}"
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${UML_GUEST_SHARED_MMAP_STRESS}
        -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGeneratedFile.cmake
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/smp/guest_shared_mmap_stress.S"
    COMMENT "Building the freestanding arm64 shared-MM guest stress binary"
    VERBATIM)
add_custom_target(uml-guest-shared-mmap-stress
    DEPENDS "${UML_GUEST_SHARED_MMAP_STRESS}")
add_dependencies(integration-test-binaries uml-guest-shared-mmap-stress)

uml_add_guest_integration_test(
    NAME guest-mmu-stress
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/smp/run_guest_mmu_stress.py
        --emulator $<TARGET_FILE:uml-darwin>
        --rootfs ${UML_INTEGRATION_ROOTFS}
        --root-device /dev/ubda
        --overlay-shell
        --shared-mmap-binary ${UML_GUEST_SHARED_MMAP_STRESS}
        --cpus 10
    LABELS guest;memory;mmu;full;pressure
    TIMEOUT 600
    PROCESSORS 10
    RESOURCE_GROUPS cpus:10)

if(UML_ENABLE_EXTERNAL_NETWORK_TESTS)
    uml_add_guest_integration_test(
        NAME guest-external-network
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/integration/network/run_guest_network_smoke.py
            --emulator $<TARGET_FILE:uml-darwin>
            --rootfs ${UML_INTEGRATION_ROOTFS}
            --external
            --external-host ${UML_EXTERNAL_NETWORK_TEST_HOST}
        LABELS guest;network;external;full
        TIMEOUT 300
        PROCESSORS 2
        RESOURCE_GROUPS cpus:2)

    uml_add_guest_integration_test(
        NAME guest-kde-vnc-webgl-aquarium
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/integration/gpu/run_chromium_webgl.py
            --client $<TARGET_FILE:uml-vnc-smoke-client>
            --emulator $<TARGET_FILE:uml-darwin>
            --rootfs ${UML_INTEGRATION_ROOTFS}
        LABELS guest;gpu;vnc;webgl;external;full
        TIMEOUT 720
        PROCESSORS 10
        RESOURCE_GROUPS cpus:10,gpus:1)

    uml_add_guest_integration_test(
        NAME guest-webgl-rendered-workflows
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/integration/gpu/run_webgl_samples.py
            --client $<TARGET_FILE:uml-vnc-smoke-client>
            --emulator $<TARGET_FILE:uml-darwin>
            --rootfs ${UML_INTEGRATION_ROOTFS}
        LABELS guest;gpu;webgl;external;full;pressure
        TIMEOUT 1800
        PROCESSORS 10
        RESOURCE_GROUPS cpus:10,gpus:1)
endif()
