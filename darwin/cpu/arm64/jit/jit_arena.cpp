// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_arena.hpp"

#include <cstddef>
#include <cstring>
#include <sys/mman.h>

#include <libkern/OSCacheControl.h>
#include <pthread.h>

namespace uml::cpu::arm64::jit::detail {

ExecutableArena::ExecutableArena(std::size_t size) noexcept : size_{size}
{
	mapping_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
	if (mapping_ == MAP_FAILED)
		mapping_ = nullptr;
}

ExecutableArena::~ExecutableArena()
{
	if (mapping_ != nullptr)
		(void)munmap(mapping_, size_);
}

bool ExecutableArena::valid() const noexcept
{
	return mapping_ != nullptr;
}

void *ExecutableArena::store(
	std::span<const std::uint32_t> words) noexcept
{
	const auto bytes = words.size_bytes();
	const auto aligned = (used_ + 15U) & ~std::size_t{15};
	if (mapping_ == nullptr || bytes == 0 || aligned > size_ ||
	    bytes > size_ - aligned)
		return nullptr;
	auto *destination = static_cast<std::byte *>(mapping_) + aligned;
#if !defined(__IPHONE_OS_VERSION_MAX_ALLOWED)
	pthread_jit_write_protect_np(0);
#endif
	std::memcpy(destination, words.data(), bytes);
#if !defined(__IPHONE_OS_VERSION_MAX_ALLOWED)
	pthread_jit_write_protect_np(1);
#endif
	sys_icache_invalidate(destination, bytes);
	used_ = aligned + bytes;
	return destination;
}

void ExecutableArena::reset() noexcept
{
	used_ = 0;
}

bool ExecutableArena::available() noexcept
{
	static const bool supported = [] {
#if defined(__APPLE__) && defined(__aarch64__) && defined(MAP_JIT)
		void *mapping = mmap(nullptr, 16U * 1024U,
			PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
		if (mapping == MAP_FAILED)
			return false;
		(void)munmap(mapping, 16U * 1024U);
		return true;
#else
		return false;
#endif
	}();
	return supported;
}

} // namespace uml::cpu::arm64::jit::detail
