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

[[nodiscard]] void *map_anonymous_alias(void *address, std::size_t length,
	int protection, int flags, std::uint64_t offset,
	const struct stat &status) noexcept
{
	std::lock_guard lock{anonymous_backing.mutex};
	if (!same_backing(status)) {
		errno = EBADF;
		return MAP_FAILED;
	}
	if (status.st_size <= 0 ||
	    static_cast<std::uintmax_t>(status.st_size) > SIZE_MAX) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	const auto backing_size = static_cast<std::size_t>(status.st_size);
	if (offset > backing_size || length > backing_size - offset) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	if (anonymous_backing.root == nullptr) {
		/* The sparse descriptor is only an identity token; every consumer
		 * aliases this anonymous root rather than mapping the token. */
		void *root = mmap(nullptr, backing_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANON, -1, 0);
		if (root == MAP_FAILED)
			return MAP_FAILED;
#if UML_WIRE_ANONYMOUS_GUEST_RAM
		struct rlimit lock_limit {};
		if (getrlimit(RLIMIT_MEMLOCK, &lock_limit) == 0) {
			dprintf(STDERR_FILENO,
				"Darwin anonymous guest RAM: wiring %zu bytes "
				"(RLIMIT_MEMLOCK soft=%llu hard=%llu)\n",
				backing_size,
				static_cast<unsigned long long>(lock_limit.rlim_cur),
				static_cast<unsigned long long>(lock_limit.rlim_max));
		} else {
			dprintf(STDERR_FILENO,
				"Darwin anonymous guest RAM: wiring %zu bytes "
				"(RLIMIT_MEMLOCK unavailable, errno=%d)\n",
				backing_size, errno);
		}
		if (mlock(root, backing_size) != 0) {
			const int saved_errno = errno;
			dprintf(STDERR_FILENO,
				"Darwin anonymous guest RAM: failed to wire %zu "
				"bytes, errno=%d; refusing lazy guest RAM\n",
				backing_size, saved_errno);
			(void)munmap(root, backing_size);
			errno = saved_errno;
			return MAP_FAILED;
		}
		dprintf(STDERR_FILENO,
			"Darwin anonymous guest RAM: committed and wired %zu "
			"bytes; host compression is disabled for the root\n",
			backing_size);
#else
		dprintf(STDERR_FILENO,
			"Darwin anonymous guest RAM: reserved %zu bytes without "
			"file backing\n",
			backing_size);
#endif
		anonymous_backing.root = root;
		anonymous_backing.size = backing_size;
	} else if (anonymous_backing.size != backing_size) {
		errno = EINVAL;
		return MAP_FAILED;
	}

	const bool fixed = (flags & MAP_FIXED) != 0;
	vm_address_t target = fixed ? reinterpret_cast<vm_address_t>(address) : 0;
	const vm_address_t source =
		reinterpret_cast<vm_address_t>(anonymous_backing.root) + offset;
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
