// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * SIMD&FP ordered memory shares the same specification oracle and
 * interpreter -> gadgets -> JIT harness as the scalar and pair families.
 * Keep this compatibility name for the split SIMD suites.
 */
#include "arm64_ordered_memory_test_support.hpp"

namespace arm64_simd_ordered_test = arm64_ordered_memory_test;
