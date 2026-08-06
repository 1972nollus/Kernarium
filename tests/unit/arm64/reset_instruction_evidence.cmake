# SPDX-License-Identifier: GPL-2.0-or-later
if(NOT DEFINED BUILD_ROOT OR BUILD_ROOT STREQUAL "")
    message(FATAL_ERROR "BUILD_ROOT is required")
endif()
set(EVIDENCE_DIRECTORY "${BUILD_ROOT}/instruction-evidence")
set(EVIDENCE_LOCK "${BUILD_ROOT}/instruction-evidence.lock")
file(LOCK "${EVIDENCE_LOCK}" GUARD PROCESS TIMEOUT 0
    RESULT_VARIABLE EVIDENCE_LOCK_RESULT)
if(NOT EVIDENCE_LOCK_RESULT STREQUAL "0")
    message(FATAL_ERROR
        "instruction evidence producer/publication is active: "
        "${EVIDENCE_LOCK_RESULT}")
endif()
file(REMOVE_RECURSE "${EVIDENCE_DIRECTORY}")
file(MAKE_DIRECTORY "${EVIDENCE_DIRECTORY}")
