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
