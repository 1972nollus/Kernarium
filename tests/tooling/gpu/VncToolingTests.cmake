# SPDX-License-Identifier: GPL-2.0-or-later

if(TARGET uml-virtio-gpu AND TARGET uml-vnc-smoke-client)
    uml_add_tooling_test(
        NAME virtio-gpu-vnc-smoke
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/tooling/gpu/run_vnc_smoke.py
            --server $<TARGET_FILE:uml-virtio-gpu>
            --client $<TARGET_FILE:uml-vnc-smoke-client>
        LABELS gpu;vnc
        TIMEOUT 30
        PROCESSORS 1
        RESOURCE_GROUPS gpus:1)
endif()
