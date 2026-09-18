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

# Exercise every declared engine-kind pairing in one audit process. This keeps
# the registered negative fixture fast while testing the exact classifier used
# below for real manifest edges. The fixture's third field is retained for
# diagnostics and must name a supported edge kind.
if(DEFINED ENGINE_EDGE_FIXTURES)
    if(NOT EXISTS "${ENGINE_EDGE_FIXTURES}")
        message(FATAL_ERROR
            "Engine-edge fixture file is missing: ${ENGINE_EDGE_FIXTURES}")
    endif()
    file(STRINGS "${ENGINE_EDGE_FIXTURES}" ENGINE_EDGE_FIXTURE_ROWS)
    foreach(FIXTURE_ROW IN LISTS ENGINE_EDGE_FIXTURE_ROWS)
        if(FIXTURE_ROW MATCHES "^#" OR FIXTURE_ROW STREQUAL "")
            continue()
        endif()
        string(REPLACE "\t" ";" FIXTURE_FIELDS "${FIXTURE_ROW}")
        list(LENGTH FIXTURE_FIELDS FIXTURE_FIELD_COUNT)
        if(NOT FIXTURE_FIELD_COUNT EQUAL 3)
            message(FATAL_ERROR
                "Malformed engine-edge fixture row: ${FIXTURE_ROW}")
        endif()
        list(GET FIXTURE_FIELDS 0 FIXTURE_SOURCE)
        list(GET FIXTURE_FIELDS 1 FIXTURE_DESTINATION)
        list(GET FIXTURE_FIELDS 2 FIXTURE_KIND)
        if(NOT FIXTURE_KIND MATCHES "^(link|include)$")
            message(FATAL_ERROR
                "Unknown engine-edge fixture kind: ${FIXTURE_ROW}")
        endif()
        uml_architecture_engine_kind("${FIXTURE_SOURCE}" FIXTURE_SOURCE_KIND)
        uml_architecture_engine_kind(
            "${FIXTURE_DESTINATION}" FIXTURE_DESTINATION_KIND)
        if(NOT FIXTURE_SOURCE_KIND OR NOT FIXTURE_DESTINATION_KIND OR
           FIXTURE_SOURCE_KIND STREQUAL FIXTURE_DESTINATION_KIND)
            message(FATAL_ERROR
                "Engine-edge fixture is not cross-engine: ${FIXTURE_ROW}")
        endif()
        uml_architecture_engine_edge_allowed(
            "${FIXTURE_SOURCE}" "${FIXTURE_DESTINATION}" FIXTURE_ALLOWED)
        if(FIXTURE_ALLOWED)
            message(FATAL_ERROR
                "Accepted cross-engine ${FIXTURE_KIND} fixture: "
                "${FIXTURE_SOURCE} -> ${FIXTURE_DESTINATION}")
        endif()
    endforeach()
endif()

# Positive classifier fixtures complement the forbidden matrix above.  They
# exercise the exact policy function in this audit process without paying for
# a separate CMake configure/File API scan for every allowed kind pairing.
if(DEFINED ENGINE_EDGE_ALLOWED_FIXTURES)
    if(NOT EXISTS "${ENGINE_EDGE_ALLOWED_FIXTURES}")
        message(FATAL_ERROR
            "Allowed engine-edge fixture file is missing: "
            "${ENGINE_EDGE_ALLOWED_FIXTURES}")
    endif()
    file(STRINGS "${ENGINE_EDGE_ALLOWED_FIXTURES}"
        ENGINE_EDGE_ALLOWED_FIXTURE_ROWS)
    foreach(FIXTURE_ROW IN LISTS ENGINE_EDGE_ALLOWED_FIXTURE_ROWS)
        if(FIXTURE_ROW MATCHES "^#" OR FIXTURE_ROW STREQUAL "")
            continue()
        endif()
        string(REPLACE "\t" ";" FIXTURE_FIELDS "${FIXTURE_ROW}")
        list(LENGTH FIXTURE_FIELDS FIXTURE_FIELD_COUNT)
        if(NOT FIXTURE_FIELD_COUNT EQUAL 3)
            message(FATAL_ERROR
                "Malformed allowed engine-edge fixture row: ${FIXTURE_ROW}")
        endif()
        list(GET FIXTURE_FIELDS 0 FIXTURE_SOURCE)
        list(GET FIXTURE_FIELDS 1 FIXTURE_DESTINATION)
        list(GET FIXTURE_FIELDS 2 FIXTURE_KIND)
        if(NOT FIXTURE_KIND MATCHES "^(link|include)$")
            message(FATAL_ERROR
                "Unknown allowed engine-edge fixture kind: ${FIXTURE_ROW}")
        endif()
        uml_architecture_engine_kind("${FIXTURE_SOURCE}"
            FIXTURE_SOURCE_KIND)
        uml_architecture_engine_kind("${FIXTURE_DESTINATION}"
            FIXTURE_DESTINATION_KIND)
        if(NOT FIXTURE_SOURCE_KIND OR NOT FIXTURE_DESTINATION_KIND)
            message(FATAL_ERROR
                "Allowed engine-edge fixture lacks ownership: ${FIXTURE_ROW}")
        endif()
        uml_architecture_engine_edge_allowed(
            "${FIXTURE_SOURCE}" "${FIXTURE_DESTINATION}" FIXTURE_ALLOWED)
        if(NOT FIXTURE_ALLOWED)
            message(FATAL_ERROR
                "Rejected allowed cross-engine ${FIXTURE_KIND} fixture: "
                "${FIXTURE_SOURCE} -> ${FIXTURE_DESTINATION}")
        endif()
    endforeach()
endif()

string(JSON TARGET_COUNT LENGTH "${CODEMODEL_JSON}" configurations 0 targets)
set(TARGET_IDS)
set(TARGET_NAMES)
set(TARGET_FILES)
set(TARGET_SOURCE_PATHS)
set(ENGINE_INCLUDE_DIRECTORIES_native)
set(ENGINE_INCLUDE_DIRECTORIES_gadget)
set(ENGINE_INCLUDE_DIRECTORIES_shared)
set(ENGINE_INCLUDE_DIRECTORIES_mixed)
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

    # The codemodel's compile groups contain the fully evaluated include path,
    # including usage requirements propagated through interface targets. Keep
    # those paths with their concrete engine owner so a basename-only include
    # cannot hide a cross-engine dependency.
    uml_architecture_engine_kind("${TARGET_NAME}" TARGET_ENGINE_KIND)
    if(TARGET_ENGINE_KIND)
        string(JSON COMPILE_GROUP_COUNT ERROR_VARIABLE COMPILE_GROUP_ERROR
            LENGTH "${TARGET_DESCRIPTION_JSON}" compileGroups)
        if(NOT COMPILE_GROUP_ERROR)
            foreach(COMPILE_GROUP_INDEX RANGE 0 ${COMPILE_GROUP_COUNT})
                if(COMPILE_GROUP_INDEX EQUAL COMPILE_GROUP_COUNT)
                    break()
                endif()
                string(JSON INCLUDE_COUNT ERROR_VARIABLE INCLUDE_ERROR
                    LENGTH "${TARGET_DESCRIPTION_JSON}"
                    compileGroups ${COMPILE_GROUP_INDEX} includes)
                if(INCLUDE_ERROR)
                    continue()
                endif()
                foreach(INCLUDE_INDEX RANGE 0 ${INCLUDE_COUNT})
                    if(INCLUDE_INDEX EQUAL INCLUDE_COUNT)
                        break()
                    endif()
                    string(JSON INCLUDE_PATH GET "${TARGET_DESCRIPTION_JSON}"
                        compileGroups ${COMPILE_GROUP_INDEX}
                        includes ${INCLUDE_INDEX} path)
                    if(NOT IS_ABSOLUTE "${INCLUDE_PATH}")
                        cmake_path(ABSOLUTE_PATH INCLUDE_PATH
                            BASE_DIRECTORY "${SOURCE_ROOT}" NORMALIZE
                            OUTPUT_VARIABLE INCLUDE_PATH)
                    else()
                        cmake_path(NORMAL_PATH INCLUDE_PATH
                            OUTPUT_VARIABLE INCLUDE_PATH)
                    endif()
                    list(APPEND
                        ENGINE_INCLUDE_DIRECTORIES_${TARGET_ENGINE_KIND}
                        "${INCLUDE_PATH}")
                endforeach()
            endforeach()
        endif()
    endif()
endforeach()
foreach(ENGINE_KIND IN ITEMS native gadget shared mixed)
    list(REMOVE_DUPLICATES ENGINE_INCLUDE_DIRECTORIES_${ENGINE_KIND})
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
            if(NOT DEPENDENCY MATCHES
               "^(link|include)@([A-Za-z0-9_.+-]+)$")
                message(FATAL_ERROR
                    "Malformed architecture dependency for ${TARGET_NAME}: ${DEPENDENCY}")
            endif()
            set(DEPENDENCY_KIND "${CMAKE_MATCH_1}")
            set(DEPENDENCY_NAME "${CMAKE_MATCH_2}")
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
                    "Forbidden cross-engine ${DEPENDENCY_KIND} edge: "
                    "${TARGET_NAME} -> ${DEPENDENCY_NAME}")
            endif()
            uml_architecture_edge_allowed(
                "${TARGET_LAYER}" "${DEPENDENCY_LAYER}" EDGE_ALLOWED)
            if(NOT EDGE_ALLOWED)
                message(FATAL_ERROR
                    "Forbidden declared ${DEPENDENCY_KIND} architecture edge: "
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
                if(DEPENDENCY MATCHES
                   "^(link|include)@([A-Za-z0-9_.+-]+)$")
                    list(APPEND DECLARED_TARGET_DEPENDENCIES
                        "${CMAKE_MATCH_2}")
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

# Resolve an on-disk path to its concrete engine owner. Shared ARM64 headers
# live above these directories and intentionally have no concrete owner.
set(UML_ENGINE_DIRECTORY_NAMES jit gadgets)
set(UML_ENGINE_KINDS native gadget)
function(uml_engine_path_kind INPUT_PATH OUTPUT)
    set(CANDIDATE_PATH "${INPUT_PATH}")
    if(EXISTS "${CANDIDATE_PATH}")
        file(REAL_PATH "${CANDIDATE_PATH}" CANDIDATE_PATH)
    else()
        cmake_path(NORMAL_PATH CANDIDATE_PATH OUTPUT_VARIABLE CANDIDATE_PATH)
    endif()
    foreach(DIRECTORY_NAME ENGINE_KIND IN ZIP_LISTS
            UML_ENGINE_DIRECTORY_NAMES UML_ENGINE_KINDS)
        set(ENGINE_ROOT
            "${SOURCE_ROOT}/darwin/cpu/arm64/${DIRECTORY_NAME}")
        if(EXISTS "${ENGINE_ROOT}")
            file(REAL_PATH "${ENGINE_ROOT}" ENGINE_ROOT)
        endif()
        cmake_path(IS_PREFIX ENGINE_ROOT "${CANDIDATE_PATH}" NORMALIZE
            PATH_IS_ENGINE_OWNED)
        if(PATH_IS_ENGINE_OWNED)
            set(${OUTPUT} "${ENGINE_KIND}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${OUTPUT} "" PARENT_SCOPE)
endfunction()

# Inspect each configured engine target's actual source files with that
# target's explicit engine metadata. Directory layout remains useful for
# assigning private-header ownership, but it must not be the way we infer the
# owner of the including source: CMake can legally compile a source from any
# repository directory. Follow first-party includes recursively with the
# source's evaluated compile-group include paths so a relocated source or an
# intermediate shared header cannot conceal a concrete cross-engine include.
function(uml_audit_engine_owned_file ENGINE_PATH FROM_ENGINE_KIND OWNER_TARGET)
    set(INCLUDE_DIRECTORIES ${ARGN})
    if(NOT EXISTS "${ENGINE_PATH}" OR IS_DIRECTORY "${ENGINE_PATH}")
        return()
    endif()
    file(REAL_PATH "${ENGINE_PATH}" ENGINE_PATH)

    string(JOIN "\n" INCLUDE_DIRECTORY_KEY ${INCLUDE_DIRECTORIES})
    string(SHA256 VISIT_KEY
        "${FROM_ENGINE_KIND}\n${ENGINE_PATH}\n${INCLUDE_DIRECTORY_KEY}")
    get_property(ALREADY_VISITED GLOBAL PROPERTY
        "UML_ENGINE_INCLUDE_VISITED_${VISIT_KEY}")
    if(ALREADY_VISITED)
        return()
    endif()
    set_property(GLOBAL PROPERTY
        "UML_ENGINE_INCLUDE_VISITED_${VISIT_KEY}" TRUE)

    uml_engine_path_kind("${ENGINE_PATH}" FILE_ENGINE_KIND)
    if(FILE_ENGINE_KIND AND
       NOT FILE_ENGINE_KIND STREQUAL FROM_ENGINE_KIND)
        message(FATAL_ERROR
            "Forbidden cross-engine source ownership: ${OWNER_TARGET} "
            "(${FROM_ENGINE_KIND}) compiles or includes ${ENGINE_PATH} "
            "(${FILE_ENGINE_KIND})")
    endif()

    file(STRINGS "${ENGINE_PATH}" INCLUDE_LINES
        REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"][^>\"]+[>\"]")
    foreach(INCLUDE_LINE IN LISTS INCLUDE_LINES)
        if(NOT INCLUDE_LINE MATCHES
           "^[ \t]*#[ \t]*include[ \t]*([<\"])([^>\"]+)[>\"]")
            continue()
        endif()
        set(INCLUDE_DELIMITER "${CMAKE_MATCH_1}")
        set(INCLUDE_NAME "${CMAKE_MATCH_2}")
        set(INCLUDE_SEARCH_DIRECTORIES ${INCLUDE_DIRECTORIES})
        if(NOT INCLUDE_DELIMITER STREQUAL "<")
            get_filename_component(ENGINE_SOURCE_DIRECTORY
                "${ENGINE_PATH}" DIRECTORY)
            list(PREPEND INCLUDE_SEARCH_DIRECTORIES
                "${ENGINE_SOURCE_DIRECTORY}")
        endif()

        set(INCLUDE_RESOLVED FALSE)
        foreach(INCLUDE_SEARCH_DIRECTORY IN LISTS INCLUDE_SEARCH_DIRECTORIES)
            set(INCLUDE_CANDIDATE
                "${INCLUDE_SEARCH_DIRECTORY}/${INCLUDE_NAME}")
            cmake_path(NORMAL_PATH INCLUDE_CANDIDATE
                OUTPUT_VARIABLE INCLUDE_CANDIDATE)
            if(NOT EXISTS "${INCLUDE_CANDIDATE}" OR
               IS_DIRECTORY "${INCLUDE_CANDIDATE}")
                continue()
            endif()
            file(REAL_PATH "${INCLUDE_CANDIDATE}" INCLUDE_CANDIDATE)
            set(INCLUDE_RESOLVED TRUE)
            uml_engine_path_kind("${INCLUDE_CANDIDATE}"
                INCLUDE_ENGINE_KIND)
            if(INCLUDE_ENGINE_KIND AND
               NOT INCLUDE_ENGINE_KIND STREQUAL FROM_ENGINE_KIND)
                message(FATAL_ERROR
                    "Forbidden cross-engine include edge: ${OWNER_TARGET} "
                    "(${FROM_ENGINE_KIND}), ${ENGINE_PATH}: ${INCLUDE_LINE} "
                    "resolves to ${INCLUDE_CANDIDATE} "
                    "(${INCLUDE_ENGINE_KIND})")
            endif()

            cmake_path(IS_PREFIX SOURCE_ROOT "${INCLUDE_CANDIDATE}"
                NORMALIZE INCLUDE_IS_FIRST_PARTY_SOURCE)
            cmake_path(IS_PREFIX BUILD_ROOT "${INCLUDE_CANDIDATE}"
                NORMALIZE INCLUDE_IS_FIRST_PARTY_GENERATED)
            if(INCLUDE_IS_FIRST_PARTY_SOURCE OR
               INCLUDE_IS_FIRST_PARTY_GENERATED)
                uml_audit_engine_owned_file(
                    "${INCLUDE_CANDIDATE}" "${FROM_ENGINE_KIND}"
                    "${OWNER_TARGET}" ${INCLUDE_DIRECTORIES})
            endif()
            break()
        endforeach()
        if(INCLUDE_RESOLVED)
            continue()
        endif()

        # Retain a lexical backstop for a platform-conditional private header
        # that is absent from the host configuration running this audit.
        foreach(TO_DIRECTORY TO_ENGINE_KIND IN ZIP_LISTS
                UML_ENGINE_DIRECTORY_NAMES UML_ENGINE_KINDS)
            if(NOT FROM_ENGINE_KIND STREQUAL TO_ENGINE_KIND AND
               INCLUDE_NAME MATCHES "(^|/)${TO_DIRECTORY}/")
                message(FATAL_ERROR
                    "Forbidden unresolved cross-engine include edge: "
                    "${OWNER_TARGET} (${FROM_ENGINE_KIND}), ${ENGINE_PATH}: "
                    "${INCLUDE_LINE}")
            endif()
        endforeach()
    endforeach()
endfunction()

foreach(INDEX RANGE 0 ${TARGET_COUNT})
    if(INDEX EQUAL TARGET_COUNT)
        break()
    endif()
    list(GET TARGET_NAMES ${INDEX} ENGINE_TARGET_NAME)
    uml_architecture_engine_kind("${ENGINE_TARGET_NAME}"
        ENGINE_TARGET_KIND)
    if(NOT ENGINE_TARGET_KIND OR ENGINE_TARGET_KIND STREQUAL "mixed")
        continue()
    endif()
    list(GET TARGET_FILES ${INDEX} ENGINE_TARGET_FILE)
    file(READ "${REPLY_ROOT}/${ENGINE_TARGET_FILE}" ENGINE_TARGET_JSON)
    string(JSON ENGINE_SOURCE_COUNT ERROR_VARIABLE ENGINE_SOURCE_ERROR
        LENGTH "${ENGINE_TARGET_JSON}" sources)
    if(ENGINE_SOURCE_ERROR)
        continue()
    endif()
    foreach(ENGINE_SOURCE_INDEX RANGE 0 ${ENGINE_SOURCE_COUNT})
        if(ENGINE_SOURCE_INDEX EQUAL ENGINE_SOURCE_COUNT)
            break()
        endif()
        string(JSON ENGINE_SOURCE_PATH GET "${ENGINE_TARGET_JSON}"
            sources ${ENGINE_SOURCE_INDEX} path)
        if(NOT IS_ABSOLUTE "${ENGINE_SOURCE_PATH}")
            cmake_path(ABSOLUTE_PATH ENGINE_SOURCE_PATH
                BASE_DIRECTORY "${SOURCE_ROOT}" NORMALIZE
                OUTPUT_VARIABLE ENGINE_SOURCE_PATH)
        else()
            cmake_path(NORMAL_PATH ENGINE_SOURCE_PATH
                OUTPUT_VARIABLE ENGINE_SOURCE_PATH)
        endif()
        if(NOT ENGINE_SOURCE_PATH MATCHES
           "\\.(c|cc|cpp|cxx|m|mm|h|hh|hpp|hxx|ipp|inl|S|s|asm|inc)$")
            continue()
        endif()

        set(ENGINE_SOURCE_INCLUDE_DIRECTORIES)
        string(JSON ENGINE_COMPILE_GROUP_INDEX ERROR_VARIABLE
            ENGINE_COMPILE_GROUP_ERROR GET "${ENGINE_TARGET_JSON}"
            sources ${ENGINE_SOURCE_INDEX} compileGroupIndex)
        if(NOT ENGINE_COMPILE_GROUP_ERROR)
            string(JSON ENGINE_INCLUDE_COUNT ERROR_VARIABLE
                ENGINE_INCLUDE_ERROR LENGTH "${ENGINE_TARGET_JSON}"
                compileGroups ${ENGINE_COMPILE_GROUP_INDEX} includes)
            if(NOT ENGINE_INCLUDE_ERROR)
                foreach(ENGINE_INCLUDE_INDEX RANGE 0 ${ENGINE_INCLUDE_COUNT})
                    if(ENGINE_INCLUDE_INDEX EQUAL ENGINE_INCLUDE_COUNT)
                        break()
                    endif()
                    string(JSON ENGINE_INCLUDE_PATH GET
                        "${ENGINE_TARGET_JSON}"
                        compileGroups ${ENGINE_COMPILE_GROUP_INDEX}
                        includes ${ENGINE_INCLUDE_INDEX} path)
                    if(NOT IS_ABSOLUTE "${ENGINE_INCLUDE_PATH}")
                        cmake_path(ABSOLUTE_PATH ENGINE_INCLUDE_PATH
                            BASE_DIRECTORY "${SOURCE_ROOT}" NORMALIZE
                            OUTPUT_VARIABLE ENGINE_INCLUDE_PATH)
                    else()
                        cmake_path(NORMAL_PATH ENGINE_INCLUDE_PATH
                            OUTPUT_VARIABLE ENGINE_INCLUDE_PATH)
                    endif()
                    list(APPEND ENGINE_SOURCE_INCLUDE_DIRECTORIES
                        "${ENGINE_INCLUDE_PATH}")
                endforeach()
            endif()
        endif()
        uml_audit_engine_owned_file(
            "${ENGINE_SOURCE_PATH}" "${ENGINE_TARGET_KIND}"
            "${ENGINE_TARGET_NAME}" ${ENGINE_SOURCE_INCLUDE_DIRECTORIES})
    endforeach()
endforeach()

# Audit configured include directories first. A foreign engine directory is
# already an architecture edge even if today's source does not happen to name
# one of its headers. This also catches transitive interface include paths.
foreach(FROM_ENGINE_KIND IN ITEMS native gadget shared mixed)
    foreach(INCLUDE_DIRECTORY IN LISTS
            ENGINE_INCLUDE_DIRECTORIES_${FROM_ENGINE_KIND})
        uml_engine_path_kind("${INCLUDE_DIRECTORY}" INCLUDE_ENGINE_KIND)
        if(NOT INCLUDE_ENGINE_KIND)
            continue()
        endif()
        if(FROM_ENGINE_KIND STREQUAL "mixed" OR
           FROM_ENGINE_KIND STREQUAL INCLUDE_ENGINE_KIND)
            continue()
        endif()
        if(FROM_ENGINE_KIND MATCHES "^(native|gadget)$" AND
           INCLUDE_ENGINE_KIND STREQUAL FROM_ENGINE_KIND)
            continue()
        endif()
        if(NOT FROM_ENGINE_KIND STREQUAL "mixed")
            message(FATAL_ERROR
                "Forbidden cross-engine include-directory edge: "
                "${FROM_ENGINE_KIND} -> ${INCLUDE_ENGINE_KIND} "
                "(${INCLUDE_DIRECTORY})")
        endif()
    endforeach()
endforeach()

# Resolve textual includes against both their source directory and the exact
# configured compile-group paths. This catches relative paths, basename-only
# includes, and paths propagated by an interface dependency without relying on
# directory-name spelling in the directive itself.
foreach(FROM_DIRECTORY FROM_ENGINE_KIND IN ZIP_LISTS
        UML_ENGINE_DIRECTORY_NAMES UML_ENGINE_KINDS)
    file(GLOB_RECURSE ENGINE_SOURCES LIST_DIRECTORIES false
        "${SOURCE_ROOT}/darwin/cpu/arm64/${FROM_DIRECTORY}/*.c"
        "${SOURCE_ROOT}/darwin/cpu/arm64/${FROM_DIRECTORY}/*.cc"
        "${SOURCE_ROOT}/darwin/cpu/arm64/${FROM_DIRECTORY}/*.cpp"
        "${SOURCE_ROOT}/darwin/cpu/arm64/${FROM_DIRECTORY}/*.h"
        "${SOURCE_ROOT}/darwin/cpu/arm64/${FROM_DIRECTORY}/*.hpp"
        "${SOURCE_ROOT}/darwin/cpu/arm64/${FROM_DIRECTORY}/*.inc")
    foreach(ENGINE_SOURCE IN LISTS ENGINE_SOURCES)
        file(STRINGS "${ENGINE_SOURCE}" INCLUDE_LINES
            REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"][^>\"]+[>\"]")
        foreach(INCLUDE_LINE IN LISTS INCLUDE_LINES)
            if(NOT INCLUDE_LINE MATCHES
               "[<\"]([^>\"]+)[>\"]")
                continue()
            endif()
            set(INCLUDE_NAME "${CMAKE_MATCH_1}")
            get_filename_component(ENGINE_SOURCE_DIRECTORY
                "${ENGINE_SOURCE}" DIRECTORY)
            set(INCLUDE_SEARCH_DIRECTORIES
                "${ENGINE_SOURCE_DIRECTORY}"
                ${ENGINE_INCLUDE_DIRECTORIES_${FROM_ENGINE_KIND}})
            set(INCLUDE_RESOLVED FALSE)
            foreach(INCLUDE_SEARCH_DIRECTORY IN LISTS
                    INCLUDE_SEARCH_DIRECTORIES)
                set(INCLUDE_CANDIDATE
                    "${INCLUDE_SEARCH_DIRECTORY}/${INCLUDE_NAME}")
                cmake_path(NORMAL_PATH INCLUDE_CANDIDATE
                    OUTPUT_VARIABLE INCLUDE_CANDIDATE)
                if(NOT EXISTS "${INCLUDE_CANDIDATE}")
                    continue()
                endif()
                set(INCLUDE_RESOLVED TRUE)
                uml_engine_path_kind("${INCLUDE_CANDIDATE}"
                    INCLUDE_ENGINE_KIND)
                if(INCLUDE_ENGINE_KIND AND
                   NOT INCLUDE_ENGINE_KIND STREQUAL FROM_ENGINE_KIND)
                    message(FATAL_ERROR
                        "Forbidden cross-engine include edge: ${ENGINE_SOURCE}: "
                        "${INCLUDE_LINE} resolves to ${INCLUDE_CANDIDATE}")
                endif()
                break()
            endforeach()
            if(INCLUDE_RESOLVED)
                continue()
            endif()

            # Keep a lexical backstop for generated or platform-conditional
            # headers absent from the host filesystem used by this audit.
            foreach(TO_DIRECTORY TO_ENGINE_KIND IN ZIP_LISTS
                    UML_ENGINE_DIRECTORY_NAMES UML_ENGINE_KINDS)
                if(NOT FROM_ENGINE_KIND STREQUAL TO_ENGINE_KIND AND
                   INCLUDE_NAME MATCHES
                   "(^|/)${TO_DIRECTORY}/")
                    message(FATAL_ERROR
                        "Forbidden unresolved cross-engine include edge: "
                        "${ENGINE_SOURCE}: ${INCLUDE_LINE}")
                endif()
            endforeach()
        endforeach()
    endforeach()
endforeach()

list(LENGTH ARCHITECTURE_TARGETS ARCHITECTURE_TARGET_TOTAL)
message(STATUS
    "CMake File API architecture audit passed: ${TARGET_COUNT} targets, "
    "${ARCHITECTURE_TARGET_TOTAL} layered targets, no forbidden edges or "
    "cycles")
