// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_ordered_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "platform/apple/host_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

extern "C" {
#define UML_DECLARE_SIMD_ORDERED_SCALAR(size, address) \
	void uml_arm64_gadget_simd_ordered_load_##size##_##address(); \
	void uml_arm64_gadget_simd_ordered_store_##size##_##address();
UML_DECLARE_SIMD_ORDERED_SCALAR(b, r)
UML_DECLARE_SIMD_ORDERED_SCALAR(b, s)
UML_DECLARE_SIMD_ORDERED_SCALAR(h, r)
UML_DECLARE_SIMD_ORDERED_SCALAR(h, s)
UML_DECLARE_SIMD_ORDERED_SCALAR(s, r)
UML_DECLARE_SIMD_ORDERED_SCALAR(s, s)
UML_DECLARE_SIMD_ORDERED_SCALAR(d, r)
UML_DECLARE_SIMD_ORDERED_SCALAR(d, s)
#undef UML_DECLARE_SIMD_ORDERED_SCALAR
#define UML_DECLARE_SIMD_ORDERED_Q(direction, address, implementation) \
	void uml_arm64_gadget_simd_ordered_##direction##_q_##address##_##implementation();
UML_DECLARE_SIMD_ORDERED_Q(load, r, fallback)
UML_DECLARE_SIMD_ORDERED_Q(load, s, fallback)
UML_DECLARE_SIMD_ORDERED_Q(load, r, native)
UML_DECLARE_SIMD_ORDERED_Q(load, s, native)
UML_DECLARE_SIMD_ORDERED_Q(store, r, fallback)
UML_DECLARE_SIMD_ORDERED_Q(store, s, fallback)
UML_DECLARE_SIMD_ORDERED_Q(store, r, native)
UML_DECLARE_SIMD_ORDERED_Q(store, s, native)
#undef UML_DECLARE_SIMD_ORDERED_Q
#define UML_DECLARE_SIMD_ORDERED_LANE(direction, lane, address) \
	void uml_arm64_gadget_simd_ordered_##direction##_lane##lane##_##address();
UML_DECLARE_SIMD_ORDERED_LANE(load, 0, r)
UML_DECLARE_SIMD_ORDERED_LANE(load, 0, s)
UML_DECLARE_SIMD_ORDERED_LANE(load, 1, r)
UML_DECLARE_SIMD_ORDERED_LANE(load, 1, s)
UML_DECLARE_SIMD_ORDERED_LANE(store, 0, r)
UML_DECLARE_SIMD_ORDERED_LANE(store, 0, s)
UML_DECLARE_SIMD_ORDERED_LANE(store, 1, r)
UML_DECLARE_SIMD_ORDERED_LANE(store, 1, s)
#undef UML_DECLARE_SIMD_ORDERED_LANE
}

namespace ordered = arm64_simd_ordered_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using Gadget = void (*)();
using AddressModes = std::array<Gadget, 2>;

#define UML_ADDRESS_MODES(direction, size) AddressModes{ \
	uml_arm64_gadget_simd_ordered_##direction##_##size##_r, \
	uml_arm64_gadget_simd_ordered_##direction##_##size##_s}
constexpr std::array scalar_load{
	UML_ADDRESS_MODES(load, b),
	UML_ADDRESS_MODES(load, h),
	UML_ADDRESS_MODES(load, s),
	UML_ADDRESS_MODES(load, d),
};
constexpr std::array scalar_store{
	UML_ADDRESS_MODES(store, b),
	UML_ADDRESS_MODES(store, h),
	UML_ADDRESS_MODES(store, s),
	UML_ADDRESS_MODES(store, d),
};
#undef UML_ADDRESS_MODES

constexpr std::array q_load{
	AddressModes{
		uml_arm64_gadget_simd_ordered_load_q_r_fallback,
		uml_arm64_gadget_simd_ordered_load_q_s_fallback},
	AddressModes{
		uml_arm64_gadget_simd_ordered_load_q_r_native,
		uml_arm64_gadget_simd_ordered_load_q_s_native},
};
constexpr std::array q_store{
	AddressModes{
		uml_arm64_gadget_simd_ordered_store_q_r_fallback,
		uml_arm64_gadget_simd_ordered_store_q_s_fallback},
	AddressModes{
		uml_arm64_gadget_simd_ordered_store_q_r_native,
		uml_arm64_gadget_simd_ordered_store_q_s_native},
};
constexpr std::array lane_store{
	AddressModes{
		uml_arm64_gadget_simd_ordered_store_lane0_r,
		uml_arm64_gadget_simd_ordered_store_lane0_s},
	AddressModes{
		uml_arm64_gadget_simd_ordered_store_lane1_r,
		uml_arm64_gadget_simd_ordered_store_lane1_s},
};
constexpr std::array lane_load{
	AddressModes{
		uml_arm64_gadget_simd_ordered_load_lane0_r,
		uml_arm64_gadget_simd_ordered_load_lane0_s},
	AddressModes{
		uml_arm64_gadget_simd_ordered_load_lane1_r,
		uml_arm64_gadget_simd_ordered_load_lane1_s},
};

[[nodiscard]] std::uintptr_t address_of(Gadget gadget)
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	constexpr std::array terminal{std::uint32_t{0xd400'0001U}};
	const std::array stream{instruction, terminal[0]};
	return gadgets::Builder{}.build(
		ordered::engine::engine_test_code_base, stream);
}

void exact_translation_selection()
{
	const bool native =
		::uml::platform::apple::probed_host_capabilities().has(
			::uml::cpu::ArmFeature::lrcpc3);
	std::set<std::uintptr_t> selected;
	std::set<std::uintptr_t> expected;
	for (unsigned load = 0; load < 2U; ++load) {
		const auto &scalar = load ? scalar_load : scalar_store;
		const auto &quad = load ? q_load : q_store;
		for (const auto &modes : scalar)
			for (const auto gadget : modes)
				expected.insert(address_of(gadget));
		for (const auto gadget : quad[native])
			expected.insert(address_of(gadget));
	}
	for (const auto &lanes : {lane_store, lane_load})
		for (const auto &modes : lanes)
			for (const auto gadget : modes)
				expected.insert(address_of(gadget));

	for (const auto &variant : ordered::simd_unscaled_variants)
		for (const bool sp_base : {false, true}) {
			constexpr int offset = -17;
			const unsigned rn = sp_base ? 31U : 2U;
			const unsigned rt = 29U;
			const auto built = build(ordered::simd_instruction(
				variant, offset, rn, rt));
			if (!built.complete || built.program.words().size() < 3U)
				std::abort();
			const auto words = built.program.words();
			const unsigned address_mode = static_cast<unsigned>(sp_base);
			Gadget selected_gadget{};
			if (variant.size == 16U)
				selected_gadget =
					(variant.load ? q_load : q_store)
						[native][address_mode];
			else
				selected_gadget =
					(variant.load ? scalar_load : scalar_store)
						[variant.size_code][address_mode];
			if (words[0] != address_of(selected_gadget))
				std::abort();
			const auto operand = rt |
				(static_cast<std::uintptr_t>(rn) << 5) |
				(static_cast<std::uintptr_t>(
					static_cast<std::uint64_t>(
						static_cast<std::int64_t>(offset))) << 10);
			if (words[1] != operand)
				std::abort();
			selected.insert(words[0]);
		}

	for (const auto &variant : ordered::simd_lane_variants)
		for (const bool sp_base : {false, true}) {
			const unsigned rn = sp_base ? 31U : 2U;
			const unsigned rt = 29U;
			const auto built = build(ordered::simd_instruction(
				variant, 0, rn, rt));
			if (!built.complete || built.program.words().size() < 3U)
				std::abort();
			const auto expected_gadget =
				(variant.load ? lane_load : lane_store)
					[variant.upper][sp_base];
			if (built.program.words()[0] !=
			    address_of(expected_gadget))
				std::abort();
			if (built.program.words()[1] !=
			    (rt | (static_cast<std::uintptr_t>(rn) << 5)))
				std::abort();
			selected.insert(built.program.words()[0]);
		}
	if (selected != expected || selected.size() != 28U)
		std::abort();
}

void all_q_implementations_are_distinct_and_referenced()
{
	std::set<std::uintptr_t> all;
	for (const auto &implementations : {q_load, q_store})
		for (const auto &modes : implementations)
			for (const auto gadget : modes)
				all.insert(address_of(gadget));
	if (all.size() != 8U)
		std::abort();
}

} // namespace

int main()
{
	exact_translation_selection();
	all_q_implementations_are_distinct_and_referenced();
	return 0;
}
