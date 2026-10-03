// SPDX-License-Identifier: GPL-2.0-or-later
#include "mach_vm_region_test_support.hpp"
#include "memory/shared_backing.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <csignal>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string_view>
#include <utility>
#include <unistd.h>

namespace {

using uml::testing::require;
using uml::testing::ScratchBacking;

/* Each test sizes its backing file itself. */
constexpr std::string_view backing_name = "uml-shared-backing-XXXXXX";

[[nodiscard]] bool is_unmapped(void *address)
{
	const auto requested = reinterpret_cast<std::uintptr_t>(address);
	const auto query = uml::testing::query_vm_region(requested);
	return query.status == KERN_INVALID_ADDRESS ||
		(query.status == KERN_SUCCESS && query.region.start > requested);
}

void test_ordinary_descriptor_view_owns_its_mapping(std::size_t page_size)
{
	const ScratchBacking file{backing_name, 0};
	require(::ftruncate(file.descriptor(),
		static_cast<off_t>(page_size * 2)) == 0);
	void *released_address = nullptr;
	{
		auto view = uml::memory::acquire_shared_backing_view(
			file.descriptor(), page_size, page_size,
			PROT_READ | PROT_WRITE);
		require(static_cast<bool>(view));
		require(view.size() == page_size);
		released_address = view.data();
		auto *bytes = static_cast<std::uint8_t *>(view.data());
		bytes[0] = 0x5a;
		std::uint8_t stored{};
		require(::pread(file.descriptor(), &stored, sizeof(stored),
			static_cast<off_t>(page_size)) ==
			static_cast<ssize_t>(sizeof(stored)));
		require(stored == 0x5a);
	}
	require(is_unmapped(released_address));
}

[[nodiscard]] vm_prot_t protection_of(void *address)
{
	const auto region = uml::testing::region_containing(address);
	require(region.has_value());
	return region->protection;
}

void *test_anonymous_consumers_borrow_one_root(std::size_t page_size,
	const ScratchBacking &token)
{
	require(::ftruncate(token.descriptor(),
		static_cast<off_t>(page_size * 4)) == 0);
	require(uml::memory::register_anonymous_shared_backing(token.descriptor()));
	require(uml::memory::is_anonymous_shared_backing(token.descriptor()));

	const auto backing_size = page_size * 4;
	struct stat identity {};
	require(::fstat(token.descriptor(), &identity) == 0);
	const auto device = static_cast<std::uint64_t>(identity.st_dev);
	const auto inode = static_cast<std::uint64_t>(identity.st_ino);
	const auto unrooted =
		uml::memory::anonymous_shared_backing_memory(device, inode);
	require(unrooted.anonymous && unrooted.memory.empty());
	require(!uml::memory::anonymous_shared_backing_memory(
		device, inode + 1).anonymous);
	errno = 0;
	auto premature = uml::memory::acquire_shared_backing_view(
		token.descriptor(), page_size, page_size, PROT_READ | PROT_WRITE);
	require(!premature);
	require(errno == EAGAIN);

	void *root_address = ::mmap(nullptr, backing_size, PROT_NONE,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	require(root_address != MAP_FAILED);
	auto *reserved_tail =
		static_cast<std::uint8_t *>(root_address) + page_size * 2;
	require(::munmap(reserved_tail, page_size) == 0);
	require(is_unmapped(reserved_tail));
	auto *anonymous_address =
		static_cast<std::uint8_t *>(root_address) + page_size * 3;
	void *anchored = uml::memory::map_shared_backing(anonymous_address,
		backing_size - page_size * 3, PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_SHARED, token.descriptor(), page_size * 3);
	require(anchored == anonymous_address);
	require(!is_unmapped(reserved_tail));
	void *released_prefix = uml::memory::map_shared_backing(
		static_cast<std::uint8_t *>(root_address) + page_size,
		page_size * 2, PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_SHARED, token.descriptor(), page_size);
	require(released_prefix ==
		static_cast<std::uint8_t *>(root_address) + page_size);
	{
		auto root = uml::memory::acquire_shared_backing_view(
			token.descriptor(), 0, backing_size,
			PROT_READ | PROT_WRITE);
		require(static_cast<bool>(root));
		require(root.data() == root_address);
		const auto rooted =
			uml::memory::anonymous_shared_backing_memory(device, inode);
		require(rooted.anonymous);
		require(rooted.memory.data() ==
			static_cast<std::byte *>(root_address));
		require(rooted.memory.size() == backing_size);
		{
			auto tail = uml::memory::acquire_shared_backing_view(
				token.descriptor(), page_size * 3, page_size,
				PROT_READ | PROT_WRITE);
			require(static_cast<bool>(tail));
			require(tail.data() ==
				static_cast<std::uint8_t *>(root.data()) + page_size * 3);
			static_cast<std::uint8_t *>(tail.data())[0] = 0xa5;
			require(static_cast<std::uint8_t *>(root.data())[page_size * 3] ==
				0xa5);

			auto moved = std::move(tail);
			require(!tail);
			require(moved.data() ==
				static_cast<std::uint8_t *>(root.data()) + page_size * 3);
		}
		char residency{};
		require(::mincore(root_address, page_size, &residency) == 0);
	}

	auto reacquired = uml::memory::acquire_shared_backing_view(
		token.descriptor(), 0, backing_size, PROT_READ | PROT_WRITE);
	require(static_cast<bool>(reacquired));
	require(reacquired.data() == root_address);
	require(static_cast<std::uint8_t *>(reacquired.data())[page_size * 3] ==
		0xa5);

	void *protected_address = nullptr;
	{
		auto protected_view = uml::memory::acquire_shared_backing_view(
			token.descriptor(), page_size * 3, page_size, PROT_READ);
		require(static_cast<bool>(protected_view));
		protected_address = protected_view.data();
		require(protected_address != anonymous_address);
		require(static_cast<const std::uint8_t *>(protected_view.data())[0] ==
			static_cast<const std::uint8_t *>(anonymous_address)[0]);
	}
	require(is_unmapped(protected_address));
	require(!is_unmapped(root_address));

	errno = 0;
	auto empty = uml::memory::acquire_shared_backing_view(
		token.descriptor(), 0, 0, PROT_READ | PROT_WRITE);
	require(!empty);
	require(errno == EINVAL);
	return root_address;
}

/* A backend still draining its queues when the run ends holds a borrowed view
 * of guest RAM; retiring the run's domain must not revoke the page under it. */
void test_borrowed_root_outlives_domain_retirement(std::size_t page_size,
	const ScratchBacking &token, void *root_address)
{
	auto *page = static_cast<std::uint8_t *>(root_address) + page_size * 3;
	require(!uml::memory::retain_borrowed_guest_ram(page, page_size));
	{
		auto consumer = uml::memory::acquire_shared_backing_view(
			token.descriptor(), page_size * 3, page_size,
			PROT_READ | PROT_WRITE);
		require(static_cast<bool>(consumer));
		require(consumer.data() == page);
		page[0] = 0x5a;
		require(uml::memory::retain_borrowed_guest_ram(page, page_size));
		require(uml::memory::retain_borrowed_guest_ram(page, page_size));
		std::uint8_t outside{};
		require(!uml::memory::retain_borrowed_guest_ram(
			&outside, page_size));
		require(!uml::memory::retain_borrowed_guest_ram(
			page + 1, page_size));
		require(protection_of(page) == (VM_PROT_READ | VM_PROT_WRITE));
		require(static_cast<volatile std::uint8_t *>(consumer.data())[0] ==
			0x5a);
	}
	require(protection_of(page) == VM_PROT_NONE);
	require(!is_unmapped(page));

	/* A later mapping over a retained page supersedes its release. */
	void *remapped = uml::memory::map_shared_backing(page, page_size,
		PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, token.descriptor(),
		page_size * 3);
	require(remapped == page);
	{
		auto consumer = uml::memory::acquire_shared_backing_view(
			token.descriptor(), page_size * 3, page_size,
			PROT_READ | PROT_WRITE);
		require(static_cast<bool>(consumer));
		require(uml::memory::retain_borrowed_guest_ram(page, page_size));
		remapped = uml::memory::map_shared_backing(page, page_size,
			PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED,
			token.descriptor(), page_size * 3);
		require(remapped == page);
		page[0] = 0xc3;
	}
	require(protection_of(page) == (VM_PROT_READ | VM_PROT_WRITE));
	require(page[0] == 0xc3);
	require(!uml::memory::retain_borrowed_guest_ram(page, page_size));
}

/* Where the host reserves the whole guest window (iOS), the kernel image is
 * mapped over the reservation and the tail of the physical prefix is still
 * part of it. The first guest RAM mapping must turn that tail into memory in
 * place, and leave the image, including a protectionless alignment gap inside
 * it, as it found it. The anonymous backing registers once per process, so
 * this runs in a child of its own. */
void test_reserved_window_prefix_tail_is_mapped_in_place(std::size_t page_size)
{
	const ScratchBacking token{backing_name, 0};
	constexpr std::size_t pages = 6;
	const auto backing_size = page_size * pages;
	require(::ftruncate(token.descriptor(),
		static_cast<off_t>(backing_size)) == 0);
	require(uml::memory::register_anonymous_shared_backing(token.descriptor()));

	void *window = ::mmap(nullptr, backing_size + page_size * 2, PROT_NONE,
		MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	require(window != MAP_FAILED);
	auto *root = static_cast<std::uint8_t *>(window);
	/* Image: a read-only page, an alignment gap, and a writable last page. */
	require(::mprotect(root, page_size, PROT_READ) == 0);
	require(::mprotect(root + page_size * 2, page_size,
		PROT_READ | PROT_WRITE) == 0);
	root[page_size * 2] = 0x3c;
	auto *tail = root + page_size * 3;
	auto *suffix = root + page_size * 4;
	require(protection_of(tail) == VM_PROT_NONE);

	void *mapped = uml::memory::map_shared_backing(suffix,
		backing_size - page_size * 4, PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_SHARED, token.descriptor(), page_size * 4);
	require(mapped == suffix);
	require(protection_of(tail) == (VM_PROT_READ | VM_PROT_WRITE));
	tail[0] = 0x5a;
	require(protection_of(root) == VM_PROT_READ);
	require(protection_of(root + page_size) == VM_PROT_NONE);
	require(protection_of(root + page_size * 2) ==
		(VM_PROT_READ | VM_PROT_WRITE));
	require(root[page_size * 2] == 0x3c);
	require(protection_of(suffix) == (VM_PROT_READ | VM_PROT_WRITE));
	require(protection_of(root + backing_size) == VM_PROT_NONE);
	require(!is_unmapped(root + backing_size + page_size));
}

void run_in_child(void (*test)(std::size_t), std::size_t page_size)
{
	const pid_t child = ::fork();
	require(child >= 0);
	if (child == 0) {
		test(page_size);
		std::_Exit(EXIT_SUCCESS);
	}
	int status = 0;
	for (int attempt = 0;; ++attempt) {
		const pid_t waited = ::waitpid(child, &status, WNOHANG);
		require(waited >= 0);
		if (waited == child)
			break;
		if (attempt == 3000) {
			(void)::kill(child, SIGKILL);
			(void)::waitpid(child, &status, 0);
			require(false);
		}
		::usleep(10000);
	}
	require(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
}

} // namespace

int main()
{
	const long configured_page_size = ::sysconf(_SC_PAGESIZE);
	require(configured_page_size > 0);
	const auto page_size = static_cast<std::size_t>(configured_page_size);
	run_in_child(test_reserved_window_prefix_tail_is_mapped_in_place,
		page_size);
	test_ordinary_descriptor_view_owns_its_mapping(page_size);
	const ScratchBacking token{backing_name, 0};
	void *root = test_anonymous_consumers_borrow_one_root(page_size, token);
	test_borrowed_root_outlives_domain_retirement(page_size, token, root);
	std::puts("shared backing view tests passed");
	return EXIT_SUCCESS;
}
