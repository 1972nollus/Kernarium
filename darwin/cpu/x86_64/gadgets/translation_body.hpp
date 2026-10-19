// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "program.hpp"

#include "cpu/gadget_body_arena.hpp"
#include "memory/guest_address_space.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace uml::cpu::x86_64::gadgets {

/* One cached translation, allocated in its address space's arena apart from
 * the slots that index it.
 *
 * A way exists whether or not it holds a translation, so what belongs to one
 * translation is kept here and the slot holds three words and a few flags.
 * The body is the arena allocation [Body][guest bounds][stream words]: the
 * stream is the one the engine runs, and the two words before it are the
 * guest range its code was read from, which a chained block reads at
 * [stream, #-16] (program.hpp). The saved source is a second, cold
 * allocation in the address space's source arena, so that streams stay dense.
 * A body's program is a view of those, and the body is the only owner of
 * what a replay decoded.
 *
 * Every field below is written before the body is published, except the
 * stamps and page links, which move with validation and with the lists they
 * are in.
 */
struct alignas(16) Body final {
	explicit Body(Program &&view) noexcept : program{std::move(view)} {}

	/* The words a body takes in the arena for a stream of `words`. */
	[[nodiscard]] static std::size_t allocation_bytes(
		std::size_t words) noexcept
	{
		return sizeof(Body) + Program::stream_bytes(words);
	}

	[[nodiscard]] std::size_t allocation_bytes() const noexcept
	{
		return allocation_bytes(program.native_stream().size());
	}

	/* The stream and its header sit right behind the body. */
	[[nodiscard]] std::uintptr_t *stream_allocation() noexcept
	{
		return reinterpret_cast<std::uintptr_t *>(this + 1);
	}

	Program program;
	/* The first and last page of the guest range, as the lists of bodies
	 * per page thread them: entry 0 is the page the first byte is on and
	 * entry 1 the page the last byte is on. */
	std::array<Body *, 2> page_previous{};
	std::array<Body *, 2> page_next{};
	/* The page stamps the translation was last validated against. */
	std::array<::uml::memory::CodePageStamp, 2> code_pages{};
	/* The instruction limit it was built under, as slots compare it
	 * (kept_instruction_limit). */
	std::uint8_t instruction_limit{};
	/* Which of `page_previous` and `page_next` are in a page's list; never
	 * set before the body is published. */
	std::uint8_t linked_pages{};
	/* How many of `code_pages` are stamps, in two bits, and above them
	 * whether they cover the whole block; zero while there are none. */
	std::uint8_t stamp_state{};

	static constexpr std::uint8_t stamps_complete_bit = 4;
};
static_assert(sizeof(Body) % 16 == 0 && alignof(Body) == 16,
	"the guest bounds that follow a body start on a sixteen-byte boundary");
static_assert(sizeof(Body) == 128);

} // namespace uml::cpu::x86_64::gadgets
