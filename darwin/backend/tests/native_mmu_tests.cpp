// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_address_space.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr std::size_t guest_page_size = 4096;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error{message};
}

class TemporaryBacking final {
public:
	TemporaryBacking()
	{
		auto path = std::to_array("/private/tmp/uml-native-mmu.XXXXXX");
		fd_ = mkstemp(path.data());
		require(fd_ >= 0, "mkstemp failed");
		(void)unlink(path.data());
		require(ftruncate(fd_, 4 * guest_page_size) == 0,
			"ftruncate failed");
	}

	~TemporaryBacking()
	{
		if (fd_ >= 0)
			(void)close(fd_);
	}

	TemporaryBacking(const TemporaryBacking &) = delete;
	TemporaryBacking &operator=(const TemporaryBacking &) = delete;

	[[nodiscard]] int fd() const noexcept { return fd_; }

private:
	int fd_{-1};
};

class ReservedSlot final {
public:
	ReservedSlot()
		: size_{static_cast<std::size_t>(getpagesize())}
	{
		void *reservation = mmap(nullptr, size_, PROT_NONE,
			MAP_PRIVATE | MAP_ANON, -1, 0);
		require(reservation != MAP_FAILED, "slot reservation failed");
		address_ = reinterpret_cast<std::uintptr_t>(reservation);
		require(munmap(reservation, size_) == 0,
			"slot reservation release failed");
	}

	~ReservedSlot()
	{
		if (address_ != 0)
			(void)munmap(reinterpret_cast<void *>(address_), size_);
	}

	ReservedSlot(const ReservedSlot &) = delete;
	ReservedSlot &operator=(const ReservedSlot &) = delete;

	[[nodiscard]] std::uintptr_t address() const noexcept { return address_; }
	[[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
	std::uintptr_t address_{};
	std::size_t size_{};
};

void test_absent_quarter_and_alias_coherency()
{
	TemporaryBacking backing;
	ReservedSlot slot;
	require(slot.size() >= 4 * guest_page_size,
		"host page is smaller than the emulated slot");
	auto &space = uml::runtime::native_address_space();
	const auto base = slot.address();
	require(space.map(base, backing.fd(), 0, guest_page_size,
		PROT_READ | PROT_WRITE) == 0, "first quarter map failed");
	auto page = std::span{
		reinterpret_cast<std::byte *>(base), guest_page_size};
	page[0] = std::byte{0x11};

	/* Darwin must not unmap the live sibling when Linux removes a hole. */
	require(space.unmap(base + guest_page_size, guest_page_size) == 0,
		"absent sibling unmap failed");
	require(page[0] == std::byte{0x11},
		"absent sibling unmap discarded the live quarter");

	/* An identical PTE synchronization must preserve live shadow bytes. */
	require(space.map(base, backing.fd(), 0, guest_page_size,
		PROT_READ | PROT_WRITE) == 0, "duplicate map failed");
	require(page[0] == std::byte{0x11},
		"duplicate map rebuilt an unchanged quarter");

	require(space.map(base + 2 * guest_page_size, backing.fd(),
		2 * guest_page_size, guest_page_size,
		PROT_READ | PROT_WRITE) == 0, "third quarter map failed");
	page[1] = std::byte{0x22};
	const std::byte authoritative{0x33};
	require(pwrite(backing.fd(), &authoritative, 1, 2) == 1,
		"authoritative alias write failed");
	require(space.unmap(base + 2 * guest_page_size, guest_page_size) == 0,
		"third quarter unmap failed");
	require(page[0] == std::byte{0x11} && page[1] == std::byte{0x22},
		"sibling unmap recopied the live quarter");
	require(space.refresh(base, guest_page_size) == 0,
		"authoritative alias refresh failed");
	require(page[1] == std::byte{0},
		"stale private shadow overwrote a changed physical alias");
	require(page[2] == authoritative,
		"slot rebuild did not load the authoritative physical alias");
	require(space.unmap(base, guest_page_size) == 0,
		"final quarter unmap failed");
}

void test_sibling_churn_keeps_live_quarter_stable()
{
	TemporaryBacking backing;
	ReservedSlot slot;
	auto &space = uml::runtime::native_address_space();
	const auto base = slot.address();
	require(space.map(base, backing.fd(), 0, guest_page_size,
		PROT_READ | PROT_WRITE) == 0, "live quarter map failed");
	require(space.map(base + 2 * guest_page_size, backing.fd(),
		2 * guest_page_size, guest_page_size,
		PROT_READ | PROT_WRITE) == 0, "churn quarter map failed");

	auto &counter = *reinterpret_cast<std::uint64_t *>(base);
	std::atomic_ref<std::uint64_t> value{counter};
	std::atomic_bool stop{};
	std::atomic_uint regressions{};
	std::thread writer{[&] {
		auto previous = value.fetch_add(1, std::memory_order_relaxed);
		while (!stop.load(std::memory_order_relaxed)) {
			const auto current = value.fetch_add(1,
				std::memory_order_relaxed);
			if (current <= previous)
				regressions.fetch_add(1, std::memory_order_relaxed);
			previous = current;
		}
	}};
	for (int iteration = 0; iteration < 2000; ++iteration) {
		require(space.unmap(base + 2 * guest_page_size,
			guest_page_size) == 0, "churn quarter unmap failed");
		require(space.map(base + 2 * guest_page_size, backing.fd(),
			2 * guest_page_size, guest_page_size,
			PROT_READ | PROT_WRITE) == 0,
			"churn quarter remap failed");
	}
	stop.store(true, std::memory_order_relaxed);
	writer.join();
	require(regressions.load(std::memory_order_relaxed) == 0,
		"sibling churn replaced a concurrently accessed live quarter");
	require(space.unmap(base + 2 * guest_page_size, guest_page_size) == 0,
		"churn quarter final unmap failed");
	require(space.unmap(base, guest_page_size) == 0,
		"live quarter final unmap failed");
}

} // namespace

int main()
{
	try {
		test_absent_quarter_and_alias_coherency();
		test_sibling_churn_keeps_live_quarter_stable();
		std::puts("native 4K shadow MMU tests passed");
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "native MMU test failed: %s\n", error.what());
		return EXIT_FAILURE;
	}
}
