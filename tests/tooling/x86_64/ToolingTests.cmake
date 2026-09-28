# SPDX-License-Identifier: GPL-2.0-or-later

# The FEX campaign parses the gadget runner's result line, so its contract is
# checked against the runner itself. The runner exists only in unit trees that
# build the x86-64 guest.
if(TARGET uml_x86_64_fex_asm_runner)
    uml_add_tooling_test(
        NAME x86-64-fex-asm-campaign-runner
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/tooling/x86_64/test_fex_asm_campaign.py
            --runner $<TARGET_FILE:uml_x86_64_fex_asm_runner>
            --campaign ${PROJECT_SOURCE_DIR}/tools/fex-tests/run-asm-corpus.py
        LABELS cpu;x86-64
        TIMEOUT 120)
endif()
