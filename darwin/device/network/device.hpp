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
	void (*stop)(void *){};
};

class ProviderContext final {
public:
	explicit ProviderContext(Provider provider) noexcept : provider_{provider} {}

	[[nodiscard]] Provider provider() const noexcept { return provider_; }

private:
	Provider provider_;
};

class ActiveProviderScope final {
public:
	explicit ActiveProviderScope(ProviderContext &context);
	~ActiveProviderScope();

	ActiveProviderScope(const ActiveProviderScope &) = delete;
	ActiveProviderScope &operator=(const ActiveProviderScope &) = delete;

private:
	ProviderContext *previous_;
};

} // namespace uml::network
