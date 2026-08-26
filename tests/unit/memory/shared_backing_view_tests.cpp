// SPDX-License-Identifier: GPL-2.0-or-later
#include "memory/shared_backing.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <source_location>
#include <sys/mman.h>
#include <utility>
#include <unistd.h>

namespace {

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

class TemporaryFile final {
public:
	TemporaryFile()
	{
		char path[] = "/tmp/uml-shared-backing-XXXXXX";
		descriptor_ = ::mkstemp(path);
		require(descriptor_ >= 0);
		require(::unlink(path) == 0);
	}

	TemporaryFile(const TemporaryFile &) = delete;
	TemporaryFile &operator=(const TemporaryFile &) = delete;
	~TemporaryFile()
	{
		if (descriptor_ >= 0)
			(void)::close(descriptor_);
	}

	[[nodiscard]] int get() const noexcept { return descriptor_; }

private:
	int descriptor_{-1};
};

[[nodiscard]] bool is_unmapped(void *address)
{
	mach_vm_address_t region_address =
		reinterpret_cast<mach_vm_address_t>(address);
	const mach_vm_address_t requested_address = region_address;
	mach_vm_size_t region_size{};
	vm_region_basic_info_data_64_t information{};
	mach_msg_type_number_t information_count =
		VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t object_name = MACH_PORT_NULL;
	const kern_return_t result = ::mach_vm_region(
		::mach_task_self(), &region_address, &region_size,
		VM_REGION_BASIC_INFO_64,
		reinterpret_cast<vm_region_info_t>(&information),
		&information_count, &object_name);
	if (object_name != MACH_PORT_NULL)
		(void)::mach_port_deallocate(::mach_task_self(), object_name);
	return result == KERN_INVALID_ADDRESS ||
		(result == KERN_SUCCESS && region_address > requested_address);
}

void test_ordinary_descriptor_view_owns_its_mapping(std::size_t page_size)
{
	TemporaryFile file;
	require(::ftruncate(file.get(), static_cast<off_t>(page_size * 2)) == 0);
	void *released_address = nullptr;
	{
		auto view = uml::memory::acquire_shared_backing_view(
			file.get(), page_size, page_size, PROT_READ | PROT_WRITE);
		require(static_cast<bool>(view));
		require(view.size() == page_size);
		released_address = view.data();
		auto *bytes = static_cast<std::uint8_t *>(view.data());
		bytes[0] = 0x5a;
		std::uint8_t stored{};
		require(::pread(file.get(), &stored, sizeof(stored),
			static_cast<off_t>(page_size)) ==
			static_cast<ssize_t>(sizeof(stored)));
		require(stored == 0x5a);
	}
	require(is_unmapped(released_address));
}

void test_anonymous_consumers_borrow_one_root(std::size_t page_size)
{
	TemporaryFile token;
	require(::ftruncate(token.get(), static_cast<off_t>(page_size * 3)) == 0);
	require(uml::memory::register_anonymous_shared_backing(token.get()));
	require(uml::memory::is_anonymous_shared_backing(token.get()));

	const auto backing_size = page_size * 3;
	errno = 0;
	auto premature = uml::memory::acquire_shared_backing_view(
		token.get(), page_size, page_size, PROT_READ | PROT_WRITE);
	require(!premature);
	require(errno == EAGAIN);

	void *root_address = ::mmap(nullptr, backing_size, PROT_NONE,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	require(root_address != MAP_FAILED);
	auto *anonymous_address =
		static_cast<std::uint8_t *>(root_address) + page_size;
	void *anchored = uml::memory::map_shared_backing(anonymous_address,
		backing_size - page_size, PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_SHARED, token.get(), page_size);
	require(anchored == anonymous_address);
	{
		auto root = uml::memory::acquire_shared_backing_view(
			token.get(), 0, backing_size, PROT_READ | PROT_WRITE);
		require(static_cast<bool>(root));
		require(root.data() == root_address);
		{
			auto tail = uml::memory::acquire_shared_backing_view(
				token.get(), page_size * 2, page_size,
				PROT_READ | PROT_WRITE);
			require(static_cast<bool>(tail));
			require(tail.data() ==
				static_cast<std::uint8_t *>(root.data()) + page_size * 2);
			static_cast<std::uint8_t *>(tail.data())[0] = 0xa5;
			require(static_cast<std::uint8_t *>(root.data())[page_size * 2] ==
				0xa5);

			auto moved = std::move(tail);
			require(!tail);
			require(moved.data() ==
				static_cast<std::uint8_t *>(root.data()) + page_size * 2);
		}
		char residency{};
		require(::mincore(root_address, page_size, &residency) == 0);
	}

	auto reacquired = uml::memory::acquire_shared_backing_view(
		token.get(), 0, backing_size, PROT_READ | PROT_WRITE);
	require(static_cast<bool>(reacquired));
	require(reacquired.data() == root_address);
	require(static_cast<std::uint8_t *>(reacquired.data())[page_size * 2] ==
		0xa5);

	void *protected_address = nullptr;
	{
		auto protected_view = uml::memory::acquire_shared_backing_view(
			token.get(), page_size, page_size, PROT_READ);
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
		token.get(), 0, 0, PROT_READ | PROT_WRITE);
	require(!empty);
	require(errno == EINVAL);
}

} // namespace

int main()
{
	const long configured_page_size = ::sysconf(_SC_PAGESIZE);
	require(configured_page_size > 0);
	const auto page_size = static_cast<std::size_t>(configured_page_size);
	test_ordinary_descriptor_view_owns_its_mapping(page_size);
	test_anonymous_consumers_borrow_one_root(page_size);
	std::puts("shared backing view tests passed");
	return EXIT_SUCCESS;
}
