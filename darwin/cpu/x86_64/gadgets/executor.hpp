// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "memory/guest_address_space.hpp"

#include <cstdint>
#include <memory>

namespace uml::cpu::x86_64::gadgets {

struct NativeTlbEntry;

class Executor final {
public:
	Executor();
	~Executor();
	Executor(Executor &&) noexcept;
	Executor &operator=(Executor &&) noexcept;

	Executor(const Executor &) = delete;
	Executor &operator=(const Executor &) = delete;

	void prepare(::uml::memory::GuestAddressSpace &memory,
		std::uint64_t generation = 0, std::uint64_t changes = 0) noexcept;
	[[nodiscard]] NativeTlbEntry *tlb_entries() noexcept;

private:
	struct Storage;
	std::unique_ptr<Storage> storage_;
};

} // namespace uml::cpu::x86_64::gadgets
