// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_structure_test_support.hpp"

#include <array>
#include <cstdlib>
#include <cstdio>

namespace structure = arm64_simd_structure_test;
namespace engine = uml::backend::tests;

namespace {

constexpr std::array modes{
	structure::AddressMode::no_offset,
	structure::AddressMode::post_immediate,
	structure::AddressMode::post_register,
};

struct ReplicateEncoding final {
	bool full_vector;
	unsigned extra;
	unsigned opcode;
	unsigned size;
	structure::SingleForm form;
};

template <typename Callback>
void for_each_replicate_encoding(Callback callback)
{
	for (const bool full_vector : {false, true})
		for (unsigned extra = 0; extra < 2U; ++extra)
			for (unsigned opcode = 6; opcode < 8U; ++opcode)
				for (unsigned size = 0; size < 4U; ++size) {
					const auto form = structure::decode_single(
						full_vector, true, extra, opcode, 0, size);
					if (form.valid)
						callback(ReplicateEncoding{
							full_vector, extra, opcode, size,
							form});
				}
}

[[nodiscard]] ReplicateEncoding representative(
	unsigned count, std::size_t width)
{
	ReplicateEncoding result{};
	bool found = false;
	for_each_replicate_encoding([&](const ReplicateEncoding &encoding) {
		if (!found && encoding.full_vector &&
		    encoding.form.register_count == count &&
		    encoding.form.element_size == width) {
			result = encoding;
			found = true;
		}
	});
	if (!found)
		std::abort();
	return result;
}

void every_replicate_and_addressing_form(engine::EngineHarness &harness,
	std::span<const std::byte> memory, std::uint64_t &seed)
{
	const auto address =
		structure::data_base + structure::page_size / 2U;
	for_each_replicate_encoding([&](const ReplicateEncoding &encoding) {
		for (const auto mode : modes) {
			char name[152];
			std::snprintf(name, sizeof(name),
				"replicate q=%u count=%u width=%zu opcode=%u r=%u "
				"size=%u mode=%u",
				static_cast<unsigned>(encoding.full_vector),
				encoding.form.register_count,
				encoding.form.element_size, encoding.opcode,
				encoding.extra, encoding.size,
				static_cast<unsigned>(mode));
			structure::check_single_success(
				harness, name, encoding.form,
				encoding.full_vector, true, encoding.extra,
				encoding.opcode, 0, encoding.size, mode, 2, 29, 5,
				address, memory, seed++);
		}
	});
}

void every_register_field_and_alias(engine::EngineHarness &harness,
	std::span<const std::byte> memory, std::uint64_t &seed)
{
	const auto address =
		structure::data_base + structure::page_size / 2U;
	for (unsigned count = 1; count <= 4U; ++count)
		for (const auto width : {1U, 2U, 4U, 8U}) {
			const auto encoding = representative(count, width);
			for (const auto mode : modes) {
				for (unsigned rn = 0; rn < 32U; ++rn) {
					char name[136];
					std::snprintf(name, sizeof(name),
						"replicate count=%u width=%u rn=%u mode=%u",
						count, width, rn,
						static_cast<unsigned>(mode));
					structure::check_single_success(
						harness, name, encoding.form, true, true,
						encoding.extra, encoding.opcode, 0,
						encoding.size, mode, rn, 29, 5, address,
						memory, seed++);
				}
				for (unsigned rt = 0; rt < 32U; ++rt) {
					char name[136];
					std::snprintf(name, sizeof(name),
						"replicate count=%u width=%u rt=%u mode=%u",
						count, width, rt,
						static_cast<unsigned>(mode));
					structure::check_single_success(
						harness, name, encoding.form, true, true,
						encoding.extra, encoding.opcode, 0,
						encoding.size, mode, 2, rt, 5, address,
						memory, seed++);
				}
			}
			for (unsigned rm = 0; rm < 31U; ++rm) {
				char name[136];
				std::snprintf(name, sizeof(name),
					"replicate count=%u width=%u rm=%u",
					count, width, rm);
				structure::check_single_success(
					harness, name, encoding.form, true, true,
					encoding.extra, encoding.opcode, 0,
					encoding.size,
					structure::AddressMode::post_register,
					2, 29, rm, address, memory, seed++);
			}
			for (unsigned alias = 0; alias < 31U; ++alias) {
				char name[136];
				std::snprintf(name, sizeof(name),
					"replicate count=%u width=%u rn-rm=x%u",
					count, width, alias);
				structure::check_single_success(
					harness, name, encoding.form, true, true,
					encoding.extra, encoding.opcode, 0,
					encoding.size,
					structure::AddressMode::post_register,
					alias, 29, alias, address, memory, seed++);
			}
		}
}

void unaligned_gpr_addresses(engine::EngineHarness &harness,
	std::span<const std::byte> memory, std::uint64_t &seed)
{
	const auto address =
		structure::data_base + structure::page_size / 2U + 1U;
	for_each_replicate_encoding([&](const ReplicateEncoding &encoding) {
		char name[128];
		std::snprintf(name, sizeof(name),
			"replicate unaligned q=%u count=%u width=%zu",
			static_cast<unsigned>(encoding.full_vector),
			encoding.form.register_count, encoding.form.element_size);
		structure::check_single_success(
			harness, name, encoding.form, encoding.full_vector, true,
			encoding.extra, encoding.opcode, 0, encoding.size,
			structure::AddressMode::no_offset, 2, 30, 5, address,
			memory, seed++);
	});
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0xda);
	std::uint64_t seed = 0xa700'0000'0000'0000ULL;
	every_replicate_and_addressing_form(harness, memory, seed);
	every_register_field_and_alias(harness, memory, seed);
	unaligned_gpr_addresses(harness, memory, seed);
	harness.print_summary("arm64-simd-structure-replicate-behavior");
	return 0;
}
