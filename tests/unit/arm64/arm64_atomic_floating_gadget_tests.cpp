// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_floating_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <span>

namespace fp_atomic = arm64_atomic_floating_test;
namespace arm64 = uml::cpu::arm64;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;
namespace gadgets = uml::cpu::arm64::gadgets;

extern "C" {
#define UML_DECLARE_VARIANTS(implementation, size, operation) \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_relaxed(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_relaxed_sp(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_release(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_release_sp(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire_sp(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire_release(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_load_acquire_release_sp(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_relaxed(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_relaxed_sp(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_release(); \
	void uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_store_release_sp();
#define UML_DECLARE_OPERATIONS(implementation, size) \
	UML_DECLARE_VARIANTS(implementation, size, add) \
	UML_DECLARE_VARIANTS(implementation, size, max) \
	UML_DECLARE_VARIANTS(implementation, size, maxnm) \
	UML_DECLARE_VARIANTS(implementation, size, min) \
	UML_DECLARE_VARIANTS(implementation, size, minnm)
#define UML_DECLARE_SIZES(implementation) \
	UML_DECLARE_OPERATIONS(implementation, bf) \
	UML_DECLARE_OPERATIONS(implementation, h) \
	UML_DECLARE_OPERATIONS(implementation, s) \
	UML_DECLARE_OPERATIONS(implementation, d)
UML_DECLARE_SIZES(native)
UML_DECLARE_SIZES(software)
#undef UML_DECLARE_SIZES
#undef UML_DECLARE_OPERATIONS
#undef UML_DECLARE_VARIANTS
}

namespace {

using Function = void (*)();

struct InstructionScan final {
	bool terminated{};
	bool contains_expected{};
	unsigned family_words{};
};

[[nodiscard]] InstructionScan scan_instructions(
	Function function, std::uint32_t expected)
{
	constexpr std::uint32_t threaded_dispatch = 0xd61f'0100U; // br x8
	const auto address = reinterpret_cast<std::uintptr_t>(function);
	InstructionScan result;
	for (unsigned index = 0; index < 96U; ++index) {
		std::uint32_t word{};
		std::memcpy(&word, reinterpret_cast<const void *>(
			address + index * sizeof(word)), sizeof(word));
		if ((word & 0x3f20'0c00U) == 0x3c20'0000U) {
			++result.family_words;
			result.contains_expected |= word == expected;
		}
		if (word == threaded_dispatch) {
			result.terminated = true;
			break;
		}
	}
	return result;
}

void require_native_function(Function function, std::uint32_t expected)
{
	const auto scan = scan_instructions(function, expected);
	fp_atomic::require(scan.terminated,
		"native atomic gadget lacks its threaded-dispatch terminator");
	fp_atomic::require(scan.contains_expected && scan.family_words == 1U,
		"native atomic gadget lacks exactly one specialized instruction");
}

void require_software_function(Function function)
{
	const auto scan = scan_instructions(function, 0);
	fp_atomic::require(scan.terminated,
		"software atomic gadget lacks its threaded-dispatch terminator");
	fp_atomic::require(scan.family_words == 0U,
		"software atomic gadget contains a native family instruction");
}

void exact_symbol_inventory()
{
	std::set<std::uintptr_t> native;
	std::set<std::uintptr_t> software;
#define UML_INSERT_ONE(set, implementation, size, operation, variant) \
	set.insert(reinterpret_cast<std::uintptr_t>( \
		uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_##variant)); \
	set.insert(reinterpret_cast<std::uintptr_t>( \
		uml_arm64_gadget_atomic_floating_##implementation##_##size##_##operation##_##variant##_sp))
#define UML_INSERT_VARIANTS(set, implementation, size, operation) \
	UML_INSERT_ONE(set, implementation, size, operation, load_relaxed); \
	UML_INSERT_ONE(set, implementation, size, operation, load_release); \
	UML_INSERT_ONE(set, implementation, size, operation, load_acquire); \
	UML_INSERT_ONE(set, implementation, size, operation, load_acquire_release); \
	UML_INSERT_ONE(set, implementation, size, operation, store_relaxed); \
	UML_INSERT_ONE(set, implementation, size, operation, store_release)
#define UML_INSERT_OPERATIONS(set, implementation, size) \
	UML_INSERT_VARIANTS(set, implementation, size, add); \
	UML_INSERT_VARIANTS(set, implementation, size, max); \
	UML_INSERT_VARIANTS(set, implementation, size, maxnm); \
	UML_INSERT_VARIANTS(set, implementation, size, min); \
	UML_INSERT_VARIANTS(set, implementation, size, minnm)
#define UML_INSERT_SIZES(set, implementation) \
	UML_INSERT_OPERATIONS(set, implementation, bf); \
	UML_INSERT_OPERATIONS(set, implementation, h); \
	UML_INSERT_OPERATIONS(set, implementation, s); \
	UML_INSERT_OPERATIONS(set, implementation, d)
	UML_INSERT_SIZES(native, native);
	UML_INSERT_SIZES(software, software);
#undef UML_INSERT_SIZES
#undef UML_INSERT_OPERATIONS
#undef UML_INSERT_VARIANTS
#undef UML_INSERT_ONE
	fp_atomic::require(native.size() == 240U,
		"native atomic gadget inventory is not exactly 240 symbols");
	fp_atomic::require(software.size() == 240U,
		"software atomic gadget inventory is not exactly 240 symbols");
	for (const auto entry : native)
		fp_atomic::require(!software.contains(entry),
			"native/software atomic gadget symbol collision");
}

void verify_native_words()
{
#define UML_VERIFY_ONE(size, size_bits, operation, opc, variant, acquire, \
	release, return_old) \
	do { \
		constexpr std::uint32_t expected = 0x3c20'0000U | size_bits | \
			(static_cast<unsigned>(acquire) << 23) | \
			(static_cast<unsigned>(release) << 22) | (15U << 16) | \
			(static_cast<unsigned>(!return_old) << 15) | (opc << 12) | \
			(7U << 5) | (return_old ? 16U : 31U); \
		require_native_function( \
			uml_arm64_gadget_atomic_floating_native_##size##_##operation##_##variant, \
			expected); \
		require_native_function( \
			uml_arm64_gadget_atomic_floating_native_##size##_##operation##_##variant##_sp, \
			expected); \
		require_software_function( \
			uml_arm64_gadget_atomic_floating_software_##size##_##operation##_##variant); \
		require_software_function( \
			uml_arm64_gadget_atomic_floating_software_##size##_##operation##_##variant##_sp); \
	} while (false)
#define UML_VERIFY_VARIANTS(size, size_bits, operation, opc) \
	UML_VERIFY_ONE(size, size_bits, operation, opc, load_relaxed, \
		false, false, true); \
	UML_VERIFY_ONE(size, size_bits, operation, opc, load_release, \
		false, true, true); \
	UML_VERIFY_ONE(size, size_bits, operation, opc, load_acquire, \
		true, false, true); \
	UML_VERIFY_ONE(size, size_bits, operation, opc, load_acquire_release, \
		true, true, true); \
	UML_VERIFY_ONE(size, size_bits, operation, opc, store_relaxed, \
		false, false, false); \
	UML_VERIFY_ONE(size, size_bits, operation, opc, store_release, \
		false, true, false)
#define UML_VERIFY_OPERATIONS(size, size_bits) \
	UML_VERIFY_VARIANTS(size, size_bits, add, 0); \
	UML_VERIFY_VARIANTS(size, size_bits, max, 4); \
	UML_VERIFY_VARIANTS(size, size_bits, maxnm, 6); \
	UML_VERIFY_VARIANTS(size, size_bits, min, 5); \
	UML_VERIFY_VARIANTS(size, size_bits, minnm, 7)
	UML_VERIFY_OPERATIONS(bf, 0x0000'0000U);
	UML_VERIFY_OPERATIONS(h, 0x4000'0000U);
	UML_VERIFY_OPERATIONS(s, 0x8000'0000U);
	UML_VERIFY_OPERATIONS(d, 0xc000'0000U);
#undef UML_VERIFY_OPERATIONS
#undef UML_VERIFY_VARIANTS
#undef UML_VERIFY_ONE
}

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	const std::array code{instruction, atomic::terminal_system_call};
	return gadgets::Builder{}.build(engine::engine_test_code_base, code);
}

[[nodiscard]] constexpr std::uintptr_t packed_operand(
	unsigned rn, unsigned rs, unsigned rt)
{
	return rs | (static_cast<std::uintptr_t>(rt) << 8) |
		(static_cast<std::uintptr_t>(rn) << 16);
}

void translation_selects_every_hot_shape()
{
	std::set<std::uintptr_t> selected;
	for (const auto &shape : fp_atomic::widths)
		for (const auto operation : fp_atomic::operations)
			for (const bool sp_base : {false, true}) {
				const unsigned rn = sp_base ? 31U : 2U;
				for (unsigned ordering = 0; ordering < 4U; ++ordering) {
					const auto encoded = fp_atomic::instruction(
						shape.size_code, operation,
						(ordering & 2U) != 0U,
						(ordering & 1U) != 0U, false, rn, 3, 4);
					const auto result = build(encoded);
					fp_atomic::require(result.complete,
						"valid atomic load did not translate");
					const auto words = result.program.words();
					fp_atomic::require(words.size() >= 3U &&
						words[1] == packed_operand(rn, 3, 4),
						"atomic load retained runtime decode fields");
					selected.insert(words[0]);
				}
				for (const bool release : {false, true}) {
					const auto encoded = fp_atomic::instruction(
						shape.size_code, operation, false, release,
						true, rn, 3, 31);
					const auto result = build(encoded);
					fp_atomic::require(result.complete,
						"valid atomic store did not translate");
					const auto words = result.program.words();
					fp_atomic::require(words.size() >= 3U &&
						words[1] == packed_operand(rn, 3, 31),
						"atomic store retained runtime decode fields");
					selected.insert(words[0]);
				}
			}
	fp_atomic::require(selected.size() == 240U,
		"translation did not select 240 operation/order/width/base gadgets");
}

} // namespace

int main()
{
	exact_symbol_inventory();
	verify_native_words();
	translation_selects_every_hot_shape();
}
