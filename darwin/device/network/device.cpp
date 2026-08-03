// SPDX-License-Identifier: GPL-2.0-or-later
#include "device.hpp"

#include "uml_darwin_network.h"

#include <cerrno>
#include <algorithm>
#include <iterator>
#include <mutex>
#include <vector>

namespace uml::network {
namespace {

std::mutex provider_mutex;
struct ActiveProviderEntry final {
	ActiveProviderScope *scope;
	ProviderContext *context;
	bool unbound_fallback_allowed;
};
std::vector<ActiveProviderEntry> active_providers;
thread_local ProviderContext *thread_provider;

Provider provider()
{
	if (thread_provider != nullptr)
		return thread_provider->provider();
	std::lock_guard lock{provider_mutex};
	return active_providers.size() == 1 &&
		active_providers.front().unbound_fallback_allowed ?
		active_providers.front().context->provider() : Provider{};
}

} // namespace

ActiveProviderScope::ActiveProviderScope(ProviderContext &context)
	: previous_{thread_provider}
{
	std::lock_guard lock{provider_mutex};
	const bool unbound_fallback_allowed = active_providers.empty();
	if (!unbound_fallback_allowed)
		for (auto &entry : active_providers)
			entry.unbound_fallback_allowed = false;
	active_providers.push_back(
		{this, &context, unbound_fallback_allowed});
	thread_provider = &context;
}

ActiveProviderScope::~ActiveProviderScope()
{
	{
		std::lock_guard lock{provider_mutex};
		const auto position = std::find_if(
			active_providers.rbegin(), active_providers.rend(),
			[this](const ActiveProviderEntry &entry) {
				return entry.scope == this;
			});
		if (position != active_providers.rend())
			active_providers.erase(std::next(position).base());
	}
	thread_provider = previous_;
}

} // namespace uml::network

extern "C" int darwin_uml_net_open(unsigned char mac[6], unsigned int *mtu)
{
	if (mac == nullptr || mtu == nullptr)
		return -EINVAL;
	try {
		const auto provider = uml::network::provider();
		return provider.open == nullptr ? -ENETDOWN :
			provider.open(provider.context, mac, mtu);
	} catch (...) {
		return -EIO;
	}
}

extern "C" int darwin_uml_net_irq_fd(void)
{
	try {
		const auto provider = uml::network::provider();
		return provider.irq_fd == nullptr ? -1 :
			provider.irq_fd(provider.context);
	} catch (...) {
		return -1;
	}
}

extern "C" int darwin_uml_net_read(void *buffer, unsigned long capacity)
{
	if (buffer == nullptr && capacity != 0)
		return -EINVAL;
	try {
		const auto provider = uml::network::provider();
		return provider.read == nullptr ? -ENETDOWN :
			provider.read(provider.context, buffer, capacity);
	} catch (...) {
		return -EIO;
	}
}

extern "C" int darwin_uml_net_write(const void *buffer, unsigned long length)
{
	if (buffer == nullptr && length != 0)
		return -EINVAL;
	try {
		const auto provider = uml::network::provider();
		return provider.write == nullptr ? -ENETDOWN :
			provider.write(provider.context, buffer, length);
	} catch (...) {
		return -EIO;
	}
}
