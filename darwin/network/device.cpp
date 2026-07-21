// SPDX-License-Identifier: GPL-2.0-or-later
#include "device.hpp"

#include "uml_darwin_runtime.h"

#include <cerrno>
#include <mutex>

namespace uml::network {
namespace {

std::mutex provider_mutex;
Provider active_provider;

Provider provider() noexcept
{
	std::lock_guard lock{provider_mutex};
	return active_provider;
}

} // namespace

void activate(const Provider &provider) noexcept
{
	std::lock_guard lock{provider_mutex};
	active_provider = provider;
}

void deactivate() noexcept
{
	std::lock_guard lock{provider_mutex};
	active_provider = {};
}

} // namespace uml::network

extern "C" int darwin_uml_net_open(unsigned char mac[6], unsigned int *mtu)
{
	if (mac == nullptr || mtu == nullptr)
		return -EINVAL;
	const auto provider = uml::network::provider();
	return provider.open == nullptr ? -ENETDOWN :
		provider.open(provider.context, mac, mtu);
}

extern "C" int darwin_uml_net_irq_fd(void)
{
	const auto provider = uml::network::provider();
	return provider.irq_fd == nullptr ? -1 :
		provider.irq_fd(provider.context);
}

extern "C" int darwin_uml_net_read(void *buffer, unsigned long capacity)
{
	if (buffer == nullptr && capacity != 0)
		return -EINVAL;
	const auto provider = uml::network::provider();
	return provider.read == nullptr ? -ENETDOWN :
		provider.read(provider.context, buffer, capacity);
}

extern "C" int darwin_uml_net_write(const void *buffer, unsigned long length)
{
	if (buffer == nullptr && length != 0)
		return -EINVAL;
	const auto provider = uml::network::provider();
	return provider.write == nullptr ? -ENETDOWN :
		provider.write(provider.context, buffer, length);
}
