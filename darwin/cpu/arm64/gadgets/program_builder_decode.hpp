// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "program.hpp"

#include <cstdint>
#include <memory_resource>
#include <vector>

namespace uml::cpu::arm64::gadgets::builder_detail {

struct DecodeContext final {
	std::uint64_t guest_address;
	std::uint64_t address;
	std::uint32_t instruction;
	std::size_t &compiled;
	bool &program_accesses_fp_state;
	std::pmr::vector<std::uintptr_t> &words;
	const Builder::LinkResolver &resolve_link;
	const Builder::IndirectLinkResolver &resolve_indirect_link;
	const BuildOptions &options;
};

enum class DecodeResult : std::uint8_t {
	decoded,
	terminal,
	implementation_gap,
};

[[nodiscard]] DecodeResult implementation_gap(DecodeContext &context);
[[nodiscard]] DecodeResult decode_branch_system(DecodeContext &context);
[[nodiscard]] DecodeResult decode_data_processing(DecodeContext &context);
[[nodiscard]] DecodeResult decode_floating_simd(DecodeContext &context);
[[nodiscard]] DecodeResult decode_load_store(DecodeContext &context);
[[nodiscard]] DecodeResult decode_exception(DecodeContext &context);

} // namespace uml::cpu::arm64::gadgets::builder_detail
