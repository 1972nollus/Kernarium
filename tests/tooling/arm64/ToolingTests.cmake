# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME arm64-instruction-inventory
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/validate_a64_instruction_inventory.py
    LABELS cpu
    TIMEOUT 120)

uml_add_tooling_test(
    NAME arm64-instruction-corpus-freshness
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_arm64_instruction_corpus.py
    LABELS cpu
    TIMEOUT 300)

uml_add_tooling_test(
    NAME arm64-instruction-corpus-absent-features
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_arm64_instruction_corpus_absent_features.py
    LABELS cpu
    TIMEOUT 300)
set_tests_properties(arm64-instruction-corpus-absent-features PROPERTIES
    SKIP_RETURN_CODE 77)

# Opt-in: the pinned Arm XML archive is not a managed dependency.
set(UML_A64_ARCHITECTURE_ARCHIVE "" CACHE FILEPATH
    "Pinned Arm A64 XML archive for arm64-instruction-oracle-freshness")
uml_add_tooling_test(
    NAME arm64-instruction-oracle-freshness
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/tooling/arm64/test_a64_instruction_oracle_freshness.py
        --architecture-archive=${UML_A64_ARCHITECTURE_ARCHIVE}
    LABELS cpu
    TIMEOUT 900)
set_tests_properties(arm64-instruction-oracle-freshness PROPERTIES
    SKIP_RETURN_CODE 77)
