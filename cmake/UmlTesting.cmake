# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

function(uml_record_registered_test NAME SUITE)
    string(REPLACE ";" "|" _encoded_command "${ARGN}")
    get_property(_registered GLOBAL PROPERTY UML_REGISTERED_TESTS)
    list(APPEND _registered "${NAME}\t${SUITE}\t${_encoded_command}")
    set_property(GLOBAL PROPERTY UML_REGISTERED_TESTS "${_registered}")

    string(TOUPPER "${SUITE}" _suite_upper)
    set(_target_property "UML_${_suite_upper}_COMMAND_TARGETS")
    get_property(_command_targets GLOBAL PROPERTY "${_target_property}")
    foreach(_argument IN LISTS ARGN)
        set(_command_target "")
        if(_argument MATCHES "^\\$<TARGET_FILE:([^>]+)>$")
            set(_command_target "${CMAKE_MATCH_1}")
        elseif(_argument MATCHES
               "^\\$<JOIN:\\$<TARGET_OBJECTS:([^>]+)>,\\|>$")
            set(_command_target "${CMAKE_MATCH_1}")
        elseif(_argument MATCHES "^\\$<TARGET_OBJECTS:([^>]+)>$")
            set(_command_target "${CMAKE_MATCH_1}")
        elseif(TARGET "${_argument}")
            set(_command_target "${_argument}")
        endif()
        if(NOT _command_target OR NOT TARGET "${_command_target}")
            continue()
        endif()
        get_property(_aliased_target TARGET "${_command_target}"
            PROPERTY ALIASED_TARGET)
        if(_aliased_target)
            set(_command_target "${_aliased_target}")
        endif()
        get_target_property(_command_target_imported "${_command_target}"
            IMPORTED)
        get_target_property(_command_target_type "${_command_target}" TYPE)
        if(NOT _command_target_imported AND
           NOT _command_target_type STREQUAL "INTERFACE_LIBRARY")
            list(APPEND _command_targets "${_command_target}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _command_targets)
    set_property(GLOBAL PROPERTY "${_target_property}" "${_command_targets}")
endfunction()

function(uml_finalize_suite_binary_target SUITE AGGREGATE_TARGET)
    if(NOT TARGET "${AGGREGATE_TARGET}")
        add_custom_target("${AGGREGATE_TARGET}")
    endif()
    string(TOUPPER "${SUITE}" _suite_upper)
    get_property(_command_targets GLOBAL PROPERTY
        "UML_${_suite_upper}_COMMAND_TARGETS")
    list(REMOVE_DUPLICATES _command_targets)
    foreach(_command_target IN LISTS _command_targets)
        if(NOT TARGET "${_command_target}")
            message(FATAL_ERROR
                "${SUITE} test command target disappeared: ${_command_target}")
        endif()
        add_dependencies("${AGGREGATE_TARGET}" "${_command_target}")
    endforeach()

    get_property(_aggregate_dependencies TARGET "${AGGREGATE_TARGET}"
        PROPERTY MANUALLY_ADDED_DEPENDENCIES)
    foreach(_command_target IN LISTS _command_targets)
        list(FIND _aggregate_dependencies "${_command_target}"
            _dependency_index)
        if(_dependency_index EQUAL -1)
            message(FATAL_ERROR
                "${AGGREGATE_TARGET} does not build registered ${SUITE} "
                "command target ${_command_target}")
        endif()
    endforeach()
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

    # Unit tests are native host binaries (or CMake-only setup actions).  Keep
    # Python and shell runners in the tooling/integration suites even when a
    # caller attempts to hide one behind an absolute interpreter path.
    foreach(_command_argument IN LISTS TEST_COMMAND)
        if(_command_argument MATCHES "(^|/)(python[0-9.]*|sh|bash|zsh)$" OR
           _command_argument MATCHES "\\.(py|sh)$")
            message(FATAL_ERROR
                "${TEST_NAME}: unit tests may not invoke Python or shell: "
                "${_command_argument}")
        endif()
    endforeach()

    add_test(NAME "${TEST_NAME}" COMMAND ${TEST_COMMAND})
    set(_labels "unit")
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
    # Scratch files stay in a per-test build-tree directory; see
    # tests/unit/scratch/scratch_directory.hpp. UML_UNIT_TEST_NAME gives each
    # test its registered name; harness producers name corpus exports with it.
    set_property(TEST "${TEST_NAME}" APPEND PROPERTY ENVIRONMENT
        "UML_UNIT_TEST_NAME=${TEST_NAME}"
        "UML_TEST_SCRATCH_DIRECTORY=${CMAKE_BINARY_DIR}/test-scratch/${TEST_NAME}")
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
        list(APPEND _labels rootfs)
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

function(_uml_collect_directory_tests DIRECTORY_PATH OUTPUT)
    get_property(_tests DIRECTORY "${DIRECTORY_PATH}" PROPERTY TESTS)
    get_property(_subdirectories DIRECTORY "${DIRECTORY_PATH}"
        PROPERTY SUBDIRECTORIES)
    foreach(_subdirectory IN LISTS _subdirectories)
        _uml_collect_directory_tests("${_subdirectory}" _subdirectory_tests)
        list(APPEND _tests ${_subdirectory_tests})
    endforeach()
    set(${OUTPUT} "${_tests}" PARENT_SCOPE)
endfunction()

function(uml_validate_registered_test_manifest)
    set(_manifest "${PROJECT_SOURCE_DIR}/tests/test-manifest.tsv")
    if(NOT EXISTS "${_manifest}")
        message(FATAL_ERROR "Test manifest is missing: ${_manifest}")
    endif()
    file(STRINGS "${_manifest}" _rows)
    list(LENGTH _rows _row_count)
    if(_row_count LESS 2)
        message(FATAL_ERROR "Test manifest has no data rows: ${_manifest}")
    endif()
    list(GET _rows 0 _header)
    if(NOT _header STREQUAL "name\towner\tsuite\tavailability")
        message(FATAL_ERROR "Test manifest has an unsupported header: ${_header}")
    endif()
    set(_manifest_names)
    set(_manifest_owners)
    set(_manifest_suites)
    set(_manifest_availabilities)
    foreach(_row IN LISTS _rows)
        if(_row MATCHES "^#" OR _row STREQUAL "" OR
           _row STREQUAL "${_header}")
            continue()
        endif()
        string(REPLACE "\t" ";" _fields "${_row}")
        list(LENGTH _fields _field_count)
        if(NOT _field_count EQUAL 4)
            message(FATAL_ERROR "Malformed test manifest row: ${_row}")
        endif()
        list(GET _fields 0 _current_name)
        list(GET _fields 1 _owner)
        list(GET _fields 2 _suite)
        list(GET _fields 3 _availability)
        if(_current_name STREQUAL "" OR _current_name STREQUAL "-")
            message(FATAL_ERROR "Test manifest has no test name: ${_row}")
        endif()
        if(NOT _suite MATCHES "^(unit|tooling|integration)$")
            message(FATAL_ERROR "Invalid test suite in manifest: ${_row}")
        endif()
        if(_owner STREQUAL "" OR _owner STREQUAL "-" OR
           IS_ABSOLUTE "${_owner}" OR _owner MATCHES "(^|/)\\.\\.(/|$)" OR
           NOT EXISTS "${PROJECT_SOURCE_DIR}/${_owner}")
            message(FATAL_ERROR
                "Invalid or missing test owner in manifest: ${_row}")
        endif()
        set(_expected_owner_root "tests/${_suite}")
        if(NOT _owner STREQUAL "${_expected_owner_root}" AND
           NOT _owner MATCHES "^${_expected_owner_root}/")
            message(FATAL_ERROR
                "Test owner is outside ${_expected_owner_root} for "
                "${_suite} suite: ${_row}")
        endif()
        if(NOT _availability MATCHES "^(retained|conditional)$")
            message(FATAL_ERROR "Invalid test availability in manifest: ${_row}")
        endif()
        list(FIND _manifest_names "${_current_name}" _duplicate)
        if(NOT _duplicate EQUAL -1)
            message(FATAL_ERROR "Duplicate current test name in manifest: ${_current_name}")
        endif()
        list(APPEND _manifest_names "${_current_name}")
        list(APPEND _manifest_owners "${_owner}")
        list(APPEND _manifest_suites "${_suite}")
        list(APPEND _manifest_availabilities "${_availability}")
    endforeach()

    get_property(_registered GLOBAL PROPERTY UML_REGISTERED_TESTS)
    set(_registered_names)
    foreach(_registration IN LISTS _registered)
        string(REPLACE "\t" ";" _registration_fields "${_registration}")
        list(GET _registration_fields 0 _registered_name)
        list(GET _registration_fields 1 _registered_suite)
        list(GET _registration_fields 2 _registered_command)
        list(APPEND _registered_names "${_registered_name}")
        list(FIND _manifest_names "${_registered_name}" _manifest_index)
        if(_manifest_index EQUAL -1)
            message(FATAL_ERROR
                "CTest registration is missing from tests/test-manifest.tsv: ${_registered_name}")
        endif()
        list(GET _manifest_suites ${_manifest_index} _manifest_suite)
        list(GET _manifest_owners ${_manifest_index} _manifest_owner)
        list(GET _manifest_availabilities ${_manifest_index} _manifest_availability)
        if(NOT _manifest_suite STREQUAL _registered_suite)
            message(FATAL_ERROR
                "CTest suite differs from tests/test-manifest.tsv: ${_registered_name} "
                "(${_registered_suite} versus ${_manifest_suite})")
        endif()
        if(NOT _manifest_availability STREQUAL "retained" AND
           NOT _manifest_availability STREQUAL "conditional")
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

    # Compare the helpers' registry with CMake's actual generated test graph.
    # This closes indirect spellings such as cmake_language(CALL ADD_TEST ...)
    # which no source-text policy can enumerate reliably.
    _uml_collect_directory_tests("${PROJECT_SOURCE_DIR}" _actual_test_names)
    list(REMOVE_DUPLICATES _registered_names)
    list(REMOVE_DUPLICATES _actual_test_names)
    list(SORT _registered_names)
    list(SORT _actual_test_names)
    if(NOT _registered_names STREQUAL _actual_test_names)
        set(_unregistered ${_actual_test_names})
        set(_missing ${_registered_names})
        foreach(_name IN LISTS _registered_names)
            list(REMOVE_ITEM _unregistered "${_name}")
        endforeach()
        foreach(_name IN LISTS _actual_test_names)
            list(REMOVE_ITEM _missing "${_name}")
        endforeach()
        message(FATAL_ERROR
            "CTest registrations differ from the centralized suite registry; "
            "unregistered=[${_unregistered}], missing=[${_missing}]")
    endif()
endfunction()
