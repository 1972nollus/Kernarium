// SPDX-License-Identifier: GPL-2.0-or-later
#include "scalar_forwarding.hpp"

#if UML_GADGET_SCALAR_FORWARDING || UML_GADGET_DEPENDENCY_COVERAGE
#include <unordered_map>

namespace uml::cpu::arm64::gadgets {
extern "C" const ScalarContract uml_arm64_scalar_contract_begin[];
extern "C" const ScalarContract uml_arm64_scalar_contract_end[];

std::span<const ScalarContract> scalar_contracts() noexcept
{
	const auto bytes = reinterpret_cast<std::uintptr_t>(uml_arm64_scalar_contract_end) -
		reinterpret_cast<std::uintptr_t>(uml_arm64_scalar_contract_begin);
	return {uml_arm64_scalar_contract_begin, bytes / sizeof(ScalarContract)};
}

ScalarAccess scalar_access(std::span<const std::uintptr_t> words)
{
	if (words.empty()) return {};
	static const auto index = [] {
		std::unordered_map<std::uintptr_t, const ScalarContract *> result;
		result.reserve(scalar_contracts().size());
		for (const auto &entry : scalar_contracts())
			result.emplace(entry.entries[0], &entry);
		return result;
	}();
	const auto found = index.find(words[0]);
	if (found == index.end() || words.size() != 1U + found->second->operand_words)
		return {};
	const auto decode = [operand = words.size() > 1 ? words[1] : 0](std::uint16_t field) {
		if (field == scalar_no_register) return scalar_no_register;
		if ((field & 0x8000U) != 0) return static_cast<std::uint16_t>(field & 0x7fffU);
		const auto shift = field & 63U, width = (field >> 6) & 63U;
		return static_cast<std::uint16_t>(((operand >> shift) &
			((std::uint64_t{1} << width) - 1)) << ((field >> 12) & 3U));
	};
	const auto *contract = found->second;
	return {contract, decode(contract->destination),
		{decode(contract->sources[0]), decode(contract->sources[1]), decode(contract->sources[2])}};
}

} // namespace uml::cpu::arm64::gadgets
#endif
