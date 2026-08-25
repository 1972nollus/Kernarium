// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace uml::memory {

[[nodiscard]] bool register_anonymous_shared_backing(int fd) noexcept;
[[nodiscard]] bool is_anonymous_shared_backing(int fd) noexcept;

[[nodiscard]] void *map_shared_backing(void *address, std::size_t length,
	int protection, int flags, int fd, std::uint64_t offset) noexcept;

} // namespace uml::memory
