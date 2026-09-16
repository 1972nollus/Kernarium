// SPDX-License-Identifier: GPL-2.0-or-later
#include "gadget_symbols.hpp"
#include "program.hpp"
#include "software_helpers.hpp"

#include "cpu/arm64/layout/assembly_layout.hpp"

#include "../detail/atomic_floating.hpp"
#include "../detail/floating_compare.hpp"
#include "../detail/bfloat_conversion.hpp"
#include "../detail/bfloat_dot.hpp"
#include "../detail/bfloat_matrix.hpp"
#include "../detail/bfloat_multiply.hpp"
#include "../detail/half_fused_long.hpp"
#include "../detail/half_dot.hpp"
#include "../detail/half_matrix.hpp"
#include "../detail/floating_complex.hpp"
#include "../detail/floating_arithmetic.hpp"
#include "../detail/floating_conversion.hpp"
#include "../detail/floating_fused.hpp"
#include "../detail/floating_horizontal.hpp"
#include "../detail/floating_indexed.hpp"
#include "../detail/floating_integer_conversion.hpp"
#include "../detail/floating_reciprocal.hpp"
#include "../detail/floating_reciprocal_software.hpp"
#include "../detail/floating_round.hpp"
#include "../detail/floating_scale.hpp"
#include "../detail/fp_state_access.hpp"
#include "../detail/fp8.hpp"
#include "../detail/gcs.hpp"
#include "../detail/instruction_helpers.hpp"
#include "../detail/integer_multiply.hpp"
#include "../detail/pointer_auth.hpp"
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

[[nodiscard]] Gadget literal_load_gadget(
	bool vector, unsigned operation, bool discard) noexcept
{
	if (vector) {
		static constexpr std::array gadgets{
			uml_arm64_gadget_literal_load_vector32_fast,
			uml_arm64_gadget_literal_load_vector64_fast,
			uml_arm64_gadget_literal_load_vector128_fast};
		return operation < gadgets.size() ? gadgets[operation] : nullptr;
	}
	using Modes = std::array<Gadget, 2>;
	static constexpr std::array gadgets{
		Modes{uml_arm64_gadget_literal_load32_fast_r,
			uml_arm64_gadget_literal_load32_fast_z},
		Modes{uml_arm64_gadget_literal_load64_fast_r,
			uml_arm64_gadget_literal_load64_fast_z},
		Modes{uml_arm64_gadget_literal_load32_signed_fast_r,
			uml_arm64_gadget_literal_load32_signed_fast_z}};
	return operation < gadgets.size() ? gadgets[operation][discard] : nullptr;
}

[[nodiscard]] Gadget scalar_unsigned_memory_gadget(unsigned size,
	unsigned operation, bool base_sp, bool transfer_zero) noexcept
{
	using Registers = std::array<Gadget, 2>;
	using Bases = std::array<Registers, 2>;
#define UML_SCALAR_UNSIGNED_BASES(name) Bases{ \
	Registers{uml_arm64_gadget_##name##_r, \
		uml_arm64_gadget_##name##_z}, \
	Registers{uml_arm64_gadget_##name##_r_sp, \
		uml_arm64_gadget_##name##_z_sp}}
	static constexpr std::array stores{
		UML_SCALAR_UNSIGNED_BASES(store8_unsigned_fast),
		UML_SCALAR_UNSIGNED_BASES(store16_unsigned_fast),
		UML_SCALAR_UNSIGNED_BASES(store32_unsigned_fast),
		UML_SCALAR_UNSIGNED_BASES(store64_unsigned_fast)};
	static constexpr std::array loads{
		UML_SCALAR_UNSIGNED_BASES(load8_unsigned_fast),
		UML_SCALAR_UNSIGNED_BASES(load16_unsigned_fast),
		UML_SCALAR_UNSIGNED_BASES(load32_unsigned_fast),
		UML_SCALAR_UNSIGNED_BASES(load64_unsigned_fast)};
	static constexpr std::array signed_x{
		UML_SCALAR_UNSIGNED_BASES(load8_signed_x_fast),
		UML_SCALAR_UNSIGNED_BASES(load16_signed_x_fast),
		UML_SCALAR_UNSIGNED_BASES(load32_signed_x_fast),
		Bases{}};
	static constexpr std::array signed_w{
		UML_SCALAR_UNSIGNED_BASES(load8_signed_w_fast),
		UML_SCALAR_UNSIGNED_BASES(load16_signed_w_fast),
		Bases{}, Bases{}};
#undef UML_SCALAR_UNSIGNED_BASES
	const auto &gadgets = operation == 0U ? stores : operation == 1U ? loads :
		operation == 2U ? signed_x : signed_w;
	return size < 4U ? gadgets[size][base_sp][transfer_zero] : nullptr;
}

[[nodiscard]] Gadget scalar_unsigned_memory_inplace_gadget(
	[[maybe_unused]] unsigned size,
	[[maybe_unused]] unsigned operation,
	[[maybe_unused]] bool base_sp,
	[[maybe_unused]] unsigned transferred) noexcept
{
#if UML_GADGET_INPLACE_MEMORY
	/* Only the scaled 32- and 64-bit forms are baked, and XZR never reaches
	 * CpuState, so everything else keeps the established entry. */
	if ((size != 2U && size != 3U) || operation > 1U || transferred >= 31U)
		return nullptr;
	const unsigned bits64 = size == 3U ? 1U : 0U;
	const unsigned index = ((operation * 2U + bits64) * 2U +
		(base_sp ? 1U : 0U)) * 31U + transferred;
	return reinterpret_cast<Gadget>(
		uml_arm64_gadget_scalar_unsigned_inplace_table[index]);
#else
	return nullptr;
#endif
}

[[nodiscard]] Gadget vector_unsigned_memory_gadget(
	unsigned size, unsigned operation, bool base_sp) noexcept
{
	using Bases = std::array<Gadget, 2>;
#define UML_VECTOR_UNSIGNED_BASES(name) Bases{ \
	uml_arm64_gadget_vector_##name##_unsigned_fast, \
	uml_arm64_gadget_vector_##name##_unsigned_fast_sp}
	static constexpr std::array loads{
		UML_VECTOR_UNSIGNED_BASES(load8),
		UML_VECTOR_UNSIGNED_BASES(load16),
		UML_VECTOR_UNSIGNED_BASES(load32),
		UML_VECTOR_UNSIGNED_BASES(load64),
		UML_VECTOR_UNSIGNED_BASES(load128)};
	static constexpr std::array stores{
		UML_VECTOR_UNSIGNED_BASES(store8),
		UML_VECTOR_UNSIGNED_BASES(store16),
		UML_VECTOR_UNSIGNED_BASES(store32),
		UML_VECTOR_UNSIGNED_BASES(store64),
		UML_VECTOR_UNSIGNED_BASES(store128)};
#undef UML_VECTOR_UNSIGNED_BASES
	const unsigned width = operation >= 2U ? 4U : size;
	return width < loads.size() ?
		((operation & 1U) != 0U ? loads : stores)[width][base_sp] : nullptr;
}

using ScalarOtherRegisterChoices = std::array<Gadget, 2>;
using ScalarOtherBaseChoices = std::array<ScalarOtherRegisterChoices, 2>;
using ScalarOtherImmediateModes = std::array<ScalarOtherBaseChoices, 4>;
using ScalarOtherRegisterModes = std::array<ScalarOtherBaseChoices, 6>;

struct ScalarOtherGadgets final {
	ScalarOtherImmediateModes immediate;
	ScalarOtherRegisterModes register_offset;
};

#define UML_SCALAR_OTHER_REGISTER_CHOICES(name, mode) \
	ScalarOtherRegisterChoices{ \
	uml_arm64_gadget_scalar_other_##name##_##mode##_r, \
	uml_arm64_gadget_scalar_other_##name##_##mode##_z}
#define UML_SCALAR_OTHER_BASE_CHOICES(name, mode) ScalarOtherBaseChoices{ \
	UML_SCALAR_OTHER_REGISTER_CHOICES(name, mode), \
	ScalarOtherRegisterChoices{ \
		uml_arm64_gadget_scalar_other_##name##_##mode##_r_sp, \
		uml_arm64_gadget_scalar_other_##name##_##mode##_z_sp}}
#define UML_SCALAR_OTHER_REGISTERS(name) ScalarOtherRegisterModes{ \
	UML_SCALAR_OTHER_BASE_CHOICES(name, uxtw_0), \
	UML_SCALAR_OTHER_BASE_CHOICES(name, x_0), \
	UML_SCALAR_OTHER_BASE_CHOICES(name, sxtw_0), \
	UML_SCALAR_OTHER_BASE_CHOICES(name, uxtw_1), \
	UML_SCALAR_OTHER_BASE_CHOICES(name, x_1), \
	UML_SCALAR_OTHER_BASE_CHOICES(name, sxtw_1)}
#define UML_SCALAR_OTHER_STORE_GADGETS(name) ScalarOtherGadgets{ \
	ScalarOtherImmediateModes{ \
		UML_SCALAR_OTHER_BASE_CHOICES(name, offset), \
		UML_SCALAR_OTHER_BASE_CHOICES(name, pre), \
		UML_SCALAR_OTHER_BASE_CHOICES(name, post), \
		ScalarOtherBaseChoices{}}, \
	UML_SCALAR_OTHER_REGISTERS(name)}
#define UML_SCALAR_OTHER_LOAD_GADGETS(name) ScalarOtherGadgets{ \
	ScalarOtherImmediateModes{ \
		UML_SCALAR_OTHER_BASE_CHOICES(name, offset), \
		UML_SCALAR_OTHER_BASE_CHOICES(name, pre), \
		UML_SCALAR_OTHER_BASE_CHOICES(name, post), \
		ScalarOtherBaseChoices{ \
			ScalarOtherRegisterChoices{ \
				uml_arm64_gadget_scalar_other_##name##_post_suppress_r, \
				nullptr}, \
			ScalarOtherRegisterChoices{}}}, \
	UML_SCALAR_OTHER_REGISTERS(name)}

[[nodiscard]] const ScalarOtherGadgets *scalar_other_gadgets(
	unsigned size, unsigned operation) noexcept
{
	static constexpr auto store8 = UML_SCALAR_OTHER_STORE_GADGETS(store8);
	static constexpr auto load8u = UML_SCALAR_OTHER_LOAD_GADGETS(load8u);
	static constexpr auto load8sx = UML_SCALAR_OTHER_LOAD_GADGETS(load8sx);
	static constexpr auto load8sw = UML_SCALAR_OTHER_LOAD_GADGETS(load8sw);
	static constexpr auto store16 = UML_SCALAR_OTHER_STORE_GADGETS(store16);
	static constexpr auto load16u = UML_SCALAR_OTHER_LOAD_GADGETS(load16u);
	static constexpr auto load16sx = UML_SCALAR_OTHER_LOAD_GADGETS(load16sx);
	static constexpr auto load16sw = UML_SCALAR_OTHER_LOAD_GADGETS(load16sw);
	static constexpr auto store32 = UML_SCALAR_OTHER_STORE_GADGETS(store32);
	static constexpr auto load32u = UML_SCALAR_OTHER_LOAD_GADGETS(load32u);
	static constexpr auto load32sx = UML_SCALAR_OTHER_LOAD_GADGETS(load32sx);
	static constexpr auto store64 = UML_SCALAR_OTHER_STORE_GADGETS(store64);
	static constexpr auto load64u = UML_SCALAR_OTHER_LOAD_GADGETS(load64u);
	switch (size * 4U + operation) {
	case 0: return &store8;
	case 1: return &load8u;
	case 2: return &load8sx;
	case 3: return &load8sw;
	case 4: return &store16;
	case 5: return &load16u;
	case 6: return &load16sx;
	case 7: return &load16sw;
	case 8: return &store32;
	case 9: return &load32u;
	case 10: return &load32sx;
	case 12: return &store64;
	case 13: return &load64u;
	default: return nullptr;
	}
}

#undef UML_SCALAR_OTHER_LOAD_GADGETS
#undef UML_SCALAR_OTHER_STORE_GADGETS
#undef UML_SCALAR_OTHER_REGISTERS
#undef UML_SCALAR_OTHER_BASE_CHOICES
#undef UML_SCALAR_OTHER_REGISTER_CHOICES

using VectorOtherBaseModes = std::array<Gadget, 2>;
using VectorOtherImmediateModes = std::array<VectorOtherBaseModes, 3>;
using VectorOtherRegisterModes = std::array<VectorOtherBaseModes, 6>;

struct VectorOtherGadgets final {
	VectorOtherImmediateModes immediate;
	VectorOtherRegisterModes register_offset;
};

#define UML_VECTOR_OTHER_GADGETS(name) VectorOtherGadgets{ \
	VectorOtherImmediateModes{ \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_offset, \
			uml_arm64_gadget_vector_other_##name##_offset_sp}, \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_pre, \
			uml_arm64_gadget_vector_other_##name##_pre_sp}, \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_post, \
			uml_arm64_gadget_vector_other_##name##_post_sp}}, \
	VectorOtherRegisterModes{ \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_uxtw_0, \
			uml_arm64_gadget_vector_other_##name##_uxtw_0_sp}, \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_x_0, \
			uml_arm64_gadget_vector_other_##name##_x_0_sp}, \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_sxtw_0, \
			uml_arm64_gadget_vector_other_##name##_sxtw_0_sp}, \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_uxtw_1, \
			uml_arm64_gadget_vector_other_##name##_uxtw_1_sp}, \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_x_1, \
			uml_arm64_gadget_vector_other_##name##_x_1_sp}, \
		VectorOtherBaseModes{ \
			uml_arm64_gadget_vector_other_##name##_sxtw_1, \
			uml_arm64_gadget_vector_other_##name##_sxtw_1_sp}}}

[[nodiscard]] const VectorOtherGadgets *vector_other_gadgets(
	unsigned size, unsigned operation) noexcept
{
	static constexpr auto store8 = UML_VECTOR_OTHER_GADGETS(store8);
	static constexpr auto load8 = UML_VECTOR_OTHER_GADGETS(load8);
	static constexpr auto store16 = UML_VECTOR_OTHER_GADGETS(store16);
	static constexpr auto load16 = UML_VECTOR_OTHER_GADGETS(load16);
	static constexpr auto store32 = UML_VECTOR_OTHER_GADGETS(store32);
	static constexpr auto load32 = UML_VECTOR_OTHER_GADGETS(load32);
	static constexpr auto store64 = UML_VECTOR_OTHER_GADGETS(store64);
	static constexpr auto load64 = UML_VECTOR_OTHER_GADGETS(load64);
	static constexpr auto store128 = UML_VECTOR_OTHER_GADGETS(store128);
	static constexpr auto load128 = UML_VECTOR_OTHER_GADGETS(load128);
	static constexpr std::array stores{
		&store8, &store16, &store32, &store64, &store128};
	static constexpr std::array loads{
		&load8, &load16, &load32, &load64, &load128};
	const unsigned width = operation >= 2U ? 4U : size;
	return width < loads.size() ?
		((operation & 1U) != 0U ? loads : stores)[width] : nullptr;
}

#undef UML_VECTOR_OTHER_GADGETS

using IntegerPairRegisterModes = std::array<Gadget, 4>;
using IntegerPairBaseModes = std::array<IntegerPairRegisterModes, 2>;
using IntegerPairAddressModes = std::array<IntegerPairBaseModes, 4>;

#define UML_INTEGER_PAIR_REGISTER_MODES(name, mode) \
	IntegerPairRegisterModes{ \
		uml_arm64_gadget_integer_pair_##name##_##mode##_rr, \
		uml_arm64_gadget_integer_pair_##name##_##mode##_rz, \
		uml_arm64_gadget_integer_pair_##name##_##mode##_zr, \
		uml_arm64_gadget_integer_pair_##name##_##mode##_zz}
#define UML_INTEGER_PAIR_SP_REGISTER_MODES(name, mode) \
	IntegerPairRegisterModes{ \
		uml_arm64_gadget_integer_pair_##name##_##mode##_rr_sp, \
		uml_arm64_gadget_integer_pair_##name##_##mode##_rz_sp, \
		uml_arm64_gadget_integer_pair_##name##_##mode##_zr_sp, \
		uml_arm64_gadget_integer_pair_##name##_##mode##_zz_sp}
#define UML_INTEGER_PAIR_BASE_MODES(name, mode) IntegerPairBaseModes{ \
	UML_INTEGER_PAIR_REGISTER_MODES(name, mode), \
	UML_INTEGER_PAIR_SP_REGISTER_MODES(name, mode)}
/* Address-mode index 3 is load-only. Reuse index 0 in store tables instead
 * of emitting a symbol the translator can never select. */
#define UML_INTEGER_PAIR_STORE_GADGETS(name) IntegerPairAddressModes{ \
	UML_INTEGER_PAIR_BASE_MODES(name, 0), \
	UML_INTEGER_PAIR_BASE_MODES(name, 1), \
	UML_INTEGER_PAIR_BASE_MODES(name, 2), \
	UML_INTEGER_PAIR_BASE_MODES(name, 0)}
#define UML_INTEGER_PAIR_LOAD_GADGETS(name) IntegerPairAddressModes{ \
	UML_INTEGER_PAIR_BASE_MODES(name, 0), \
	UML_INTEGER_PAIR_BASE_MODES(name, 1), \
	UML_INTEGER_PAIR_BASE_MODES(name, 2), \
	UML_INTEGER_PAIR_BASE_MODES(name, 3)}

[[nodiscard]] const IntegerPairAddressModes *integer_pair_gadgets(
	unsigned operation, bool load) noexcept
{
	static constexpr auto store32 = UML_INTEGER_PAIR_STORE_GADGETS(store32);
	static constexpr auto load32 = UML_INTEGER_PAIR_LOAD_GADGETS(load32);
	static constexpr auto load32s = UML_INTEGER_PAIR_LOAD_GADGETS(load32s);
	static constexpr auto store64 = UML_INTEGER_PAIR_STORE_GADGETS(store64);
	static constexpr auto load64 = UML_INTEGER_PAIR_LOAD_GADGETS(load64);
	if (operation == 1U)
		return load ? &load32s : nullptr;
	if (operation >= 2U)
		return load ? &load64 : &store64;
	return load ? &load32 : &store32;
}

#undef UML_INTEGER_PAIR_LOAD_GADGETS
#undef UML_INTEGER_PAIR_STORE_GADGETS
#undef UML_INTEGER_PAIR_BASE_MODES
#undef UML_INTEGER_PAIR_SP_REGISTER_MODES
#undef UML_INTEGER_PAIR_REGISTER_MODES

using VectorPairBaseModes = std::array<Gadget, 2>;
using VectorPairAddressModes = std::array<VectorPairBaseModes, 3>;

#define UML_VECTOR_PAIR_GADGETS(name) VectorPairAddressModes{ \
	VectorPairBaseModes{uml_arm64_gadget_vector_pair_##name##_0, \
		uml_arm64_gadget_vector_pair_##name##_0_sp}, \
	VectorPairBaseModes{uml_arm64_gadget_vector_pair_##name##_1, \
		uml_arm64_gadget_vector_pair_##name##_1_sp}, \
	VectorPairBaseModes{uml_arm64_gadget_vector_pair_##name##_2, \
		uml_arm64_gadget_vector_pair_##name##_2_sp}}

[[nodiscard]] const VectorPairAddressModes &vector_pair_gadgets(
	unsigned operation, bool load) noexcept
{
	static constexpr auto store32 = UML_VECTOR_PAIR_GADGETS(store32);
	static constexpr auto load32 = UML_VECTOR_PAIR_GADGETS(load32);
	static constexpr auto store64 = UML_VECTOR_PAIR_GADGETS(store64);
	static constexpr auto load64 = UML_VECTOR_PAIR_GADGETS(load64);
	static constexpr auto store128 = UML_VECTOR_PAIR_GADGETS(store128);
	static constexpr auto load128 = UML_VECTOR_PAIR_GADGETS(load128);
	if (operation == 0U)
		return load ? load32 : store32;
	if (operation == 1U)
		return load ? load64 : store64;
	return load ? load128 : store128;
}

#undef UML_VECTOR_PAIR_GADGETS

using SimdStructureAddressModes = std::array<Gadget, 3>;
using SimdStructureBases = std::array<SimdStructureAddressModes, 2>;
struct SimdStructureGadgets final {
	SimdStructureBases store;
	SimdStructureBases load;
};

#define UML_SIMD_STRUCTURE_DIRECTION(base, direction) \
	SimdStructureBases{ \
		SimdStructureAddressModes{ \
			uml_arm64_gadget_simd_structure_##base##_##direction##_0, \
			uml_arm64_gadget_simd_structure_##base##_##direction##_1, \
			uml_arm64_gadget_simd_structure_##base##_##direction##_2}, \
		SimdStructureAddressModes{ \
			uml_arm64_gadget_simd_structure_##base##_##direction##_s0, \
			uml_arm64_gadget_simd_structure_##base##_##direction##_s1, \
			uml_arm64_gadget_simd_structure_##base##_##direction##_s2}}
#define UML_SIMD_STRUCTURE_GADGETS(base) SimdStructureGadgets{ \
	UML_SIMD_STRUCTURE_DIRECTION(base, store), \
	UML_SIMD_STRUCTURE_DIRECTION(base, load)}

[[nodiscard]] const SimdStructureGadgets *simd_structure_gadgets(
	unsigned opcode, bool q, unsigned size) noexcept
{
	static constexpr std::array contiguous_d{
		UML_SIMD_STRUCTURE_GADGETS(contig_d_1),
		UML_SIMD_STRUCTURE_GADGETS(contig_d_2),
		UML_SIMD_STRUCTURE_GADGETS(contig_d_3),
		UML_SIMD_STRUCTURE_GADGETS(contig_d_4)};
	static constexpr std::array contiguous_q{
		UML_SIMD_STRUCTURE_GADGETS(contig_q_1),
		UML_SIMD_STRUCTURE_GADGETS(contig_q_2),
		UML_SIMD_STRUCTURE_GADGETS(contig_q_3),
		UML_SIMD_STRUCTURE_GADGETS(contig_q_4)};
#define UML_SIMD_STRUCTURE_INTERLEAVED_ARRAYS(count) \
	static constexpr std::array interleaved_d_##count{ \
		UML_SIMD_STRUCTURE_GADGETS(interleaved_d_b_##count), \
		UML_SIMD_STRUCTURE_GADGETS(interleaved_d_h_##count), \
		UML_SIMD_STRUCTURE_GADGETS(interleaved_d_s_##count)}; \
	static constexpr std::array interleaved_q_##count{ \
		UML_SIMD_STRUCTURE_GADGETS(interleaved_q_b_##count), \
		UML_SIMD_STRUCTURE_GADGETS(interleaved_q_h_##count), \
		UML_SIMD_STRUCTURE_GADGETS(interleaved_q_s_##count), \
		UML_SIMD_STRUCTURE_GADGETS(interleaved_q_d_##count)}
	UML_SIMD_STRUCTURE_INTERLEAVED_ARRAYS(2);
	UML_SIMD_STRUCTURE_INTERLEAVED_ARRAYS(3);
	UML_SIMD_STRUCTURE_INTERLEAVED_ARRAYS(4);
#undef UML_SIMD_STRUCTURE_INTERLEAVED_ARRAYS
	if (opcode == 7U)
		return &(q ? contiguous_q : contiguous_d)[0];
	if (opcode == 10U)
		return &(q ? contiguous_q : contiguous_d)[1];
	if (opcode == 6U)
		return &(q ? contiguous_q : contiguous_d)[2];
	if (opcode == 2U)
		return &(q ? contiguous_q : contiguous_d)[3];
	if (opcode == 8U)
		return q ? &interleaved_q_2[size] : &interleaved_d_2[size];
	if (opcode == 4U)
		return q ? &interleaved_q_3[size] : &interleaved_d_3[size];
	if (opcode == 0U)
		return q ? &interleaved_q_4[size] : &interleaved_d_4[size];
	return nullptr;
}

#undef UML_SIMD_STRUCTURE_GADGETS
#undef UML_SIMD_STRUCTURE_DIRECTION

using SimdReplicateAddressModes = std::array<Gadget, 3>;
using SimdReplicateBases = std::array<SimdReplicateAddressModes, 2>;
using SimdReplicateCounts = std::array<SimdReplicateBases, 4>;

#define UML_SIMD_REPLICATE_MODES(type, count) SimdReplicateBases{ \
	SimdReplicateAddressModes{ \
		uml_arm64_gadget_simd_replicate_##type##_##count##_0, \
		uml_arm64_gadget_simd_replicate_##type##_##count##_1, \
		uml_arm64_gadget_simd_replicate_##type##_##count##_2}, \
	SimdReplicateAddressModes{ \
		uml_arm64_gadget_simd_replicate_##type##_##count##_s0, \
		uml_arm64_gadget_simd_replicate_##type##_##count##_s1, \
		uml_arm64_gadget_simd_replicate_##type##_##count##_s2}}
#define UML_SIMD_REPLICATE_GADGETS(type) SimdReplicateCounts{ \
	UML_SIMD_REPLICATE_MODES(type, 1), \
	UML_SIMD_REPLICATE_MODES(type, 2), \
	UML_SIMD_REPLICATE_MODES(type, 3), \
	UML_SIMD_REPLICATE_MODES(type, 4)}

[[nodiscard]] const SimdReplicateCounts &simd_replicate_gadgets(
	unsigned size, bool q) noexcept
{
	static constexpr auto b_d = UML_SIMD_REPLICATE_GADGETS(b_d);
	static constexpr auto b_q = UML_SIMD_REPLICATE_GADGETS(b_q);
	static constexpr auto h_d = UML_SIMD_REPLICATE_GADGETS(h_d);
	static constexpr auto h_q = UML_SIMD_REPLICATE_GADGETS(h_q);
	static constexpr auto s_d = UML_SIMD_REPLICATE_GADGETS(s_d);
	static constexpr auto s_q = UML_SIMD_REPLICATE_GADGETS(s_q);
	static constexpr auto d_d = UML_SIMD_REPLICATE_GADGETS(d_d);
	static constexpr auto d_q = UML_SIMD_REPLICATE_GADGETS(d_q);
	static constexpr std::array d{&b_d, &h_d, &s_d, &d_d};
	static constexpr std::array qv{&b_q, &h_q, &s_q, &d_q};
	return *(q ? qv : d)[size];
}

#undef UML_SIMD_REPLICATE_GADGETS
#undef UML_SIMD_REPLICATE_MODES

using SimdLaneAddressModes = std::array<Gadget, 3>;
using SimdLaneBases = std::array<SimdLaneAddressModes, 2>;
struct SimdLaneGadgets final {
	SimdLaneBases store;
	SimdLaneBases load;
};
using SimdLaneCounts = std::array<SimdLaneGadgets, 4>;

#define UML_SIMD_LANE_DIRECTION(type, count, direction) \
	SimdLaneBases{ \
		SimdLaneAddressModes{ \
			uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_0, \
			uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_1, \
			uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_2}, \
		SimdLaneAddressModes{ \
			uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_s0, \
			uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_s1, \
			uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_s2}}
#define UML_SIMD_LANE_COUNT(type, count) SimdLaneGadgets{ \
	UML_SIMD_LANE_DIRECTION(type, count, store), \
	UML_SIMD_LANE_DIRECTION(type, count, load)}
#define UML_SIMD_LANE_GADGETS(type) SimdLaneCounts{ \
	UML_SIMD_LANE_COUNT(type, 1), \
	UML_SIMD_LANE_COUNT(type, 2), \
	UML_SIMD_LANE_COUNT(type, 3), \
	UML_SIMD_LANE_COUNT(type, 4)}

[[nodiscard]] const SimdLaneCounts &simd_lane_gadgets(
	unsigned width_index) noexcept
{
	static constexpr auto b = UML_SIMD_LANE_GADGETS(b);
	static constexpr auto h = UML_SIMD_LANE_GADGETS(h);
	static constexpr auto s = UML_SIMD_LANE_GADGETS(s);
	static constexpr auto d = UML_SIMD_LANE_GADGETS(d);
	static constexpr std::array tables{&b, &h, &s, &d};
	return *tables[width_index];
}

#undef UML_SIMD_LANE_GADGETS
#undef UML_SIMD_LANE_COUNT
#undef UML_SIMD_LANE_DIRECTION

[[nodiscard]] bool host_has_sm3() noexcept
{
	return host_capabilities().has(ArmFeature::sm3);
}

[[nodiscard]] bool host_has_sm4() noexcept
{
	return host_capabilities().has(ArmFeature::sm4);
}

[[nodiscard]] bool host_has_sha1() noexcept
{
	return host_capabilities().has(ArmFeature::sha1);
}

[[nodiscard]] bool host_has_sha256() noexcept
{
	return host_capabilities().has(ArmFeature::sha256);
}

[[nodiscard]] bool host_has_sha512() noexcept
{
	return host_capabilities().has(ArmFeature::sha512);
}

[[nodiscard]] bool host_has_sha3() noexcept
{
	return host_capabilities().has(ArmFeature::sha3);
}

[[nodiscard]] bool host_has_aes() noexcept
{
	return host_capabilities().has(ArmFeature::advanced_simd) &&
		host_capabilities().has(ArmFeature::aes);
}

[[nodiscard]] bool host_has_pmull() noexcept
{
	return host_capabilities().has(ArmFeature::pmull);
}

[[nodiscard]] bool host_has_rdm() noexcept
{
	return host_capabilities().has(ArmFeature::rdm);
}

[[nodiscard]] bool host_has_dotprod() noexcept
{
	return host_capabilities().has(ArmFeature::dotprod);
}

[[nodiscard]] bool host_has_i8mm() noexcept
{
	return host_capabilities().has(ArmFeature::i8mm);
}

[[nodiscard]] bool host_has_lrcpc3() noexcept
{
	return host_capabilities().has(ArmFeature::lrcpc3);
}

[[nodiscard]] bool host_has_lut() noexcept
{
	return host_capabilities().has(ArmFeature::lut);
}

[[nodiscard]] bool host_has_frintts() noexcept
{
	return host_capabilities().has(ArmFeature::frintts);
}

[[nodiscard]] bool host_has_afp() noexcept
{
	return host_capabilities().afp;
}

[[nodiscard]] bool host_has_faminmax() noexcept
{
	return host_capabilities().has(ArmFeature::faminmax);
}

[[nodiscard]] bool host_has_fp8() noexcept
{
	return host_capabilities().has(ArmFeature::fp8);
}

[[nodiscard]] bool host_has_fp8dot2() noexcept
{
	return host_capabilities().has(ArmFeature::fp8dot2);
}

[[nodiscard]] bool host_has_fp8dot4() noexcept
{
	return host_capabilities().has(ArmFeature::fp8dot4);
}

[[nodiscard]] bool host_has_fp8fma() noexcept
{
	return host_capabilities().has(ArmFeature::fp8fma);
}

[[nodiscard]] bool host_has_f8f16mm() noexcept
{
	return host_capabilities().has(ArmFeature::f8f16mm);
}

[[nodiscard]] bool host_has_f8f32mm() noexcept
{
	return host_capabilities().has(ArmFeature::f8f32mm);
}

[[nodiscard]] bool host_has_bf16() noexcept
{
	return host_capabilities().has(ArmFeature::bf16);
}

[[nodiscard]] bool host_has_ebf16() noexcept
{
	return host_capabilities().has(ArmFeature::ebf16);
}

[[nodiscard]] bool host_has_fhm() noexcept
{
	return host_capabilities().has(ArmFeature::fhm);
}

[[nodiscard]] bool host_has_f16mm() noexcept
{
	return host_capabilities().has(ArmFeature::f16mm);
}

[[nodiscard]] bool host_has_f16f32mm() noexcept
{
	return host_capabilities().has(ArmFeature::f16f32mm);
}

[[nodiscard]] bool host_has_f16f32dot() noexcept
{
	return host_capabilities().has(ArmFeature::f16f32dot);
}

[[nodiscard]] bool host_has_lsfe() noexcept
{
	return host_capabilities().has(ArmFeature::lsfe);
}

using AtomicFloatingVariants = std::array<Gadget, 6>;
using AtomicFloatingBases = std::array<AtomicFloatingVariants, 2>;
using AtomicFloatingOperations = std::array<AtomicFloatingBases, 5>;
using AtomicFloatingGadgets = std::array<AtomicFloatingOperations, 4>;

#define UML_ATOMIC_FLOATING_VARIANTS(implementation, size, operation) \
	AtomicFloatingBases{ \
		AtomicFloatingVariants{ \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_relaxed, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_release, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire_release, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_relaxed, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_release}, \
		AtomicFloatingVariants{ \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_relaxed_sp, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_release_sp, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire_sp, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire_release_sp, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_relaxed_sp, \
			uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_release_sp}}
#define UML_ATOMIC_FLOATING_OPERATIONS(implementation, size) \
	AtomicFloatingOperations{ \
		UML_ATOMIC_FLOATING_VARIANTS(implementation, size, add), \
		UML_ATOMIC_FLOATING_VARIANTS(implementation, size, max), \
		UML_ATOMIC_FLOATING_VARIANTS(implementation, size, maxnm), \
		UML_ATOMIC_FLOATING_VARIANTS(implementation, size, min), \
		UML_ATOMIC_FLOATING_VARIANTS(implementation, size, minnm)}
#define UML_ATOMIC_FLOATING_GADGETS(implementation) AtomicFloatingGadgets{ \
	UML_ATOMIC_FLOATING_OPERATIONS(implementation, bf), \
	UML_ATOMIC_FLOATING_OPERATIONS(implementation, h), \
	UML_ATOMIC_FLOATING_OPERATIONS(implementation, s), \
	UML_ATOMIC_FLOATING_OPERATIONS(implementation, d)}

extern constexpr auto atomic_floating_native_gadgets =
	UML_ATOMIC_FLOATING_GADGETS(native);
extern constexpr auto atomic_floating_software_gadgets =
	UML_ATOMIC_FLOATING_GADGETS(software);

#undef UML_ATOMIC_FLOATING_GADGETS
#undef UML_ATOMIC_FLOATING_OPERATIONS
#undef UML_ATOMIC_FLOATING_VARIANTS

using AtomicRegisterGadgets = std::array<Gadget, 4>;
using AtomicSizeGadgets = std::array<AtomicRegisterGadgets, 4>;
using AtomicOrderGadgets = std::array<AtomicSizeGadgets, 4>;
using AtomicOperationGadgets = std::array<AtomicOrderGadgets, 9>;

#define UML_ATOMIC_REGISTER_GADGETS(operation, ordering, size) \
	AtomicRegisterGadgets{ \
		uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_rr, \
		uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_rz, \
		uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_zr, \
		uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_zz}
#define UML_ATOMIC_SIZE_GADGETS(operation, ordering) \
	AtomicSizeGadgets{ \
		UML_ATOMIC_REGISTER_GADGETS(operation, ordering, b), \
		UML_ATOMIC_REGISTER_GADGETS(operation, ordering, h), \
		UML_ATOMIC_REGISTER_GADGETS(operation, ordering, w), \
		UML_ATOMIC_REGISTER_GADGETS(operation, ordering, x)}
#define UML_ATOMIC_ORDER_GADGETS(operation) \
	AtomicOrderGadgets{ \
		UML_ATOMIC_SIZE_GADGETS(operation, relaxed), \
		UML_ATOMIC_SIZE_GADGETS(operation, release), \
		UML_ATOMIC_SIZE_GADGETS(operation, acquire), \
		UML_ATOMIC_SIZE_GADGETS(operation, acquire_release)}

extern constexpr AtomicOperationGadgets atomic_rmw_gadgets{
	UML_ATOMIC_ORDER_GADGETS(ldadd),
	UML_ATOMIC_ORDER_GADGETS(ldclr),
	UML_ATOMIC_ORDER_GADGETS(ldeor),
	UML_ATOMIC_ORDER_GADGETS(ldset),
	UML_ATOMIC_ORDER_GADGETS(ldsmax),
	UML_ATOMIC_ORDER_GADGETS(ldsmin),
	UML_ATOMIC_ORDER_GADGETS(ldumax),
	UML_ATOMIC_ORDER_GADGETS(ldumin),
	UML_ATOMIC_ORDER_GADGETS(swp),
};

#undef UML_ATOMIC_ORDER_GADGETS
#undef UML_ATOMIC_SIZE_GADGETS
#undef UML_ATOMIC_REGISTER_GADGETS

using AtomicPair128OrderGadgets = std::array<Gadget, 4>;
using AtomicPair128Gadgets = std::array<AtomicPair128OrderGadgets, 3>;
#define UML_ATOMIC_PAIR128_GADGETS(operation) \
	AtomicPair128OrderGadgets{ \
		uml_arm64_gadget_atomic_pair128_##operation##_relaxed, \
		uml_arm64_gadget_atomic_pair128_##operation##_release, \
		uml_arm64_gadget_atomic_pair128_##operation##_acquire, \
		uml_arm64_gadget_atomic_pair128_##operation##_acquire_release}
extern constexpr AtomicPair128Gadgets atomic_pair128_gadgets{
	UML_ATOMIC_PAIR128_GADGETS(clr),
	UML_ATOMIC_PAIR128_GADGETS(set),
	UML_ATOMIC_PAIR128_GADGETS(swp),
};
#undef UML_ATOMIC_PAIR128_GADGETS

using CompareSwapGadgets = std::array<AtomicSizeGadgets, 4>;
using CompareSwapPairSizeGadgets = std::array<AtomicRegisterGadgets, 2>;
using CompareSwapPairGadgets = std::array<CompareSwapPairSizeGadgets, 4>;

#define UML_COMPARE_SWAP_REGISTER_GADGETS(ordering, size) \
	AtomicRegisterGadgets{ \
		uml_arm64_gadget_compare_swap_##ordering##_##size##_rr, \
		uml_arm64_gadget_compare_swap_##ordering##_##size##_rz, \
		uml_arm64_gadget_compare_swap_##ordering##_##size##_zr, \
		uml_arm64_gadget_compare_swap_##ordering##_##size##_zz}
#define UML_COMPARE_SWAP_SIZE_GADGETS(ordering) \
	AtomicSizeGadgets{ \
		UML_COMPARE_SWAP_REGISTER_GADGETS(ordering, b), \
		UML_COMPARE_SWAP_REGISTER_GADGETS(ordering, h), \
		UML_COMPARE_SWAP_REGISTER_GADGETS(ordering, w), \
		UML_COMPARE_SWAP_REGISTER_GADGETS(ordering, x)}
extern constexpr CompareSwapGadgets compare_swap_gadgets{
	UML_COMPARE_SWAP_SIZE_GADGETS(relaxed),
	UML_COMPARE_SWAP_SIZE_GADGETS(release),
	UML_COMPARE_SWAP_SIZE_GADGETS(acquire),
	UML_COMPARE_SWAP_SIZE_GADGETS(acquire_release),
};
#undef UML_COMPARE_SWAP_SIZE_GADGETS
#undef UML_COMPARE_SWAP_REGISTER_GADGETS

using RcwScalarOrderingGadgets = std::array<AtomicRegisterGadgets, 4>;
using RcwScalarSoftGadgets = std::array<RcwScalarOrderingGadgets, 2>;
using RcwScalarGadgets = std::array<RcwScalarSoftGadgets, 4>;
#define UML_RCW_SCALAR_REGISTER_GADGETS(operation, soft, ordering) \
	AtomicRegisterGadgets{ \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_rr, \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_rz, \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_zr, \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_zz}
#define UML_RCW_SCALAR_ACQUIRE_GADGETS(operation, soft, ordering, suppressed) \
	AtomicRegisterGadgets{ \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_rr, \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##suppressed##_rz, \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_zr, \
		uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##suppressed##_zz}
#define UML_RCW_SCALAR_ORDERING_GADGETS(operation, soft) \
	RcwScalarOrderingGadgets{ \
		UML_RCW_SCALAR_REGISTER_GADGETS(operation, soft, relaxed), \
		UML_RCW_SCALAR_REGISTER_GADGETS(operation, soft, release), \
		UML_RCW_SCALAR_ACQUIRE_GADGETS( \
			operation, soft, acquire, relaxed), \
		UML_RCW_SCALAR_ACQUIRE_GADGETS( \
			operation, soft, acquire_release, release)}
#define UML_RCW_SCALAR_SOFT_GADGETS(operation) RcwScalarSoftGadgets{ \
	UML_RCW_SCALAR_ORDERING_GADGETS(operation, hard), \
	UML_RCW_SCALAR_ORDERING_GADGETS(operation, soft)}
extern constexpr RcwScalarGadgets rcw_scalar_gadgets{
	UML_RCW_SCALAR_SOFT_GADGETS(cas),
	UML_RCW_SCALAR_SOFT_GADGETS(bic),
	UML_RCW_SCALAR_SOFT_GADGETS(swp),
	UML_RCW_SCALAR_SOFT_GADGETS(orr),
};
#undef UML_RCW_SCALAR_SOFT_GADGETS
#undef UML_RCW_SCALAR_ORDERING_GADGETS
#undef UML_RCW_SCALAR_ACQUIRE_GADGETS
#undef UML_RCW_SCALAR_REGISTER_GADGETS

using RcwPairCasOrderingGadgets = std::array<AtomicRegisterGadgets, 4>;
using RcwPairCasGadgets = std::array<RcwPairCasOrderingGadgets, 2>;
#define UML_RCW_PAIR_CAS_VARIANTS(soft, ordering) AtomicRegisterGadgets{ \
	uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_rr, \
	uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_rz, \
	uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_zr, \
	uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_zz}
#define UML_RCW_PAIR_CAS_ORDERINGS(soft) RcwPairCasOrderingGadgets{ \
	UML_RCW_PAIR_CAS_VARIANTS(soft, relaxed), \
	UML_RCW_PAIR_CAS_VARIANTS(soft, release), \
	UML_RCW_PAIR_CAS_VARIANTS(soft, acquire), \
	UML_RCW_PAIR_CAS_VARIANTS(soft, acquire_release)}
extern constexpr RcwPairCasGadgets rcw_pair_cas_gadgets{
	UML_RCW_PAIR_CAS_ORDERINGS(hard),
	UML_RCW_PAIR_CAS_ORDERINGS(soft),
};
#undef UML_RCW_PAIR_CAS_ORDERINGS
#undef UML_RCW_PAIR_CAS_VARIANTS

using RcwPairModifyOrderingGadgets = std::array<Gadget, 4>;
using RcwPairModifySoftGadgets =
	std::array<RcwPairModifyOrderingGadgets, 2>;
using RcwPairModifyGadgets = std::array<RcwPairModifySoftGadgets, 3>;
#define UML_RCW_PAIR_MODIFY_ORDERINGS(operation, soft) \
	RcwPairModifyOrderingGadgets{ \
		uml_arm64_gadget_rcw_pair_##operation##_##soft##_relaxed, \
		uml_arm64_gadget_rcw_pair_##operation##_##soft##_release, \
		uml_arm64_gadget_rcw_pair_##operation##_##soft##_acquire, \
		uml_arm64_gadget_rcw_pair_##operation##_##soft##_acquire_release}
#define UML_RCW_PAIR_MODIFY_SOFTNESS(operation) RcwPairModifySoftGadgets{ \
	UML_RCW_PAIR_MODIFY_ORDERINGS(operation, hard), \
	UML_RCW_PAIR_MODIFY_ORDERINGS(operation, soft)}
extern constexpr RcwPairModifyGadgets rcw_pair_modify_gadgets{
	UML_RCW_PAIR_MODIFY_SOFTNESS(bic),
	UML_RCW_PAIR_MODIFY_SOFTNESS(swp),
	UML_RCW_PAIR_MODIFY_SOFTNESS(orr),
};
#undef UML_RCW_PAIR_MODIFY_SOFTNESS
#undef UML_RCW_PAIR_MODIFY_ORDERINGS

#define UML_COMPARE_SWAP_PAIR_REGISTER_GADGETS(ordering, size) \
	AtomicRegisterGadgets{ \
		uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_rr, \
		uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_rz, \
		uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_zr, \
		uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_zz}
#define UML_COMPARE_SWAP_PAIR_SIZE_GADGETS(ordering) \
	CompareSwapPairSizeGadgets{ \
		UML_COMPARE_SWAP_PAIR_REGISTER_GADGETS(ordering, w), \
		UML_COMPARE_SWAP_PAIR_REGISTER_GADGETS(ordering, x)}
extern constexpr CompareSwapPairGadgets compare_swap_pair_gadgets{
	UML_COMPARE_SWAP_PAIR_SIZE_GADGETS(relaxed),
	UML_COMPARE_SWAP_PAIR_SIZE_GADGETS(release),
	UML_COMPARE_SWAP_PAIR_SIZE_GADGETS(acquire),
	UML_COMPARE_SWAP_PAIR_SIZE_GADGETS(acquire_release),
};
#undef UML_COMPARE_SWAP_PAIR_SIZE_GADGETS
#undef UML_COMPARE_SWAP_PAIR_REGISTER_GADGETS

using ExclusiveLoadRegisterGadgets = std::array<Gadget, 2>;
using ExclusiveLoadSizeGadgets = std::array<ExclusiveLoadRegisterGadgets, 4>;
using ExclusiveLoadGadgets = std::array<ExclusiveLoadSizeGadgets, 2>;
using ExclusiveStoreGadgets = std::array<AtomicSizeGadgets, 2>;
using ExclusivePairLoadSizeGadgets =
	std::array<AtomicRegisterGadgets, 2>;
using ExclusivePairLoadGadgets =
	std::array<ExclusivePairLoadSizeGadgets, 2>;
using ExclusivePairStoreRegisterGadgets = std::array<Gadget, 8>;
using ExclusivePairStoreSizeGadgets =
	std::array<ExclusivePairStoreRegisterGadgets, 2>;
using ExclusivePairStoreGadgets =
	std::array<ExclusivePairStoreSizeGadgets, 2>;

#define UML_EXCLUSIVE_LOAD_REGISTER_GADGETS(ordering, size) \
	ExclusiveLoadRegisterGadgets{ \
		uml_arm64_gadget_exclusive_load_##ordering##_##size##_r, \
		uml_arm64_gadget_exclusive_load_##ordering##_##size##_z}
#define UML_EXCLUSIVE_LOAD_SIZE_GADGETS(ordering) \
	ExclusiveLoadSizeGadgets{ \
		UML_EXCLUSIVE_LOAD_REGISTER_GADGETS(ordering, b), \
		UML_EXCLUSIVE_LOAD_REGISTER_GADGETS(ordering, h), \
		UML_EXCLUSIVE_LOAD_REGISTER_GADGETS(ordering, w), \
		UML_EXCLUSIVE_LOAD_REGISTER_GADGETS(ordering, x)}
extern constexpr ExclusiveLoadGadgets exclusive_load_gadgets{
	UML_EXCLUSIVE_LOAD_SIZE_GADGETS(relaxed),
	UML_EXCLUSIVE_LOAD_SIZE_GADGETS(acquire),
};
#undef UML_EXCLUSIVE_LOAD_SIZE_GADGETS
#undef UML_EXCLUSIVE_LOAD_REGISTER_GADGETS

#define UML_EXCLUSIVE_STORE_REGISTER_GADGETS(ordering, size) \
	AtomicRegisterGadgets{ \
		uml_arm64_gadget_exclusive_store_##ordering##_##size##_rr, \
		uml_arm64_gadget_exclusive_store_##ordering##_##size##_rz, \
		uml_arm64_gadget_exclusive_store_##ordering##_##size##_zr, \
		uml_arm64_gadget_exclusive_store_##ordering##_##size##_zz}
#define UML_EXCLUSIVE_STORE_SIZE_GADGETS(ordering) \
	AtomicSizeGadgets{ \
		UML_EXCLUSIVE_STORE_REGISTER_GADGETS(ordering, b), \
		UML_EXCLUSIVE_STORE_REGISTER_GADGETS(ordering, h), \
		UML_EXCLUSIVE_STORE_REGISTER_GADGETS(ordering, w), \
		UML_EXCLUSIVE_STORE_REGISTER_GADGETS(ordering, x)}
extern constexpr ExclusiveStoreGadgets exclusive_store_gadgets{
	UML_EXCLUSIVE_STORE_SIZE_GADGETS(relaxed),
	UML_EXCLUSIVE_STORE_SIZE_GADGETS(release),
};
#undef UML_EXCLUSIVE_STORE_SIZE_GADGETS
#undef UML_EXCLUSIVE_STORE_REGISTER_GADGETS

#define UML_EXCLUSIVE_PAIR_LOAD_REGISTER_GADGETS(ordering, size) \
	AtomicRegisterGadgets{ \
		uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_rr, \
		uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_rz, \
		uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_zr, \
		uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_zz}
#define UML_EXCLUSIVE_PAIR_LOAD_SIZE_GADGETS(ordering) \
	ExclusivePairLoadSizeGadgets{ \
		UML_EXCLUSIVE_PAIR_LOAD_REGISTER_GADGETS(ordering, w), \
		UML_EXCLUSIVE_PAIR_LOAD_REGISTER_GADGETS(ordering, x)}
extern constexpr ExclusivePairLoadGadgets exclusive_pair_load_gadgets{
	UML_EXCLUSIVE_PAIR_LOAD_SIZE_GADGETS(relaxed),
	UML_EXCLUSIVE_PAIR_LOAD_SIZE_GADGETS(acquire),
};
#undef UML_EXCLUSIVE_PAIR_LOAD_SIZE_GADGETS
#undef UML_EXCLUSIVE_PAIR_LOAD_REGISTER_GADGETS

#define UML_EXCLUSIVE_PAIR_STORE_REGISTER_GADGETS(ordering, size) \
	ExclusivePairStoreRegisterGadgets{ \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rrr, \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rrz, \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rzr, \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rzz, \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zrr, \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zrz, \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zzr, \
		uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zzz}
#define UML_EXCLUSIVE_PAIR_STORE_SIZE_GADGETS(ordering) \
	ExclusivePairStoreSizeGadgets{ \
		UML_EXCLUSIVE_PAIR_STORE_REGISTER_GADGETS(ordering, w), \
		UML_EXCLUSIVE_PAIR_STORE_REGISTER_GADGETS(ordering, x)}
extern constexpr ExclusivePairStoreGadgets exclusive_pair_store_gadgets{
	UML_EXCLUSIVE_PAIR_STORE_SIZE_GADGETS(relaxed),
	UML_EXCLUSIVE_PAIR_STORE_SIZE_GADGETS(release),
};
#undef UML_EXCLUSIVE_PAIR_STORE_SIZE_GADGETS
#undef UML_EXCLUSIVE_PAIR_STORE_REGISTER_GADGETS

using OrderedLoadGadgets = ExclusiveLoadGadgets;
using OrderedStoreGadgets = ExclusiveLoadGadgets;

#define UML_ORDERED_REGISTER_GADGETS(kind, ordering, size) \
	ExclusiveLoadRegisterGadgets{ \
		uml_arm64_gadget_ordered_##kind##_##ordering##_##size##_r, \
		uml_arm64_gadget_ordered_##kind##_##ordering##_##size##_z}
#define UML_ORDERED_SIZE_GADGETS(kind, ordering) \
	ExclusiveLoadSizeGadgets{ \
		UML_ORDERED_REGISTER_GADGETS(kind, ordering, b), \
		UML_ORDERED_REGISTER_GADGETS(kind, ordering, h), \
		UML_ORDERED_REGISTER_GADGETS(kind, ordering, w), \
		UML_ORDERED_REGISTER_GADGETS(kind, ordering, x)}
extern constexpr OrderedLoadGadgets ordered_load_gadgets{
	UML_ORDERED_SIZE_GADGETS(load, relaxed),
	UML_ORDERED_SIZE_GADGETS(load, acquire),
};
extern constexpr OrderedStoreGadgets ordered_store_gadgets{
	UML_ORDERED_SIZE_GADGETS(store, limited),
	UML_ORDERED_SIZE_GADGETS(store, release),
};
#undef UML_ORDERED_SIZE_GADGETS
#undef UML_ORDERED_REGISTER_GADGETS

using RcpcRegisterGadgets = std::array<Gadget, 2>;
using RcpcLoadGadgets = std::array<RcpcRegisterGadgets, 9>;
using RcpcStoreGadgets = std::array<RcpcRegisterGadgets, 4>;
#define UML_RCPC_REGISTER_GADGETS(kind, name) \
	RcpcRegisterGadgets{ \
		uml_arm64_gadget_rcpc_##kind##_##name##_r, \
		uml_arm64_gadget_rcpc_##kind##_##name##_z}
extern constexpr RcpcLoadGadgets rcpc_load_gadgets{
	UML_RCPC_REGISTER_GADGETS(load, zero_b),
	UML_RCPC_REGISTER_GADGETS(load, zero_h),
	UML_RCPC_REGISTER_GADGETS(load, zero_w),
	UML_RCPC_REGISTER_GADGETS(load, zero_x),
	UML_RCPC_REGISTER_GADGETS(load, sb_w),
	UML_RCPC_REGISTER_GADGETS(load, sb_x),
	UML_RCPC_REGISTER_GADGETS(load, sh_w),
	UML_RCPC_REGISTER_GADGETS(load, sh_x),
	UML_RCPC_REGISTER_GADGETS(load, sw_x),
};
extern constexpr RcpcStoreGadgets rcpc_store_gadgets{
	UML_RCPC_REGISTER_GADGETS(store, b),
	UML_RCPC_REGISTER_GADGETS(store, h),
	UML_RCPC_REGISTER_GADGETS(store, w),
	UML_RCPC_REGISTER_GADGETS(store, x),
};
#undef UML_RCPC_REGISTER_GADGETS

using SimdOrderedAddressGadgets = std::array<Gadget, 2>;
using SimdOrderedScalarGadgets =
	std::array<SimdOrderedAddressGadgets, 4>;
#define UML_SIMD_ORDERED_ADDRESS_GADGETS(direction, size) \
	SimdOrderedAddressGadgets{ \
		uml_arm64_gadget_simd_ordered_##direction##_##size##_r, \
		uml_arm64_gadget_simd_ordered_##direction##_##size##_s}
extern constexpr SimdOrderedScalarGadgets simd_ordered_scalar_load_gadgets{
	UML_SIMD_ORDERED_ADDRESS_GADGETS(load, b),
	UML_SIMD_ORDERED_ADDRESS_GADGETS(load, h),
	UML_SIMD_ORDERED_ADDRESS_GADGETS(load, s),
	UML_SIMD_ORDERED_ADDRESS_GADGETS(load, d),
};
extern constexpr SimdOrderedScalarGadgets simd_ordered_scalar_store_gadgets{
	UML_SIMD_ORDERED_ADDRESS_GADGETS(store, b),
	UML_SIMD_ORDERED_ADDRESS_GADGETS(store, h),
	UML_SIMD_ORDERED_ADDRESS_GADGETS(store, s),
	UML_SIMD_ORDERED_ADDRESS_GADGETS(store, d),
};
#undef UML_SIMD_ORDERED_ADDRESS_GADGETS

using SimdOrderedImplementationGadgets =
	std::array<SimdOrderedAddressGadgets, 2>;
extern constexpr SimdOrderedImplementationGadgets simd_ordered_q_load_gadgets{
	SimdOrderedAddressGadgets{
		uml_arm64_gadget_simd_ordered_load_q_r_fallback,
		uml_arm64_gadget_simd_ordered_load_q_s_fallback},
	SimdOrderedAddressGadgets{
		uml_arm64_gadget_simd_ordered_load_q_r_native,
		uml_arm64_gadget_simd_ordered_load_q_s_native},
};
extern constexpr SimdOrderedImplementationGadgets simd_ordered_q_store_gadgets{
	SimdOrderedAddressGadgets{
		uml_arm64_gadget_simd_ordered_store_q_r_fallback,
		uml_arm64_gadget_simd_ordered_store_q_s_fallback},
	SimdOrderedAddressGadgets{
		uml_arm64_gadget_simd_ordered_store_q_r_native,
		uml_arm64_gadget_simd_ordered_store_q_s_native},
};

using SimdOrderedLaneGadgets =
	std::array<std::array<SimdOrderedAddressGadgets, 2>, 2>;
extern constexpr SimdOrderedLaneGadgets simd_ordered_lane_gadgets{
	std::array<SimdOrderedAddressGadgets, 2>{
		SimdOrderedAddressGadgets{
			uml_arm64_gadget_simd_ordered_store_lane0_r,
			uml_arm64_gadget_simd_ordered_store_lane0_s},
		SimdOrderedAddressGadgets{
			uml_arm64_gadget_simd_ordered_store_lane1_r,
			uml_arm64_gadget_simd_ordered_store_lane1_s}},
	std::array<SimdOrderedAddressGadgets, 2>{
		SimdOrderedAddressGadgets{
			uml_arm64_gadget_simd_ordered_load_lane0_r,
			uml_arm64_gadget_simd_ordered_load_lane0_s},
		SimdOrderedAddressGadgets{
			uml_arm64_gadget_simd_ordered_load_lane1_r,
			uml_arm64_gadget_simd_ordered_load_lane1_s}},
};

using OrderedPairRegisterGadgets = std::array<Gadget, 4>;
using OrderedPairAddressGadgets =
	std::array<OrderedPairRegisterGadgets, 2>;
using OrderedPairGadgets = std::array<OrderedPairAddressGadgets, 2>;
#define UML_ORDERED_PAIR_REGISTER_GADGETS(kind, size, address) \
	OrderedPairRegisterGadgets{ \
		uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_rr, \
		uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_rz, \
		uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_zr, \
		uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_zz}
extern constexpr OrderedPairGadgets ordered_pair_load_gadgets{
	OrderedPairAddressGadgets{
		UML_ORDERED_PAIR_REGISTER_GADGETS(load, w, no),
		UML_ORDERED_PAIR_REGISTER_GADGETS(load, w, post)},
	OrderedPairAddressGadgets{
		UML_ORDERED_PAIR_REGISTER_GADGETS(load, x, no),
		UML_ORDERED_PAIR_REGISTER_GADGETS(load, x, post)},
};
extern constexpr OrderedPairGadgets ordered_pair_store_gadgets{
	OrderedPairAddressGadgets{
		UML_ORDERED_PAIR_REGISTER_GADGETS(store, w, no),
		UML_ORDERED_PAIR_REGISTER_GADGETS(store, w, pre)},
	OrderedPairAddressGadgets{
		UML_ORDERED_PAIR_REGISTER_GADGETS(store, x, no),
		UML_ORDERED_PAIR_REGISTER_GADGETS(store, x, pre)},
};
#undef UML_ORDERED_PAIR_REGISTER_GADGETS

using ConsistentPairGadgets = std::array<Gadget, 4>;
#define UML_CONSISTENT_PAIR_GADGETS(kind) ConsistentPairGadgets{ \
	uml_arm64_gadget_consistent_pair_##kind##_rr, \
	uml_arm64_gadget_consistent_pair_##kind##_rz, \
	uml_arm64_gadget_consistent_pair_##kind##_zr, \
	uml_arm64_gadget_consistent_pair_##kind##_zz}
extern constexpr auto consistent_pair_ldap_gadgets =
	UML_CONSISTENT_PAIR_GADGETS(ldap);
extern constexpr auto consistent_pair_ldapp_gadgets =
	UML_CONSISTENT_PAIR_GADGETS(ldapp);
extern constexpr auto consistent_pair_store_gadgets =
	UML_CONSISTENT_PAIR_GADGETS(store);
#undef UML_CONSISTENT_PAIR_GADGETS

static_assert(offsetof(CpuState, registers) == 0);
static_assert(offsetof(CpuState, sp) == 248);
static_assert(offsetof(CpuState, pc) == 256);
static_assert(offsetof(CpuState, nzcv) == 264);
static_assert(offsetof(CpuState, vectors) == 272);
static_assert(offsetof(CpuState, fpcr) == 784);
static_assert(offsetof(CpuState, fpsr) == 788);
static_assert(offsetof(CpuState, tls) == 792);
static_assert(offsetof(CpuState, mte_enabled) == 800);
static_assert(offsetof(CpuState, tbi_enabled) == 801);
static_assert(offsetof(CpuState, mte_exclude) == 802);
static_assert(offsetof(CpuState, mte_tag_choices) == 804);
static_assert(offsetof(CpuState, mte_random_tags) == 1060);
static_assert(offsetof(CpuState, mte_seed) == 1062);
static_assert(offsetof(CpuState, mte_tag) == 1064);
static_assert(offsetof(CpuState, mte_random_state) == 1072);
static_assert(offsetof(CpuState, cpa_enabled) == 1080);
static_assert(offsetof(CpuState, cpa_multiply_enabled) == 1081);
static_assert(offsetof(CpuState, accdata) == 1084);
static_assert(offsetof(CpuState, pstate_ssbs) == 1088);
static_assert(offsetof(CpuState, pstate_dit) == 1089);
static_assert(offsetof(CpuState, pstate_tco) == 1090);
static_assert(offsetof(CpuState, pstate_sm) == 1091);
static_assert(offsetof(CpuState, pstate_za) == 1092);
static_assert(offsetof(CpuState, pstate_daif_access) == 1093);
static_assert(offsetof(CpuState, pstate_daif) == 1094);
static_assert(offsetof(CpuState, exclusive_address) == 1096);
static_assert(offsetof(CpuState, exclusive_key) == 1104);
static_assert(offsetof(CpuState, exclusive_value) == 1112);
static_assert(offsetof(CpuState, exclusive_value2) == 1120);
static_assert(offsetof(CpuState, exclusive_version) == 1128);
static_assert(offsetof(CpuState, exclusive_size) == 1136);
static_assert(offsetof(CpuState, deferred_vectors) == 1144);
static_assert(offsetof(CpuState, deferred_fpcr) == 1152);
static_assert(offsetof(CpuState, deferred_fpsr) == 1156);
static_assert(offsetof(CpuState, fp_loaded) == 1160);
static_assert(offsetof(CpuState, fp_dirty) == 1161);
static_assert(offsetof(CpuState, fpmr) == 1168);
static_assert(offsetof(CpuState, gadget_zero) == 1176);
static_assert(offsetof(CpuState, gadget_host_fpcr) == 1184);
static_assert(offsetof(CpuState, gadget_host_fpsr) == 1192);
static_assert(offsetof(CpuState, gadget_fp_active) == 1200);
static_assert(offsetof(CpuState, gadget_indirect_miss) == 1208);
static_assert(offsetof(CpuState, gadget_host_fpmr) == 1216);
static_assert(offsetof(CpuState, gadget_fpmr_active) == 1224);
static_assert(offsetof(CpuState, gcs_pointer) == 1232);
static_assert(offsetof(CpuState, gcs_implemented) == 1240);
static_assert(offsetof(CpuState, gcs_push_enabled) == 1241);
static_assert(offsetof(CpuState, gcs_store_enabled) == 1242);

} // namespace uml::cpu::arm64::gadgets::selection
