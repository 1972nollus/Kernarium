// SPDX-License-Identifier: GPL-2.0-or-later
#include "shared_backing.hpp"

#include <cerrno>
#include <cstdio>
#include <limits>
#include <mach/mach.h>
#include <mutex>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace uml::memory {
namespace {

struct AnonymousSharedBacking final {
	std::mutex mutex;
	dev_t device{};
	ino_t inode{};
	void *root{};
	std::size_t size{};
	bool registered{};
};

AnonymousSharedBacking anonymous_backing;

[[nodiscard]] bool same_backing(const struct stat &status) noexcept
{
	return anonymous_backing.registered &&
		status.st_dev == anonymous_backing.device &&
		status.st_ino == anonymous_backing.inode;
}

[[nodiscard]] bool validate_anonymous_range_locked(
	const struct stat &status, std::uint64_t offset,
	std::size_t length) noexcept
{
	if (!same_backing(status)) {
		errno = EBADF;
		return false;
	}
	if (status.st_size <= 0 ||
	    static_cast<std::uintmax_t>(status.st_size) > SIZE_MAX ||
	    length == 0 || offset % vm_page_size != 0 ||
	    length % vm_page_size != 0) {
		errno = EINVAL;
		return false;
	}
	const auto backing_size = static_cast<std::size_t>(status.st_size);
	if (offset > backing_size || length > backing_size - offset) {
		errno = EINVAL;
		return false;
	}
	if (anonymous_backing.root != nullptr &&
	    anonymous_backing.size != backing_size) {
		errno = EINVAL;
		return false;
	}
	return true;
}

[[nodiscard]] void *establish_anonymous_root_locked(
	void *address, std::size_t length, std::uint64_t offset,
	const struct stat &status) noexcept
{
	if (!validate_anonymous_range_locked(status, offset, length))
		return MAP_FAILED;
	const auto backing_size = static_cast<std::size_t>(status.st_size);
	const auto target = reinterpret_cast<std::uintptr_t>(address);
	if (address == nullptr || offset > target ||
	    length != backing_size - static_cast<std::size_t>(offset)) {
		errno = EINVAL;
		return MAP_FAILED;
	}

	/* UML's physical window begins with the signed kernel image. Only the
	 * allocatable suffix is created from the physical-memory descriptor. Make
	 * that first fixed suffix mapping the anonymous allocation itself and use
	 * address - offset as the linear physical base. A separate anywhere
	 * allocation would leave iOS accounting both full-span resident mappings. */
	void *root = mmap(address, length, PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
	if (root == MAP_FAILED)
		return MAP_FAILED;
#if UML_WIRE_ANONYMOUS_GUEST_RAM
	struct rlimit lock_limit {};
	if (getrlimit(RLIMIT_MEMLOCK, &lock_limit) == 0) {
		dprintf(STDERR_FILENO,
			"Darwin anonymous guest RAM: wiring %zu bytes "
			"(RLIMIT_MEMLOCK soft=%llu hard=%llu)\n",
			length,
			static_cast<unsigned long long>(lock_limit.rlim_cur),
			static_cast<unsigned long long>(lock_limit.rlim_max));
	} else {
		dprintf(STDERR_FILENO,
			"Darwin anonymous guest RAM: wiring %zu bytes "
			"(RLIMIT_MEMLOCK unavailable, errno=%d)\n",
			length, errno);
	}
	if (mlock(root, length) != 0) {
		const int saved_errno = errno;
		dprintf(STDERR_FILENO,
			"Darwin anonymous guest RAM: failed to wire %zu "
			"bytes, errno=%d; refusing lazy guest RAM\n",
			length, saved_errno);
		(void)munmap(root, length);
		errno = saved_errno;
		return MAP_FAILED;
	}
	dprintf(STDERR_FILENO,
		"Darwin anonymous guest RAM: committed and wired %zu "
		"allocatable bytes in the %zu-byte physical window; host "
		"compression is disabled; direct-root=yes offset=%llu\n",
		length, backing_size,
		static_cast<unsigned long long>(offset));
#else
	dprintf(STDERR_FILENO,
		"Darwin anonymous guest RAM: reserved %zu allocatable bytes "
		"in the %zu-byte physical window without file backing; "
		"direct-root=yes offset=%llu\n",
		length, backing_size,
		static_cast<unsigned long long>(offset));
#endif
	anonymous_backing.root = reinterpret_cast<void *>(
		reinterpret_cast<std::uintptr_t>(root) -
		static_cast<std::size_t>(offset));
	anonymous_backing.size = backing_size;
	return root;
}

[[nodiscard]] void *map_anonymous_alias(void *address, std::size_t length,
	int protection, int flags, std::uint64_t offset,
	const struct stat &status) noexcept
{
	std::lock_guard lock{anonymous_backing.mutex};
	const bool fixed = (flags & MAP_FIXED) != 0;
	if (!validate_anonymous_range_locked(status, offset, length))
		return MAP_FAILED;
	if (anonymous_backing.root == nullptr) {
		if (!fixed || protection != (PROT_READ | PROT_WRITE)) {
			errno = EAGAIN;
			return MAP_FAILED;
		}
		return establish_anonymous_root_locked(
			address, length, offset, status);
	}

	vm_address_t target = fixed ? reinterpret_cast<vm_address_t>(address) : 0;
	const vm_address_t source =
		reinterpret_cast<vm_address_t>(anonymous_backing.root) + offset;
	if (fixed && target == source) {
		if (mprotect(address, length, protection) != 0)
			return MAP_FAILED;
		return address;
	}
	vm_prot_t current = VM_PROT_NONE;
	vm_prot_t maximum = VM_PROT_NONE;
	const int mach_flags = fixed ? VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE :
		VM_FLAGS_ANYWHERE;
	const kern_return_t result = vm_remap(
		mach_task_self(), &target, length, 0, mach_flags,
		mach_task_self(), source, FALSE, &current, &maximum,
		VM_INHERIT_NONE);
	if (result != KERN_SUCCESS ||
	    (fixed && target != reinterpret_cast<vm_address_t>(address))) {
		if (result == KERN_SUCCESS)
			(void)munmap(reinterpret_cast<void *>(target), length);
		errno = result == KERN_NO_SPACE ? ENOMEM : EFAULT;
		return MAP_FAILED;
	}
	void *mapped = reinterpret_cast<void *>(target);
	if (mprotect(mapped, length, protection) != 0) {
		const int saved_errno = errno;
		(void)munmap(mapped, length);
		errno = saved_errno;
		return MAP_FAILED;
	}
	return mapped;
}

} // namespace

SharedBackingView::SharedBackingView(
	void *data, std::size_t size, bool owns_mapping) noexcept
	: data_(data), size_(size), owns_mapping_(owns_mapping)
{
}

SharedBackingView::SharedBackingView(SharedBackingView &&other) noexcept
	: data_(other.data_), size_(other.size_),
	  owns_mapping_(other.owns_mapping_)
{
	other.data_ = nullptr;
	other.size_ = 0;
	other.owns_mapping_ = false;
}

SharedBackingView &SharedBackingView::operator=(
	SharedBackingView &&other) noexcept
{
	if (this == &other)
		return *this;
	reset();
	data_ = other.data_;
	size_ = other.size_;
	owns_mapping_ = other.owns_mapping_;
	other.data_ = nullptr;
	other.size_ = 0;
	other.owns_mapping_ = false;
	return *this;
}

SharedBackingView::~SharedBackingView()
{
	reset();
}

void SharedBackingView::reset() noexcept
{
	if (owns_mapping_ && data_ != nullptr)
		(void)munmap(data_, size_);
	data_ = nullptr;
	size_ = 0;
	owns_mapping_ = false;
}

bool register_anonymous_shared_backing(int fd) noexcept
{
	struct stat status {};
	if (fd < 0 || fstat(fd, &status) != 0)
		return false;
	std::lock_guard lock{anonymous_backing.mutex};
	if (anonymous_backing.registered) {
		errno = EBUSY;
		return false;
	}
	anonymous_backing.device = status.st_dev;
	anonymous_backing.inode = status.st_ino;
	anonymous_backing.registered = true;
	dprintf(STDERR_FILENO,
		"Darwin anonymous guest RAM: registered sparse descriptor token "
		"fd=%d\n",
		fd);
	return true;
}

bool is_anonymous_shared_backing(int fd) noexcept
{
	struct stat status {};
	if (fd < 0 || fstat(fd, &status) != 0)
		return false;
	std::lock_guard lock{anonymous_backing.mutex};
	return same_backing(status);
}

SharedBackingView acquire_shared_backing_view(int fd,
	std::uint64_t offset, std::size_t length, int protection) noexcept
{
	struct stat status {};
	if (fd < 0 || fstat(fd, &status) != 0)
		return {};
	{
		std::lock_guard lock{anonymous_backing.mutex};
		if (same_backing(status) &&
		    protection == (PROT_READ | PROT_WRITE)) {
			if (!validate_anonymous_range_locked(status, offset, length))
				return {};
			if (anonymous_backing.root == nullptr) {
				errno = EAGAIN;
				return {};
			}
			auto *data = static_cast<std::byte *>(
				anonymous_backing.root) + offset;
			return SharedBackingView{data, length, false};
		}
	}

	void *mapping = map_shared_backing(nullptr, length, protection,
		MAP_SHARED, fd, offset);
	if (mapping == MAP_FAILED)
		return {};
	return SharedBackingView{mapping, length, true};
}

void *map_shared_backing(void *address, std::size_t length,
	int protection, int flags, int fd, std::uint64_t offset) noexcept
{
	struct stat status {};
	if (fstat(fd, &status) != 0)
		return MAP_FAILED;
	{
		std::lock_guard lock{anonymous_backing.mutex};
		if (!same_backing(status)) {
			if (offset > static_cast<std::uint64_t>(
					std::numeric_limits<off_t>::max())) {
				errno = EOVERFLOW;
				return MAP_FAILED;
			}
			return mmap(address, length, protection, flags, fd,
				static_cast<off_t>(offset));
		}
	}
	return map_anonymous_alias(
		address, length, protection, flags, offset, status);
}

} // namespace uml::memory
