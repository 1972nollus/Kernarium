# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME arm64-instruction-inventory
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/validate_a64_instruction_inventory.py
    LABELS cpu
    TIMEOUT 120)

execute_process(
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/validate_a64_aslref_corpus.py
    RESULT_VARIABLE _uml_aslref_corpus_validation_result
    OUTPUT_VARIABLE _uml_aslref_corpus_validation_output
    ERROR_VARIABLE _uml_aslref_corpus_validation_error)
if(NOT _uml_aslref_corpus_validation_result EQUAL 0)
    message(FATAL_ERROR
        "Checked-in A64 ASLRef corpus is invalid:\n"
        "${_uml_aslref_corpus_validation_output}"
        "${_uml_aslref_corpus_validation_error}")
endif()

uml_add_tooling_test(
    NAME arm64-aslref-corpus-validation
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/validate_a64_aslref_corpus.py
    LABELS cpu;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME arm64-instruction-evidence-negative-fixtures
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_instruction_evidence_validator.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-instruction-aggregate-negative-fixtures
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_instruction_aggregate_manifest.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-raw-evidence-compression-fixtures
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_raw_evidence_compression.py
        $<TARGET_FILE:uml_instruction_evidence_test_runner>
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-input-materialization-negative-fixtures
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_input_materialization.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-module-composition
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_module_composition.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-execution-oracle
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_execution_oracle.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-materialization-batch
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_materialization_batch.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-campaign-publication
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_campaign.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-campaign-pilot
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_campaign_pilot.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-campaign-authority
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_campaign_authority.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-campaign-sharding
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_campaign_sharding.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-campaign-result-join
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_campaign_result_join.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-user-state-interface
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_user_state_interface.py
    LABELS cpu;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-aslref-system-state
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_aslref_system_state.py
    LABELS cpu;audit
    TIMEOUT 120)

if(UML_BUILD_UNIT_TESTS)
    get_property(_uml_unit_evidence_dependencies GLOBAL PROPERTY UML_UNIT_TESTS)
    uml_add_tooling_test(
        NAME arm64-instruction-evidence
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/validate_a64_instruction_evidence.py
            --evidence-directory ${CMAKE_BINARY_DIR}/instruction-evidence
            --build-root ${CMAKE_BINARY_DIR}
        LABELS cpu;audit;instruction-evidence-validation
        TIMEOUT 120)
    # DEPENDS orders producers that are already selected. Selecting only this
    # validator preserves and checks an existing evidence directory instead
    # of selecting either the reset or any producer implicitly.
    set_tests_properties(arm64-instruction-evidence PROPERTIES
        DEPENDS "${_uml_unit_evidence_dependencies}")

    # Raw inputs remain beside their producer evidence so missing producers are
    # observable. The much larger resumable join state may be placed on a
    # separately provisioned volume without weakening complete-coverage checks.
    set(_uml_input_materialization_storage_args
        --free-space-reserve-gib 5)
    if(UML_INSTRUCTION_MATERIALIZATION_WORK_ROOT)
        set(_uml_input_materialization_profile_root
            "${UML_INSTRUCTION_MATERIALIZATION_WORK_ROOT}/${UML_GUEST_PAGE_SIZE}")
        list(APPEND _uml_input_materialization_storage_args
            --contract-index-output
                "${_uml_input_materialization_profile_root}/a64-input-materialization-contract-index.tsv.gz"
            --checkpoint-directory
                "${_uml_input_materialization_profile_root}/a64-input-materialization-checkpoints")
    endif()

    # Registered only alongside its producers. The sidecars they emit are the
    # dominant build-tree cost, so both sides share one option and the join can
    # never be selected without the inputs it requires.
    if(UML_INSTRUCTION_INPUT_MATERIALIZATION)
        uml_add_tooling_test(
            NAME arm64-input-materialization-join
            COMMAND ${Python3_EXECUTABLE} -B
                ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/join_a64_input_materializations.py
                --evidence-directory ${CMAKE_BINARY_DIR}/instruction-evidence
                --guest-page-size ${UML_GUEST_PAGE_SIZE}
                --output
                    ${CMAKE_BINARY_DIR}/instruction-evidence/a64-input-materialization-index.tsv
                ${_uml_input_materialization_storage_args}
            LABELS cpu;audit;instruction-evidence-validation
            # A complete profile streams roughly 30 million contributions
            # through 449 resumable producer checkpoints. Keep the exhaustive
            # tooling gate bounded, but do not impose the old ten-minute
            # fixture-scale limit.
            TIMEOUT 14400)
        set_tests_properties(arm64-input-materialization-join PROPERTIES
            DEPENDS "arm64-instruction-evidence;${_uml_unit_evidence_dependencies}")
    endif()
endif()
