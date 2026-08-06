// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"

#include <cstdlib>

int main()
{
    uml::gpu::testing::runVhostUserServerTests();
    uml::gpu::testing::runVhostUserProtocolTests();
    uml::gpu::testing::runInProcessServiceTests();
    std::puts("GPU lifecycle and ownership tests passed");
    return 0;
}
