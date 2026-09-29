# SPDX-License-Identifier: GPL-2.0-or-later

# Test support only: an external balloon backend that raises a config change
# every few milliseconds.
add_executable(uml-balloon-churn-server balloon_churn_server.cpp)
target_include_directories(uml-balloon-churn-server PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/device)
target_link_libraries(uml-balloon-churn-server PRIVATE
    UML::BalloonDevice
    UML::VhostUserTransport)
add_dependencies(integration-test-binaries uml-balloon-churn-server)

# One-vCPU guest: balloon config changes and config reads must not deadlock.
uml_add_guest_integration_test(
    NAME guest-balloon-config-churn
    SCRIPT run_balloon_config_churn.py
    ARGS --server $<TARGET_FILE:uml-balloon-churn-server>
    LABELS guest;balloon;smoke
    TIMEOUT 300
    CPUS 1)
