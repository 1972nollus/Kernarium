// SPDX-License-Identifier: GPL-2.0-or-later
#include "shared_backing.hpp"

#include <atomic>
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

/* Mach reports an exhausted host with KERN_RESOURCE_SHORTAGE and an exhausted
 * address space with KERN_NO_SPACE. Both are the host running out, and the
 * kernel distinguishes that case by ENOMEM alone: it is what makes it say so
 * rather than treat the failure as a bad address it can only retry.
 */
int mach_failure_errno(int result) noexcept
{
	switch (result) {
	case KERN_NO_SPACE:
	case KERN_RESOURCE_SHORTAGE:
		return ENOMEM;
	default:
		return EFAULT;
	}
}

namespace {
std::atomic<unsigned> mapping_failure_reports{0};
} // namespace

void report_mapping_failure(const char *what, const void *address,
	std::size_t length, int error) noexcept
{
	constexpr unsigned reported_limit = 16;
	const unsigned seen =
		mapping_failure_reports.fetch_add(1, std::memory_order_relaxed);
	if (seen >= reported_limit)
		return;
	dprintf(STDERR_FILENO,
		"Darwin guest memory: %s failed for %zu bytes at %p, errno=%d%s\n",
		what, length, address, error,
		seen + 1 == reported_limit ? "; further failures are not reported"
					   : "");
}

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

struct AnonymousTail final {
	void *address{};
	std::size_t length{};
};

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

[[nodiscard]] bool materialize_anonymous_tail_locked(
	std::uintptr_t root, std::uintptr_t limit,
	AnonymousTail &tail) noexcept
{
	vm_address_t cursor = root;
	while (cursor < limit) {
		vm_address_t region = cursor;
		vm_size_t region_size = 0;
		vm_region_basic_info_data_64_t information {};
		mach_msg_type_number_t information_count =
			VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t object = MACH_PORT_NULL;
		const kern_return_t result = vm_region_64(
			mach_task_self(), &region, &region_size,
			VM_REGION_BASIC_INFO_64,
			reinterpret_cast<vm_region_info_t>(&information),
			&information_count, &object);
		if (object != MACH_PORT_NULL)
			(void)mach_port_deallocate(mach_task_self(), object);
		if (result == KERN_INVALID_ADDRESS || region >= limit)
			break;
		if (result != KERN_SUCCESS || region > cursor || region_size == 0 ||
		    region > std::numeric_limits<vm_address_t>::max() - region_size ||
		    region + region_size <= cursor) {
			errno = mach_failure_errno(result);
			report_mapping_failure("guest RAM tail inspection",
				reinterpret_cast<const void *>(cursor),
				limit - cursor, errno);
			return false;
		}
		cursor = region + region_size;
	}
	if (cursor >= limit)
		return true;

	/* The signed loader reserves through the final ELF segment. UML rounds
	 * its reserved physical prefix farther, leaving one unmapped tail before
	 * the allocatable suffix. Materialize only that verified hole, without
	 * replacing any signed or anonymous kernel segment. */
	const auto length = static_cast<std::size_t>(limit - cursor);
	void *mapping = mmap(reinterpret_cast<void *>(cursor), length,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (mapping == MAP_FAILED)
		return false;
	if (mapping != reinterpret_cast<void *>(cursor)) {
		(void)munmap(mapping, length);
		errno = ENOMEM;
		return false;
	}
	tail = {mapping, length};
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
	if (address == nullptr || target % vm_page_size != 0 || offset > target ||
	    length != backing_size - static_cast<std::size_t>(offset)) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	const auto physical_root = target - static_cast<std::size_t>(offset);
	AnonymousTail tail;
	if (!materialize_anonymous_tail_locked(physical_root, target, tail))
		return MAP_FAILED;

	/* UML's physical window begins with the signed kernel image. Only the
	 * allocatable suffix is created from the physical-memory descriptor. Make
	 * that first fixed suffix mapping the anonymous allocation itself and use
	 * address - offset as the linear physical base. A separate anywhere
	 * allocation would leave iOS accounting both full-span resident mappings. */
	void *root = mmap(address, length, PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
	if (root == MAP_FAILED) {
		if (tail.address != nullptr)
			(void)reserve_host_range(tail.address, tail.length);
		return MAP_FAILED;
	}
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
	if ((tail.address != nullptr && mlock(tail.address, tail.length) != 0) ||
	    mlock(root, length) != 0) {
		const int saved_errno = errno;
		dprintf(STDERR_FILENO,
			"Darwin anonymous guest RAM: failed to wire the direct "
			"physical window, errno=%d; refusing lazy guest RAM\n",
			saved_errno);
		(void)reserve_host_range(root, length);
		if (tail.address != nullptr)
			(void)reserve_host_range(tail.address, tail.length);
		errno = saved_errno;
		return MAP_FAILED;
	}
	dprintf(STDERR_FILENO,
		"Darwin anonymous guest RAM: committed and wired %zu suffix "
		"bytes plus %zu reserved-tail bytes in the %zu-byte physical "
		"window; host compression is disabled; direct-root=yes "
		"offset=%llu\n",
		length, tail.length, backing_size,
		static_cast<unsigned long long>(offset));
#else
	dprintf(STDERR_FILENO,
		"Darwin anonymous guest RAM: reserved %zu suffix bytes plus "
		"%zu reserved-tail bytes in the %zu-byte physical window "
		"without file backing; direct-root=yes offset=%llu\n",
		length, tail.length, backing_size,
		static_cast<unsigned long long>(offset));
#endif
	anonymous_backing.root = reinterpret_cast<void *>(physical_root);
	anonymous_backing.size = backing_size;
	return root;
}

/* Undo a half-built alias. Every MAP_FIXED caller of this file maps inside the
 * reserved guest window, so a fixed range is handed back to the window's
 * reservation rather than to the process; an anywhere alias is this call's own
 * allocation and is released outright. */
int release_alias(void *address, std::size_t length, bool fixed) noexcept
{
	return fixed ? reserve_host_range(address, length) :
		munmap(address, length);
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
		if (mprotect(address, length, protection) != 0) {
			report_mapping_failure("guest RAM reprotect", address,
				length, errno);
			return MAP_FAILED;
		}
#if UML_WIRE_ANONYMOUS_GUEST_RAM
		/* Early UML releases its final reserved-prefix pages into guest RAM
		 * with an in-place writable mapping. Wire them at that transition so
		 * no newly allocatable physical page can enter host compression.
		 *
		 * The mapping itself is already in place and correct by this point,
		 * so a refused wiring is reported and kept rather than failed: the
		 * caller would answer a failure by dropping its record of a mapping
		 * the host still has, and a host mapping nothing owns outlives every
		 * page table that named it. Compression costs latency; a mapping the
		 * kernel has forgotten costs correctness.
		 */
		if (protection != PROT_NONE && mlock(address, length) != 0)
			report_mapping_failure("guest RAM wiring", address,
				length, errno);
#endif
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
			(void)release_alias(
				reinterpret_cast<void *>(target), length, fixed);
		errno = mach_failure_errno(result);
		report_mapping_failure("guest RAM alias", address, length, errno);
		return MAP_FAILED;
	}
	void *mapped = reinterpret_cast<void *>(target);
	if (mprotect(mapped, length, protection) != 0) {
		const int saved_errno = errno;
		(void)release_alias(mapped, length, fixed);
		errno = saved_errno;
		report_mapping_failure("guest RAM alias protect", address, length,
			saved_errno);
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

AnonymousBackingMemory anonymous_shared_backing_memory(
	std::uint64_t device, std::uint64_t inode) noexcept
{
	std::lock_guard lock{anonymous_backing.mutex};
	if (!anonymous_backing.registered ||
	    static_cast<std::uint64_t>(anonymous_backing.device) != device ||
	    static_cast<std::uint64_t>(anonymous_backing.inode) != inode)
		return {};
	if (anonymous_backing.root == nullptr)
		return {true, {}};
	return {true, {static_cast<std::byte *>(anonymous_backing.root),
		anonymous_backing.size}};
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

int reserve_host_range(void *address, std::size_t length) noexcept
{
	if (length == 0)
		return 0;
	void *reserved = mmap(address, length, PROT_NONE,
		MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	if (reserved == MAP_FAILED)
		return -1;
	if (reserved != address) {
		/* MAP_FIXED cannot answer with a different address. Release only
		 * what this call created rather than leave a second mapping
		 * behind, and report the impossible answer as a failure. */
		(void)munmap(reserved, length);
		errno = EFAULT;
		return -1;
	}
	return 0;
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
