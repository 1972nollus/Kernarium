// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gadget_symbols.hpp"
#include "program.hpp"
#include "software_helpers.hpp"

#include "cpu/arm64/layout/assembly_layout.hpp"

#include "../detail/bfloat_conversion.hpp"
#include "../detail/bfloat_dot.hpp"
#include "../detail/bfloat_matrix.hpp"
#include "../detail/bfloat_multiply.hpp"
#include "../detail/floating_arithmetic.hpp"
#include "../detail/floating_compare.hpp"
#include "../detail/floating_complex.hpp"
#include "../detail/floating_conversion.hpp"
#include "../detail/floating_fused.hpp"
#include "../detail/floating_horizontal.hpp"
#include "../detail/floating_integer_conversion.hpp"
#include "../detail/floating_reciprocal.hpp"
#include "../detail/floating_round.hpp"
#include "../detail/fp_state_access.hpp"
#include "../detail/half_fused_long.hpp"
#include "../detail/instruction_primitives.hpp"
#include "../detail/integer_multiply.hpp"
#include "cpu/host_capabilities.hpp"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace uml::cpu::arm64::gadgets::selection {

using Gadget = void (*)();

[[nodiscard]] bool host_has_sha1() noexcept;
[[nodiscard]] bool host_has_sha256() noexcept;
[[nodiscard]] bool host_has_sha512() noexcept;
[[nodiscard]] bool host_has_sha3() noexcept;
[[nodiscard]] bool host_has_aes() noexcept;
[[nodiscard]] bool host_has_pmull() noexcept;
[[nodiscard]] bool host_has_rdm() noexcept;
[[nodiscard]] bool host_has_dotprod() noexcept;
[[nodiscard]] bool host_has_i8mm() noexcept;
[[nodiscard]] bool host_has_frintts() noexcept;
[[nodiscard]] bool host_has_bf16() noexcept;
[[nodiscard]] bool host_has_fhm() noexcept;

} // namespace uml::cpu::arm64::gadgets::selection
