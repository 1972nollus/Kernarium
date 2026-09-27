// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace uml::emulator::tests {

/* The first architectural field in which `actual` differs from `expected`,
 * described with both values where one value names it, or nothing when the
 * states agree. The instruction corpus and the focused engine harness both
 * compare through this, so a CpuState field added here is checked by both. */
[[nodiscard]] inline std::optional<std::string> first_architecture_difference(
	const ::uml::cpu::arm64::CpuState &expected,
	const ::uml::cpu::arm64::CpuState &actual)
{
	const auto hex = [](std::uint64_t value) {
		char text[24];
		std::snprintf(text, sizeof(text), "%#" PRIx64, value);
		return std::string{text};
	};
	const auto values = [&](std::string_view field, std::uint64_t want,
				std::uint64_t have) {
		return std::string{field} + " expected " + hex(want) +
			" actual " + hex(have);
	};

	for (std::size_t index = 0; index < expected.registers.size(); ++index)
		if (expected.registers[index] != actual.registers[index])
			return values("x" + std::to_string(index),
				expected.registers[index],
				actual.registers[index]);
	if (expected.sp != actual.sp)
		return values("sp", expected.sp, actual.sp);
	if (expected.pc != actual.pc)
		return values("pc", expected.pc, actual.pc);
	if (expected.nzcv != actual.nzcv)
		return values("nzcv", expected.nzcv, actual.nzcv);
	for (std::size_t index = 0; index < expected.vectors.size(); ++index)
		for (std::size_t byte = 0; byte < 16U; ++byte)
			if (expected.vectors[index].bytes[byte] !=
			    actual.vectors[index].bytes[byte])
				return values("v" + std::to_string(index) +
						"[" + std::to_string(byte) + "]",
					std::to_integer<unsigned>(
						expected.vectors[index].bytes[byte]),
					std::to_integer<unsigned>(
						actual.vectors[index].bytes[byte]));
	if (expected.fpcr != actual.fpcr)
		return values("fpcr", expected.fpcr, actual.fpcr);
	if (expected.fpsr != actual.fpsr)
		return values("fpsr", expected.fpsr, actual.fpsr);
	if (expected.fpmr != actual.fpmr)
		return values("fpmr", expected.fpmr, actual.fpmr);
	if (expected.tls != actual.tls)
		return values("tls", expected.tls, actual.tls);
	if (!(expected.mte_enabled == actual.mte_enabled &&
	      expected.tbi_enabled == actual.tbi_enabled &&
	      expected.mte_exclude == actual.mte_exclude &&
	      expected.mte_tag_choices == actual.mte_tag_choices &&
	      expected.mte_random_tags == actual.mte_random_tags &&
	      expected.mte_seed == actual.mte_seed &&
	      expected.mte_tag == actual.mte_tag &&
	      expected.mte_random_state == actual.mte_random_state))
		return std::string{"tag state"};
	if (!(expected.cpa_enabled == actual.cpa_enabled &&
	      expected.cpa_multiply_enabled == actual.cpa_multiply_enabled &&
	      expected.accdata == actual.accdata))
		return std::string{"checked arithmetic state"};
	if (!(expected.pstate_ssbs == actual.pstate_ssbs &&
	      expected.pstate_dit == actual.pstate_dit &&
	      expected.pstate_tco == actual.pstate_tco &&
	      expected.pstate_sm == actual.pstate_sm &&
	      expected.pstate_za == actual.pstate_za &&
	      expected.pstate_daif_access == actual.pstate_daif_access &&
	      expected.pstate_daif == actual.pstate_daif))
		return std::string{"pstate"};
	if (!(expected.exclusive_address == actual.exclusive_address &&
	      expected.exclusive_key == actual.exclusive_key &&
	      expected.exclusive_value == actual.exclusive_value &&
	      expected.exclusive_value2 == actual.exclusive_value2 &&
	      expected.exclusive_version == actual.exclusive_version &&
	      expected.exclusive_size == actual.exclusive_size))
		return std::string{"exclusive monitor"};
	if (!(expected.gcs_pointer == actual.gcs_pointer &&
	      expected.gcs_implemented == actual.gcs_implemented &&
	      expected.gcs_push_enabled == actual.gcs_push_enabled &&
	      expected.gcs_store_enabled == actual.gcs_store_enabled))
		return "guarded control stack (pointer expected " +
			hex(expected.gcs_pointer) + " actual " +
			hex(actual.gcs_pointer) + ")";
	if (!(expected.pointer_auth_keys == actual.pointer_auth_keys &&
	      expected.pointer_authentication_enabled ==
		      actual.pointer_authentication_enabled))
		return std::string{"pointer-authentication state"};
	return std::nullopt;
}

} // namespace uml::emulator::tests
