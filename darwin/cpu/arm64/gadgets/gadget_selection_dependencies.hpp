// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gadget_symbols.hpp"
#include "program.hpp"
#include "software_helpers.hpp"

#include "cpu/arm64/layout/assembly_layout.hpp"

#include "../detail/atomic_floating.hpp"
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
#include "../detail/floating_indexed.hpp"
#include "../detail/floating_integer_conversion.hpp"
#include "../detail/floating_reciprocal.hpp"
#include "../detail/floating_reciprocal_software.hpp"
#include "../detail/floating_round.hpp"
#include "../detail/floating_scale.hpp"
#include "../detail/fp8.hpp"
#include "../detail/fp_state_access.hpp"
#include "../detail/gcs.hpp"
#include "../detail/half_dot.hpp"
#include "../detail/half_fused_long.hpp"
#include "../detail/half_matrix.hpp"
#include "../detail/instruction_primitives.hpp"
#include "../detail/integer_multiply.hpp"
#include "../detail/pointer_auth.hpp"
#include "../detail/read_check_write.hpp"
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

[[nodiscard]] bool host_has_sm3() noexcept;
[[nodiscard]] bool host_has_sm4() noexcept;
[[nodiscard]] bool host_has_sha1() noexcept;
[[nodiscard]] bool host_has_sha256() noexcept;
[[nodiscard]] bool host_has_sha512() noexcept;
[[nodiscard]] bool host_has_sha3() noexcept;
[[nodiscard]] bool host_has_aes() noexcept;
[[nodiscard]] bool host_has_pmull() noexcept;
[[nodiscard]] bool host_has_rdm() noexcept;
[[nodiscard]] bool host_has_dotprod() noexcept;
[[nodiscard]] bool host_has_i8mm() noexcept;
[[nodiscard]] bool host_has_lrcpc3() noexcept;
[[nodiscard]] bool host_has_lut() noexcept;
[[nodiscard]] bool host_has_frintts() noexcept;
[[nodiscard]] bool host_has_afp() noexcept;
[[nodiscard]] bool host_has_faminmax() noexcept;
[[nodiscard]] bool host_has_fp8() noexcept;
[[nodiscard]] bool host_has_fp8dot2() noexcept;
[[nodiscard]] bool host_has_fp8dot4() noexcept;
[[nodiscard]] bool host_has_fp8fma() noexcept;
[[nodiscard]] bool host_has_f8f16mm() noexcept;
[[nodiscard]] bool host_has_f8f32mm() noexcept;
[[nodiscard]] bool host_has_bf16() noexcept;
[[nodiscard]] bool host_has_ebf16() noexcept;
[[nodiscard]] bool host_has_fhm() noexcept;
[[nodiscard]] bool host_has_f16mm() noexcept;
[[nodiscard]] bool host_has_f16f32mm() noexcept;
[[nodiscard]] bool host_has_f16f32dot() noexcept;
[[nodiscard]] bool host_has_lsfe() noexcept;

} // namespace uml::cpu::arm64::gadgets::selection
