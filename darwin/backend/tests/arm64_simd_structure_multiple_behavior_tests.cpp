// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_structure_test_support.hpp"

#include <array>
#include <cstdio>

namespace structure = arm64_simd_structure_test;
namespace engine = uml::backend::tests;

namespace {

constexpr std::array modes{
	structure::AddressMode::no_offset,
	structure::AddressMode::post_immediate,
	structure::AddressMode::post_register,
};

template <typename Callback>
void for_each_layout(Callback callback)
{
	for (const auto &form : structure::multiple_forms)
		for (const bool full_vector : {false, true})
			for (unsigned size = 0; size < 4U; ++size) {
				if (structure::multiple_arrangement_valid(
					form, full_vector, size))
					callback(form, full_vector, size);
			}
}

void every_layout_and_addressing_form(engine::EngineHarness &harness,
	std::span<const std::byte> memory, std::uint64_t &seed)
{
	const auto address =
		structure::data_base + structure::page_size / 2U;
	for_each_layout([&](const structure::MultipleForm &form,
			    bool full_vector, unsigned size) {
		for (const bool load : {false, true})
			for (const auto mode : modes) {
				char name[144];
				std::snprintf(name, sizeof(name),
					"%.*s layout q=%u size=%u load=%u mode=%u",
					static_cast<int>(form.mnemonic.size()),
					form.mnemonic.data(),
					static_cast<unsigned>(full_vector), size,
					static_cast<unsigned>(load),
					static_cast<unsigned>(mode));
				structure::check_multiple_success(
					harness, name, form, full_vector, size, load,
					mode, 2, 29, 5, address, memory, seed++);
			}
	});
}

void every_base_and_transfer_register(engine::EngineHarness &harness,
	std::span<const std::byte> memory, std::uint64_t &seed)
{
	const auto address =
		structure::data_base + structure::page_size / 2U;
	for (const auto &form : structure::multiple_forms)
		for (const bool load : {false, true})
			for (const auto mode : modes) {
				for (unsigned rn = 0; rn < 32U; ++rn) {
					char name[136];
					std::snprintf(name, sizeof(name),
						"%.*s base rn=%u load=%u mode=%u",
						static_cast<int>(form.mnemonic.size()),
						form.mnemonic.data(), rn,
						static_cast<unsigned>(load),
						static_cast<unsigned>(mode));
					structure::check_multiple_success(
						harness, name, form, true, 0, load, mode,
						rn, 13, 5, address, memory, seed++);
				}
				for (unsigned rt = 0; rt < 32U; ++rt) {
					char name[136];
					std::snprintf(name, sizeof(name),
						"%.*s first rt=%u load=%u mode=%u",
						static_cast<int>(form.mnemonic.size()),
						form.mnemonic.data(), rt,
						static_cast<unsigned>(load),
						static_cast<unsigned>(mode));
					structure::check_multiple_success(
						harness, name, form, true, 0, load, mode,
						2, rt, 5, address, memory, seed++);
				}
			}
}

void every_post_index_register_and_alias(engine::EngineHarness &harness,
	std::span<const std::byte> memory, std::uint64_t &seed)
{
	const auto address =
		structure::data_base + structure::page_size / 2U;
	for (const auto &form : structure::multiple_forms)
		for (const bool load : {false, true}) {
			for (unsigned rm = 0; rm < 31U; ++rm) {
				char name[136];
				std::snprintf(name, sizeof(name),
					"%.*s post-register rm=%u load=%u",
					static_cast<int>(form.mnemonic.size()),
					form.mnemonic.data(), rm,
					static_cast<unsigned>(load));
				structure::check_multiple_success(
					harness, name, form, true, 2, load,
					structure::AddressMode::post_register,
					2, 29, rm, address, memory, seed++);
			}
			for (unsigned alias = 0; alias < 31U; ++alias) {
				char name[136];
				std::snprintf(name, sizeof(name),
					"%.*s rn-rm-alias x%u load=%u",
					static_cast<int>(form.mnemonic.size()),
					form.mnemonic.data(), alias,
					static_cast<unsigned>(load));
				structure::check_multiple_success(
					harness, name, form, true, 1, load,
					structure::AddressMode::post_register,
					alias, 30, alias, address, memory, seed++);
			}
		}
}

void unaligned_gpr_addresses(engine::EngineHarness &harness,
	std::span<const std::byte> memory, std::uint64_t &seed)
{
	const auto address =
		structure::data_base + structure::page_size / 2U + 3U;
	for_each_layout([&](const structure::MultipleForm &form,
			    bool full_vector, unsigned size) {
		for (const bool load : {false, true}) {
			char name[136];
			std::snprintf(name, sizeof(name),
				"%.*s unaligned-gpr q=%u size=%u load=%u",
				static_cast<int>(form.mnemonic.size()),
				form.mnemonic.data(),
				static_cast<unsigned>(full_vector), size,
				static_cast<unsigned>(load));
			structure::check_multiple_success(
				harness, name, form, full_vector, size, load,
				structure::AddressMode::no_offset, 2, 31, 5,
				address, memory, seed++);
		}
	});
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0x52);
	std::uint64_t seed = 0xa200'0000'0000'0000ULL;
	every_layout_and_addressing_form(harness, memory, seed);
	every_base_and_transfer_register(harness, memory, seed);
	every_post_index_register_and_alias(harness, memory, seed);
	unaligned_gpr_addresses(harness, memory, seed);
	harness.print_summary("arm64-simd-structure-multiple-behavior");
	return 0;
}
