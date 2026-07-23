// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace test = arm64_fp8_test;
namespace gadgets = uml::backend::arm64::gadgets;

extern "C" {
void uml_arm64_gadget_fp8_software();
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
void uml_arm64_gadget_decoded();
#endif
extern void (*uml_arm64_fp8_native_up_table[])();
extern void (*uml_arm64_fp8_native_down_table[])();
extern void (*uml_arm64_fp8_native_dot_h_vector_table[])();
extern void (*uml_arm64_fp8_native_dot_h_indexed_table[])();
extern void (*uml_arm64_fp8_native_dot_s_vector_table[])();
extern void (*uml_arm64_fp8_native_dot_s_indexed_table[])();
extern void (*uml_arm64_fp8_native_mul_h_vector_table[])();
extern void (*uml_arm64_fp8_native_mul_h_indexed_table[])();
extern void (*uml_arm64_fp8_native_mul_s_vector_table[])();
extern void (*uml_arm64_fp8_native_mul_s_indexed_table[])();
extern void (*uml_arm64_fp8_native_matrix_table[])();
}

namespace {

[[nodiscard]] bool contains_word(void (*gadget)(), std::uint32_t word)
{
	const auto *bytes = reinterpret_cast<const std::byte *>(gadget);
	for (unsigned offset = 0; offset < 192U; offset += 4U) {
		std::uint32_t candidate{};
		std::memcpy(&candidate, bytes + offset, sizeof(candidate));
		if (candidate == word)
			return true;
	}
	return false;
}

void conversion_words()
{
	constexpr std::array up{0x2e21'7820U, 0x2e61'7820U,
		0x6e21'7820U, 0x6e61'7820U, 0x2ea1'7820U, 0x2ee1'7820U,
		0x6ea1'7820U, 0x6ee1'7820U};
	for (unsigned i = 0; i < up.size(); ++i)
		test::common::require(contains_word(uml_arm64_fp8_native_up_table[i], up[i]));
	constexpr std::array down{0x0e42'f420U, 0x4e42'f420U,
		0x0e02'f420U, 0x4e02'f420U};
	for (unsigned i = 0; i < down.size(); ++i)
		test::common::require(contains_word(
			uml_arm64_fp8_native_down_table[i], down[i]));
}

void dot_words()
{
	for (unsigned q = 0; q < 2U; ++q) {
		test::common::require(contains_word(
			uml_arm64_fp8_native_dot_h_vector_table[q],
			0x0e42'fc20U | (q << 30)));
		test::common::require(contains_word(
			uml_arm64_fp8_native_dot_s_vector_table[q],
			0x0e02'fc20U | (q << 30)));
		for (unsigned index = 0; index < 8U; ++index) {
			const auto word = 0x0f42'0020U | (q << 30) |
				(((index >> 2) & 1U) << 11) |
				(((index >> 1) & 1U) << 21) | ((index & 1U) << 20);
			test::common::require(contains_word(
				uml_arm64_fp8_native_dot_h_indexed_table[q * 8U + index], word));
		}
		for (unsigned index = 0; index < 4U; ++index) {
			const auto word = 0x0f02'0020U | (q << 30) |
				(((index >> 1) & 1U) << 11) | ((index & 1U) << 21);
			test::common::require(contains_word(
				uml_arm64_fp8_native_dot_s_indexed_table[q * 4U + index], word));
		}
	}
}

void multiply_and_matrix_words()
{
	for (unsigned selection = 0; selection < 2U; ++selection) {
		test::common::require(contains_word(
			uml_arm64_fp8_native_mul_h_vector_table[selection],
			0x0ec2'fc20U | (selection << 30)));
		for (unsigned index = 0; index < 16U; ++index) {
			const auto word = 0x0fc2'0020U | (selection << 30) |
				((index & 1U) << 19) | (((index >> 1) & 1U) << 20) |
				(((index >> 2) & 1U) << 21) | (((index >> 3) & 1U) << 11);
			test::common::require(contains_word(
				uml_arm64_fp8_native_mul_h_indexed_table[
					selection * 16U + index], word));
		}
	}
	for (unsigned selection = 0; selection < 4U; ++selection) {
		const auto selectors = ((selection >> 1) << 30) |
			((selection & 1U) << 22);
		test::common::require(contains_word(
			uml_arm64_fp8_native_mul_s_vector_table[selection],
			0x0e02'c420U | selectors));
		for (unsigned index = 0; index < 16U; ++index) {
			const auto word = 0x2f02'8020U | selectors |
				((index & 1U) << 19) | (((index >> 1) & 1U) << 20) |
				(((index >> 2) & 1U) << 21) | (((index >> 3) & 1U) << 11);
			test::common::require(contains_word(
				uml_arm64_fp8_native_mul_s_indexed_table[
					selection * 16U + index], word));
		}
	}
	test::common::require(contains_word(uml_arm64_fp8_native_matrix_table[0],
		0x6e02'ec20U));
	test::common::require(contains_word(uml_arm64_fp8_native_matrix_table[1],
		0x6e82'ec20U));
}

void build_uses_the_selected_runpath()
{
	const std::array instructions{test::convert_up(false, false, false),
		0xd400'0001U};
	const auto built = gadgets::Builder{}.build(test::common::pc, instructions);
	test::common::require(built.complete);
	const auto first = built.program.words().front();
#if UML_TEST_GADGET_INTERPRETER_RUNPATH
	test::common::require(first == reinterpret_cast<std::uintptr_t>(
		&uml_arm64_gadget_decoded));
	test::common::require(built.program.words().size() >= 2 &&
		built.program.words()[1] == instructions.front());
#else
	const auto software = reinterpret_cast<std::uintptr_t>(
		&uml_arm64_gadget_fp8_software);
	const auto native = reinterpret_cast<std::uintptr_t>(
		uml_arm64_fp8_native_up_table[0]);
	test::common::require(first == software || first == native);
#endif
}

void software_flags_survive_live_native_environment()
{
	auto initial = test::common::state(450);
	initial.fpsr = 0;
	initial.fpmr = 0;
	test::set_lane(initial, 1, 0, 32, 0x3f80'0000U);
	test::set_lane(initial, 2, 0, 32, 0x3f80'0000U);
	initial.vectors[3].bytes.fill(std::byte{});
	test::set_byte(initial, 3, 0, 0x7d); // FP8 SNaN raises IOC.
	constexpr std::uint32_t fadd_s0_s1_s2 = 0x1e22'2820U;
	const std::array instructions{fadd_s0_s1_s2,
		test::convert_up(false, false, false, {4, 3, 0}), 0xd400'0001U};

	auto expected = initial;
	test::common::NoMemory memory;
	uml::backend::arm64::Dispatcher dispatcher;
	for (unsigned i = 0; i < 2U; ++i) {
		expected.pc = test::common::pc + i * 4U;
		const auto result = dispatcher.execute(expected, memory, instructions[i]);
		test::common::require(result.reason ==
			uml::backend::arm64::StopReason::continued);
	}
	const auto built = gadgets::Builder{}.build(test::common::pc, instructions);
	test::common::require(built.complete);
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	auto actual = initial;
	actual.pc = test::common::pc;
	const auto event = gadgets::execute(built.program, actual, *tlb);
	test::common::require(event == gadgets::Event::system_call);
	expected.pc = actual.pc;
	test::common::require((actual.fpsr & 1U) != 0);
	test::common::require_architecture_equal(expected, actual);
}

} // namespace

int main()
{
	conversion_words();
	dot_words();
	multiply_and_matrix_words();
	build_uses_the_selected_runpath();
	software_flags_survive_live_native_environment();
}
