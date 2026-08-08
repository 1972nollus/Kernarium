// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "program.hpp"

#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace uml::cpu::arm64::gadgets::builder_detail {

struct DecodeContext final {
	std::uint64_t guest_address;
	std::uint64_t address;
	std::uint32_t instruction;
	/* The instructions after this one, for the decoders that fuse a pair into
	 * a single entry. Empty at the end of the window, so a decoder must check
	 * before reading it, and a fusing decoder owns advancing `compiled` past
	 * everything it consumed. */
	std::span<const std::uint32_t> following;
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
