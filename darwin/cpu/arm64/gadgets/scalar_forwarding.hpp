// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <span>

#ifndef UML_GADGET_SCALAR_FORWARDING
#define UML_GADGET_SCALAR_FORWARDING 0
#endif
#ifndef UML_GADGET_DEPENDENCY_COVERAGE
#define UML_GADGET_DEPENDENCY_COVERAGE 0
#endif
#ifndef UML_GADGET_VECTOR_FORWARDING
#define UML_GADGET_VECTOR_FORWARDING 0
#endif

namespace uml::cpu::arm64::gadgets {

inline constexpr std::uint16_t scalar_no_register = 0xffff;
/* Numeric order shared with scalar_entry declarations in assembly. */
inline constexpr std::array scalar_family_names{
	"move", "add_imm", "add_shifted", "add_extended", "carry", "multiply",
	"shift", "divide", "extract", "select", "compare", "logical_shifted",
	"logical_imm", "bitfield", "unary", "minmax", "load", "store", "crc",
};

struct ScalarContract final {
	std::array<std::uintptr_t, 8> entries;
	std::uint16_t destination;
	std::array<std::uint16_t, 3> sources;
	std::uint16_t family;
	std::uint16_t operand_words;
	std::uint32_t reserved;
};
static_assert(sizeof(ScalarContract) == 80);

struct ScalarAccess final {
	const ScalarContract *contract{};
	std::uint16_t destination{scalar_no_register};
	std::array<std::uint16_t, 3> sources{
		scalar_no_register, scalar_no_register, scalar_no_register};
	[[nodiscard]] unsigned source_count() const noexcept
	{
		unsigned count{};
		for (auto source : sources) count += source != scalar_no_register;
		return count;
	}
	[[nodiscard]] unsigned source_mask(std::uint16_t previous) const noexcept
	{
		if (previous == scalar_no_register) return 0;
		unsigned mask{};
		for (unsigned i = 0; i < sources.size(); ++i)
			if (sources[i] == previous) mask |= 1U << i;
		return mask;
	}
	[[nodiscard]] unsigned coverage_bucket(unsigned mask, unsigned family_base = 0) const noexcept
	{
		return contract ? 2U + (family_base + contract->family) * 32U +
			source_count() * 8U + mask : 1U;
	}
};

#if UML_GADGET_SCALAR_FORWARDING || UML_GADGET_DEPENDENCY_COVERAGE
[[nodiscard]] std::span<const ScalarContract> scalar_contracts() noexcept;
[[nodiscard]] ScalarAccess scalar_access(std::span<const std::uintptr_t> words);
#endif

#if UML_GADGET_VECTOR_FORWARDING || UML_GADGET_DEPENDENCY_COVERAGE
/* Vector contracts use the same immutable descriptor format, with bit 14
 * encoding a five-bit vector index relative to CpuState::vectors. Results
 * remain write-through and are live in native v2 only at declared edges. */
[[nodiscard]] std::span<const ScalarContract> vector_contracts() noexcept;
[[nodiscard]] ScalarAccess vector_access(std::span<const std::uintptr_t> words);
#endif

} // namespace uml::cpu::arm64::gadgets
