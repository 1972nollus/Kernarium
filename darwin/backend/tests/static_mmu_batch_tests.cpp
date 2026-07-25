// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <source_location>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

class TemporaryBacking final {
public:
	explicit TemporaryBacking(std::size_t size)
	{
		char path[] = "/tmp/uml-static-mmu-batch-XXXXXX";
		fd_ = mkstemp(path);
		require(fd_ >= 0);
		require(unlink(path) == 0);
		require(ftruncate(fd_, static_cast<off_t>(size)) == 0);
	}

	TemporaryBacking(const TemporaryBacking &) = delete;
	TemporaryBacking &operator=(const TemporaryBacking &) = delete;

	~TemporaryBacking()
	{
		if (fd_ >= 0)
			close(fd_);
	}

	[[nodiscard]] int fd() const noexcept
	{
		return fd_;
	}

private:
	int fd_{-1};
};

class HeldExecution final {
public:
	HeldExecution(unsigned long generation, unsigned long guest_pc)
	{
		context_ =
			darwin_uml_static_vm_execution_context_create(generation);
		require(context_ != nullptr);
		require(darwin_uml_static_vm_execution_context_begin(
				context_, guest_pc) == 0);
	}

	HeldExecution(const HeldExecution &) = delete;
	HeldExecution &operator=(const HeldExecution &) = delete;

	~HeldExecution()
	{
		if (context_ == nullptr)
			return;
		require(darwin_uml_static_vm_execution_context_end(context_) == 0);
		darwin_uml_static_vm_execution_context_destroy(context_);
	}

	[[nodiscard]] void *context() const noexcept
	{
		return context_;
	}

private:
	void *context_{};
};

void wait_until_started(const std::atomic_bool &started)
{
	started.wait(false, std::memory_order_acquire);
}

void test_additive_batch_does_not_stop_execution()
{
	constexpr unsigned long generation = 201;
	constexpr unsigned long base = 0x4'0000;
	constexpr unsigned long added = base + DARWIN_UML_GUEST_PAGE_SIZE;
	TemporaryBacking backing{2 * DARWIN_UML_GUEST_PAGE_SIZE};

	require(darwin_uml_static_vm_map_generation(
			generation, reinterpret_cast<void *>(base), backing.fd(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) ==
		0);
	{
		HeldExecution execution{generation, base};
		std::atomic_bool started{};
		std::atomic_bool complete{};
		std::thread publisher{[&] {
			require(darwin_uml_static_vm_batch_begin(generation) == 0);
			require(darwin_uml_static_vm_map_generation(
					generation, reinterpret_cast<void *>(added),
					backing.fd(), DARWIN_UML_GUEST_PAGE_SIZE,
					DARWIN_UML_GUEST_PAGE_SIZE,
					PROT_READ | PROT_WRITE, 0, 1) == 0);
			started.store(true, std::memory_order_release);
			started.notify_all();
			require(darwin_uml_static_vm_batch_end() == 0);
			complete.store(true, std::memory_order_release);
			complete.notify_all();
		}};
		wait_until_started(started);
		complete.wait(false, std::memory_order_acquire);
		require(darwin_uml_static_vm_execution_context_sequences(
				execution.context()).execution_interrupt == 0);
		publisher.join();
	}

	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(
			generation, added, DARWIN_UML_STATIC_WRITE, &translated) ==
		DARWIN_UML_STATIC_TRANSLATED);
	require(translated != nullptr);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void test_replacing_batch_still_quiesces_execution()
{
	constexpr unsigned long generation = 202;
	constexpr unsigned long base = 0x5'0000;
	TemporaryBacking backing{2 * DARWIN_UML_GUEST_PAGE_SIZE};
	require(darwin_uml_static_vm_map_generation(
			generation, reinterpret_cast<void *>(base), backing.fd(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) ==
		0);

	void *context =
		darwin_uml_static_vm_execution_context_create(generation);
	require(context != nullptr);
	require(darwin_uml_static_vm_execution_context_begin(context, base) == 0);
	std::atomic_bool started{};
	std::atomic_bool complete{};
	std::thread replacer{[&] {
		require(darwin_uml_static_vm_batch_begin(generation) == 0);
		require(darwin_uml_static_vm_map_generation(
				generation, reinterpret_cast<void *>(base),
				backing.fd(), DARWIN_UML_GUEST_PAGE_SIZE,
				DARWIN_UML_GUEST_PAGE_SIZE,
				PROT_READ | PROT_WRITE, 0, 1) == 0);
		started.store(true, std::memory_order_release);
		started.notify_all();
		require(darwin_uml_static_vm_batch_end() == 0);
		complete.store(true, std::memory_order_release);
		complete.notify_all();
	}};
	wait_until_started(started);
	std::this_thread::sleep_for(20ms);
	require(!complete.load(std::memory_order_acquire));
	require(darwin_uml_static_vm_execution_context_sequences(
			context).execution_interrupt != 0);
	require(darwin_uml_static_vm_execution_context_end(context) == 0);
	darwin_uml_static_vm_execution_context_destroy(context);
	replacer.join();
	require(complete.load(std::memory_order_acquire));
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

} // namespace

int main()
{
	test_additive_batch_does_not_stop_execution();
	test_replacing_batch_still_quiesces_execution();
	std::puts("Static MMU batch tests passed");
	return 0;
}
