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

option(UML_TEST_APPLE_AUDIO "Qualify the real Apple audio output with a short stereo tone" OFF)
if(UML_TEST_APPLE_AUDIO)
    add_subdirectory(audio)
endif()

set(_uml_integration_emulator uml-darwin)
if(NOT TARGET "${_uml_integration_emulator}")
    message(FATAL_ERROR
        "Integration emulator target is unavailable: ${_uml_integration_emulator}")
endif()

set(UML_V4L2_PROBE_BINARY "" CACHE FILEPATH
    "Static guest V4L2 regression executable built from tests/integration/video/v4l2_decode_probe.cpp")
if(UML_V4L2_PROBE_BINARY)
    if(NOT EXISTS "${UML_V4L2_PROBE_BINARY}")
        message(FATAL_ERROR "V4L2 qualification requires its guest probe")
    endif()
    if(UML_GUEST_16K_PAGES)
        set(_uml_video_page_size 16384)
    else()
        set(_uml_video_page_size 4096)
    endif()
    uml_add_integration_test(NAME guest-v4l2-decode
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/video/run_v4l2_decode.py"
            --emulator "$<TARGET_FILE:${_uml_integration_emulator}>"
            --rootfs "${UML_INTEGRATION_ROOTFS}"
            --probe "${UML_V4L2_PROBE_BINARY}"
            --page-size "${_uml_video_page_size}"
            --log "${CMAKE_CURRENT_BINARY_DIR}/v4l2-guest.log"
        LABELS video TIMEOUT 600)
endif()

set(UML_FUTEX_PROBE_BINARY "" CACHE FILEPATH
    "Static guest executable built from tests/integration/futex/futex_atomic_probe.cpp")
if(UML_FUTEX_PROBE_BINARY)
    if(NOT EXISTS "${UML_FUTEX_PROBE_BINARY}")
        message(FATAL_ERROR "Futex qualification requires its compiled guest probe")
    endif()
    if(UML_GUEST_16K_PAGES)
        set(_uml_futex_page_size 16384)
    else()
        set(_uml_futex_page_size 4096)
    endif()
    uml_add_integration_test(NAME guest-futex-atomic
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/futex/run_futex_atomic.py"
            --emulator "$<TARGET_FILE:${_uml_integration_emulator}>"
            --rootfs "${UML_INTEGRATION_ROOTFS}"
            --probe "${UML_FUTEX_PROBE_BINARY}"
            --page-size "${_uml_futex_page_size}"
            --log "${CMAKE_CURRENT_BINARY_DIR}/futex-guest.log"
        LABELS futex TIMEOUT 320)
endif()

if(NOT UML_ACTIVE_KERNEL_ARTIFACT_TARGET OR
   NOT TARGET "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}")
    message(FATAL_ERROR "Integration tests require a selected kernel artifact")
endif()
get_target_property(_uml_integration_modules_builtin
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}" UML_KERNEL_MODULES_BUILTIN)
if(NOT _uml_integration_modules_builtin)
    message(FATAL_ERROR "Selected kernel artifact lacks modules.builtin evidence")
endif()

set(UML_LTP_JOBS 16 CACHE STRING
    "Concurrent isolated guest workers for each LTP engine campaign")
if(NOT UML_LTP_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "UML_LTP_JOBS must be a positive integer")
endif()

# These are explicit guest campaigns, deliberately outside ordinary CTest.
add_custom_target(guest-smp-parallel
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/smp/run_guest_smp.py
        --emulator $<TARGET_FILE:${_uml_integration_emulator}>
        --rootfs ${UML_INTEGRATION_ROOTFS}
    DEPENDS uml-darwin
    COMMENT "Running standalone guest SMP integration suite"
    USES_TERMINAL
    VERBATIM)

function(uml_add_ltp_engine_campaign ENGINE)
    set(_results
        "${PROJECT_BINARY_DIR}/results/refactor/ltp/${ENGINE}")
    add_custom_target(guest-ltp-${ENGINE}
        COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/integration/ltp/run_guest_ltp.py
            --emulator $<TARGET_FILE:${_uml_integration_emulator}>
            --rootfs ${UML_INTEGRATION_ROOTFS}
            --manifest ${PROJECT_SOURCE_DIR}/tests/integration/ltp/verified-tests.tsv
            --modules-builtin ${_uml_integration_modules_builtin}
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

set(UML_GUEST_HWCAP_SANITY
    "${CMAKE_CURRENT_BINARY_DIR}/guest-hwcap-sanity")
add_custom_command(
    OUTPUT "${UML_GUEST_HWCAP_SANITY}"
    COMMAND "${CMAKE_CXX_COMPILER}"
        --target=aarch64-unknown-linux-gnu
        -nostdlib
        -static
        -fuse-ld=lld
        -Wl,-e,_start
        "${CMAKE_CURRENT_SOURCE_DIR}/features/guest_hwcap_sanity.S"
        -o "${UML_GUEST_HWCAP_SANITY}"
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${UML_GUEST_HWCAP_SANITY}
        -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGeneratedFile.cmake
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/guest_hwcap_sanity.S"
    COMMENT "Building the arm64 guest HWCAP sanity payload"
    VERBATIM)
add_custom_target(uml-guest-hwcap-sanity
    DEPENDS "${UML_GUEST_HWCAP_SANITY}")
add_dependencies(integration-test-binaries uml-guest-hwcap-sanity)

# Guest campaigns share the EROFS image directly. The filesystem supplies the
# read-only contract; writable guest state lives in disposable overlays. The
# fast suite contains short real boots; full adds topology, MMU, and rendering.
uml_add_guest_integration_test(
    NAME guest-overlay-systemd-smoke
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/gpu/run_gpu_guest.py
        --emulator $<TARGET_FILE:${_uml_integration_emulator}>
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
    set(_labels guest cpu ${ENGINE} dynamic-code smoke)
    uml_add_guest_integration_test(
        NAME guest-${ENGINE}-smoke
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/integration/jit/run_guest_jit_smoke.py
            --emulator $<TARGET_FILE:${_uml_integration_emulator}>
            --rootfs ${UML_INTEGRATION_ROOTFS}
            --engine ${ENGINE}
            --hwcap-sanity-binary ${UML_GUEST_HWCAP_SANITY}
        LABELS ${_labels}
        TIMEOUT 420
        PROCESSORS 2
        RESOURCE_GROUPS cpus:2)
endfunction()

uml_add_guest_engine_smoke(gadgets)
uml_add_guest_engine_smoke(jit)

uml_add_guest_integration_test(
    NAME guest-slirp-network
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/network/run_guest_network_smoke.py
        --emulator $<TARGET_FILE:${_uml_integration_emulator}>
        --rootfs ${UML_INTEGRATION_ROOTFS}
    LABELS guest;network;slirp;smoke
    TIMEOUT 240
    PROCESSORS 2
    RESOURCE_GROUPS cpus:2)

uml_add_guest_integration_test(
    NAME guest-slirp-network-transfer
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/network/run_guest_network_transfer.py
        --emulator $<TARGET_FILE:${_uml_integration_emulator}>
        --rootfs ${UML_INTEGRATION_ROOTFS}
    LABELS guest;network;slirp
    TIMEOUT 300
    PROCESSORS 2
    RESOURCE_GROUPS cpus:2)

add_subdirectory(balloon)

# One-vCPU guest: an input burst past the tty buffer must not end console input.
uml_add_guest_integration_test(
    NAME guest-console-input-burst
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/console/run_guest_console_burst.py
        --emulator $<TARGET_FILE:${_uml_integration_emulator}>
        --rootfs ${UML_INTEGRATION_ROOTFS}
    LABELS guest;console;smoke
    TIMEOUT 180
    PROCESSORS 1
    RESOURCE_GROUPS cpus:1)

# Four-vCPU guest: two CPUs draining one serial line must not split its input.
uml_add_guest_integration_test(
    NAME guest-console-concurrent-input
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/console/run_guest_console_concurrent_input.py
        --emulator $<TARGET_FILE:${_uml_integration_emulator}>
        --rootfs ${UML_INTEGRATION_ROOTFS}
    LABELS guest;console;smoke
    TIMEOUT 420
    PROCESSORS 4
    RESOURCE_GROUPS cpus:4)

# The interpreter runs BPF callbacks through its fixed entry-point pool.
uml_add_guest_integration_test(
    NAME guest-bpf-callbacks
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/bpf/run_guest_bpf_callbacks.py
        --emulator $<TARGET_FILE:${_uml_integration_emulator}>
        --rootfs ${UML_INTEGRATION_ROOTFS}
    LABELS guest;bpf;smoke
    TIMEOUT 600
    PROCESSORS 4
    RESOURCE_GROUPS cpus:4)

# The base archive and a per-run initramfs boot the EROFS image, and every
# broken initramfs stops the guest with a named reason.
if(NOT UML_BOOT_BASE_ARCHIVE OR NOT TARGET uml-initramfs)
    message(FATAL_ERROR
        "guest-boot-initramfs requires the accepted boot base and uml-initramfs")
endif()
uml_add_guest_integration_test(
    NAME guest-boot-initramfs
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/boot/run_guest_boot_initramfs.py
        --emulator $<TARGET_FILE:${_uml_integration_emulator}>
        --rootfs ${UML_INTEGRATION_ROOTFS}
        --base ${UML_BOOT_BASE_ARCHIVE}
        --initramfs-tool $<TARGET_FILE:uml-initramfs>
    LABELS guest;boot;smoke
    TIMEOUT 600
    PROCESSORS 1
    RESOURCE_GROUPS cpus:1)

# A faulting nofault kernel read resumes at its recovery point.
uml_add_guest_integration_test(
    NAME guest-kernel-nofault
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/kernel/run_guest_kernel_nofault.py
        --emulator $<TARGET_FILE:${_uml_integration_emulator}>
        --rootfs ${UML_INTEGRATION_ROOTFS}
    LABELS guest;kernel;smoke
    TIMEOUT 300
    PROCESSORS 2
    RESOURCE_GROUPS cpus:2)

# Path and name strings copied from user memory keep their contract.
uml_add_guest_integration_test(
    NAME guest-user-strings
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/kernel/run_guest_user_strings.py
        --emulator $<TARGET_FILE:${_uml_integration_emulator}>
        --rootfs ${UML_INTEGRATION_ROOTFS}
    LABELS guest;kernel;smoke
    TIMEOUT 300
    PROCESSORS 2
    RESOURCE_GROUPS cpus:2)

# Unmapping user pages beside the native window leaves the kernel's own
# mappings in it alone.
uml_add_guest_integration_test(
    NAME guest-native-window-unmap
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/kernel/run_guest_native_window_unmap.py
        --emulator $<TARGET_FILE:${_uml_integration_emulator}>
        --rootfs ${UML_INTEGRATION_ROOTFS}
    LABELS guest;kernel;smoke
    TIMEOUT 300
    PROCESSORS 2
    RESOURCE_GROUPS cpus:2)

# A tracer's PSTATE writes are filtered as native arm64 filters them.
uml_add_guest_integration_test(
    NAME guest-ptrace-pstate
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/kernel/run_guest_ptrace_pstate.py
        --emulator $<TARGET_FILE:${_uml_integration_emulator}>
        --rootfs ${UML_INTEGRATION_ROOTFS}
    LABELS guest;kernel;smoke
    TIMEOUT 300
    PROCESSORS 2
    RESOURCE_GROUPS cpus:2)

# Unwinders cross an arm64 signal frame as on native Linux: backtrace() in a
# handler and cancellation of a thread blocked in a system call.
uml_add_guest_integration_test(
    NAME guest-signal-unwind
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/kernel/run_guest_signal_unwind.py
        --emulator $<TARGET_FILE:${_uml_integration_emulator}>
        --rootfs ${UML_INTEGRATION_ROOTFS}
    LABELS guest;kernel;smoke
    TIMEOUT 420
    PROCESSORS 2
    RESOURCE_GROUPS cpus:2)

# A stopped embedded guest returns to its host only after every kernel
# thread has left the image the loader then unmaps.
add_executable(uml_run_end_stop_tests kernel/run_end_stop_tests.cpp)
target_link_libraries(uml_run_end_stop_tests PRIVATE UML::Emulator)
target_compile_features(uml_run_end_stop_tests PRIVATE cxx_std_20)
target_compile_options(uml_run_end_stop_tests PRIVATE -Wall -Wextra -Werror)
# The embedded ELF resolves the host ABI with dlsym(), as in uml-darwin.
target_link_options(uml_run_end_stop_tests PRIVATE
    "LINKER:-export_dynamic"
    "LINKER:-force_load,$<TARGET_FILE:uml_emulator>")
add_dependencies(uml_run_end_stop_tests "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}")
add_dependencies(integration-test-binaries uml_run_end_stop_tests)
get_target_property(_uml_run_end_kernel
    "${UML_ACTIVE_KERNEL_ARTIFACT_TARGET}" UML_KERNEL_IMAGE)
uml_add_integration_test(
    NAME guest-run-end-stop
    COMMAND uml_run_end_stop_tests
        ${_uml_run_end_kernel}
        ${CMAKE_CURRENT_BINARY_DIR}/run-end-stop
        20
    LABELS guest;kernel
    TIMEOUT 600
    PROCESSORS 4
    RESOURCE_GROUPS cpus:4)

set(UML_GUEST_SAMPLER_WORKLOAD
    "${CMAKE_CURRENT_BINARY_DIR}/guest-sampler-workload")
add_custom_command(
    OUTPUT "${UML_GUEST_SAMPLER_WORKLOAD}"
    COMMAND "${CMAKE_CXX_COMPILER}"
        --target=aarch64-unknown-linux-gnu
        -nostdlib
        -static
        -fuse-ld=lld
        -Wl,-e,_start
        "${CMAKE_CURRENT_SOURCE_DIR}/profiling/guest_sampler_workload.S"
        -o "${UML_GUEST_SAMPLER_WORKLOAD}"
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${UML_GUEST_SAMPLER_WORKLOAD}
        -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGeneratedFile.cmake
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/profiling/guest_sampler_workload.S"
    COMMENT "Building the freestanding arm64 guest sampler workload"
    VERBATIM)
add_custom_target(uml-guest-sampler-workload
    DEPENDS "${UML_GUEST_SAMPLER_WORKLOAD}")
add_dependencies(integration-test-binaries uml-guest-sampler-workload)

uml_add_guest_integration_test(
    NAME guest-code-sampler
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/profiling/run_guest_code_sampler.py
        --emulator $<TARGET_FILE:${_uml_integration_emulator}>
        --rootfs ${UML_INTEGRATION_ROOTFS}
        --workload ${UML_GUEST_SAMPLER_WORKLOAD}
    LABELS guest;profiling
    TIMEOUT 300
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

set(UML_GUEST_FORK_COW_COHERENCE
    "${CMAKE_CURRENT_BINARY_DIR}/guest-fork-cow-coherence")
add_custom_command(
    OUTPUT "${UML_GUEST_FORK_COW_COHERENCE}"
    COMMAND "${CMAKE_CXX_COMPILER}"
        --target=aarch64-unknown-linux-gnu
        -std=c++20
        -O2
        -Wall
        -Wextra
        -Werror
        -march=armv8.1-a
        -mno-outline-atomics
        -ffreestanding
        -fno-exceptions
        -fno-rtti
        -fno-stack-protector
        -fno-asynchronous-unwind-tables
        -nostdlib
        -static
        -fuse-ld=lld
        -Wl,-e,_start
        -Wl,-z,max-page-size=16384
        -Wl,-s
        "${CMAKE_CURRENT_SOURCE_DIR}/smp/guest_fork_cow_coherence.cpp"
        -o "${UML_GUEST_FORK_COW_COHERENCE}"
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${UML_GUEST_FORK_COW_COHERENCE}
        -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGeneratedFile.cmake
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/smp/guest_fork_cow_coherence.cpp"
    COMMENT "Building the freestanding arm64 fork copy-on-write guest stress binary"
    VERBATIM)
add_custom_target(uml-guest-fork-cow-coherence
    DEPENDS "${UML_GUEST_FORK_COW_COHERENCE}")
add_dependencies(integration-test-binaries uml-guest-fork-cow-coherence)

uml_add_guest_integration_test(
    NAME guest-mmu-stress
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/integration/smp/run_guest_mmu_stress.py
        --emulator $<TARGET_FILE:${_uml_integration_emulator}>
        --rootfs ${UML_INTEGRATION_ROOTFS}
        --shared-mmap-binary ${UML_GUEST_SHARED_MMAP_STRESS}
        --fork-cow-binary ${UML_GUEST_FORK_COW_COHERENCE}
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
            --emulator $<TARGET_FILE:${_uml_integration_emulator}>
            --rootfs ${UML_INTEGRATION_ROOTFS}
            --external
            --external-host ${UML_EXTERNAL_NETWORK_TEST_HOST}
        LABELS guest;network;external;full
        TIMEOUT 300
        PROCESSORS 2
        RESOURCE_GROUPS cpus:2)

    # guest-overlay-systemd-smoke proves Docker runs a locally imported image
    # with no network at all.  This proves the rest of the path a real user
    # takes: registry DNS and TLS over SLiRP, an image pull, and container
    # networking over the docker0 bridge.  It also exercises a large amount of
    # Go: dockerd, containerd and the client all read HWCAP-gated system
    # registers at startup, which is how the missing MRS/MSR DIT encoding was
    # found.  A benchmark cannot see that failure; nothing in Geekbench
    # executes those instructions.
    uml_add_guest_integration_test(
        NAME guest-docker-hello-world
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/integration/gpu/run_gpu_guest.py
            --emulator $<TARGET_FILE:${_uml_integration_emulator}>
            --rootfs ${UML_INTEGRATION_ROOTFS}
            --timeout 600
            --command "timeout 180 sh -c 'until systemctl is-active --quiet docker.service
do
    sleep 1
done'"
            --command "docker run --rm hello-world | grep -Fxq 'Hello from Docker!'"
            --command "docker image rm -f hello-world >/dev/null"
        LABELS guest;docker;network;external;full
        TIMEOUT 900
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)

    uml_add_guest_integration_test(
        NAME guest-kde-vnc-webgl-aquarium
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/integration/gpu/run_chromium_webgl.py
            --client $<TARGET_FILE:uml-vnc-smoke-client>
            --emulator $<TARGET_FILE:${_uml_integration_emulator}>
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
            --emulator $<TARGET_FILE:${_uml_integration_emulator}>
            --rootfs ${UML_INTEGRATION_ROOTFS}
        LABELS guest;gpu;webgl;external;full;pressure
        TIMEOUT 1800
        PROCESSORS 10
        RESOURCE_GROUPS cpus:10,gpus:1)
endif()

# The iOS slice renders through Mesa Zink on KosmicKrisp, which requires Metal
# 4; the iOS Simulator publishes no Metal 4 device, so the unmodified device
# build is verified by running it on this Apple silicon Mac in Designed for
# iPad mode. That needs Xcode's local install service and a sealed rendered
# image the application copies into its own container, neither of which a
# checkout provides, so the lane is opt-in: name the image and it registers.
if(APPLE AND UML_IOS_MAC_CTS_ROOTFS)
    if(NOT EXISTS "${UML_IOS_MAC_CTS_ROOTFS}")
        message(FATAL_ERROR
            "UML_IOS_MAC_CTS_ROOTFS does not exist: ${UML_IOS_MAC_CTS_ROOTFS}")
    endif()
    if(NOT UML_IOS_MAC_CTS_ROOTFS_RECEIPT)
        message(FATAL_ERROR
            "The iOS-on-Mac CTS lane needs the image's identity receipt in "
            "UML_IOS_MAC_CTS_ROOTFS_RECEIPT")
    endif()
    if(NOT UML_IOS_MAC_CTS_CAMPAIGN)
        set(UML_IOS_MAC_CTS_CAMPAIGN "gles3-identity-sentinel-v1")
    endif()
    uml_add_integration_test(
        NAME apple-ios-mac-vk-gl-cts
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/integration/gpu/run_ios_mac_vk_gl_cts.py
            --campaign ${UML_IOS_MAC_CTS_CAMPAIGN}
            --rootfs ${UML_IOS_MAC_CTS_ROOTFS}
            --rootfs-receipt ${UML_IOS_MAC_CTS_ROOTFS_RECEIPT}
            --artifact-root ${CMAKE_CURRENT_BINARY_DIR}/ios-mac-cts
        LABELS gpu;apple;ios;external;full
        TIMEOUT 5400
        PROCESSORS 8
        RESOURCE_GROUPS cpus:8,gpus:1)
endif()

# The device counterpart drives a connected iOS device through devicectl. The
# guest runs on the device, so the host only holds a console; what a checkout
# cannot provide is the paired device and an installed application already
# carrying the campaign, so this lane is opt-in too: name the device and it
# registers.
if(APPLE AND UML_IOS_DEVICE_VK_CTS_DEVICE)
    if(NOT UML_IOS_DEVICE_VK_CTS_CAMPAIGN)
        set(UML_IOS_DEVICE_VK_CTS_CAMPAIGN "vulkan-info-receipt-smoke-v1")
    endif()
    uml_add_integration_test(
        NAME apple-ios-device-vk-cts
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/integration/gpu/run_ios_device_vk_cts.py
            --device ${UML_IOS_DEVICE_VK_CTS_DEVICE}
            --campaign ${UML_IOS_DEVICE_VK_CTS_CAMPAIGN}
            --artifact-root ${CMAKE_CURRENT_BINARY_DIR}/ios-device-vk-cts
        LABELS gpu;apple;ios;external;full
        TIMEOUT 5400
        PROCESSORS 1)
endif()
