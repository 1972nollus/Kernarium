// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"

#include <cstdio>

int main()
{
    uml::gpu::testing::runVirtioGpuDeviceTests();
    std::puts("Virtio-GPU command atomicity tests passed");
    return 0;
}
