// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_structure_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <set>

namespace structure = arm64_simd_structure_test;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace arm64 = uml::cpu::arm64;

namespace {

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	const std::array code{instruction, std::uint32_t{0xd400'0001U}};
	return gadgets::Builder{}.build(
		structure::engine::engine_test_code_base, code);
}

[[nodiscard]] constexpr std::uintptr_t base_offset(unsigned rn)
{
	return rn < 31U ? rn * sizeof(std::uint64_t) :
		offsetof(arm64::CpuState, sp);
}

[[nodiscard]] constexpr std::uintptr_t offset_register_offset(unsigned rm)
{
	return rm < 31U ? rm * sizeof(std::uint64_t) : 0U;
}

[[nodiscard]] constexpr std::uintptr_t packed_operand(
	unsigned rn, unsigned rt, unsigned rm, std::size_t lane_offset = 0)
{
	return rt | (base_offset(rn) << 16U) |
		(offset_register_offset(rm) << 32U) |
		(static_cast<std::uintptr_t>(lane_offset) << 48U);
}

[[nodiscard]] std::uintptr_t require_stream(
	const gadgets::BuildResult &built, std::uintptr_t operand)
{
	if (!built.complete)
		std::abort();
	const auto words = built.program.words();
	if (words.size() <= 1U)
		std::abort();
	if (words[1] != operand)
		std::abort();
	return words[0];
}

void multiple_translation_is_fully_specialized()
{
	std::set<std::uintptr_t> selected;
	for (const auto &form : structure::multiple_forms)
		for (const bool full_vector : {false, true})
			for (unsigned size = 0; size < 4U; ++size) {
				if (!structure::multiple_arrangement_valid(
					form, full_vector, size))
					continue;
				for (const bool load : {false, true})
					for (unsigned mode = 0; mode < 3U; ++mode)
						for (const bool sp_base : {false, true}) {
							const auto address_mode =
								static_cast<structure::AddressMode>(
									mode);
							const auto rn =
								sp_base ? 31U : 30U;
							const auto rm =
								structure::
									encoded_offset_register(
										address_mode, 5);
							const auto built = build(
								structure::
									multiple_instruction(
										full_vector, load,
										structure::post_index(
											address_mode),
										rm, form.opcode, size,
										rn, 29));
							selected.insert(require_stream(
								built,
								packed_operand(
									rn, 29, rm)));
						}
			}
	/*
	 * Each of the 48 contiguous and 126 interleaved transfer shapes has a
	 * distinct SP-specialized counterpart. The size field deliberately
	 * aliases the contiguous family because it has no runtime significance.
	 */
	if (selected.size() != 348U)
		std::abort();
}

void replicate_translation_is_fully_specialized()
{
	std::set<std::uintptr_t> selected;
	for (const bool full_vector : {false, true})
		for (unsigned extra = 0; extra < 2U; ++extra)
			for (unsigned opcode = 6; opcode < 8U; ++opcode)
				for (unsigned size = 0; size < 4U; ++size) {
					const auto form = structure::decode_single(
						full_vector, true, extra, opcode, 0,
						size);
					if (!form.valid)
						std::abort();
					for (unsigned mode = 0; mode < 3U; ++mode)
						for (const bool sp_base : {false, true}) {
							const auto address_mode =
								static_cast<structure::AddressMode>(
									mode);
							const auto rn =
								sp_base ? 31U : 30U;
							const auto rm =
								structure::
									encoded_offset_register(
										address_mode, 5);
							const auto built = build(
								structure::
									single_instruction(
										full_vector, true,
										structure::post_index(
											address_mode),
										extra, rm, opcode, 0,
										size, rn, 29));
							selected.insert(require_stream(
								built,
								packed_operand(
									rn, 29, rm)));
						}
				}
	if (selected.size() != 192U)
		std::abort();
}

void lane_translation_is_fully_specialized()
{
	std::set<std::uintptr_t> selected;
	for (const bool full_vector : {false, true})
		for (const bool load : {false, true})
			for (unsigned extra = 0; extra < 2U; ++extra)
				for (unsigned opcode = 0; opcode < 6U; ++opcode)
					for (unsigned scale_bit = 0; scale_bit < 2U;
					     ++scale_bit)
						for (unsigned size = 0; size < 4U;
						     ++size) {
							const auto form =
								structure::decode_single(
									full_vector, load,
									extra, opcode,
									scale_bit, size);
							if (!form.valid)
								continue;
							for (unsigned mode = 0; mode < 3U;
							     ++mode)
								for (const bool sp_base :
								     {false, true}) {
									const auto address_mode =
										static_cast<
											structure::
												AddressMode>(
											mode);
									const auto rn =
										sp_base ? 31U : 30U;
									const auto rm =
										structure::
											encoded_offset_register(
												address_mode,
												5);
									const auto built = build(
										structure::
											single_instruction(
												full_vector,
												load,
												structure::
													post_index(
														address_mode),
												extra, rm,
												opcode,
												scale_bit,
												size, rn,
												29));
									selected.insert(require_stream(
										built,
										packed_operand(
											rn, 29, rm,
											form.lane *
												form.element_size)));
								}
						}
	if (selected.size() != 192U)
		std::abort();
}

} // namespace

int main()
{
	multiple_translation_is_fully_specialized();
	replicate_translation_is_fully_specialized();
	lane_translation_is_fully_specialized();
	return 0;
}
