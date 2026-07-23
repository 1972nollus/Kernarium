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

void register_modifier_forms()
{
	for (unsigned operation = 0; operation < 4; ++operation) {
		const std::span<const std::uint64_t> values = operation >= 2 ?
			std::span<const std::uint64_t>{data_values} :
			std::span<const std::uint64_t>{instruction_values};
		for (unsigned destination = 0; destination < 32; ++destination)
			for (const unsigned modifier : modifiers)
				for (const auto value : values) {
					const auto initial = test::state(value, destination,
						modifier, 0x2468'ace0'1357'9bdfULL);
					const auto result = test::execute_both(
						test::data_processing(operation, destination,
							modifier), initial);
					test::require_only_destination_changed(
						initial, result.interpreted, destination);
				}
	}
}

void zero_modifier_forms()
{
	for (unsigned operation = 8; operation < 12; ++operation) {
		const std::span<const std::uint64_t> values = operation >= 10 ?
			std::span<const std::uint64_t>{data_values} :
			std::span<const std::uint64_t>{instruction_values};
		for (unsigned destination = 0; destination < 32; ++destination)
			for (const auto value : values) {
				const auto initial = test::state(value, destination);
				const auto result = test::execute_both(
					test::data_processing(operation, destination), initial);
				test::require_only_destination_changed(
					initial, result.interpreted, destination);
			}
	}
}

void hint_forms()
{
	struct Form final {
		unsigned immediate;
		unsigned destination;
		unsigned modifier;
	};
	constexpr std::array forms{
		Form{8, 17, 16}, Form{10, 17, 16},
		Form{24, 30, 0}, Form{25, 30, 31},
		Form{26, 30, 0}, Form{27, 30, 31},
	};
	for (const auto &form : forms)
		for (const auto value : instruction_values) {
			const auto initial = test::state(value, form.destination,
				form.modifier, 0x1357'9bdf'2468'ace0ULL);
			const auto result = test::execute_both(
				test::hint(form.immediate), initial);
			test::require_only_destination_changed(
				initial, result.interpreted, form.destination);
		}
}

void reserved_zero_modifier_encodings()
{
	for (unsigned operation = 8; operation < 16; ++operation)
		for (unsigned modifier = 0; modifier < 31; ++modifier)
			for (const unsigned destination : {0U, 17U, 31U})
				test::require_illegal(test::data_processing(
					operation, destination, modifier));
}

void production_specialization()
{
#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	const auto pacia = test::gadgets::Builder{}.build(test::pc,
		std::array{test::data_processing(0, 3, 7)});
	const auto pacib = test::gadgets::Builder{}.build(test::pc,
		std::array{test::data_processing(1, 3, 7)});
	const auto paciza = test::gadgets::Builder{}.build(test::pc,
		std::array{test::data_processing(8, 3)});
	const auto sp = test::gadgets::Builder{}.build(test::pc,
		std::array{test::data_processing(0, 3, 31)});
	const auto hint = test::gadgets::Builder{}.build(test::pc,
		std::array{test::hint(8)});
	test::require(pacia.program.words().front() !=
		pacib.program.words().front());
	test::require(pacia.program.words().front() !=
		paciza.program.words().front());
	test::require(pacia.program.words().front() == sp.program.words().front());
	test::require(pacia.program.words().front() == hint.program.words().front());
	const auto discard = test::gadgets::Builder{}.build(test::pc,
		std::array{test::data_processing(0, 31, 7)});
	const auto nop = test::gadgets::Builder{}.build(test::pc,
		std::array{test::hint(0)});
	test::require(discard.program.words().front() == nop.program.words().front());
#endif
}

} // namespace

int main()
{
	register_modifier_forms();
	zero_modifier_forms();
	hint_forms();
	reserved_zero_modifier_encodings();
	production_specialization();
	return 0;
}
