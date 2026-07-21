// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

namespace uml::network {

struct Provider final {
	void *context{};
	int (*open)(void *, unsigned char[6], unsigned int *){};
	int (*irq_fd)(void *){};
	int (*read)(void *, void *, std::size_t){};
	int (*write)(void *, const void *, std::size_t){};
};

void activate(const Provider &provider) noexcept;
void deactivate() noexcept;

} // namespace uml::network
