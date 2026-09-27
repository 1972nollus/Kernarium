# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED BUILD_ROOT)
    message(FATAL_ERROR "BUILD_ROOT is required")
endif()
cmake_path(ABSOLUTE_PATH BUILD_ROOT NORMALIZE OUTPUT_VARIABLE BUILD_ROOT)

set(REPLY_ROOT "${BUILD_ROOT}/.cmake/api/v1/reply")
if(NOT DEFINED ARCHITECTURE_MANIFEST)
    set(ARCHITECTURE_MANIFEST "${BUILD_ROOT}/uml-architecture-targets.tsv")
endif()
file(GLOB INDEX_FILES LIST_DIRECTORIES false "${REPLY_ROOT}/index-*.json")
if(NOT INDEX_FILES)
    message(FATAL_ERROR
        "CMake File API reply is missing below ${REPLY_ROOT}; reconfigure first")
endif()
list(SORT INDEX_FILES)
list(GET INDEX_FILES -1 INDEX_FILE)
file(READ "${INDEX_FILE}" INDEX_JSON)

string(JSON CODEMODEL_FILE ERROR_VARIABLE CODEMODEL_ERROR
    GET "${INDEX_JSON}" reply codemodel-v2 jsonFile)
if(CODEMODEL_ERROR)
    message(FATAL_ERROR "CMake File API codemodel reply is unavailable: ${CODEMODEL_ERROR}")
endif()
file(READ "${REPLY_ROOT}/${CODEMODEL_FILE}" CODEMODEL_JSON)
string(JSON CONFIGURATION_COUNT LENGTH "${CODEMODEL_JSON}" configurations)
if(CONFIGURATION_COUNT LESS 1)
    message(FATAL_ERROR "CMake File API codemodel has no configurations")
endif()
string(JSON SOURCE_ROOT GET "${CODEMODEL_JSON}" paths source)

if(NOT EXISTS "${ARCHITECTURE_MANIFEST}")
    message(FATAL_ERROR
        "Architecture target manifest is missing: ${ARCHITECTURE_MANIFEST}")
endif()
file(STRINGS "${ARCHITECTURE_MANIFEST}" ARCHITECTURE_ROWS)
set(ARCHITECTURE_TARGETS)
set(ARCHITECTURE_LAYERS)
set(ARCHITECTURE_KINDS)
set(ARCHITECTURE_ENGINE_KINDS)
set(ARCHITECTURE_DECLARED_DEPENDENCIES)
foreach(ROW IN LISTS ARCHITECTURE_ROWS)
    if(ROW MATCHES "^#" OR ROW STREQUAL "")
        continue()
    endif()
    string(REPLACE "\t" ";" FIELDS "${ROW}")
    list(LENGTH FIELDS FIELD_COUNT)
    if(NOT FIELD_COUNT EQUAL 5)
        message(FATAL_ERROR "Malformed architecture manifest row: ${ROW}")
    endif()
    list(GET FIELDS 0 TARGET)
    list(GET FIELDS 1 LAYER)
    list(GET FIELDS 2 KIND)
    list(GET FIELDS 3 ENGINE_KIND)
    list(GET FIELDS 4 DEPENDENCIES)
    if(NOT KIND MATCHES "^(interface|codemodel)$")
        message(FATAL_ERROR "Unknown architecture target kind for ${TARGET}: ${KIND}")
    endif()
    if(LAYER STREQUAL "cpu")
        if(NOT ENGINE_KIND MATCHES
           "^(native|gadget|shared|mixed)$")
            message(FATAL_ERROR
                "CPU architecture target ${TARGET} has invalid engine kind: "
                "${ENGINE_KIND}")
        endif()
    elseif(NOT ENGINE_KIND STREQUAL "-")
        message(FATAL_ERROR
            "Non-CPU architecture target ${TARGET} has engine kind "
            "${ENGINE_KIND}")
    endif()
    list(FIND ARCHITECTURE_TARGETS "${TARGET}" EXISTING_TARGET)
    if(NOT EXISTING_TARGET EQUAL -1)
        message(FATAL_ERROR "Duplicate architecture target: ${TARGET}")
    endif()
    list(APPEND ARCHITECTURE_TARGETS "${TARGET}")
    list(APPEND ARCHITECTURE_LAYERS "${LAYER}")
    list(APPEND ARCHITECTURE_KINDS "${KIND}")
    list(APPEND ARCHITECTURE_ENGINE_KINDS "${ENGINE_KIND}")
    list(APPEND ARCHITECTURE_DECLARED_DEPENDENCIES "${DEPENDENCIES}")
endforeach()

function(uml_architecture_layer TARGET OUTPUT)
    list(FIND ARCHITECTURE_TARGETS "${TARGET}" TARGET_INDEX)
    if(TARGET_INDEX EQUAL -1)
        set(${OUTPUT} "" PARENT_SCOPE)
        return()
    endif()
    list(GET ARCHITECTURE_LAYERS ${TARGET_INDEX} TARGET_LAYER)
    set(${OUTPUT} "${TARGET_LAYER}" PARENT_SCOPE)
endfunction()

function(uml_architecture_edge_allowed FROM TO OUTPUT)
    if(FROM STREQUAL TO)
        set(${OUTPUT} TRUE PARENT_SCOPE)
        return()
    endif()
    set(ALLOWED FALSE)
    if(FROM STREQUAL "memory")
        if(TO STREQUAL "platform")
            set(ALLOWED TRUE)
        endif()
    elseif(FROM STREQUAL "cpu")
        if(TO STREQUAL "memory" OR TO STREQUAL "platform")
            set(ALLOWED TRUE)
        endif()
    elseif(FROM STREQUAL "device")
        if(TO STREQUAL "memory" OR TO STREQUAL "platform")
            set(ALLOWED TRUE)
        endif()
    elseif(FROM STREQUAL "runtime")
        if(TO STREQUAL "cpu" OR TO STREQUAL "memory" OR
           TO STREQUAL "device" OR TO STREQUAL "platform")
            set(ALLOWED TRUE)
        endif()
    elseif(FROM STREQUAL "emulator")
        if(TO STREQUAL "api" OR TO STREQUAL "runtime" OR
           TO STREQUAL "device")
            set(ALLOWED TRUE)
        endif()
    elseif(FROM STREQUAL "app")
        if(TO STREQUAL "api" OR TO STREQUAL "emulator" OR
           TO STREQUAL "device")
            set(ALLOWED TRUE)
        endif()
    endif()
    set(${OUTPUT} ${ALLOWED} PARENT_SCOPE)
endfunction()

function(uml_architecture_engine_kind TARGET OUTPUT)
    list(FIND ARCHITECTURE_TARGETS "${TARGET}" ENGINE_INDEX)
    if(NOT ENGINE_INDEX EQUAL -1)
        list(GET ARCHITECTURE_ENGINE_KINDS ${ENGINE_INDEX} ENGINE_KIND)
        if(ENGINE_KIND MATCHES
           "^(native|gadget|shared|mixed)$")
            set(${OUTPUT} "${ENGINE_KIND}" PARENT_SCOPE)
            return()
        endif()
    endif()
    set(${OUTPUT} "" PARENT_SCOPE)
endfunction()

function(uml_architecture_engine_edge_allowed FROM_TARGET TO_TARGET OUTPUT)
    uml_architecture_engine_kind("${FROM_TARGET}" FROM_ENGINE_KIND)
    uml_architecture_engine_kind("${TO_TARGET}" TO_ENGINE_KIND)
    if(NOT FROM_ENGINE_KIND OR NOT TO_ENGINE_KIND OR
       FROM_ENGINE_KIND STREQUAL "mixed")
        set(${OUTPUT} TRUE PARENT_SCOPE)
        return()
    endif()
    if(FROM_ENGINE_KIND STREQUAL "shared")
        set(ALLOWED_KIND "shared")
    else()
        set(ALLOWED_KIND "^(shared|${FROM_ENGINE_KIND})$")
    endif()
    if(TO_ENGINE_KIND MATCHES "${ALLOWED_KIND}")
        set(${OUTPUT} TRUE PARENT_SCOPE)
    else()
        set(${OUTPUT} FALSE PARENT_SCOPE)
    endif()
endfunction()

string(JSON TARGET_COUNT LENGTH "${CODEMODEL_JSON}" configurations 0 targets)
set(TARGET_IDS)
set(TARGET_NAMES)
set(TARGET_FILES)
set(TARGET_SOURCE_PATHS)
foreach(INDEX RANGE 0 ${TARGET_COUNT})
    if(INDEX EQUAL TARGET_COUNT)
        break()
    endif()
    string(JSON TARGET_ID GET "${CODEMODEL_JSON}" configurations 0 targets ${INDEX} id)
    string(JSON TARGET_NAME GET "${CODEMODEL_JSON}" configurations 0 targets ${INDEX} name)
    string(JSON TARGET_FILE GET "${CODEMODEL_JSON}" configurations 0 targets ${INDEX} jsonFile)
    file(READ "${REPLY_ROOT}/${TARGET_FILE}" TARGET_DESCRIPTION_JSON)
    string(JSON TARGET_SOURCE_PATH GET "${TARGET_DESCRIPTION_JSON}" paths source)
    list(APPEND TARGET_IDS "${TARGET_ID}")
    list(APPEND TARGET_NAMES "${TARGET_NAME}")
    list(APPEND TARGET_FILES "${TARGET_FILE}")
    list(APPEND TARGET_SOURCE_PATHS "${TARGET_SOURCE_PATH}")
endforeach()

foreach(DECLARED_TARGET IN LISTS ARCHITECTURE_TARGETS)
    list(FIND ARCHITECTURE_TARGETS "${DECLARED_TARGET}" DECLARED_TARGET_LIST_INDEX)
    list(GET ARCHITECTURE_KINDS ${DECLARED_TARGET_LIST_INDEX} DECLARED_TARGET_KIND)
    if(DECLARED_TARGET_KIND STREQUAL "interface")
        # CMake's codemodel intentionally omits pure interface libraries.
        # Their configured dependency edges are still audited below from the
        # generated architecture manifest.
        continue()
    endif()
    list(FIND TARGET_NAMES "${DECLARED_TARGET}" DECLARED_TARGET_INDEX)
    if(DECLARED_TARGET_INDEX EQUAL -1)
        message(FATAL_ERROR
            "Declared architecture target is absent from the CMake File API: ${DECLARED_TARGET}")
    endif()
endforeach()

# The declaration list must also be complete in the other direction. Catch a
# new concrete Darwin target even when nobody remembered to add it to the
# hand-maintained layer graph. Pure interface targets are covered by the
# configure-time recursive target check in UmlArchitecture.cmake because the
# File API intentionally omits them.
foreach(INDEX RANGE 0 ${TARGET_COUNT})
    if(INDEX EQUAL TARGET_COUNT)
        break()
    endif()
    list(GET TARGET_SOURCE_PATHS ${INDEX} TARGET_SOURCE_PATH)
    if(TARGET_SOURCE_PATH MATCHES "^darwin(/|$)")
        list(GET TARGET_NAMES ${INDEX} TARGET_NAME)
        list(FIND ARCHITECTURE_TARGETS "${TARGET_NAME}" TARGET_LAYER_INDEX)
        if(TARGET_LAYER_INDEX EQUAL -1)
            message(FATAL_ERROR
                "Configured Darwin target has no architecture declaration: "
                "${TARGET_NAME} (${TARGET_SOURCE_PATH})")
        endif()
    endif()
endforeach()

# Audit every declared first-party edge, including pure interface contracts
# which do not appear in CMake's File API codemodel.  The manifest is emitted
# from the configured target LINK_LIBRARIES properties, not handwritten data.
list(LENGTH ARCHITECTURE_TARGETS ARCHITECTURE_TARGET_COUNT)
if(ARCHITECTURE_TARGET_COUNT GREATER 0)
    math(EXPR ARCHITECTURE_TARGET_LAST_INDEX "${ARCHITECTURE_TARGET_COUNT} - 1")
    foreach(ARCHITECTURE_TARGET_INDEX RANGE 0 ${ARCHITECTURE_TARGET_LAST_INDEX})
        list(GET ARCHITECTURE_TARGETS ${ARCHITECTURE_TARGET_INDEX} TARGET_NAME)
        list(GET ARCHITECTURE_LAYERS ${ARCHITECTURE_TARGET_INDEX} TARGET_LAYER)
        list(GET ARCHITECTURE_DECLARED_DEPENDENCIES ${ARCHITECTURE_TARGET_INDEX}
            DEPENDENCY_TEXT)
        if(DEPENDENCY_TEXT STREQUAL "-")
            continue()
        endif()
        string(REPLACE "," ";" DECLARED_DEPENDENCIES "${DEPENDENCY_TEXT}")
        foreach(DEPENDENCY IN LISTS DECLARED_DEPENDENCIES)
            if(NOT DEPENDENCY MATCHES "^link@([A-Za-z0-9_.+-]+)$")
                message(FATAL_ERROR
                    "Malformed architecture dependency for ${TARGET_NAME}: ${DEPENDENCY}")
            endif()
            set(DEPENDENCY_NAME "${CMAKE_MATCH_1}")
            uml_architecture_layer("${DEPENDENCY_NAME}" DEPENDENCY_LAYER)
            if(NOT DEPENDENCY_LAYER)
                message(FATAL_ERROR
                    "Architecture manifest references an undeclared dependency: "
                    "${TARGET_NAME} -> ${DEPENDENCY_NAME}")
            endif()
            uml_architecture_engine_edge_allowed(
                "${TARGET_NAME}" "${DEPENDENCY_NAME}" ENGINE_EDGE_ALLOWED)
            if(NOT ENGINE_EDGE_ALLOWED)
                message(FATAL_ERROR
                    "Forbidden cross-engine link edge: "
                    "${TARGET_NAME} -> ${DEPENDENCY_NAME}")
            endif()
            uml_architecture_edge_allowed(
                "${TARGET_LAYER}" "${DEPENDENCY_LAYER}" EDGE_ALLOWED)
            if(NOT EDGE_ALLOWED)
                message(FATAL_ERROR
                    "Forbidden declared link architecture edge: "
                    "${TARGET_NAME} (${TARGET_LAYER}) -> "
                    "${DEPENDENCY_NAME} (${DEPENDENCY_LAYER})")
            endif()
        endforeach()
    endforeach()
endif()

# Traverse the configured declaration graph separately from the codemodel so
# cycles made entirely of interface libraries cannot disappear from the audit.
function(uml_audit_declared_visit NODE PATH)
    get_property(STATE GLOBAL PROPERTY "UML_DECLARED_AUDIT_STATE_${NODE}")
    if(STATE STREQUAL "active")
        set(CYCLE_PATH "${PATH};${NODE}")
        string(REPLACE ";" " -> " CYCLE_TEXT "${CYCLE_PATH}")
        message(FATAL_ERROR
            "Declared architecture target dependency cycle: ${CYCLE_TEXT}")
    endif()
    if(STATE STREQUAL "done")
        return()
    endif()
    set_property(GLOBAL PROPERTY "UML_DECLARED_AUDIT_STATE_${NODE}" active)
    set(NEXT_PATH "${PATH};${NODE}")
    get_property(DEPENDENCIES GLOBAL PROPERTY
        "UML_DECLARED_AUDIT_DEPS_${NODE}")
    foreach(DEPENDENCY IN LISTS DEPENDENCIES)
        uml_audit_declared_visit("${DEPENDENCY}" "${NEXT_PATH}")
    endforeach()
    set_property(GLOBAL PROPERTY "UML_DECLARED_AUDIT_STATE_${NODE}" done)
endfunction()

if(ARCHITECTURE_TARGET_COUNT GREATER 0)
    foreach(ARCHITECTURE_TARGET_INDEX RANGE 0 ${ARCHITECTURE_TARGET_LAST_INDEX})
        list(GET ARCHITECTURE_TARGETS ${ARCHITECTURE_TARGET_INDEX} TARGET_NAME)
        list(GET ARCHITECTURE_DECLARED_DEPENDENCIES ${ARCHITECTURE_TARGET_INDEX}
            DEPENDENCY_TEXT)
        set(DECLARED_TARGET_DEPENDENCIES)
        if(NOT DEPENDENCY_TEXT STREQUAL "-")
            string(REPLACE "," ";" DECLARED_DEPENDENCIES "${DEPENDENCY_TEXT}")
            foreach(DEPENDENCY IN LISTS DECLARED_DEPENDENCIES)
                if(DEPENDENCY MATCHES "^link@([A-Za-z0-9_.+-]+)$")
                    list(APPEND DECLARED_TARGET_DEPENDENCIES
                        "${CMAKE_MATCH_1}")
                endif()
            endforeach()
        endif()
        set_property(GLOBAL PROPERTY
            "UML_DECLARED_AUDIT_DEPS_${TARGET_NAME}"
            "${DECLARED_TARGET_DEPENDENCIES}")
    endforeach()
    foreach(TARGET_NAME IN LISTS ARCHITECTURE_TARGETS)
        uml_audit_declared_visit("${TARGET_NAME}" "")
    endforeach()
endif()

# A neutral interface carrier must not conceal an engine-to-engine dependency.
# Check reachability from every declared engine owner after the direct-edge and
# cycle audits have established a well-formed graph.  File API dependencies
# provide the same protection for concrete configured targets; this traversal
# additionally covers pure interface chains omitted from the codemodel.
function(uml_audit_engine_reachability ROOT NODE PATH)
    get_property(VISITED GLOBAL PROPERTY
        "UML_ENGINE_REACHABILITY_${ROOT}_${NODE}")
    if(VISITED)
        return()
    endif()
    set_property(GLOBAL PROPERTY
        "UML_ENGINE_REACHABILITY_${ROOT}_${NODE}" TRUE)

    uml_architecture_engine_kind("${ROOT}" ROOT_ENGINE_KIND)
    uml_architecture_engine_kind("${NODE}" NODE_ENGINE_KIND)
    uml_architecture_engine_edge_allowed(
        "${ROOT}" "${NODE}" REACHABILITY_ALLOWED)
    if(NODE_ENGINE_KIND AND NOT REACHABILITY_ALLOWED)
        if(PATH)
            set(EDGE_PATH "${PATH};${NODE}")
        else()
            set(EDGE_PATH "${NODE}")
        endif()
        string(REPLACE ";" " -> " EDGE_TEXT "${EDGE_PATH}")
        message(FATAL_ERROR
            "Forbidden cross-engine transitive dependency edge: ${EDGE_TEXT}")
    endif()

    get_property(DEPENDENCIES GLOBAL PROPERTY
        "UML_DECLARED_AUDIT_DEPS_${NODE}")
    if(PATH)
        set(NEXT_PATH "${PATH};${NODE}")
    else()
        set(NEXT_PATH "${NODE}")
    endif()
    foreach(DEPENDENCY IN LISTS DEPENDENCIES)
        uml_audit_engine_reachability(
            "${ROOT}" "${DEPENDENCY}" "${NEXT_PATH}")
    endforeach()
endfunction()

foreach(TARGET_NAME IN LISTS ARCHITECTURE_TARGETS)
    uml_architecture_engine_kind("${TARGET_NAME}" TARGET_ENGINE_KIND)
    if(TARGET_ENGINE_KIND)
        uml_audit_engine_reachability(
            "${TARGET_NAME}" "${TARGET_NAME}" "")
    endif()
endforeach()

function(uml_audit_visit NODE PATH)
    get_property(STATE GLOBAL PROPERTY "UML_AUDIT_STATE_${NODE}")
    if(STATE STREQUAL "active")
        set(CYCLE_PATH "${PATH};${NODE}")
        string(REPLACE ";" " -> " CYCLE_TEXT "${CYCLE_PATH}")
        message(FATAL_ERROR "CMake target dependency cycle: ${CYCLE_TEXT}")
    endif()
    if(STATE STREQUAL "done")
        return()
    endif()

    set_property(GLOBAL PROPERTY "UML_AUDIT_STATE_${NODE}" active)
    set(NEXT_PATH "${PATH};${NODE}")
    get_property(DEPENDENCIES GLOBAL PROPERTY "UML_AUDIT_DEPS_${NODE}")
    foreach(DEPENDENCY IN LISTS DEPENDENCIES)
        uml_audit_visit(${DEPENDENCY} "${NEXT_PATH}")
    endforeach()
    set_property(GLOBAL PROPERTY "UML_AUDIT_STATE_${NODE}" done)
endfunction()

foreach(INDEX RANGE 0 ${TARGET_COUNT})
    if(INDEX EQUAL TARGET_COUNT)
        break()
    endif()
    list(GET TARGET_FILES ${INDEX} TARGET_FILE)
    file(READ "${REPLY_ROOT}/${TARGET_FILE}" TARGET_JSON)
    string(JSON DEPENDENCY_COUNT ERROR_VARIABLE DEPENDENCY_ERROR
        LENGTH "${TARGET_JSON}" dependencies)
    set(DEPENDENCY_INDICES)
    if(NOT DEPENDENCY_ERROR)
        foreach(DEPENDENCY_INDEX RANGE 0 ${DEPENDENCY_COUNT})
            if(DEPENDENCY_INDEX EQUAL DEPENDENCY_COUNT)
                break()
            endif()
            string(JSON DEPENDENCY_ID GET "${TARGET_JSON}"
                dependencies ${DEPENDENCY_INDEX} id)
            list(FIND TARGET_IDS "${DEPENDENCY_ID}" DEPENDENCY_TARGET_INDEX)
            if(NOT DEPENDENCY_TARGET_INDEX EQUAL -1)
                list(APPEND DEPENDENCY_INDICES ${DEPENDENCY_TARGET_INDEX})
                list(GET TARGET_NAMES ${INDEX} DEPENDENT_TARGET_NAME)
                list(GET TARGET_NAMES ${DEPENDENCY_TARGET_INDEX}
                    DEPENDENCY_TARGET_NAME)
                uml_architecture_engine_edge_allowed(
                    "${DEPENDENT_TARGET_NAME}" "${DEPENDENCY_TARGET_NAME}"
                    CODEMODEL_ENGINE_EDGE_ALLOWED)
                if(NOT CODEMODEL_ENGINE_EDGE_ALLOWED)
                    message(FATAL_ERROR
                        "Forbidden cross-engine configured dependency edge: "
                        "${DEPENDENT_TARGET_NAME} -> ${DEPENDENCY_TARGET_NAME}")
                endif()
                # File API dependencies include transitive static-link
                # archives.  They remain authoritative for cycle detection,
                # and for engine isolation: interface carriers must not hide
                # a concrete engine dependency. Direct configured edges above
                # retain the finer direct-link diagnostics and layer
                # direction without mistaking archive propagation for a
                # source-level architecture dependency.
            endif()
        endforeach()
    endif()
    set_property(GLOBAL PROPERTY "UML_AUDIT_DEPS_${INDEX}" "${DEPENDENCY_INDICES}")
endforeach()

foreach(INDEX RANGE 0 ${TARGET_COUNT})
    if(INDEX EQUAL TARGET_COUNT)
        break()
    endif()
    uml_audit_visit(${INDEX} "")
endforeach()

list(LENGTH ARCHITECTURE_TARGETS ARCHITECTURE_TARGET_TOTAL)
message(STATUS
    "CMake File API architecture audit passed: ${TARGET_COUNT} targets, "
    "${ARCHITECTURE_TARGET_TOTAL} layered targets, no forbidden edges or "
    "cycles")
