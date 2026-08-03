// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_structure_test_support.hpp"

#include <array>
#include <cstdlib>
#include <cstdio>

namespace structure = arm64_simd_structure_test;
namespace engine = uml::emulator::tests;

namespace {

constexpr std::array modes{
	structure::AddressMode::no_offset,
	structure::AddressMode::post_immediate,
	structure::AddressMode::post_register,
};

struct LaneEncoding final {
	bool full_vector;
	bool load;
	unsigned extra;
	unsigned opcode;
	unsigned scale_bit;
	unsigned size;
	structure::SingleForm form;
};

template <typename Callback>
void for_each_lane_encoding(Callback callback)
{
	for (const bool full_vector : {false, true})
		for (const bool load : {false, true})
			for (unsigned extra = 0; extra < 2U; ++extra)
				for (unsigned opcode = 0; opcode < 6U; ++opcode)
					for (unsigned scale_bit = 0; scale_bit < 2U;
					     ++scale_bit)
						for (unsigned size = 0; size < 4U; ++size) {
							const auto form =
								structure::decode_single(
									full_vector, load, extra,
									opcode, scale_bit, size);
							if (form.valid)
								callback(LaneEncoding{
									full_vector, load, extra,
									opcode, scale_bit, size,
									form});
						}
}

[[nodiscard]] LaneEncoding representative(
	unsigned count, std::size_t width, bool load)
{
	LaneEncoding result{};
	bool found = false;
	for_each_lane_encoding([&](const LaneEncoding &encoding) {
		if (!found && encoding.full_vector && encoding.load == load &&
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

void every_lane_and_addressing_form(engine::EngineHarness &harness,
	std::span<const std::byte> memory, std::uint64_t &seed)
{
	const auto address =
		structure::data_base + structure::page_size / 2U;
	for_each_lane_encoding([&](const LaneEncoding &encoding) {
		for (const auto mode : modes) {
			char name[168];
			std::snprintf(name, sizeof(name),
				"lane q=%u load=%u count=%u width=%zu lane=%zu "
				"opcode=%u r=%u s=%u size=%u mode=%u",
				static_cast<unsigned>(encoding.full_vector),
				static_cast<unsigned>(encoding.load),
				encoding.form.register_count,
				encoding.form.element_size, encoding.form.lane,
				encoding.opcode, encoding.extra,
				encoding.scale_bit, encoding.size,
				static_cast<unsigned>(mode));
			structure::check_single_success(
				harness, name, encoding.form,
				encoding.full_vector, encoding.load, encoding.extra,
				encoding.opcode, encoding.scale_bit, encoding.size,
				mode, 2, 29, 5, address, memory, seed++);
		}
	});
}

void every_register_field_and_alias(engine::EngineHarness &harness,
	std::span<const std::byte> memory, std::uint64_t &seed)
{
	const auto address =
		structure::data_base + structure::page_size / 2U;
	for (unsigned count = 1; count <= 4U; ++count)
		for (const auto width : {1U, 2U, 4U, 8U})
			for (const bool load : {false, true}) {
				const auto encoding = representative(count, width, load);
				for (const auto mode : modes) {
					for (unsigned rn = 0; rn < 32U; ++rn) {
						char name[144];
						std::snprintf(name, sizeof(name),
							"lane register-count=%u width=%u "
							"load=%u rn=%u mode=%u",
							count, width,
							static_cast<unsigned>(load), rn,
							static_cast<unsigned>(mode));
						structure::check_single_success(
							harness, name, encoding.form,
							encoding.full_vector, load,
							encoding.extra, encoding.opcode,
							encoding.scale_bit, encoding.size,
							mode, rn, 29, 5, address, memory,
							seed++);
					}
					for (unsigned rt = 0; rt < 32U; ++rt) {
						char name[144];
						std::snprintf(name, sizeof(name),
							"lane register-count=%u width=%u "
							"load=%u rt=%u mode=%u",
							count, width,
							static_cast<unsigned>(load), rt,
							static_cast<unsigned>(mode));
						structure::check_single_success(
							harness, name, encoding.form,
							encoding.full_vector, load,
							encoding.extra, encoding.opcode,
							encoding.scale_bit, encoding.size,
							mode, 2, rt, 5, address, memory,
							seed++);
					}
				}
				for (unsigned rm = 0; rm < 31U; ++rm) {
					char name[144];
					std::snprintf(name, sizeof(name),
						"lane register-count=%u width=%u load=%u "
						"rm=%u",
						count, width,
						static_cast<unsigned>(load), rm);
					structure::check_single_success(
						harness, name, encoding.form,
						encoding.full_vector, load,
						encoding.extra, encoding.opcode,
						encoding.scale_bit, encoding.size,
						structure::AddressMode::post_register,
						2, 29, rm, address, memory, seed++);
				}
				for (unsigned alias = 0; alias < 31U; ++alias) {
					char name[144];
					std::snprintf(name, sizeof(name),
						"lane register-count=%u width=%u load=%u "
						"rn-rm=x%u",
						count, width,
						static_cast<unsigned>(load), alias);
					structure::check_single_success(
						harness, name, encoding.form,
						encoding.full_vector, load,
						encoding.extra, encoding.opcode,
						encoding.scale_bit, encoding.size,
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
	for_each_lane_encoding([&](const LaneEncoding &encoding) {
		char name[152];
		std::snprintf(name, sizeof(name),
			"lane unaligned-gpr q=%u count=%u width=%zu lane=%zu load=%u",
			static_cast<unsigned>(encoding.full_vector),
			encoding.form.register_count, encoding.form.element_size,
			encoding.form.lane, static_cast<unsigned>(encoding.load));
		structure::check_single_success(
			harness, name, encoding.form, encoding.full_vector,
			encoding.load, encoding.extra, encoding.opcode,
			encoding.scale_bit, encoding.size,
			structure::AddressMode::no_offset, 2, 30, 5, address,
			memory, seed++);
	});
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0xc9);
	std::uint64_t seed = 0xa600'0000'0000'0000ULL;
	every_lane_and_addressing_form(harness, memory, seed);
	every_register_field_and_alias(harness, memory, seed);
	unaligned_gpr_addresses(harness, memory, seed);
	harness.print_summary("arm64-simd-structure-lane-behavior");
	return 0;
}
