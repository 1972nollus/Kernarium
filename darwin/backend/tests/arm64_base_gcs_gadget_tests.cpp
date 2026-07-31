// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_gcs_test_support.hpp"

#include <array>
#include <set>

namespace gcs = arm64_base_gcs_test;
namespace gadgets = uml::backend::arm64::gadgets;
namespace test = uml::backend::tests;

namespace {

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	return gadgets::Builder{}.build(
		test::engine_test_code_base, std::array{instruction});
}

void operation_and_xzr_specialization()
{
	std::set<std::uintptr_t> entries;
	constexpr std::array operations{
		gcs::Operation::pop_main,
		gcs::Operation::push_main,
		gcs::Operation::switch_stack_1,
		gcs::Operation::switch_stack_2,
	};
	for (const auto operation : operations) {
		const auto real = build(gcs::encode(operation, 3));
		const auto zero = build(gcs::encode(operation, 31));
		gcs::require(real.complete && zero.complete);
		gcs::require(real.program.instruction_count() == 1 &&
			zero.program.instruction_count() == 1);
		gcs::require(real.program.words().size() == 5);
		gcs::require(zero.program.words().size() == 4);
		gcs::require(real.program.words()[1] == 3 * sizeof(std::uint64_t));
		entries.insert(real.program.words().front());
		entries.insert(zero.program.words().front());
	}
	gcs::require(entries.size() == 8);
}

void store_address_and_source_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const unsigned base : {5U, 31U}) {
		for (const unsigned transfer : {4U, 31U}) {
			const auto normal = build(
				gcs::encode(gcs::Operation::store, transfer, base));
			const auto unprivileged = build(gcs::encode(
				gcs::Operation::store_unprivileged, transfer, base));
			gcs::require(normal.complete && unprivileged.complete);
			gcs::require(normal.program.words().size() == 5 &&
				unprivileged.program.words().size() == 5);
			/* The EL0 access type is identical, so both architectural
			 * encodings intentionally select the same specialized entry. */
			gcs::require(normal.program.words().front() ==
				unprivileged.program.words().front());
			gcs::require(normal.program.words()[1] ==
				unprivileged.program.words()[1]);
			entries.insert(normal.program.words().front());
		}
	}
	gcs::require(entries.size() == 4);
}

void privileged_aliases_are_unconditional()
{
	std::uintptr_t illegal_entry = 0;
	for (const auto operation :
	     {gcs::Operation::pop_exception_context,
	      gcs::Operation::pop_exception,
	      gcs::Operation::push_exception}) {
		for (unsigned transfer = 0; transfer < 32; ++transfer) {
			const auto built = build(gcs::encode(operation, transfer));
			gcs::require(built.complete &&
				built.program.instruction_count() == 1);
			gcs::require(built.program.words().size() == 2);
			if (illegal_entry == 0)
				illegal_entry = built.program.words().front();
			gcs::require(built.program.words().front() == illegal_entry);
		}
	}
	gcs::require(illegal_entry != 0);
}

} // namespace

int main()
{
	operation_and_xzr_specialization();
	store_address_and_source_specialization();
	privileged_aliases_are_unconditional();
	std::puts("arm64-base-gcs-gadget: specialization passed");
}
