# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

function(uml_record_registered_test NAME SUITE)
    string(REPLACE ";" "|" _encoded_command "${ARGN}")
    get_property(_registered GLOBAL PROPERTY UML_REGISTERED_TESTS)
    list(APPEND _registered "${NAME}\t${SUITE}\t${_encoded_command}")
    set_property(GLOBAL PROPERTY UML_REGISTERED_TESTS "${_registered}")
    if(SUITE STREQUAL "unit")
        get_property(_unit_tests GLOBAL PROPERTY UML_UNIT_TESTS)
        list(APPEND _unit_tests "${NAME}")
        set_property(GLOBAL PROPERTY UML_UNIT_TESTS "${_unit_tests}")
    endif()
endfunction()

function(uml_apply_test_properties NAME)
    cmake_parse_arguments(TEST "" "WORKING_DIRECTORY;TIMEOUT;PROCESSORS;RESOURCE_LOCK"
        "LABELS;RESOURCE_GROUPS;FIXTURES_SETUP;FIXTURES_CLEANUP;FIXTURES_REQUIRED" ${ARGN})

    if(NOT TEST_LABELS)
        message(FATAL_ERROR "${NAME}: a suite label is required")
    endif()
    set_tests_properties("${NAME}" PROPERTIES LABELS "${TEST_LABELS}")
    if(TEST_WORKING_DIRECTORY)
        set_tests_properties("${NAME}" PROPERTIES
            WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}")
    endif()
    if(TEST_TIMEOUT)
        set_tests_properties("${NAME}" PROPERTIES TIMEOUT "${TEST_TIMEOUT}")
    endif()
    if(TEST_PROCESSORS)
        set_tests_properties("${NAME}" PROPERTIES PROCESSORS "${TEST_PROCESSORS}")
    endif()
    if(TEST_RESOURCE_LOCK)
        set_tests_properties("${NAME}" PROPERTIES RESOURCE_LOCK "${TEST_RESOURCE_LOCK}")
    endif()
    if(TEST_RESOURCE_GROUPS)
        set_tests_properties("${NAME}" PROPERTIES RESOURCE_GROUPS "${TEST_RESOURCE_GROUPS}")
    endif()
    if(TEST_FIXTURES_SETUP)
        set_tests_properties("${NAME}" PROPERTIES FIXTURES_SETUP "${TEST_FIXTURES_SETUP}")
    endif()
    if(TEST_FIXTURES_CLEANUP)
        set_tests_properties("${NAME}" PROPERTIES FIXTURES_CLEANUP "${TEST_FIXTURES_CLEANUP}")
    endif()
    if(TEST_FIXTURES_REQUIRED)
        set_tests_properties("${NAME}" PROPERTIES FIXTURES_REQUIRED "${TEST_FIXTURES_REQUIRED}")
    endif()
endfunction()

function(uml_add_unit_test)
    cmake_parse_arguments(TEST "" "NAME;WORKING_DIRECTORY;TIMEOUT;PROCESSORS;RESOURCE_LOCK"
        "COMMAND;LABELS;RESOURCE_GROUPS;FIXTURES_SETUP;FIXTURES_CLEANUP;FIXTURES_REQUIRED" ${ARGN})
    if(NOT TEST_NAME OR NOT TEST_COMMAND)
        message(FATAL_ERROR
            "uml_add_unit_test requires NAME and COMMAND")
    endif()

    add_test(NAME "${TEST_NAME}" COMMAND ${TEST_COMMAND})
    set(_labels "unit")
    if(TEST_LABELS)
        list(APPEND _labels ${TEST_LABELS})
    endif()
    set(_fixtures_required ${TEST_FIXTURES_REQUIRED})
    if(NOT TEST_FIXTURES_SETUP)
        list(APPEND _fixtures_required uml-instruction-evidence-run)
    endif()
    uml_apply_test_properties("${TEST_NAME}"
        LABELS "${_labels}"
        WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}"
        TIMEOUT "${TEST_TIMEOUT}"
        PROCESSORS "${TEST_PROCESSORS}"
        RESOURCE_LOCK "${TEST_RESOURCE_LOCK}"
        RESOURCE_GROUPS "${TEST_RESOURCE_GROUPS}"
        FIXTURES_SETUP "${TEST_FIXTURES_SETUP}"
        FIXTURES_CLEANUP "${TEST_FIXTURES_CLEANUP}"
        FIXTURES_REQUIRED "${_fixtures_required}")
    set_property(TEST "${TEST_NAME}" APPEND PROPERTY ENVIRONMENT
        "UML_INSTRUCTION_EVIDENCE=${CMAKE_BINARY_DIR}/instruction-evidence/${TEST_NAME}.tsv"
        "UML_INSTRUCTION_EVIDENCE_TEST=${TEST_NAME}")
    uml_record_registered_test("${TEST_NAME}" unit ${TEST_COMMAND})
endfunction()

function(uml_add_tooling_test)
    cmake_parse_arguments(TEST "" "NAME;WORKING_DIRECTORY;TIMEOUT;PROCESSORS;RESOURCE_LOCK"
        "COMMAND;LABELS;RESOURCE_GROUPS;FIXTURES_SETUP;FIXTURES_CLEANUP;FIXTURES_REQUIRED" ${ARGN})
    if(NOT TEST_NAME OR NOT TEST_COMMAND)
        message(FATAL_ERROR
            "uml_add_tooling_test requires NAME and COMMAND")
    endif()

    add_test(NAME "${TEST_NAME}" COMMAND ${TEST_COMMAND})
    set(_labels "tooling")
    if(TEST_LABELS)
        list(APPEND _labels ${TEST_LABELS})
    endif()
    uml_apply_test_properties("${TEST_NAME}"
        LABELS "${_labels}"
        WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}"
        TIMEOUT "${TEST_TIMEOUT}"
        PROCESSORS "${TEST_PROCESSORS}"
        RESOURCE_LOCK "${TEST_RESOURCE_LOCK}"
        RESOURCE_GROUPS "${TEST_RESOURCE_GROUPS}"
        FIXTURES_SETUP "${TEST_FIXTURES_SETUP}"
        FIXTURES_CLEANUP "${TEST_FIXTURES_CLEANUP}"
        FIXTURES_REQUIRED "${TEST_FIXTURES_REQUIRED}")
    set_property(TEST "${TEST_NAME}" APPEND PROPERTY
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    uml_record_registered_test("${TEST_NAME}" tooling ${TEST_COMMAND})
endfunction()

function(uml_add_integration_test)
    cmake_parse_arguments(TEST "ROOTFS" "NAME;WORKING_DIRECTORY;TIMEOUT;PROCESSORS;RESOURCE_LOCK"
        "COMMAND;LABELS;RESOURCE_GROUPS;FIXTURES_SETUP;FIXTURES_CLEANUP;FIXTURES_REQUIRED" ${ARGN})
    if(NOT TEST_NAME OR NOT TEST_COMMAND)
        message(FATAL_ERROR
            "uml_add_integration_test requires NAME and COMMAND")
    endif()

    set(_command ${TEST_COMMAND})
    set(_labels "integration")
    if(TEST_ROOTFS)
        if(NOT UML_INTEGRATION_ROOTFS)
            message(FATAL_ERROR
                "${TEST_NAME}: ROOTFS integration tests require UML_INTEGRATION_ROOTFS")
        endif()
        set(_command
            "${PROJECT_SOURCE_DIR}/tools/rootfs/with-verified-rootfs.sh"
            --rootfs "${UML_INTEGRATION_ROOTFS}" -- ${TEST_COMMAND})
        list(APPEND _labels rootfs)
        if(NOT TEST_RESOURCE_LOCK)
            set(TEST_RESOURCE_LOCK uml-rootfs)
        endif()
    endif()
    add_test(NAME "${TEST_NAME}" COMMAND ${_command})
    if(TEST_LABELS)
        list(APPEND _labels ${TEST_LABELS})
    endif()
    uml_apply_test_properties("${TEST_NAME}"
        LABELS "${_labels}"
        WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}"
        TIMEOUT "${TEST_TIMEOUT}"
        PROCESSORS "${TEST_PROCESSORS}"
        RESOURCE_LOCK "${TEST_RESOURCE_LOCK}"
        RESOURCE_GROUPS "${TEST_RESOURCE_GROUPS}"
        FIXTURES_SETUP "${TEST_FIXTURES_SETUP}"
        FIXTURES_CLEANUP "${TEST_FIXTURES_CLEANUP}"
        FIXTURES_REQUIRED "${TEST_FIXTURES_REQUIRED}")
    # Integration runners import helpers from neighboring source directories.
    # Keep validation from recreating source-adjacent __pycache__ debris.
    set_property(TEST "${TEST_NAME}" APPEND PROPERTY
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    uml_record_registered_test("${TEST_NAME}" integration ${_command})
endfunction()

function(uml_add_guest_integration_test)
    uml_add_integration_test(ROOTFS ${ARGN})
endfunction()

function(uml_test_owner_matches NAME OWNER COMMAND_TEXT OUTPUT)
    set(_owner_path "${PROJECT_SOURCE_DIR}/${OWNER}")
    cmake_path(NORMAL_PATH _owner_path)
    set(_candidates)
    string(REPLACE "|" ";" _command "${COMMAND_TEXT}")
    foreach(_argument IN LISTS _command)
        set(_command_target "")
        if(_argument MATCHES "^\\$<TARGET_FILE:([^>]+)>$")
            set(_command_target "${CMAKE_MATCH_1}")
        elseif(TARGET "${_argument}")
            set(_command_target "${_argument}")
        elseif(IS_ABSOLUTE "${_argument}")
            list(APPEND _candidates "${_argument}")
        endif()
        if(_command_target AND TARGET "${_command_target}")
            get_property(_aliased_target TARGET "${_command_target}"
                PROPERTY ALIASED_TARGET)
            if(_aliased_target)
                set(_command_target "${_aliased_target}")
            endif()
            get_property(_target_source_directory TARGET "${_command_target}"
                PROPERTY SOURCE_DIR)
            list(APPEND _candidates
                "${_target_source_directory}"
                "${_target_source_directory}/CMakeLists.txt")
            get_property(_target_sources TARGET "${_command_target}"
                PROPERTY SOURCES)
            foreach(_target_source IN LISTS _target_sources)
                if(_target_source MATCHES "^\\$<")
                    continue()
                endif()
                if(IS_ABSOLUTE "${_target_source}")
                    list(APPEND _candidates "${_target_source}")
                else()
                    list(APPEND _candidates
                        "${_target_source_directory}/${_target_source}")
                endif()
            endforeach()
        endif()
    endforeach()

    set(_matches FALSE)
    foreach(_candidate IN LISTS _candidates)
        cmake_path(NORMAL_PATH _candidate)
        if(IS_DIRECTORY "${_owner_path}")
            cmake_path(IS_PREFIX _owner_path "${_candidate}"
                NORMALIZE _candidate_is_owned)
            if(_candidate_is_owned)
                set(_matches TRUE)
                break()
            endif()
        elseif(_candidate STREQUAL _owner_path)
            set(_matches TRUE)
            break()
        endif()
    endforeach()
    set(${OUTPUT} ${_matches} PARENT_SCOPE)
endfunction()

function(uml_validate_registered_test_manifest)
    set(_manifest "${PROJECT_SOURCE_DIR}/tests/test-manifest.tsv")
    if(NOT EXISTS "${_manifest}")
        message(FATAL_ERROR "Test manifest is missing: ${_manifest}")
    endif()
    file(STRINGS "${_manifest}" _rows)
    set(_manifest_names)
    set(_manifest_owners)
    set(_manifest_suites)
    set(_manifest_dispositions)
    foreach(_row IN LISTS _rows)
        if(_row MATCHES "^#" OR _row STREQUAL "" OR
           _row MATCHES "^legacy_name[\t]")
            continue()
        endif()
        string(REPLACE "\t" ";" _fields "${_row}")
        list(LENGTH _fields _field_count)
        if(NOT _field_count EQUAL 6)
            message(FATAL_ERROR "Malformed test manifest row: ${_row}")
        endif()
        list(GET _fields 1 _current_name)
        list(GET _fields 2 _owner)
        list(GET _fields 3 _suite)
        list(GET _fields 4 _disposition)
        if(NOT _suite MATCHES "^(unit|tooling|integration)$")
            message(FATAL_ERROR "Invalid test suite in manifest: ${_row}")
        endif()
        if(_owner STREQUAL "" OR _owner STREQUAL "-" OR
           IS_ABSOLUTE "${_owner}" OR _owner MATCHES "(^|/)\\.\\.(/|$)" OR
           NOT EXISTS "${PROJECT_SOURCE_DIR}/${_owner}")
            message(FATAL_ERROR
                "Invalid or missing test owner in manifest: ${_row}")
        endif()
        if(NOT _disposition MATCHES "^(retained|conditional|renamed|removed)$")
            message(FATAL_ERROR "Invalid test disposition in manifest: ${_row}")
        endif()
        list(FIND _manifest_names "${_current_name}" _duplicate)
        if(NOT _duplicate EQUAL -1)
            message(FATAL_ERROR "Duplicate current test name in manifest: ${_current_name}")
        endif()
        list(APPEND _manifest_names "${_current_name}")
        list(APPEND _manifest_owners "${_owner}")
        list(APPEND _manifest_suites "${_suite}")
        list(APPEND _manifest_dispositions "${_disposition}")
    endforeach()

    set(_baseline "${PROJECT_SOURCE_DIR}/tests/pre-refactor-test-inventory.txt")
    set(_ledger "${PROJECT_SOURCE_DIR}/tests/test-migration-ledger.tsv")
    if(NOT EXISTS "${_baseline}" OR NOT EXISTS "${_ledger}")
        message(FATAL_ERROR
            "Test baseline inventory or migration ledger is missing")
    endif()
    file(STRINGS "${_baseline}" _baseline_rows)
    set(_baseline_names)
    foreach(_row IN LISTS _baseline_rows)
        if(NOT _row MATCHES "^#" AND NOT _row STREQUAL "")
            list(APPEND _baseline_names "${_row}")
        endif()
    endforeach()
    list(LENGTH _baseline_names _baseline_count)
    if(NOT _baseline_count EQUAL 560)
        message(FATAL_ERROR
            "Pre-refactor test inventory has ${_baseline_count} names, expected 560")
    endif()
    file(STRINGS "${_ledger}" _migration_rows)
    set(_migration_baselines)
    set(_migration_currents)
    foreach(_row IN LISTS _migration_rows)
        if(_row MATCHES "^baseline_name[\t]" OR _row MATCHES "^#" OR
           _row STREQUAL "")
            continue()
        endif()
        string(REPLACE "\t" ";" _fields "${_row}")
        list(LENGTH _fields _field_count)
        if(NOT _field_count EQUAL 4)
            message(FATAL_ERROR "Malformed test migration row: ${_row}")
        endif()
        list(GET _fields 0 _baseline_name)
        list(GET _fields 1 _current_name)
        list(APPEND _migration_baselines "${_baseline_name}")
        list(APPEND _migration_currents "${_current_name}")
    endforeach()
    foreach(_baseline_name IN LISTS _baseline_names)
        list(FIND _manifest_names "${_baseline_name}" _current_index)
        if(NOT _current_index EQUAL -1)
            continue()
        endif()
        list(FIND _migration_baselines "${_baseline_name}" _migration_index)
        if(_migration_index EQUAL -1)
            message(FATAL_ERROR
                "Pre-refactor test vanished without a reviewed migration: ${_baseline_name}")
        endif()
        list(GET _migration_currents ${_migration_index} _current_name)
        if(NOT _current_name STREQUAL "-")
            list(FIND _manifest_names "${_current_name}" _renamed_index)
            if(_renamed_index EQUAL -1)
                message(FATAL_ERROR
                    "Renamed test is absent from current manifest: ${_current_name}")
            endif()
        endif()
    endforeach()

    get_property(_registered GLOBAL PROPERTY UML_REGISTERED_TESTS)
    foreach(_registration IN LISTS _registered)
        string(REPLACE "\t" ";" _registration_fields "${_registration}")
        list(GET _registration_fields 0 _registered_name)
        list(GET _registration_fields 1 _registered_suite)
        list(GET _registration_fields 2 _registered_command)
        list(FIND _manifest_names "${_registered_name}" _manifest_index)
        if(_manifest_index EQUAL -1)
            message(FATAL_ERROR
                "CTest registration is missing from tests/test-manifest.tsv: ${_registered_name}")
        endif()
        list(GET _manifest_suites ${_manifest_index} _manifest_suite)
        list(GET _manifest_owners ${_manifest_index} _manifest_owner)
        list(GET _manifest_dispositions ${_manifest_index} _manifest_disposition)
        if(NOT _manifest_suite STREQUAL _registered_suite)
            message(FATAL_ERROR
                "CTest suite differs from tests/test-manifest.tsv: ${_registered_name} "
                "(${_registered_suite} versus ${_manifest_suite})")
        endif()
        if(NOT _manifest_disposition STREQUAL "retained" AND
           NOT _manifest_disposition STREQUAL "conditional")
            message(FATAL_ERROR
                "Registered test is not retained in tests/test-manifest.tsv: ${_registered_name}")
        endif()
        uml_test_owner_matches("${_registered_name}" "${_manifest_owner}"
            "${_registered_command}" _owner_matches_command)
        if(NOT _owner_matches_command)
            message(FATAL_ERROR
                "CTest owner does not match its registered command or source: "
                "${_registered_name} -> ${_manifest_owner}")
        endif()
    endforeach()
endfunction()
