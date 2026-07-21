# SPDX-License-Identifier: GPL-2.0-or-later

foreach(required ROOTFS KERNEL_BUILD DEBUGFS)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(READ "${KERNEL_BUILD}/include/config/kernel.release" kernel_release)
string(STRIP "${kernel_release}" kernel_release)
set(modules_dir "/lib/modules/${kernel_release}")
set(modules_builtin "${KERNEL_BUILD}/modules.builtin")
set(empty_modules_dep "${KERNEL_BUILD}/modules.dep.empty")

if(NOT EXISTS "${ROOTFS}")
    message(FATAL_ERROR "rootfs image does not exist: ${ROOTFS}")
endif()
if(NOT EXISTS "${modules_builtin}")
    message(FATAL_ERROR "kernel built-in metadata does not exist: ${modules_builtin}")
endif()

file(WRITE "${empty_modules_dep}" "")

function(run_debugfs command)
    execute_process(
        COMMAND "${DEBUGFS}" -w -R "${command}" "${ROOTFS}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "debugfs command failed (${command}):\n${output}${error}")
    endif()
endfunction()

# mkdir and rm are intentionally idempotent; debugfs reports an in-filesystem
# diagnostic but still returns success when the directory/file already exists.
run_debugfs("mkdir /lib/modules")
run_debugfs("mkdir ${modules_dir}")
run_debugfs("rm ${modules_dir}/modules.builtin")
run_debugfs("rm ${modules_dir}/modules.dep")
run_debugfs("write ${modules_builtin} ${modules_dir}/modules.builtin")
run_debugfs("write ${empty_modules_dep} ${modules_dir}/modules.dep")

message(STATUS
    "Updated ${ROOTFS} with static-kernel metadata for ${kernel_release}")
