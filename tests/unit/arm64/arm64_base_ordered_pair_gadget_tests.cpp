// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_ordered_memory_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <set>

extern "C" {
#define UML_DECLARE_PAIR(kind, size, address) \
	void uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_rr(); \
	void uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_rz(); \
	void uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_zr(); \
	void uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_zz();
#define UML_DECLARE_PAIR_SIZE(kind, size, indexed) \
	UML_DECLARE_PAIR(kind, size, no) \
	UML_DECLARE_PAIR(kind, size, indexed)
UML_DECLARE_PAIR_SIZE(load, w, post)
UML_DECLARE_PAIR_SIZE(load, x, post)
UML_DECLARE_PAIR_SIZE(store, w, pre)
UML_DECLARE_PAIR_SIZE(store, x, pre)
#undef UML_DECLARE_PAIR_SIZE
#undef UML_DECLARE_PAIR

#define UML_DECLARE_CONSISTENT(kind) \
	void uml_arm64_gadget_consistent_pair_##kind##_rr(); \
	void uml_arm64_gadget_consistent_pair_##kind##_rz(); \
	void uml_arm64_gadget_consistent_pair_##kind##_zr(); \
	void uml_arm64_gadget_consistent_pair_##kind##_zz();
UML_DECLARE_CONSISTENT(ldap)
UML_DECLARE_CONSISTENT(ldapp)
UML_DECLARE_CONSISTENT(store)
#undef UML_DECLARE_CONSISTENT
}

namespace ordered = arm64_ordered_memory_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using Gadget = void (*)();
using RegisterModes = std::array<Gadget, 4>;
using AddressModes = std::array<RegisterModes, 2>;
using SizeModes = std::array<AddressModes, 2>;

#define UML_REGISTER_MODES(kind, size, address) RegisterModes{ \
	uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_rr, \
	uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_rz, \
	uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_zr, \
	uml_arm64_gadget_ordered_pair_##kind##_##size##_##address##_zz}
constexpr SizeModes pair_load{
	AddressModes{
		UML_REGISTER_MODES(load, w, no),
		UML_REGISTER_MODES(load, w, post)},
	AddressModes{
		UML_REGISTER_MODES(load, x, no),
		UML_REGISTER_MODES(load, x, post)},
};
constexpr SizeModes pair_store{
	AddressModes{
		UML_REGISTER_MODES(store, w, no),
		UML_REGISTER_MODES(store, w, pre)},
	AddressModes{
		UML_REGISTER_MODES(store, x, no),
		UML_REGISTER_MODES(store, x, pre)},
};
#undef UML_REGISTER_MODES

#define UML_CONSISTENT_MODES(kind) RegisterModes{ \
	uml_arm64_gadget_consistent_pair_##kind##_rr, \
	uml_arm64_gadget_consistent_pair_##kind##_rz, \
	uml_arm64_gadget_consistent_pair_##kind##_zr, \
	uml_arm64_gadget_consistent_pair_##kind##_zz}
constexpr std::array consistent{
	UML_CONSISTENT_MODES(store),
	UML_CONSISTENT_MODES(ldap),
	UML_CONSISTENT_MODES(ldapp),
};
#undef UML_CONSISTENT_MODES

[[nodiscard]] std::uintptr_t address_of(Gadget gadget)
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	const std::array stream{
		instruction,
		ordered::terminal_system_call,
	};
	return gadgets::Builder{}.build(
		ordered::engine::engine_test_code_base, stream);
}

[[nodiscard]] constexpr unsigned register_mode(
	unsigned first, unsigned second)
{
	return (first == 31U ? 2U : 0U) | (second == 31U ? 1U : 0U);
}

[[nodiscard]] constexpr std::uintptr_t operand(
	unsigned rn, unsigned first, unsigned second)
{
	return rn | (static_cast<std::uintptr_t>(first) << 5) |
		(static_cast<std::uintptr_t>(second) << 10);
}

void require_selected(std::uint32_t instruction, Gadget expected,
	std::uintptr_t expected_operand, std::set<std::uintptr_t> &selected)
{
	const auto built = build(instruction);
	const auto words = built.program.words();
	if (!built.complete || words.size() < 3U ||
	    words[0] != address_of(expected) ||
	    words[1] != expected_operand)
		std::abort();
	selected.insert(words[0]);
}

void exact_translation_selection()
{
	std::set<std::uintptr_t> selected;
	std::set<std::uintptr_t> expected;
	for (const auto &directions : {pair_load, pair_store})
		for (const auto &addresses : directions)
			for (const auto &modes : addresses)
				for (const auto gadget : modes)
					expected.insert(address_of(gadget));
	for (const auto &modes : consistent)
		for (const auto gadget : modes)
			expected.insert(address_of(gadget));

	for (const auto &variant : ordered::pair_variants) {
		for (const unsigned rn : {2U, 31U})
			for (const unsigned first : {3U, 31U})
				for (const unsigned second : {4U, 31U}) {
					const auto mode = register_mode(first, second);
					Gadget gadget{};
					if (variant.kind == ordered::PairKind::stilp ||
					    variant.kind == ordered::PairKind::ldiapp) {
						const auto &table =
							variant.load ? pair_load : pair_store;
						gadget = table[variant.size_code - 2U]
							[variant.writeback][mode];
					} else {
						const unsigned kind =
							variant.kind ==
									ordered::PairKind::stlp ?
								0U :
							variant.kind ==
									ordered::PairKind::ldap ?
								1U : 2U;
						gadget = consistent[kind][mode];
					}
					require_selected(
						ordered::pair_instruction(
							variant, rn, first, second),
						gadget, operand(rn, first, second), selected);
				}

		if (variant.kind == ordered::PairKind::ldiapp &&
		    variant.writeback) {
			for (const bool second_overlap : {false, true}) {
				const unsigned first = second_overlap ? 3U : 2U;
				const unsigned second = second_overlap ? 2U : 4U;
				const auto mode = register_mode(first, second);
				require_selected(
					ordered::pair_instruction(
						variant, 2, first, second),
					pair_load[variant.size_code - 2U][0][mode],
					operand(2, first, second), selected);
			}
		}
	}
	if (selected != expected || selected.size() != 44U)
		std::abort();
}

} // namespace

int main()
{
	exact_translation_selection();
	return 0;
}
