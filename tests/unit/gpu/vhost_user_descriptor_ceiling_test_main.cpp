// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"

#include <cstdio>

int main()
{
    uml::gpu::testing::runVhostUserDescriptorCeilingTests();
    std::puts("GPU transport descriptor ceiling tests passed");
    return 0;
}
