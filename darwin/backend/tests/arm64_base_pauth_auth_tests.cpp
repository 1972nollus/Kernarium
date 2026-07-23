// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

namespace test = arm64_pauth_test;

namespace {

constexpr std::array instruction_values{
	0x0000'1234'5678'9abcULL,
	0xffff'9234'5678'9abcULL,
};
constexpr std::array data_values{
	0x0000'1234'5678'9abcULL,
	0xab00'1234'5678'9abcULL,
	0xffff'9234'5678'9abcULL,
};
constexpr std::array modifiers{0U, 7U, 16U, 30U, 31U};

void authenticate_register_modifier_forms()
{
	for (unsigned signing_operation = 0; signing_operation < 4;
	     ++signing_operation) {
		const auto authentication_operation = signing_operation + 4;
		const std::span<const std::uint64_t> values = signing_operation >= 2 ?
			std::span<const std::uint64_t>{data_values} :
			std::span<const std::uint64_t>{instruction_values};
		for (unsigned destination = 0; destination < 31; ++destination)
			for (const unsigned modifier : modifiers) {
				/* When Xd==Xn, the PAC write replaces the context before a
				 * following AUT reads it. Use distinct registers so this is a
				 * valid matching-authentication test. */
				if (modifier == destination)
					continue;
				for (const auto value : values) {
					const auto initial = test::state(value, destination,
						modifier, 0x2468'ace0'1357'9bdfULL);
					const auto signed_result = test::execute_both(
						test::data_processing(signing_operation,
							destination, modifier), initial);
					const auto authenticated = test::execute_both(
						test::data_processing(authentication_operation,
							destination, modifier),
						signed_result.interpreted);
					test::require(authenticated.interpreted
						.registers[destination] ==
						test::stripped(initial.registers[destination],
							signing_operation >= 2));
					test::require_only_destination_changed(
						signed_result.interpreted,
						authenticated.interpreted, destination);
				}
			}
	}
}

void authenticate_zero_modifier_forms()
{
	for (unsigned signing_operation = 8; signing_operation < 12;
	     ++signing_operation) {
		const auto authentication_operation = signing_operation + 4;
		const std::span<const std::uint64_t> values = signing_operation >= 10 ?
			std::span<const std::uint64_t>{data_values} :
			std::span<const std::uint64_t>{instruction_values};
		for (unsigned destination = 0; destination < 31; ++destination)
			for (const auto value : values) {
				const auto initial = test::state(value, destination);
				const auto signed_result = test::execute_both(
					test::data_processing(signing_operation,
						destination), initial);
				const auto authenticated = test::execute_both(
					test::data_processing(authentication_operation,
						destination), signed_result.interpreted);
				test::require(authenticated.interpreted
					.registers[destination] ==
					test::stripped(initial.registers[destination],
						signing_operation >= 10));
			}
	}
}

void authenticate_hint_forms()
{
	struct Pair final {
		unsigned sign;
		unsigned authenticate;
		unsigned destination;
		unsigned modifier;
	};
	constexpr std::array pairs{
		Pair{8, 12, 17, 16}, Pair{10, 14, 17, 16},
		Pair{24, 28, 30, 0}, Pair{25, 29, 30, 31},
		Pair{26, 30, 30, 0}, Pair{27, 31, 30, 31},
	};
	for (const auto &pair : pairs)
		for (const auto value : instruction_values) {
			const auto initial = test::state(value, pair.destination,
				pair.modifier, 0x1357'9bdf'2468'ace0ULL);
			const auto signed_result = test::execute_both(
				test::hint(pair.sign), initial);
			const auto authenticated = test::execute_both(
				test::hint(pair.authenticate),
				signed_result.interpreted);
			test::require(authenticated.interpreted
				.registers[pair.destination] ==
				test::stripped(initial.registers[pair.destination], false));
		}
}

void discarded_authentication_forms()
{
	for (unsigned operation = 4; operation < 8; ++operation)
		for (const unsigned modifier : modifiers) {
			const auto initial = test::state(0x1234'5678'9abcULL,
				31, modifier);
			const auto result = test::execute_both(test::data_processing(
				operation, 31, modifier), initial);
			test::require(result.interpreted.registers == initial.registers);
		}
	for (unsigned operation = 12; operation < 16; ++operation) {
		const auto initial = test::state(0x1234'5678'9abcULL, 31);
		const auto result = test::execute_both(
			test::data_processing(operation, 31), initial);
		test::require(result.interpreted.registers == initial.registers);
	}
}

} // namespace

int main()
{
	authenticate_register_modifier_forms();
	authenticate_zero_modifier_forms();
	authenticate_hint_forms();
	discarded_authentication_forms();
	return 0;
}
