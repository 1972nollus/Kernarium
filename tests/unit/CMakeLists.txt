# SPDX-License-Identifier: GPL-2.0-or-later

# CPU conformance and engine/runtime tests have their own directory scope.
# This keeps the suite entry point small while preserving one explicit source
# manifest per test domain.
add_subdirectory(arm64)
add_subdirectory(runtime)
