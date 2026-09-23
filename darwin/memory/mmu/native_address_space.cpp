// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_address_space.hpp"
#include "allocation_fault.hpp"
#include "domain_registry_testing.hpp"
#include "word_shared_mutex.hpp"
#include "../domain_contract.hpp"
#include "../shared_backing.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <mach/mach.h>
#include <mach/arm/thread_status.h>
#include <mutex>
#include <memory>
#include <new>
#include <optional>
#include <pthread.h>
#include <shared_mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace uml::memory {
namespace {

[[nodiscard]] std::uintptr_t host_page_size() noexcept
{
	static const auto size = static_cast<std::uintptr_t>(getpagesize());
	return size;
}

[[nodiscard]] std::uintptr_t host_slot_for(std::uintptr_t address) noexcept
{
	return address & ~(host_page_size() - 1);
}

#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
constexpr std::uintptr_t guest_page_size = 4096;
constexpr std::uintptr_t kernel_stack_size = 128 * 1024;

[[nodiscard]] std::uintptr_t current_carrier_id() noexcept
{
	return static_cast<std::uintptr_t>(
		pthread_mach_thread_np(pthread_self()));
}

/* Trace switches sit on per-page paths, so each is read once per process. */
[[nodiscard]] bool environment_switch(const char *name) noexcept
{
	const auto *value = std::getenv(name);
	return value != nullptr && value[0] != '\0' && value[0] != '0';
}

[[nodiscard]] bool mmu_trace_enabled() noexcept
{
	static const bool enabled = environment_switch("UML_MMU_TRACE");
	return enabled;
}

[[nodiscard]] bool mmu_conflict_trace_enabled() noexcept
{
	static const bool enabled =
		environment_switch("UML_MMU_CONFLICT_TRACE");
	return enabled;
}

[[nodiscard]] bool mmu_live_stack_trace_enabled() noexcept
{
	static const bool enabled =
		environment_switch("UML_MMU_LIVE_STACK_TRACE");
	return enabled;
}

void mmu_trace_live_stacks(const char *operation, std::uintptr_t address,
	std::uint64_t length) noexcept
{
	if (!mmu_live_stack_trace_enabled() || length == 0 ||
	    address + length < address)
		return;
	thread_act_array_t threads = nullptr;
	mach_msg_type_number_t thread_count = 0;
	if (task_threads(mach_task_self(), &threads, &thread_count) !=
	    KERN_SUCCESS)
		return;
	const auto first = host_slot_for(address);
	const auto end = host_slot_for(address + length - 1) + host_page_size();
	for (mach_msg_type_number_t index = 0; index < thread_count; ++index) {
		arm_thread_state64_t state{};
		mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
		if (thread_get_state(threads[index], ARM_THREAD_STATE64,
			reinterpret_cast<thread_state_t>(&state), &state_count) ==
		    KERN_SUCCESS) {
			const auto stack = static_cast<std::uintptr_t>(
				__darwin_arm_thread_state64_get_sp(state));
			const auto slot = host_slot_for(stack);
			if (slot >= first && slot < end)
				dprintf(STDERR_FILENO,
					"Darwin native live-stack operation=%s "
					"address=%#lx length=%#llx slot=%#lx sp=%#lx "
					"thread=%#x frame=%p\n",
					operation, static_cast<unsigned long>(address),
					static_cast<unsigned long long>(length),
					static_cast<unsigned long>(slot),
					static_cast<unsigned long>(stack), threads[index],
					__builtin_frame_address(0));
		}
		(void)mach_port_deallocate(mach_task_self(), threads[index]);
	}
	(void)vm_deallocate(mach_task_self(),
		reinterpret_cast<vm_address_t>(threads),
		static_cast<vm_size_t>(thread_count * sizeof(*threads)));
}

[[nodiscard]] std::uintptr_t parse_mmu_trace_address() noexcept
{
	const auto *value = std::getenv("UML_MMU_TRACE_DYNAMIC_ADDRESS");
	if (value == nullptr || value[0] == '\0')
		value = std::getenv("UML_MMU_TRACE_ADDRESS");
	if (value == nullptr || value[0] == '\0')
		return 0;
	char *end = nullptr;
	const auto parsed = std::strtoull(value, &end, 0);
	if (end == value || *end != '\0')
		return 0;
	return static_cast<std::uintptr_t>(parsed);
}

[[nodiscard]] std::uintptr_t mmu_trace_address() noexcept
{
	static const auto address = parse_mmu_trace_address();
	return address;
}

[[nodiscard]] std::uintptr_t mmu_trace_page() noexcept
{
	return mmu_trace_address() & ~(guest_page_size - 1);
}

[[nodiscard]] bool mmu_trace_target(std::uintptr_t page) noexcept
{
	if (!mmu_trace_enabled())
		return false;
	const auto target = mmu_trace_page();
	return target != 0 && host_slot_for(page) == host_slot_for(target);
}

[[nodiscard]] std::uint64_t mmu_trace_word(
	const std::byte *page, std::size_t offset) noexcept
{
	/* The three trace sites historically bracketed one fixed diagnostic word.
	 * Retarget that bracket to the exact address supplied for the current run
	 * without duplicating address parsing across every log site. */
	const auto address = mmu_trace_address();
	const auto target_offset = address == 0 ? std::size_t{0x278} :
		static_cast<std::size_t>(address & (guest_page_size - 1)) &
			~(sizeof(std::uint64_t) - 1);
	if (offset >= 0x270 && offset <= 0x280) {
		const auto relative = static_cast<std::ptrdiff_t>(offset) - 0x278;
		const auto adjusted = static_cast<std::ptrdiff_t>(target_offset) +
			relative;
		if (adjusted >= 0 &&
		    adjusted + static_cast<std::ptrdiff_t>(sizeof(std::uint64_t)) <=
			    static_cast<std::ptrdiff_t>(guest_page_size))
			offset = static_cast<std::size_t>(adjusted);
	}
	std::uint64_t value{};
	std::memcpy(&value, page + offset, sizeof(value));
	return value;
}

void snapshot_shadow_page(std::uintptr_t page,
	std::array<std::byte, guest_page_size> &snapshot) noexcept
{
	static_assert(guest_page_size % sizeof(std::uint64_t) == 0);
	const auto *source = reinterpret_cast<const std::uint64_t *>(page);
	for (std::size_t position = 0; position < guest_page_size;
	     position += sizeof(std::uint64_t)) {
		const auto value = __atomic_load_n(
			source + position / sizeof(std::uint64_t), __ATOMIC_RELAXED);
		std::memcpy(snapshot.data() + position, &value, sizeof(value));
	}
}

void merge_shadow_page(std::uintptr_t page,
	const std::array<std::byte, guest_page_size> &expected,
	const std::array<std::byte, guest_page_size> &replacement,
	const char *operation) noexcept
{
	static_assert(guest_page_size % sizeof(std::uint64_t) == 0);
	auto *destination = reinterpret_cast<std::uint64_t *>(page);
	for (std::size_t position = 0; position < guest_page_size;
	     position += sizeof(std::uint64_t)) {
		std::uint64_t expected_word{};
		std::uint64_t replacement_word{};
		std::memcpy(&expected_word, expected.data() + position,
			sizeof(expected_word));
		std::memcpy(&replacement_word, replacement.data() + position,
			sizeof(replacement_word));
		auto observed = __atomic_load_n(
			destination + position / sizeof(std::uint64_t),
			__ATOMIC_RELAXED);
		bool conflict_logged = false;
		for (;;) {
			if (!conflict_logged && mmu_conflict_trace_enabled() &&
			    observed != expected_word &&
			    replacement_word != expected_word &&
			    observed != replacement_word) {
				dprintf(STDERR_FILENO,
					"Darwin native word conflict operation=%s "
					"page=%#lx offset=%#zx expected=%#llx "
					"replacement=%#llx observed=%#llx\n",
					operation, static_cast<unsigned long>(page),
					position,
					static_cast<unsigned long long>(expected_word),
					static_cast<unsigned long long>(replacement_word),
					static_cast<unsigned long long>(observed));
				conflict_logged = true;
			}
			std::array<std::byte, sizeof(observed)> bytes{};
			std::memcpy(bytes.data(), &observed, sizeof(observed));
			for (std::size_t byte = 0; byte < bytes.size(); ++byte)
				if (bytes[byte] == expected[position + byte])
					bytes[byte] = replacement[position + byte];
			std::uint64_t desired{};
			std::memcpy(&desired, bytes.data(), sizeof(desired));
			if (desired == observed || __atomic_compare_exchange_n(
				destination + position / sizeof(observed), &observed,
				desired, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
				break;
		}
	}
}

[[nodiscard]] std::uintptr_t slot_for(std::uintptr_t address) noexcept
{
	return host_slot_for(address);
}

[[nodiscard]] int shadow_data_protection(int protection) noexcept
{
	/* kern_map() currently supplies execute for every native kernel alias,
	 * even though these private 4 KiB shadows only access physical backing.
	 * Keep the entire shared MMU path data-only.  Genuine native kernel text
	 * is protected directly by the host-page-granular ELF loader, while JIT
	 * code uses its separate publication allocator.
	 */
	return protection & ~PROT_EXEC;
}

[[nodiscard]] int direct_data_protection(int protection) noexcept
{
	if ((protection & (PROT_WRITE | PROT_EXEC)) ==
	    (PROT_WRITE | PROT_EXEC))
		protection &= ~PROT_EXEC;
	return protection;
}

int read_exact(int fd, std::uint64_t offset, std::byte *destination,
	       std::size_t length)
{
	if (length == 0)
		return 0;
	if (is_anonymous_shared_backing(fd)) {
		struct stat status {};
		if (fstat(fd, &status) != 0)
			return -1;
		if (status.st_size <= 0 ||
		    static_cast<std::uintmax_t>(status.st_size) > SIZE_MAX) {
			errno = EINVAL;
			return -1;
		}
		const auto backing_size = static_cast<std::size_t>(status.st_size);
		if (offset > backing_size || length > backing_size - offset) {
			errno = EINVAL;
			return -1;
		}
		auto view = acquire_shared_backing_view(fd, 0, backing_size,
			PROT_READ | PROT_WRITE);
		if (!view)
			return -1;
		const auto *source = static_cast<const std::byte *>(view.data()) +
			static_cast<std::size_t>(offset);
		std::size_t completed = 0;
		while (completed < length &&
		       (reinterpret_cast<std::uintptr_t>(source + completed) &
			(sizeof(std::uint64_t) - 1)) != 0) {
			const auto value = __atomic_load_n(
				reinterpret_cast<const unsigned char *>(source + completed),
				__ATOMIC_RELAXED);
			destination[completed++] = static_cast<std::byte>(value);
		}
		while (length - completed >= sizeof(std::uint64_t)) {
			const auto value = __atomic_load_n(
				reinterpret_cast<const std::uint64_t *>(source + completed),
				__ATOMIC_RELAXED);
			std::memcpy(destination + completed, &value, sizeof(value));
			completed += sizeof(value);
		}
		while (completed < length) {
			const auto value = __atomic_load_n(
				reinterpret_cast<const unsigned char *>(source + completed),
				__ATOMIC_RELAXED);
			destination[completed++] = static_cast<std::byte>(value);
		}
		return 0;
	}
	std::size_t completed = 0;
	while (completed < length) {
		const auto result = pread(fd, destination + completed,
			length - completed, static_cast<off_t>(offset + completed));
		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0) {
			if (result == 0)
				errno = EIO;
			return -1;
		}
		completed += static_cast<std::size_t>(result);
	}
	return 0;
}

int write_exact(int fd, std::uint64_t offset, const std::byte *source,
		std::size_t length)
{
	if (length == 0)
		return 0;
	if (is_anonymous_shared_backing(fd)) {
		struct stat status {};
		if (fstat(fd, &status) != 0)
			return -1;
		if (status.st_size <= 0 ||
		    static_cast<std::uintmax_t>(status.st_size) > SIZE_MAX) {
			errno = EINVAL;
			return -1;
		}
		const auto backing_size = static_cast<std::size_t>(status.st_size);
		if (offset > backing_size || length > backing_size - offset) {
			errno = EINVAL;
			return -1;
		}
		auto view = acquire_shared_backing_view(fd, 0, backing_size,
			PROT_READ | PROT_WRITE);
		if (!view)
			return -1;
		auto *destination = static_cast<std::byte *>(view.data()) +
			static_cast<std::size_t>(offset);
		std::size_t completed = 0;
		while (completed < length &&
		       (reinterpret_cast<std::uintptr_t>(destination + completed) &
			(sizeof(std::uint64_t) - 1)) != 0) {
			__atomic_store_n(
				reinterpret_cast<unsigned char *>(destination + completed),
				std::to_integer<unsigned char>(source[completed]),
				__ATOMIC_RELAXED);
			++completed;
		}
		while (length - completed >= sizeof(std::uint64_t)) {
			std::uint64_t value{};
			std::memcpy(&value, source + completed, sizeof(value));
			__atomic_store_n(
				reinterpret_cast<std::uint64_t *>(destination + completed),
				value, __ATOMIC_RELAXED);
			completed += sizeof(value);
		}
		while (completed < length) {
			__atomic_store_n(
				reinterpret_cast<unsigned char *>(destination + completed),
				std::to_integer<unsigned char>(source[completed]),
				__ATOMIC_RELAXED);
			++completed;
		}
		return 0;
	}
	std::size_t completed = 0;
	while (completed < length) {
		const auto result = pwrite(fd, source + completed,
			length - completed, static_cast<off_t>(offset + completed));
		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0) {
			if (result == 0)
				errno = EIO;
			return -1;
		}
		completed += static_cast<std::size_t>(result);
	}
	return 0;
}
#endif

#if DARWIN_UML_GUEST_PAGE_SIZE != 4096
void mmu_trace_live_stacks(const char *, std::uintptr_t,
	std::uint64_t) noexcept
{
}
#endif

[[nodiscard]] bool covered_host_range(std::uintptr_t address,
	std::uint64_t length, std::uintptr_t &first,
	std::uintptr_t &end) noexcept
{
	if (length == 0 || address + length < address)
		return false;
	const auto mask = host_page_size() - 1;
	first = address & ~mask;
	const auto last = address + length;
	if (last > std::numeric_limits<std::uintptr_t>::max() - mask)
		return false;
	end = (last + mask) & ~mask;
	return end > first;
}

[[nodiscard]] bool exact_host_range(std::uintptr_t address,
	std::uint64_t length, std::uintptr_t &first,
	std::uintptr_t &end) noexcept
{
	return covered_host_range(address, length, first, end) &&
		first == address && end == address + length;
}

#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
enum class OwnershipKind : std::uint8_t {
	direct,
	shadow,
};
#endif

struct Ownership final {
	std::uint64_t owner;
#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	OwnershipKind kind;
#endif
};

[[nodiscard]] constexpr Ownership direct_ownership(
	std::uint64_t owner) noexcept
{
#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	return {owner, OwnershipKind::direct};
#else
	return {owner};
#endif
}

[[nodiscard]] constexpr bool is_direct_ownership(
	const Ownership &ownership, std::uint64_t owner) noexcept
{
#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	return ownership.owner == owner && ownership.kind == OwnershipKind::direct;
#else
	return ownership.owner == owner;
#endif
}

#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
[[nodiscard]] constexpr Ownership shadow_ownership(
	std::uint64_t owner) noexcept
{
	return {owner, OwnershipKind::shadow};
}

[[nodiscard]] constexpr bool is_shadow_ownership(
	const Ownership &ownership, std::uint64_t owner) noexcept
{
	return ownership.owner == owner && ownership.kind == OwnershipKind::shadow;
}
#endif

class HostOwnershipRegistry final {
public:
	enum class DirectRetryStatus : std::uint8_t {
		absent,
		denied,
		permitted,
	};

	int map_direct(std::uint64_t owner, std::uintptr_t address, int fd,
		std::uint64_t offset, std::uint64_t length, int protection)
	{
		std::uintptr_t first;
		std::uintptr_t end;
		if (!exact_host_range(address, length, first, end) ||
		    (offset & (host_page_size() - 1)) != 0 ||
		    offset > static_cast<std::uint64_t>(
				std::numeric_limits<off_t>::max())) {
			errno = EINVAL;
			return -1;
		}

		std::lock_guard lock{mutex_};
		std::vector<std::uintptr_t> inserted;
		std::size_t new_page_count = 0;
		for (auto page = first; page < end; page += host_page_size()) {
			if (const auto found = owners_.find(page);
			    found != owners_.end()) {
				if (!is_direct_ownership(found->second, owner)) {
					errno = EBUSY;
					return -1;
				}
				continue;
			}
			++new_page_count;
		}
		inserted.reserve(new_page_count);
		for (auto page = first; page < end; page += host_page_size())
			if (!owners_.contains(page))
				inserted.push_back(page);
		if (!inserted.empty())
			reserve_additional(owners_, inserted.size());
		try {
			for (const auto page : inserted) {
				UML_MEMORY_ALLOCATION_CHECKPOINT(
					testing::AllocationSite::native_owner_record);
				owners_.emplace(page, direct_ownership(owner));
			}
		} catch (...) {
			for (const auto page : inserted)
				owners_.erase(page);
			throw;
		}

		const auto rollback = [&]() noexcept {
			for (const auto page : inserted)
				owners_.erase(page);
		};

		try {
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::native_after_mmap);
		} catch (...) {
			rollback();
			throw;
		}

		mmu_trace_live_stacks("map-direct", address, length);
		void *mapped = map_shared_backing(
			reinterpret_cast<void *>(address), length, protection,
			MAP_FIXED | MAP_SHARED, fd, offset);
		if (mapped == MAP_FAILED) {
			rollback();
			return -1;
		}
		if (mapped != reinterpret_cast<void *>(address)) {
			(void)munmap(mapped, length);
			errno = EFAULT;
			rollback();
			return -1;
		}
		return 0;
	}

#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	int map_shadow(std::uint64_t owner, std::uintptr_t slot,
		int protection)
	{
		std::lock_guard lock{mutex_};
		if (owners_.contains(slot)) {
			errno = EBUSY;
			return -1;
		}
		try {
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::native_owner_record);
			owners_.emplace(slot, shadow_ownership(owner));
		} catch (...) {
			owners_.erase(slot);
			throw;
		}
		try {
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::native_after_mmap);
		} catch (...) {
			owners_.erase(slot);
			throw;
		}
		mmu_trace_live_stacks("map-shadow", slot, host_page_size());
		void *mapped = mmap(reinterpret_cast<void *>(slot), host_page_size(),
			protection, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
		if (mapped == MAP_FAILED || mapped != reinterpret_cast<void *>(slot)) {
			const auto saved_errno = mapped == MAP_FAILED ? errno : EFAULT;
			if (mapped != MAP_FAILED)
				(void)munmap(mapped, host_page_size());
			owners_.erase(slot);
			errno = saved_errno;
			return -1;
		}
		return 0;
	}

	int replace_direct_with_shadow(std::uint64_t owner,
		std::uintptr_t slot, std::uintptr_t prepared)
	{
		std::lock_guard lock{mutex_};
		const auto found = owners_.find(slot);
		if (found == owners_.end() ||
		    !is_direct_ownership(found->second, owner)) {
			errno = found == owners_.end() ? ENOMEM : EBUSY;
			return -1;
		}
		vm_address_t target = static_cast<vm_address_t>(slot);
		vm_prot_t current = VM_PROT_NONE;
		vm_prot_t maximum = VM_PROT_NONE;
		mmu_trace_live_stacks(
			"direct-to-shadow", slot, host_page_size());
		const auto result = vm_remap(
			mach_task_self(), &target, host_page_size(), 0,
			VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
			reinterpret_cast<vm_address_t>(prepared), TRUE,
			&current, &maximum, VM_INHERIT_NONE);
		if (result != KERN_SUCCESS || target != slot) {
			errno = mach_failure_errno(result);
			report_mapping_failure("shadow publication",
				reinterpret_cast<const void *>(slot),
				host_page_size(), errno);
			return -1;
		}
		found->second = shadow_ownership(owner);
		return 0;
	}

	int replace_shadow_with_direct(std::uint64_t owner,
		std::uintptr_t slot, int fd, std::uint64_t offset,
		int protection)
	{
		std::lock_guard lock{mutex_};
		const auto found = owners_.find(slot);
		if (found == owners_.end() ||
		    !is_shadow_ownership(found->second, owner)) {
			errno = found == owners_.end() ? ENOMEM : EBUSY;
			return -1;
		}
		mmu_trace_live_stacks(
			"shadow-to-direct", slot, host_page_size());
		void *mapped = map_shared_backing(
			reinterpret_cast<void *>(slot), host_page_size(), protection,
			MAP_FIXED | MAP_SHARED, fd, offset);
		if (mapped == MAP_FAILED || mapped != reinterpret_cast<void *>(slot)) {
			const int saved_errno = mapped == MAP_FAILED ? errno : EFAULT;
			if (mapped != MAP_FAILED)
				(void)munmap(mapped, host_page_size());
			errno = saved_errno;
			return -1;
		}
		found->second = direct_ownership(owner);
		return 0;
	}
#endif

	int protect_direct(std::uint64_t owner, std::uintptr_t address,
		std::uint64_t length, int protection)
	{
		std::uintptr_t first;
		std::uintptr_t end;
		if (!exact_host_range(address, length, first, end)) {
			errno = EINVAL;
			return -1;
		}
		std::lock_guard lock{mutex_};
		for (auto page = first; page < end; page += host_page_size()) {
			const auto found = owners_.find(page);
			if (found == owners_.end() ||
			    !is_direct_ownership(found->second, owner)) {
				errno = found == owners_.end() ? ENOMEM : EBUSY;
				return -1;
			}
		}
		mmu_trace_live_stacks("protect-direct", address, length);
		return mprotect(reinterpret_cast<void *>(address), length,
			protection);
	}

	int unmap_direct(std::uint64_t owner, std::uintptr_t address,
		std::uint64_t length)
	{
		std::uintptr_t first;
		std::uintptr_t end;
		if (!exact_host_range(address, length, first, end)) {
			errno = EINVAL;
			return -1;
		}
		std::lock_guard lock{mutex_};
		for (auto page = first; page < end; page += host_page_size())
			if (const auto found = owners_.find(page);
			    found != owners_.end() &&
			    !is_direct_ownership(found->second, owner)) {
				errno = EBUSY;
				return -1;
			}
		for (auto page = first; page < end;) {
			if (!owners_.contains(page)) {
				page += host_page_size();
				continue;
			}
			const auto run = page;
			do {
				page += host_page_size();
			} while (page < end && owners_.contains(page));
			mmu_trace_live_stacks("unmap-direct", run, page - run);
			/* Hand the run back to the window's reservation rather
			 * than to the process. munmap here would leave a free
			 * gap inside the guest window that an unrelated host
			 * allocation may be given, after which the next guest
			 * MAP_FIXED silently replaces it and its owner's own
			 * release removes live guest mappings. */
			if (reserve_host_range(reinterpret_cast<void *>(run),
				    page - run) != 0) {
				/* A mapping the kernel believes it removed but the
				 * host still serves outlives the page table that
				 * named it, so say so rather than only failing. */
				report_mapping_failure("guest page unmap",
					reinterpret_cast<const void *>(run),
					page - run, errno);
				return -1;
			}
			for (auto erased = run; erased < page;
			     erased += host_page_size())
				owners_.erase(erased);
		}
		return 0;
	}

	int flush_direct(std::uint64_t owner, std::uintptr_t address,
		std::uint64_t length)
	{
		if (length == 0)
			return 0;
		std::uintptr_t first;
		std::uintptr_t end;
		if (!covered_host_range(address, length, first, end)) {
			errno = EINVAL;
			return -1;
		}
		std::lock_guard lock{mutex_};
		for (auto page = first; page < end; page += host_page_size()) {
			const auto found = owners_.find(page);
			if (found == owners_.end())
				continue;
			if (!is_direct_ownership(found->second, owner)) {
				errno = EBUSY;
				return -1;
			}
			if (msync(reinterpret_cast<void *>(page),
				    host_page_size(), MS_SYNC) != 0)
				return -1;
		}
		return 0;
	}

	int validate_direct(std::uint64_t owner, std::uintptr_t address,
		std::uint64_t length) const noexcept
	{
		std::uintptr_t first;
		std::uintptr_t end;
		if (!covered_host_range(address, length, first, end)) {
			errno = EINVAL;
			return -1;
		}
		try {
			std::lock_guard lock{mutex_};
			for (auto page = first; page < end;
			     page += host_page_size()) {
				const auto found = owners_.find(page);
				if (found == owners_.end())
					continue;
				if (!is_direct_ownership(found->second, owner)) {
					errno = EBUSY;
					return -1;
				}
			}
			return 0;
		} catch (...) {
			errno = EIO;
			return -1;
		}
	}

	int inspect_direct_retry(std::uint64_t owner, std::uintptr_t address,
		bool execute, bool write, DirectRetryStatus &status) const noexcept
	{
		status = DirectRetryStatus::absent;
		try {
			std::lock_guard lock{mutex_};
			const auto found = owners_.find(host_slot_for(address));
			if (found == owners_.end())
				return 0;
			if (!is_direct_ownership(found->second, owner)) {
				errno = EBUSY;
				return -1;
			}

			vm_address_t region_start = static_cast<vm_address_t>(address);
			vm_size_t region_size = 0;
			vm_region_basic_info_data_64_t info{};
			mach_msg_type_number_t info_count =
				VM_REGION_BASIC_INFO_COUNT_64;
			mach_port_t object_name = MACH_PORT_NULL;
			const auto result = vm_region_64(
				mach_task_self(), &region_start, &region_size,
				VM_REGION_BASIC_INFO_64,
				reinterpret_cast<vm_region_info_t>(&info),
				&info_count, &object_name);
			if (object_name != MACH_PORT_NULL)
				(void)mach_port_deallocate(mach_task_self(), object_name);

			if (result == KERN_INVALID_ADDRESS) {
				status = DirectRetryStatus::denied;
				return 0;
			}
			if (result != KERN_SUCCESS) {
				errno = EIO;
				return -1;
			}
			if (info_count < VM_REGION_BASIC_INFO_COUNT_64 ||
			    region_size == 0) {
				errno = EIO;
				return -1;
			}
			const auto start = static_cast<std::uintptr_t>(region_start);
			const auto size = static_cast<std::uintptr_t>(region_size);
			if (address < start || address - start >= size) {
				status = DirectRetryStatus::denied;
				return 0;
			}

			vm_prot_t required = VM_PROT_NONE;
			if (execute)
				required |= VM_PROT_EXECUTE;
			if (write)
				required |= VM_PROT_WRITE;
			if (!execute && !write)
				required = VM_PROT_READ;
			status = (info.protection & required) == required ?
				DirectRetryStatus::permitted :
				DirectRetryStatus::denied;
			return 0;
		} catch (...) {
			errno = EIO;
			return -1;
		}
	}

	[[nodiscard]] bool owns_direct(
		std::uint64_t owner, std::uintptr_t page) const noexcept
	{
		try {
			std::lock_guard lock{mutex_};
			const auto found = owners_.find(page);
			return found != owners_.end() &&
				is_direct_ownership(found->second, owner);
		} catch (...) {
			/* Retain the local claim if ownership cannot be inspected. */
			return true;
		}
	}

#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	int release_shadow(std::uint64_t owner, std::uintptr_t slot) noexcept
	{
		std::lock_guard lock{mutex_};
		const auto found = owners_.find(slot);
		if (found == owners_.end())
			return 0;
		if (!is_shadow_ownership(found->second, owner)) {
			errno = EBUSY;
			return -1;
		}
		mmu_trace_live_stacks("release-shadow", slot, host_page_size());
		if (reserve_host_range(reinterpret_cast<void *>(slot),
			    host_page_size()) != 0)
			return -1;
		owners_.erase(found);
		return 0;
	}
#endif

	/* Retirement ends one domain, not the process, and a later domain maps
	 * the same window. Its pages go back to the window's reservation for the
	 * same reason an ordinary unmap's do. */
	void retire(std::uint64_t owner) noexcept
	{
		std::lock_guard lock{mutex_};
		for (auto position = owners_.begin(); position != owners_.end();) {
			if (position->second.owner != owner) {
				++position;
				continue;
			}
			#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
			if (position->second.kind == OwnershipKind::direct)
			#endif
				(void)msync(reinterpret_cast<void *>(position->first),
					host_page_size(), MS_SYNC);
			(void)reserve_host_range(
				reinterpret_cast<void *>(position->first),
				host_page_size());
			position = owners_.erase(position);
		}
	}

#if UML_MEMORY_ALLOCATION_TESTING
	[[nodiscard]] std::size_t size_for_testing() const noexcept
	{
		try {
			std::lock_guard lock{mutex_};
			return owners_.size();
		} catch (...) {
			return std::numeric_limits<std::size_t>::max();
		}
	}
#endif

private:
	mutable std::mutex mutex_;
	PageAddressMap<Ownership> owners_;
};

std::atomic<HostOwnershipRegistry *> published_host_ownership_registry{};
std::atomic_flag host_ownership_registry_initialization = ATOMIC_FLAG_INIT;

HostOwnershipRegistry *host_ownership_registry() noexcept
{
	if (auto *registry = published_host_ownership_registry.load(
		    std::memory_order_acquire); registry != nullptr)
		return registry;
	while (host_ownership_registry_initialization.test_and_set(
		       std::memory_order_acquire))
		std::this_thread::yield();
	struct InitializationUnlock final {
		~InitializationUnlock()
		{
			host_ownership_registry_initialization.clear(
				std::memory_order_release);
		}
	};
	[[maybe_unused]] InitializationUnlock unlock;
	if (auto *registry = published_host_ownership_registry.load(
		    std::memory_order_acquire); registry != nullptr)
		return registry;
	try {
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::native_owner_registry);
		auto *registry = new (std::nothrow) HostOwnershipRegistry;
		if (registry != nullptr)
			published_host_ownership_registry.store(
				registry, std::memory_order_release);
		return registry;
	} catch (...) {
		return nullptr;
	}
}

std::atomic_uint64_t next_ownership_id{1};

[[nodiscard]] std::uint64_t fresh_ownership_id() noexcept
{
	auto result = next_ownership_id.fetch_add(1, std::memory_order_relaxed);
	if (result == 0)
		result = next_ownership_id.fetch_add(1, std::memory_order_relaxed);
	return result;
}

} // namespace

NativeAddressSpace::NativeAddressSpace() noexcept
	: ownership_id_{fresh_ownership_id()}
{
}

int NativeAddressSpace::map_direct(
	std::uintptr_t address, int fd, std::uint64_t offset,
	std::uint64_t length, int protection)
{
	std::uintptr_t first;
	std::uintptr_t end;
	if (!exact_host_range(address, length, first, end)) {
		errno = EINVAL;
		return -1;
	}
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	auto *registry = host_ownership_registry();
	if (registry == nullptr) {
		errno = ENOMEM;
		return -1;
	}
	std::vector<std::uintptr_t> inserted;
	#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	struct DirectSnapshot final {
		std::uintptr_t page;
		std::optional<DirectMapping> mapping;
	};
	std::vector<DirectSnapshot> direct_snapshots;
	#endif
	std::size_t new_page_count = 0;
	for (auto page = first; page < end; page += host_page_size()) {
#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
		if (materialized_slots_.contains(page)) {
			errno = EBUSY;
			return -1;
		}
#endif
		if (!direct_slots_.contains(page))
			++new_page_count;
	}
	inserted.reserve(new_page_count);
	#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	const auto page_count = static_cast<std::size_t>(
		(end - first) / host_page_size());
	direct_snapshots.reserve(page_count);
	reserve_additional(direct_mappings_, new_page_count);
	for (auto page = first; page < end; page += host_page_size()) {
		std::optional<DirectMapping> previous;
		if (const auto found = direct_mappings_.find(page);
		    found != direct_mappings_.end())
			previous = found->second;
		direct_snapshots.push_back({page, previous});
	}
	const auto rollback_direct_metadata = [&]() noexcept {
		for (const auto &snapshot : direct_snapshots) {
			if (snapshot.mapping)
				direct_mappings_.insert_or_assign(
					snapshot.page, *snapshot.mapping);
			else
				direct_mappings_.erase(snapshot.page);
		}
	};
	#endif
	try {
		for (auto page = first; page < end; page += host_page_size()) {
			if (!direct_slots_.contains(page)) {
				UML_MEMORY_ALLOCATION_CHECKPOINT(
					testing::AllocationSite::native_direct_record);
				direct_slots_.insert(page);
				inserted.push_back(page);
			}
			#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::native_direct_record);
			direct_mappings_.insert_or_assign(page, DirectMapping{
				fd, offset + (page - first), protection});
			#endif
		}
		const auto result = registry->map_direct(
			ownership_id_, address, fd, offset, length, protection);
		if (result != 0) {
			for (const auto rollback : inserted)
				direct_slots_.erase(rollback);
			#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
			rollback_direct_metadata();
			#endif
		}
		return result;
	} catch (...) {
		for (const auto rollback : inserted)
			direct_slots_.erase(rollback);
		#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
		rollback_direct_metadata();
		#endif
		throw;
	}
}

int NativeAddressSpace::protect_direct(
	std::uintptr_t address, std::uint64_t length, int protection)
{
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	auto *registry = host_ownership_registry();
	if (registry == nullptr) {
		errno = ENOMEM;
		return -1;
	}
	const auto result = registry->protect_direct(
		ownership_id_, address, length, protection);
	#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	if (result == 0) {
		const auto end = address + length;
		for (auto slot = address; slot < end; slot += host_page_size())
			if (auto mapping = direct_mappings_.find(slot);
			    mapping != direct_mappings_.end())
				mapping->second.protection = protection;
	}
	#endif
	return result;
}

int NativeAddressSpace::unmap_direct(
	std::uintptr_t address, std::uint64_t length)
{
	std::uintptr_t first;
	std::uintptr_t end;
	if (!exact_host_range(address, length, first, end)) {
		errno = EINVAL;
		return -1;
	}
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	auto *registry = host_ownership_registry();
	if (registry == nullptr) {
		errno = ENOMEM;
		return -1;
	}
	const auto reconcile = [&]() noexcept {
		for (auto page = first; page < end; page += host_page_size())
			if (!registry->owns_direct(ownership_id_, page)) {
				direct_slots_.erase(page);
				#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
				direct_mappings_.erase(page);
				#endif
			}
	};
	int result;
	try {
		result = registry->unmap_direct(
			ownership_id_, address, length);
	} catch (...) {
		reconcile();
		throw;
	}
	const auto saved_errno = errno;
	reconcile();
	if (result != 0)
		errno = saved_errno;
	return result;
}

int NativeAddressSpace::flush_direct(
	std::uintptr_t address, std::uint64_t length)
{
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	auto *registry = host_ownership_registry();
	if (registry == nullptr) {
		errno = ENOMEM;
		return -1;
	}
	return registry->flush_direct(
		ownership_id_, address, length);
}

int NativeAddressSpace::sync_direct(
	std::uintptr_t address, std::uint64_t length)
{
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	auto *registry = host_ownership_registry();
	if (registry == nullptr) {
		errno = ENOMEM;
		return -1;
	}
	/* Direct mappings are MAP_SHARED, so there is no shadow buffer to refill.
	 * Validation still prevents a foreign domain from treating another run's
	 * high mapping as its own. */
	return registry->validate_direct(ownership_id_, address, length);
}

int NativeAddressSpace::retry_direct(
	std::uintptr_t address, bool execute, bool write)
{
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	auto *registry = host_ownership_registry();
	if (registry == nullptr) {
		errno = ENOMEM;
		return -1;
	}
	HostOwnershipRegistry::DirectRetryStatus status;
	if (registry->inspect_direct_retry(
		    ownership_id_, address, execute, write, status) != 0)
		return -1;

	const bool locally_owned = direct_slots_.contains(host_slot_for(address));
	if (status == HostOwnershipRegistry::DirectRetryStatus::absent) {
		if (locally_owned) {
			errno = EFAULT;
			return -1;
		}
		return 0;
	}
	if (!locally_owned) {
		errno = EFAULT;
		return -1;
	}
	return status == HostOwnershipRegistry::DirectRetryStatus::permitted ?
		1 : 0;
}

#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
bool NativeAddressSpace::needs_shadow(
	std::uintptr_t address, std::uint64_t offset,
	std::uint64_t length) noexcept
{
	const auto mask = host_page_size() - 1;
	return ((address | offset | length) & mask) != 0;
}

bool NativeAddressSpace::contains_shadow(
	std::uintptr_t address, std::uint64_t length) const
{
	if (length == 0 || address + length < address)
		return false;
	std::lock_guard lock{mutex_};
	const auto first = slot_for(address);
	const auto end = address + length;
	for (auto slot = first; slot < end; slot += host_page_size())
		if (materialized_slots_.contains(slot))
			return true;
	return false;
}

bool NativeAddressSpace::contains_direct(
	std::uintptr_t address, std::uint64_t length) const
{
	if (length == 0 || address + length < address)
		return false;
	std::lock_guard lock{mutex_};
	const auto first = slot_for(address);
	const auto end = address + length;
	for (auto slot = first; slot < end; slot += host_page_size())
		if (direct_slots_.contains(slot))
			return true;
	return false;
}

NativeAddressSpace::DirectTransitionGuard::DirectTransitionGuard(
	NativeAddressSpace &owner_value, std::size_t capacity)
	: owner{owner_value}
{
	transitions.reserve(capacity);
}

NativeAddressSpace::DirectTransitionGuard::~DirectTransitionGuard()
{
	if (committed)
		return;
	const int saved_errno = errno;
	owner.restore_direct_transitions_locked(transitions);
	errno = saved_errno;
}

void NativeAddressSpace::restore_direct_transitions_locked(
	std::vector<DirectTransition> &transitions) noexcept
{
	auto *registry = host_ownership_registry();
	if (registry == nullptr)
		return;
	for (auto position = transitions.rbegin();
	     position != transitions.rend(); ++position) {
		if (!position->converted || position->direct_mapping.empty() ||
		    position->direct_slot.empty())
			continue;
		const auto mapping = position->direct_mapping.mapped();
		try {
			if (registry->replace_shadow_with_direct(
				ownership_id_, position->slot, mapping.fd,
				mapping.offset, mapping.protection) < 0)
				continue;
		} catch (...) {
			continue;
		}
		for (std::uintptr_t offset = 0; offset < host_page_size();
		     offset += guest_page_size) {
			const auto page = position->slot + offset;
			if (backing_index_ready_)
				if (const auto found = mappings_.find(page);
				    found != mappings_.end())
					erase_backing_index_entry_locked(
						BackingPageKey{found->second.identity,
							found->second.offset}, page);
			mappings_.erase(page);
		}
		materialized_slots_.erase(position->slot);
		slot_protections_.erase(position->slot);
		direct_slots_.insert(std::move(position->direct_slot));
		direct_mappings_.insert(std::move(position->direct_mapping));
	}
}

int NativeAddressSpace::convert_direct_slot_to_shadow_locked(
	std::uintptr_t slot)
{
	const auto direct = direct_mappings_.find(slot);
	if (direct == direct_mappings_.end())
		return 0;
	if (mmu_trace_enabled())
		dprintf(STDERR_FILENO,
			"Darwin native direct-to-shadow slot=%#lx frame=%p "
			"fd=%d offset=%#llx protection=%#x\n",
			static_cast<unsigned long>(slot), __builtin_frame_address(0),
			direct->second.fd,
			static_cast<unsigned long long>(direct->second.offset),
			direct->second.protection);
	if (!direct_slots_.contains(slot) || materialized_slots_.contains(slot)) {
		errno = EBUSY;
		return -1;
	}
	for (std::uintptr_t position = 0; position < host_page_size();
	     position += guest_page_size)
		if (mappings_.contains(slot + position)) {
			errno = EBUSY;
			return -1;
		}
	auto *registry = host_ownership_registry();
	if (registry == nullptr) {
		errno = ENOMEM;
		return -1;
	}

	/* A temporary, fully populated host page lets vm_remap replace the
	 * direct slot atomically.  No CPU can observe the zero-filled interval
	 * produced by replacing a mapping with MAP_FIXED anonymous memory first. */
	std::array<std::byte, 16 * 1024> snapshot{};
	static_assert(snapshot.size() >= 4 * guest_page_size);
	void *shared_alias = map_shared_backing(nullptr, host_page_size(), PROT_READ,
		MAP_SHARED, direct->second.fd, direct->second.offset);
	if (shared_alias == MAP_FAILED)
		return -1;
	std::copy_n(static_cast<const std::byte *>(shared_alias), host_page_size(),
		snapshot.begin());
	if (munmap(shared_alias, host_page_size()) != 0)
		return -1;

	BackingIdentity identity{};
	if (identify_backing(direct->second.fd, identity) < 0)
		return -1;
	const int shadow_protection =
		shadow_data_protection(direct->second.protection);
	std::array<std::uintptr_t, 4> pages{};
	std::vector<decltype(backing_index_)::iterator> index_entries;
	index_entries.reserve(pages.size());
	void *prepared = mmap(nullptr, host_page_size(),
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (prepared == MAP_FAILED)
		return -1;
	std::copy_n(snapshot.begin(), host_page_size(),
		static_cast<std::byte *>(prepared));
	if (mprotect(prepared, host_page_size(), shadow_protection) != 0) {
		const int saved_errno = errno;
		(void)munmap(prepared, host_page_size());
		errno = saved_errno;
		return -1;
	}

	const auto rollback_metadata = [&]() noexcept {
		for (const auto entry : index_entries)
			backing_index_.erase(entry);
		for (const auto page : pages)
			if (page != 0)
				mappings_.erase(page);
		slot_protections_.erase(slot);
		materialized_slots_.erase(slot);
	};
	try {
		reserve_additional(mappings_, pages.size());
		reserve_additional(materialized_slots_, 1);
		reserve_additional(slot_protections_, 1);
		if (backing_index_ready_)
			reserve_additional(backing_index_, pages.size());
		for (std::size_t index = 0; index < pages.size(); ++index) {
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::native_mapping);
			const auto page = slot + index * guest_page_size;
			pages[index] = page;
			Mapping mapping{
				direct->second.fd,
				direct->second.offset + index * guest_page_size,
				direct->second.protection,
				identity,
			};
			std::copy_n(snapshot.begin() + index * guest_page_size,
				guest_page_size, mapping.original.begin());
			mapping.loaded = true;
			note_shadow_offset_locked(mapping.offset);
			mappings_.emplace(page, std::move(mapping));
			if (backing_index_ready_) {
				UML_MEMORY_ALLOCATION_CHECKPOINT(
					testing::AllocationSite::native_backing_index);
				index_entries.push_back(backing_index_.emplace(
					BackingPageKey{identity,
						direct->second.offset +
						index * guest_page_size},
					page));
			}
		}
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::native_slot);
		materialized_slots_.insert(slot);
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::native_slot);
		slot_protections_.emplace(slot, shadow_protection);
	} catch (...) {
		rollback_metadata();
		(void)munmap(prepared, host_page_size());
		throw;
	}

	if (registry->replace_direct_with_shadow(
		ownership_id_, slot,
		reinterpret_cast<std::uintptr_t>(prepared)) < 0) {
		const int saved_errno = errno;
		rollback_metadata();
		(void)munmap(prepared, host_page_size());
		errno = saved_errno;
		return -1;
	}
	(void)munmap(prepared, host_page_size());
	return 0;
}

int NativeAddressSpace::ensure_shadow_range_locked(
	std::uintptr_t address, std::uint64_t length,
	DirectTransitionGuard &guard)
{
	if (length == 0 || address + length < address) {
		errno = EINVAL;
		return -1;
	}
	const auto first = slot_for(address);
	const auto end = address + length;
	for (auto slot = first; slot < end; slot += host_page_size()) {
		if (!direct_mappings_.contains(slot))
			continue;
		guard.transitions.emplace_back();
		auto &transition = guard.transitions.back();
		transition.slot = slot;
		if (convert_direct_slot_to_shadow_locked(slot) < 0)
			return -1;
		transition.direct_slot = direct_slots_.extract(slot);
		transition.direct_mapping = direct_mappings_.extract(slot);
		transition.converted = true;
	}
	return 0;
}

std::size_t NativeAddressSpace::BackingPageKeyHash::operator()(
	const BackingPageKey &key) const noexcept
{
	const auto mix = [](std::size_t seed, std::uint64_t value) noexcept {
		const auto hashed = std::hash<std::uint64_t>{}(value);
		return seed ^ (hashed + 0x9e3779b97f4a7c15ULL +
			(seed << 6U) + (seed >> 2U));
	};
	/* Pages of one file differ only in a page-aligned offset, which leaves
	 * the low bits of the combination constant. */
	return static_cast<std::size_t>(fold_page_key(
		mix(mix(std::hash<std::uint64_t>{}(key.identity.device),
			key.identity.inode), key.offset)));
}

int NativeAddressSpace::identify_backing(
	int fd, BackingIdentity &identity) noexcept
{
	struct stat status {};
	if (fstat(fd, &status) != 0)
		return -1;
	identity = {
		static_cast<std::uint64_t>(status.st_dev),
		static_cast<std::uint64_t>(status.st_ino),
	};
	return 0;
}

bool NativeAddressSpace::live_kernel_stack_slot_locked(
	std::uintptr_t slot) const noexcept
{
	const auto contains = [slot](std::uintptr_t address) noexcept {
		return address != 0 && slot >= address &&
			slot - address < kernel_stack_size;
	};
	return std::ranges::any_of(active_kernel_stacks_,
		[&](const auto &entry) noexcept {
			return contains(entry.second.current) ||
				(entry.second.transition &&
				 contains(entry.second.staged));
		});
}

int NativeAddressSpace::restore_inactive_stack_protections_locked(
	std::uintptr_t address)
{
	if (address == 0)
		return 0;
	int saved_errno = 0;
	for (auto slot = address; slot < address + kernel_stack_size;
	     slot += host_page_size())
		if (!live_kernel_stack_slot_locked(slot) &&
		    update_slot_protection_locked(slot) < 0 && saved_errno == 0)
			saved_errno = errno;
	if (saved_errno == 0)
		return 0;
	errno = saved_errno;
	return -1;
}

int NativeAddressSpace::stage_kernel_stack(std::uintptr_t address)
{
	if (address != 0 && (address & (kernel_stack_size - 1)) != 0) {
		errno = EINVAL;
		return -1;
	}
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	const auto carrier = current_carrier_id();
	auto [found, inserted] = active_kernel_stacks_.try_emplace(carrier);
	(void)inserted;
	if (found->second.transition) {
		errno = EBUSY;
		return -1;
	}
	found->second.staged = address;
	found->second.transition = true;
	if (address == 0)
		return 0;
	for (auto slot = address; slot < address + kernel_stack_size;
	     slot += host_page_size())
		if (materialized_slots_.contains(slot) &&
		    set_slot_protection_locked(
			    slot, PROT_READ | PROT_WRITE) < 0) {
			const int saved_errno = errno;
			found->second.staged = 0;
			found->second.transition = false;
			(void)restore_inactive_stack_protections_locked(address);
			if (found->second.current == 0)
				active_kernel_stacks_.erase(found);
			errno = saved_errno;
			return -1;
		}
	return 0;
}

int NativeAddressSpace::finish_kernel_stack(std::uintptr_t address)
{
	if (address != 0 && (address & (kernel_stack_size - 1)) != 0) {
		errno = EINVAL;
		return -1;
	}
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	const auto carrier = current_carrier_id();
	const auto found = active_kernel_stacks_.find(carrier);
	if (found == active_kernel_stacks_.end() ||
	    !found->second.transition || found->second.staged != address) {
		errno = EINVAL;
		return -1;
	}
	const auto previous = found->second.current;
	found->second.current = address;
	found->second.staged = 0;
	found->second.transition = false;
	const int result = previous == address ? 0 :
		restore_inactive_stack_protections_locked(previous);
	if (address == 0)
		active_kernel_stacks_.erase(found);
	return result;
}

int NativeAddressSpace::ensure_backing_index_locked()
{
	if (backing_index_ready_)
		return 0;
	decltype(backing_index_) index;
	UML_MEMORY_ALLOCATION_CHECKPOINT(
		testing::AllocationSite::native_backing_index);
	index.reserve(mappings_.size());
	for (const auto &[page, mapping] : mappings_) {
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::native_backing_index);
		index.emplace(BackingPageKey{mapping.identity, mapping.offset}, page);
	}
	backing_index_.swap(index);
	backing_index_ready_ = true;
	return 0;
}

void NativeAddressSpace::erase_backing_index_entry_locked(
	const BackingPageKey &key, std::uintptr_t page) noexcept
{
	const auto [first, end] = backing_index_.equal_range(key);
	for (auto position = first; position != end; ++position) {
		if (position->second != page)
			continue;
		backing_index_.erase(position);
		return;
	}
}

int NativeAddressSpace::flush_page_locked(std::uintptr_t page)
{
	if (live_kernel_stack_slot_locked(slot_for(page)))
		return 0;
	auto found = mappings_.find(page);
	if (found == mappings_.end() || !found->second.loaded)
		return 0;
	const auto slot = slot_for(page);
	const auto protection = slot_protections_.find(slot);
	if (!materialized_slots_.contains(slot) ||
	    protection == slot_protections_.end()) {
		errno = EFAULT;
		return -1;
	}
	const int original_protection = protection->second;
	/* An unyoung kernel PTE can leave dirty shadow bytes behind PROT_NONE.
	 * Publishing those bytes must not recursively fault in the host. The final
	 * compare/exchange also needs write access to reconcile shared changes, so
	 * temporarily add both data permissions and restore the exact tracked
	 * protection on every path; never add execute here. */
	const int synchronization_protection = original_protection |
		PROT_READ | PROT_WRITE;
	const bool lifted = synchronization_protection != original_protection;
	if (lifted && mprotect(reinterpret_cast<void *>(slot), host_page_size(),
		synchronization_protection) != 0)
		return -1;
	const auto finish = [&](int result) noexcept {
		const int saved_errno = errno;
		if (lifted && mprotect(reinterpret_cast<void *>(slot), host_page_size(),
			original_protection) != 0)
			return -1;
		if (result != 0)
			errno = saved_errno;
		return result;
	};
	std::array<std::byte, guest_page_size> shadow{};
	snapshot_shadow_page(page, shadow);
	std::array<std::byte, guest_page_size> backing{};
	const bool trace = mmu_trace_target(page);
	if (trace)
		dprintf(STDERR_FILENO,
			"Darwin native target flush begin page=%#lx fd=%d "
			"offset=%#llx loaded=%d current=%#llx/%#llx/%#llx "
			"original=%#llx/%#llx/%#llx\n",
			static_cast<unsigned long>(page), found->second.fd,
			static_cast<unsigned long long>(found->second.offset),
			found->second.loaded,
			static_cast<unsigned long long>(
				mmu_trace_word(shadow.data(), 0x270)),
			static_cast<unsigned long long>(
				mmu_trace_word(shadow.data(), 0x278)),
			static_cast<unsigned long long>(
				mmu_trace_word(shadow.data(), 0x280)),
			static_cast<unsigned long long>(
				mmu_trace_word(found->second.original.data(), 0x270)),
			static_cast<unsigned long long>(
				mmu_trace_word(found->second.original.data(), 0x278)),
			static_cast<unsigned long long>(
				mmu_trace_word(found->second.original.data(), 0x280)));
	if (shadow == found->second.original) {
		if (trace)
			dprintf(STDERR_FILENO,
				"Darwin native target flush clean page=%#lx\n",
				static_cast<unsigned long>(page));
		return finish(0);
	}
	mmu_trace_live_stacks("flush-shadow-page", page, guest_page_size);
	if (read_exact(found->second.fd, found->second.offset,
		backing.data(), backing.size()) < 0)
		return finish(-1);
	const bool backing_changed = backing != found->second.original;
	if (mmu_conflict_trace_enabled())
		for (std::size_t word_offset = 0; word_offset < guest_page_size;
		     word_offset += sizeof(std::uint64_t)) {
			std::uint64_t shadow_word{};
			std::uint64_t backing_word{};
			std::uint64_t original_word{};
			std::memcpy(&shadow_word, shadow.data() + word_offset,
				sizeof(shadow_word));
			std::memcpy(&backing_word, backing.data() + word_offset,
				sizeof(backing_word));
			std::memcpy(&original_word,
				found->second.original.data() + word_offset,
				sizeof(original_word));
			if (shadow_word == original_word ||
			    backing_word == original_word ||
			    shadow_word == backing_word)
				continue;
			dprintf(STDERR_FILENO,
				"Darwin native word conflict operation=flush-sources "
				"page=%#lx offset=%#zx original=%#llx "
				"shadow=%#llx backing=%#llx fd=%d file-offset=%#llx\n",
				static_cast<unsigned long>(page), word_offset,
				static_cast<unsigned long long>(original_word),
				static_cast<unsigned long long>(shadow_word),
				static_cast<unsigned long long>(backing_word),
				found->second.fd,
				static_cast<unsigned long long>(found->second.offset));
		}
	if (trace)
		dprintf(STDERR_FILENO,
			"Darwin native target flush backing page=%#lx changed=%d "
			"backing=%#llx/%#llx/%#llx\n",
			static_cast<unsigned long>(page), backing_changed,
			static_cast<unsigned long long>(
				mmu_trace_word(backing.data(), 0x270)),
			static_cast<unsigned long long>(
				mmu_trace_word(backing.data(), 0x278)),
			static_cast<unsigned long long>(
				mmu_trace_word(backing.data(), 0x280)));
	/* Merge disjoint native-shadow and shared-backing writes byte-for-byte.
	 * A whole-page pwrite would lose a sibling CPU's update to an unrelated
	 * byte.  If both sides changed the same byte, the already-shared value is
	 * authoritative because another translated task can have observed it. */
	std::size_t position = 0;
	while (position < guest_page_size) {
		if (shadow[position] == found->second.original[position] ||
		    backing[position] != found->second.original[position]) {
			++position;
			continue;
		}
		const auto first = position;
		do {
			backing[position] = shadow[position];
			++position;
		} while (position < guest_page_size &&
			 shadow[position] != found->second.original[position] &&
			 backing[position] == found->second.original[position]);
		if (write_exact(found->second.fd, found->second.offset + first,
			backing.data() + first, position - first) < 0)
			return finish(-1);
	}
	/* The page can remain live on another native carrier throughout this
	 * publication. Replace only bytes that still match the stable snapshot;
	 * a newer native write wins the compare/exchange and remains dirty against
	 * the newly published baseline. */
	merge_shadow_page(page, shadow, backing, "flush-live");
	found->second.original = backing;
	found->second.loaded = true;
	if (trace)
		dprintf(STDERR_FILENO,
			"Darwin native target flush end page=%#lx loaded=%d "
			"original=%#llx/%#llx/%#llx\n",
			static_cast<unsigned long>(page), found->second.loaded,
			static_cast<unsigned long long>(
				mmu_trace_word(found->second.original.data(), 0x270)),
			static_cast<unsigned long long>(
				mmu_trace_word(found->second.original.data(), 0x278)),
			static_cast<unsigned long long>(
				mmu_trace_word(found->second.original.data(), 0x280)));
	return finish(0);
}

int NativeAddressSpace::flush_slot_locked(std::uintptr_t slot)
{
	if (!materialized_slots_.contains(slot))
		return 0;
	for (std::uintptr_t position = 0; position < host_page_size();
	     position += guest_page_size)
		if (flush_page_locked(slot + position) < 0)
			return -1;
	return 0;
}

int NativeAddressSpace::reconcile_page_locked(std::uintptr_t page)
{
	if (live_kernel_stack_slot_locked(slot_for(page)))
		return 0;
	auto found = mappings_.find(page);
	if (found == mappings_.end())
		return 0;
	const bool trace = mmu_trace_target(page);
	if (trace)
		dprintf(STDERR_FILENO,
			"Darwin native target reconcile begin page=%#lx fd=%d "
			"offset=%#llx loaded=%d current=%#llx/%#llx/%#llx "
			"original=%#llx/%#llx/%#llx\n",
			static_cast<unsigned long>(page), found->second.fd,
			static_cast<unsigned long long>(found->second.offset),
			found->second.loaded,
			static_cast<unsigned long long>(mmu_trace_word(
				reinterpret_cast<const std::byte *>(page), 0x270)),
			static_cast<unsigned long long>(mmu_trace_word(
				reinterpret_cast<const std::byte *>(page), 0x278)),
			static_cast<unsigned long long>(mmu_trace_word(
				reinterpret_cast<const std::byte *>(page), 0x280)),
			static_cast<unsigned long long>(
				mmu_trace_word(found->second.original.data(), 0x270)),
			static_cast<unsigned long long>(
				mmu_trace_word(found->second.original.data(), 0x278)),
			static_cast<unsigned long long>(
				mmu_trace_word(found->second.original.data(), 0x280)));
	if (!found->second.loaded)
		return load_page_locked(page);
	std::array<std::byte, guest_page_size> backing{};
	if (read_exact(found->second.fd, found->second.offset,
		backing.data(), backing.size()) < 0)
		return -1;
	if (trace)
		dprintf(STDERR_FILENO,
			"Darwin native target reconcile backing page=%#lx "
			"backing=%#llx/%#llx/%#llx\n",
			static_cast<unsigned long>(page),
			static_cast<unsigned long long>(
				mmu_trace_word(backing.data(), 0x270)),
			static_cast<unsigned long long>(
				mmu_trace_word(backing.data(), 0x278)),
			static_cast<unsigned long long>(
				mmu_trace_word(backing.data(), 0x280)));
	if (backing == found->second.original)
		return 0;
	std::array<std::byte, guest_page_size> shadow{};
	snapshot_shadow_page(page, shadow);
	if (shadow != found->second.original) {
		if (flush_page_locked(page) < 0)
			return -1;
		if (found->second.loaded)
			return 0;
	}
	return load_page_locked(page);
}

int NativeAddressSpace::load_page_locked(std::uintptr_t page)
{
	if (live_kernel_stack_slot_locked(slot_for(page)))
		return 0;
	auto found = mappings_.find(page);

	if (found == mappings_.end())
		return 0;
	std::array<std::byte, guest_page_size> shadow{};
	snapshot_shadow_page(page, shadow);
	if (mmu_trace_enabled()) {
		const auto frame = reinterpret_cast<std::uintptr_t>(
			__builtin_frame_address(0));
		if (slot_for(page) == slot_for(frame))
			dprintf(STDERR_FILENO,
				"Darwin native load-active page=%#lx frame=%#lx "
				"fd=%d offset=%#llx\n",
				static_cast<unsigned long>(page),
				static_cast<unsigned long>(frame), found->second.fd,
				static_cast<unsigned long long>(found->second.offset));
	}
	const bool trace = mmu_trace_target(page);
	if (trace)
		dprintf(STDERR_FILENO,
			"Darwin native target load begin page=%#lx fd=%d "
			"offset=%#llx loaded=%d current=%#llx/%#llx/%#llx "
			"original=%#llx/%#llx/%#llx\n",
			static_cast<unsigned long>(page), found->second.fd,
			static_cast<unsigned long long>(found->second.offset),
			found->second.loaded,
			static_cast<unsigned long long>(
				mmu_trace_word(shadow.data(), 0x270)),
			static_cast<unsigned long long>(
				mmu_trace_word(shadow.data(), 0x278)),
			static_cast<unsigned long long>(
				mmu_trace_word(shadow.data(), 0x280)),
			static_cast<unsigned long long>(
				mmu_trace_word(found->second.original.data(), 0x270)),
			static_cast<unsigned long long>(
				mmu_trace_word(found->second.original.data(), 0x278)),
			static_cast<unsigned long long>(
				mmu_trace_word(found->second.original.data(), 0x280)));
	mmu_trace_live_stacks("load-shadow-page", page, guest_page_size);
	std::array<std::byte, guest_page_size> backing{};
	if (read_exact(found->second.fd, found->second.offset,
		backing.data(), backing.size()) < 0)
		return -1;
	/* A new mapping replaces the quarter captured above. For an established
	 * mapping, retain bytes changed from its published baseline while the
	 * backing read was in flight. */
	const auto &expected = found->second.loaded ?
		found->second.original : shadow;
	merge_shadow_page(page, expected, backing, "load");
	found->second.original = backing;
	found->second.loaded = true;
	if (trace)
		dprintf(STDERR_FILENO,
			"Darwin native target load end page=%#lx current=%#llx/%#llx/%#llx\n",
			static_cast<unsigned long>(page),
			static_cast<unsigned long long>(mmu_trace_word(
				reinterpret_cast<const std::byte *>(page), 0x270)),
			static_cast<unsigned long long>(mmu_trace_word(
				reinterpret_cast<const std::byte *>(page), 0x278)),
			static_cast<unsigned long long>(mmu_trace_word(
				reinterpret_cast<const std::byte *>(page), 0x280)));
	return 0;
}

int NativeAddressSpace::reload_page_from_backing_locked(std::uintptr_t page)
{
	if (live_kernel_stack_slot_locked(slot_for(page)))
		return 0;
	auto found = mappings_.find(page);
	if (found == mappings_.end())
		return 0;
	std::array<std::byte, guest_page_size> shadow{};
	snapshot_shadow_page(page, shadow);
	std::array<std::byte, guest_page_size> backing{};
	if (read_exact(found->second.fd, found->second.offset,
		backing.data(), backing.size()) < 0)
		return -1;
	/* Translated x86 userspace writes the shared physical backing directly.
	 * Its kernel-side physical alias is only a cache for uaccess, so discard
	 * changes which predate this snapshot instead of publishing them over the
	 * userspace bytes. Preserve native writes which race the backing read. */
	merge_shadow_page(page, shadow, backing, "reload-from-backing");
	found->second.original = backing;
	found->second.loaded = true;
	return 0;
}

int NativeAddressSpace::materialize_slot_locked(std::uintptr_t slot)
{
	if (materialized_slots_.contains(slot))
		return set_slot_protection_locked(
			slot, PROT_READ | PROT_WRITE);
	if (direct_slots_.contains(slot)) {
		errno = EBUSY;
		return -1;
	}
	auto *registry = host_ownership_registry();
	if (registry == nullptr) {
		errno = ENOMEM;
		return -1;
	}
	try {
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::native_slot);
		materialized_slots_.insert(slot);
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::native_slot);
		slot_protections_.insert_or_assign(
			slot, PROT_READ | PROT_WRITE);
	} catch (...) {
		slot_protections_.erase(slot);
		materialized_slots_.erase(slot);
		throw;
	}
	try {
		const auto result = registry->map_shadow(
			ownership_id_, slot, PROT_READ | PROT_WRITE);
		if (result != 0) {
			slot_protections_.erase(slot);
			materialized_slots_.erase(slot);
		}
		return result;
	} catch (...) {
		slot_protections_.erase(slot);
		materialized_slots_.erase(slot);
		throw;
	}
}

int NativeAddressSpace::set_slot_protection_locked(
	std::uintptr_t slot, int protection)
{
	if (live_kernel_stack_slot_locked(slot))
		protection = PROT_READ | PROT_WRITE;
	/* This is the sole protection transition for every tracked host slot.
	 * Refuse W+X even if a future caller bypasses the normal RX/RW selection.
	 */
	if ((protection & (PROT_WRITE | PROT_EXEC)) ==
	    (PROT_WRITE | PROT_EXEC)) {
		errno = EACCES;
		return -1;
	}
	const auto current = slot_protections_.find(slot);
	if (current == slot_protections_.end()) {
		errno = EINVAL;
		return -1;
	}
	if (current->second == protection)
		return 0;
	mmu_trace_live_stacks("protect-shadow", slot, host_page_size());
	if (mprotect(reinterpret_cast<void *>(slot), host_page_size(),
		     protection) != 0)
		return -1;
	current->second = protection;
	return 0;
}

int NativeAddressSpace::update_slot_protection_locked(std::uintptr_t slot)
{
	int protection = PROT_NONE;
	bool populated = false;

	for (std::uintptr_t position = 0; position < host_page_size();
	     position += guest_page_size) {
		const auto page = slot + position;
		if (const auto found = mappings_.find(page);
		    found != mappings_.end()) {
			populated = true;
			protection |= shadow_data_protection(
				found->second.protection);
		}
	}
	if (!populated) {
		if (materialized_slots_.contains(slot)) {
			auto *registry = host_ownership_registry();
			if (registry == nullptr) {
				errno = ENOMEM;
				return -1;
			}
			if (registry->release_shadow(ownership_id_, slot) != 0)
				return -1;
		}
		materialized_slots_.erase(slot);
		slot_protections_.erase(slot);
		return 0;
	}
	if (!materialized_slots_.contains(slot) &&
	    materialize_slot_locked(slot) < 0)
		return -1;
	/* Darwin applies VM permissions to the complete host slot.  Every tracked
	 * mapping was made data-only above, so this transition can never add
	 * execute permission or become an iOS-incompatible RW-to-RX publication.
	 *
	 * Never publish a write-only slot.  vmalloc hands out 4 KiB pages, so a
	 * quarter this space does not track shares the transition, and a UML
	 * kernel stack a carrier is executing on is the common case: withdrawing
	 * read access faults that carrier on its own frame, which manifests far
	 * away as a corrupted pointer or a wild branch.  Guest read permission is
	 * enforced by translate() rather than by these host protections, so a
	 * readable slot costs the guest nothing, and flush_page_locked() needs the
	 * quarter readable to publish it anyway.
	 */
	if (protection != PROT_NONE)
		protection |= PROT_READ;
	return set_slot_protection_locked(slot, protection);
}

int NativeAddressSpace::map(
	std::uintptr_t address, int fd, std::uint64_t offset,
	std::uint64_t length, int protection)
{
	if (((address | offset | length) & (guest_page_size - 1)) != 0 ||
	    length == 0 || address + length < address ||
	    offset > static_cast<std::uint64_t>(
		std::numeric_limits<off_t>::max()) ||
	    length > static_cast<std::uint64_t>(
		std::numeric_limits<off_t>::max()) - offset) {
		errno = EINVAL;
		return -1;
	}
	BackingIdentity identity{};
	if (identify_backing(fd, identity) < 0)
		return -1;
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	const auto traced_page = mmu_trace_page();
	const bool trace = mmu_trace_enabled() && traced_page >= address &&
		traced_page - address < length;
	if (trace) {
		const auto existing = mappings_.find(traced_page);
		dprintf(STDERR_FILENO,
			"Darwin native target map request page=%#lx address=%#lx "
			"length=%#llx fd=%d offset=%#llx page-offset=%#llx "
			"protection=%#x existing=%d direct=%d\n",
			static_cast<unsigned long>(traced_page),
			static_cast<unsigned long>(address),
			static_cast<unsigned long long>(length), fd,
			static_cast<unsigned long long>(offset),
			static_cast<unsigned long long>(
				offset + traced_page - address), protection,
			existing != mappings_.end(),
			direct_mappings_.contains(slot_for(traced_page)));
	}
	/* Linux can repeat one 4 KiB PTE from an already-published, contiguous
	 * 16 KiB native alias. Keep that mapping shared when the backing page and
	 * permissions are unchanged. Converting the complete Darwin slot would be
	 * both unnecessary and unsafe when a sibling quarter carries this call's
	 * live kernel stack. */
	const int required_protection = direct_data_protection(protection);
	for (std::uint64_t position = 0; position < length;
	     position += guest_page_size) {
		const auto page = address + position;
		if (!live_kernel_stack_slot_locked(slot_for(page)))
			continue;
		const auto direct = direct_mappings_.find(slot_for(page));
		const auto existing = mappings_.find(page);
		bool same_direct = false;
		if (direct != direct_mappings_.end()) {
			BackingIdentity direct_identity{};
			same_direct =
				identify_backing(direct->second.fd, direct_identity) == 0 &&
				direct_identity == identity &&
				direct->second.offset + (page - slot_for(page)) ==
					offset + position &&
				direct_data_protection(direct->second.protection) ==
					required_protection;
		}
		const bool same_shadow = existing != mappings_.end() &&
			existing->second.identity == identity &&
			existing->second.offset == offset + position;
		if (!same_shadow && !same_direct) {
			errno = EBUSY;
			return -1;
		}
	}
	bool compatible_direct = true;
	for (std::uint64_t position = 0; position < length;
	     position += guest_page_size) {
		const auto page = address + position;
		const auto direct = direct_mappings_.find(slot_for(page));
		if (direct == direct_mappings_.end()) {
			compatible_direct = false;
			break;
		}
		BackingIdentity direct_identity{};
		if (identify_backing(direct->second.fd, direct_identity) < 0 ||
		    direct_identity != identity ||
		    direct->second.offset + (page - slot_for(page)) !=
			offset + position ||
		    direct_data_protection(direct->second.protection) !=
			required_protection) {
			compatible_direct = false;
			break;
		}
	}
	if (compatible_direct)
		return 0;
	const auto active_stack_slot = slot_for(reinterpret_cast<std::uintptr_t>(
		__builtin_frame_address(0)));
	for (auto slot = slot_for(address); slot < address + length;
	     slot += host_page_size())
		if (slot == active_stack_slot && direct_mappings_.contains(slot)) {
			errno = EBUSY;
			return -1;
		}
	DirectTransitionGuard transitions{*this,
		static_cast<std::size_t>(
			(slot_for(address + length - 1) - slot_for(address)) /
			host_page_size() + 1)};
	if (ensure_shadow_range_locked(address, length, transitions) < 0)
		return -1;
	struct PageSnapshot final {
		std::uintptr_t page;
		std::optional<Mapping> mapping;
		std::array<std::byte, guest_page_size> bytes{};
		bool bytes_valid{};
	};
	struct SlotSnapshot final {
		std::uintptr_t slot;
		int protection;
		bool materialized;
	};
	std::vector<PageSnapshot> pages;
	std::vector<SlotSnapshot> slots;
	using BackingIndex = decltype(backing_index_);
	std::vector<BackingIndex::iterator> inserted_index_entries;
	const auto page_count = static_cast<std::size_t>(length / guest_page_size);
	pages.reserve(page_count);
	slots.reserve(page_count);
	for (std::uint64_t position = 0; position < length;
	     position += guest_page_size) {
		const auto page = address + position;
		std::optional<Mapping> previous;
		if (const auto found = mappings_.find(page);
		    found != mappings_.end())
			previous = found->second;
		pages.push_back({page, std::move(previous)});
		const auto slot = slot_for(page);
		if (std::ranges::find_if(slots,
				[slot](const SlotSnapshot &entry) {
					return entry.slot == slot;
				}) != slots.end())
			continue;
		const bool materialized = materialized_slots_.contains(slot);
		const auto current = slot_protections_.find(slot);
		slots.push_back({slot,
			current == slot_protections_.end() ? PROT_NONE :
				current->second,
			materialized});
	}
	if (backing_index_ready_) {
		std::size_t changed_pages = 0;
		for (std::size_t index = 0; index < pages.size(); ++index) {
			const auto &previous = pages[index].mapping;
			const BackingPageKey key{
				identity, offset + index * guest_page_size};
			if (!previous || previous->identity != key.identity ||
			    previous->offset != key.offset)
				++changed_pages;
		}
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::native_backing_index);
		inserted_index_entries.reserve(changed_pages);
		reserve_additional(backing_index_, changed_pages);
		try {
			for (std::size_t index = 0; index < pages.size(); ++index) {
				const auto &previous = pages[index].mapping;
				const BackingPageKey key{
					identity, offset + index * guest_page_size};
				if (previous && previous->identity == key.identity &&
				    previous->offset == key.offset)
					continue;
				UML_MEMORY_ALLOCATION_CHECKPOINT(
					testing::AllocationSite::native_backing_index);
				inserted_index_entries.push_back(
					backing_index_.emplace(key, pages[index].page));
			}
		} catch (...) {
			for (const auto position : inserted_index_entries)
				backing_index_.erase(position);
			throw;
		}
	}

	const auto rollback = [&]() noexcept {
		for (const auto position : inserted_index_entries)
			backing_index_.erase(position);
		for (const auto &slot : slots)
			if (materialized_slots_.contains(slot.slot))
				(void)mprotect(reinterpret_cast<void *>(slot.slot),
					host_page_size(), PROT_READ | PROT_WRITE);
		for (const auto &page : pages)
			if (page.bytes_valid)
				std::copy(page.bytes.begin(), page.bytes.end(),
					reinterpret_cast<std::byte *>(page.page));
		for (const auto &page : pages) {
			if (!page.mapping) {
				mappings_.erase(page.page);
				continue;
			}
			if (auto found = mappings_.find(page.page);
			    found != mappings_.end())
				found->second = *page.mapping;
		}
		for (const auto &slot : slots) {
			if (!slot.materialized) {
				if (materialized_slots_.contains(slot.slot)) {
					if (auto *registry = host_ownership_registry();
					    registry != nullptr)
						(void)registry->release_shadow(
							ownership_id_, slot.slot);
				}
				materialized_slots_.erase(slot.slot);
				slot_protections_.erase(slot.slot);
				continue;
			}
			(void)mprotect(reinterpret_cast<void *>(slot.slot),
				host_page_size(), slot.protection);
			if (auto found = slot_protections_.find(slot.slot);
			    found != slot_protections_.end())
				found->second = slot.protection;
		}
	};

	try {
		/* Capture every existing quarter only after making its host slot
		 * readable and writable.  The saved bytes protect live siblings if a
		 * later page or bookkeeping allocation fails. */
		for (const auto &slot : slots)
			if (slot.materialized &&
			    set_slot_protection_locked(
				    slot.slot, PROT_READ | PROT_WRITE) < 0) {
				rollback();
				return -1;
			}
		for (auto &page : pages) {
			if (!materialized_slots_.contains(slot_for(page.page)))
				continue;
			std::copy_n(reinterpret_cast<const std::byte *>(page.page),
				guest_page_size, page.bytes.begin());
			page.bytes_valid = true;
		}

		for (std::size_t index = 0; index < pages.size(); ++index) {
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::native_operation);
			const auto page = pages[index].page;
			const auto page_offset = offset + index * guest_page_size;
			auto existing = mappings_.find(page);
			const bool same_backing = existing != mappings_.end() &&
				existing->second.identity == identity &&
				existing->second.offset == page_offset;
			if (same_backing) {
				existing->second.fd = fd;
				existing->second.protection = protection;
				/* A physical page can be freed, zeroed through its shared
				 * alias, and then installed at the same virtual address with
				 * the same backing offset. Its effective PTE protection can
				 * either remain unchanged or be republished with new access
				 * bits. In both cases the private 4 KiB shadow and baseline
				 * can still describe the previous object. Compare backing
				 * before treating an already-loaded remap as a no-op;
				 * reconcile_page_locked() preserves unflushed native writes
				 * when the backing itself has not changed. */
				if (existing->second.loaded) {
					if (reconcile_page_locked(page) < 0) {
						rollback();
						return -1;
					}
					continue;
				}
			} else {
				if (existing != mappings_.end() &&
				    flush_page_locked(page) < 0) {
					rollback();
					return -1;
				}
				UML_MEMORY_ALLOCATION_CHECKPOINT(
					testing::AllocationSite::native_mapping);
				note_shadow_offset_locked(page_offset);
				mappings_.insert_or_assign(page,
					Mapping{fd, page_offset, protection, identity});
			}
			/* Never replace an already materialized host slot.  Populate only
			 * the quarter whose Linux mapping changed. */
			if (materialize_slot_locked(slot_for(page)) < 0 ||
			    load_page_locked(page) < 0) {
				rollback();
				return -1;
			}
		}
		for (const auto &slot : slots)
			if (update_slot_protection_locked(slot.slot) < 0) {
				rollback();
				return -1;
			}
	} catch (...) {
		rollback();
		throw;
	}
	if (backing_index_ready_)
		for (std::size_t index = 0; index < pages.size(); ++index) {
			const auto &page = pages[index];
			if (!page.mapping ||
			    (page.mapping->identity == identity &&
			     page.mapping->offset ==
				offset + index * guest_page_size))
				continue;
			erase_backing_index_entry_locked(
				BackingPageKey{page.mapping->identity,
					page.mapping->offset}, page.page);
		}
	transitions.commit();
	if (trace) {
		const auto mapped = mappings_.find(traced_page);
		if (mapped != mappings_.end()) {
			dprintf(STDERR_FILENO,
				"Darwin native target map end page=%#lx fd=%d "
				"offset=%#llx loaded=%d current=%#llx/%#llx/%#llx\n",
				static_cast<unsigned long>(traced_page),
				mapped->second.fd,
				static_cast<unsigned long long>(mapped->second.offset),
				mapped->second.loaded,
				static_cast<unsigned long long>(mmu_trace_word(
					reinterpret_cast<const std::byte *>(traced_page),
					0x270)),
				static_cast<unsigned long long>(mmu_trace_word(
					reinterpret_cast<const std::byte *>(traced_page),
					0x278)),
				static_cast<unsigned long long>(mmu_trace_word(
					reinterpret_cast<const std::byte *>(traced_page),
					0x280)));
			std::size_t aliases = 0;
			for (const auto &[alias_page, alias] : mappings_)
				if (alias.identity == mapped->second.identity &&
				    alias.offset == mapped->second.offset) {
					++aliases;
					dprintf(STDERR_FILENO,
						"Darwin native target shadow alias page=%#lx "
						"loaded=%d protection=%#x\n",
						static_cast<unsigned long>(alias_page),
						alias.loaded, alias.protection);
				}
			std::size_t direct_aliases = 0;
			for (const auto &[alias_slot, alias] : direct_mappings_) {
				BackingIdentity alias_identity{};
				if (alias.fd != mapped->second.fd ||
				    mapped->second.offset < alias.offset ||
				    mapped->second.offset - alias.offset >= host_page_size() ||
				    identify_backing(alias.fd, alias_identity) < 0 ||
				    alias_identity != mapped->second.identity)
					continue;
				++direct_aliases;
				dprintf(STDERR_FILENO,
					"Darwin native target direct alias slot=%#lx "
					"address=%#lx offset=%#llx protection=%#x\n",
					static_cast<unsigned long>(alias_slot),
					static_cast<unsigned long>(alias_slot +
						(mapped->second.offset - alias.offset)),
					static_cast<unsigned long long>(alias.offset),
					alias.protection);
			}
			dprintf(STDERR_FILENO,
				"Darwin native target aliases shadow=%zu direct=%zu\n",
				aliases, direct_aliases);
		}
	}
	static const bool watch = std::getenv("UML_MMU_WATCH") != nullptr;
	if (watch && traced_page >= address && traced_page - address < length)
		__builtin_debugtrap();
	return 0;
}

std::size_t NativeAddressSpace::shadow_filter_bit(
	std::uint64_t offset) noexcept
{
	/* Backing offsets are page multiples; spread the page number. */
	return static_cast<std::size_t>(
		((offset / guest_page_size) * 0x9e3779b97f4a7c15ULL) >> (64 - 20));
}

bool NativeAddressSpace::may_alias_shadow(
	std::uint64_t offset, std::uint64_t length) const noexcept
{
	if (length / guest_page_size > shadow_filter_scan_pages)
		return true;
	const auto sequence =
		shadow_filter_sequence_.load(std::memory_order_seq_cst);
	if ((sequence & 1U) != 0)
		return true;
	for (std::uint64_t position = 0; position < length;
	     position += guest_page_size) {
		const auto bit = shadow_filter_bit(offset + position);
		if ((shadow_filter_[bit / 64].load(std::memory_order_seq_cst) &
		     (std::uint64_t{1} << (bit % 64))) != 0)
			return true;
	}
	return shadow_filter_sequence_.load(std::memory_order_seq_cst) !=
		sequence;
}

void NativeAddressSpace::note_shadow_offset_locked(
	std::uint64_t offset) noexcept
{
	/* Rebuild before the new bit: the mapping it stands for is not in
	 * mappings_ yet, and a rebuild knows only what is. */
	if (shadow_filter_population_ > 2 * mappings_.size() + 4096)
		rebuild_shadow_filter_locked();
	const auto bit = shadow_filter_bit(offset);
	const auto mask = std::uint64_t{1} << (bit % 64);
	auto &word = shadow_filter_[bit / 64];
	if ((word.load(std::memory_order_relaxed) & mask) != 0)
		return;
	word.fetch_or(mask, std::memory_order_seq_cst);
	++shadow_filter_population_;
}

void NativeAddressSpace::rebuild_shadow_filter_locked() noexcept
{
	shadow_filter_sequence_.fetch_add(1, std::memory_order_seq_cst);
	for (auto &word : shadow_filter_)
		word.store(0, std::memory_order_seq_cst);
	shadow_filter_population_ = 0;
	for (const auto &[page, mapping] : mappings_) {
		const auto bit = shadow_filter_bit(mapping.offset);
		const auto mask = std::uint64_t{1} << (bit % 64);
		auto &word = shadow_filter_[bit / 64];
		if ((word.load(std::memory_order_relaxed) & mask) != 0)
			continue;
		word.fetch_or(mask, std::memory_order_seq_cst);
		++shadow_filter_population_;
	}
	shadow_filter_sequence_.fetch_add(1, std::memory_order_seq_cst);
}

int NativeAddressSpace::publish_backing(
	int fd, std::uint64_t offset, std::uint64_t length)
{
	if (((offset | length) & (guest_page_size - 1)) != 0 || length == 0 ||
	    offset > static_cast<std::uint64_t>(
		std::numeric_limits<off_t>::max()) ||
	    length > static_cast<std::uint64_t>(
		std::numeric_limits<off_t>::max()) - offset) {
		errno = EINVAL;
		return -1;
	}
	if (!retired_.load(std::memory_order_acquire) &&
	    !may_alias_shadow(offset, length))
		return 0;
	BackingIdentity identity{};
	if (identify_backing(fd, identity) < 0)
		return -1;
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	const auto active_frame = reinterpret_cast<std::uintptr_t>(
		__builtin_frame_address(0));
	const auto active_page = active_frame & ~(guest_page_size - 1);
	const auto active_mapping = mappings_.find(active_page);
	if (mmu_trace_enabled() && active_mapping != mappings_.end() &&
	    active_mapping->second.identity == identity &&
	    active_mapping->second.offset >= offset &&
	    active_mapping->second.offset - offset < length)
		dprintf(STDERR_FILENO,
			"Darwin native publish-active frame=%#lx page=%#lx "
			"backing-offset=%#llx request-offset=%#llx length=%#llx\n",
			static_cast<unsigned long>(active_frame),
			static_cast<unsigned long>(active_page),
			static_cast<unsigned long long>(active_mapping->second.offset),
			static_cast<unsigned long long>(offset),
			static_cast<unsigned long long>(length));
	/* Keep every potentially throwing bookkeeping operation ahead of the first
	 * write to shared backing.  A failed software-map publication can therefore
	 * be retried without a half-built index or partially visible mapping. */
	UML_MEMORY_ALLOCATION_CHECKPOINT(
		testing::AllocationSite::native_backing_index);
	if (ensure_backing_index_locked() < 0)
		return -1;
	std::vector<std::uintptr_t> slots;
	for (std::uint64_t position = 0; position < length;
	     position += guest_page_size) {
		const BackingPageKey key{identity, offset + position};
		const auto [first, end] = backing_index_.equal_range(key);
		for (auto mapping = first; mapping != end; ++mapping) {
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::native_backing_index);
			slots.push_back(slot_for(mapping->second));
		}
	}
	std::ranges::sort(slots);
	slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
	std::vector<int> protections;
	UML_MEMORY_ALLOCATION_CHECKPOINT(
		testing::AllocationSite::native_backing_index);
	protections.reserve(slots.size());
	for (const auto slot : slots) {
		const auto protection = slot_protections_.find(slot);
		if (!materialized_slots_.contains(slot) ||
		    protection == slot_protections_.end()) {
			errno = EFAULT;
			return -1;
		}
		protections.push_back(protection->second);
	}
	const auto restore = [&](std::size_t count) noexcept {
		int result = 0;
		for (std::size_t index = 0; index < count; ++index)
			if (set_slot_protection_locked(
				slots[index], protections[index]) < 0)
				result = -1;
		return result;
	};
	std::size_t writable_slots = 0;
	for (; writable_slots < slots.size(); ++writable_slots)
		if (set_slot_protection_locked(slots[writable_slots],
			PROT_READ | PROT_WRITE) < 0) {
			const int saved_errno = errno;
			(void)restore(writable_slots);
			errno = saved_errno;
			return -1;
		}
	/* A Darwin protection applies to a complete 16 KiB host slot.  Once any
	 * quarter aliases a page being published, flush all dirty quarters in that
	 * slot.  Then reconcile every alias while all touched slots remain writable;
	 * otherwise the last alias to publish could leave an earlier alias stale. */
	for (const auto slot : slots)
		if (flush_slot_locked(slot) < 0) {
			const int saved_errno = errno;
			(void)restore(writable_slots);
			errno = saved_errno;
			return -1;
		}
	for (const auto slot : slots)
		for (std::uintptr_t position = 0; position < host_page_size();
		     position += guest_page_size)
			if (reconcile_page_locked(slot + position) < 0) {
				const int saved_errno = errno;
				(void)restore(writable_slots);
				errno = saved_errno;
				return -1;
			}
	if (restore(writable_slots) < 0)
		return -1;
	return 0;
}

int NativeAddressSpace::protect(
	std::uintptr_t address, std::uint64_t length, int protection)
{
	if (length == 0 || address + length < address ||
	    ((address | length) & (guest_page_size - 1)) != 0 ||
	    (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) {
		errno = EINVAL;
		return -1;
	}
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	DirectTransitionGuard transitions{*this,
		static_cast<std::size_t>(
			(slot_for(address + length - 1) - slot_for(address)) /
			host_page_size() + 1)};
	if (ensure_shadow_range_locked(address, length, transitions) < 0)
		return -1;
	struct PageProtection final {
		std::uintptr_t page;
		int protection;
	};
	struct SlotProtection final {
		std::uintptr_t slot;
		int protection;
	};
	const auto page_count = static_cast<std::size_t>(length / guest_page_size);
	std::vector<PageProtection> pages;
	std::vector<SlotProtection> slots;
	pages.reserve(page_count);
	slots.reserve(page_count);
	for (std::uint64_t position = 0; position < length;
	     position += guest_page_size) {
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::native_operation);
		const auto page = address + position;
		auto mapping = mappings_.find(page);
		if (mapping == mappings_.end()) {
			errno = ENOMEM;
			return -1;
		}
		pages.push_back({page, mapping->second.protection});
		const auto slot = slot_for(page);
		if (std::ranges::find_if(slots,
				[slot](const SlotProtection &entry) {
					return entry.slot == slot;
				}) != slots.end())
			continue;
		const auto current = slot_protections_.find(slot);
		if (!materialized_slots_.contains(slot) ||
		    current == slot_protections_.end()) {
			errno = EFAULT;
			return -1;
		}
		slots.push_back({slot, current->second});
	}
	if (std::ranges::all_of(pages, [protection](const auto &page) {
		return page.protection == protection;
	})) {
		transitions.commit();
		return 0;
	}
	const auto rollback = [&]() noexcept {
		for (const auto &page : pages)
			if (auto mapping = mappings_.find(page.page);
			    mapping != mappings_.end())
				mapping->second.protection = page.protection;
		for (const auto &slot : slots)
			(void)set_slot_protection_locked(slot.slot, slot.protection);
	};
	for (const auto &page : pages)
		mappings_.find(page.page)->second.protection = protection;
	try {
		for (const auto &slot : slots)
			if (update_slot_protection_locked(slot.slot) < 0) {
				const auto saved_errno = errno;
				rollback();
				errno = saved_errno;
				return -1;
			}
	} catch (...) {
		rollback();
		throw;
	}
	transitions.commit();
	return 0;
}

int NativeAddressSpace::unmap(
	std::uintptr_t address, std::uint64_t length)
{
	if (length == 0 || address + length < address ||
	    ((address | length) & (guest_page_size - 1)) != 0) {
		errno = EINVAL;
		return -1;
	}
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	for (auto slot = slot_for(address); slot < address + length;
	     slot += host_page_size())
		if (live_kernel_stack_slot_locked(slot)) {
			errno = EBUSY;
			return -1;
		}
	DirectTransitionGuard transitions{*this,
		static_cast<std::size_t>(
			(slot_for(address + length - 1) - slot_for(address)) /
			host_page_size() + 1)};
	if (ensure_shadow_range_locked(address, length, transitions) < 0)
		return -1;
	std::vector<std::uintptr_t> pages;
	std::vector<std::uintptr_t> slots;
	std::vector<std::pair<BackingPageKey, std::uintptr_t>> indexed_pages;
	address &= ~(guest_page_size - 1);
	for (std::uint64_t position = 0; position < length;
	     position += guest_page_size) {
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::native_operation);
		const auto page = address + position;
		pages.push_back(page);
		if (backing_index_ready_)
			if (const auto mapping = mappings_.find(page);
			    mapping != mappings_.end())
				indexed_pages.push_back({BackingPageKey{
					mapping->second.identity,
					mapping->second.offset}, page});
		const auto slot = slot_for(page);
		if (std::ranges::find(slots, slot) == slots.end())
			slots.push_back(slot);
	}
	for (const auto page : pages)
		if (flush_page_locked(page) < 0)
			return -1;
	for (const auto page : pages)
		mappings_.erase(page);
	for (const auto &[key, page] : indexed_pages)
		erase_backing_index_entry_locked(key, page);
	for (const auto slot : slots)
		if (update_slot_protection_locked(slot) < 0)
			return -1;
	transitions.commit();
	return 0;
}

int NativeAddressSpace::flush(
	std::uintptr_t address, std::uint64_t length)
{
	if (length == 0)
		return 0;
	if (address + length < address ||
	    ((address | length) & (guest_page_size - 1)) != 0) {
		errno = EINVAL;
		return -1;
	}
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	const auto first = slot_for(address);
	const auto end = address + length;
	for (auto slot = first; slot < end; slot += host_page_size())
		if (flush_slot_locked(slot) < 0)
			return -1;
	return 0;
}

int NativeAddressSpace::refresh(
	std::uintptr_t address, std::uint64_t length)
{
	if (length == 0)
		return 0;
	if (address + length < address) {
		errno = EINVAL;
		return -1;
	}
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	std::vector<std::uintptr_t> pages;
	std::vector<std::uintptr_t> slots;
	const auto first = address & ~(guest_page_size - 1);
	const auto end = address + length;
	for (auto page = first; page < end; page += guest_page_size) {
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::native_operation);
		if (!mappings_.contains(page))
			continue;
		pages.push_back(page);
		const auto slot = slot_for(page);
		if (std::ranges::find(slots, slot) == slots.end())
			slots.push_back(slot);
	}
	for (const auto page : pages) {
		/* Synchronize only the requested Linux page.  Rebuilding the host
		 * slot would overwrite unrelated live pages beneath other CPUs.
		 */
		if (flush_page_locked(page) < 0 ||
		    materialize_slot_locked(slot_for(page)) < 0 ||
		    load_page_locked(page) < 0)
			return -1;
	}
	for (const auto slot : slots)
		if (update_slot_protection_locked(slot) < 0)
			return -1;
	return 0;
}

int NativeAddressSpace::reload_from_backing(
	std::uintptr_t address, std::uint64_t length)
{
	if (length == 0)
		return 0;
	if (address + length < address) {
		errno = EINVAL;
		return -1;
	}
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	std::vector<std::uintptr_t> pages;
	std::vector<std::uintptr_t> slots;
	const auto first = address & ~(guest_page_size - 1);
	const auto end = address + length;
	for (auto page = first; page < end; page += guest_page_size) {
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::native_operation);
		if (!mappings_.contains(page))
			continue;
		pages.push_back(page);
		const auto slot = slot_for(page);
		if (std::ranges::find(slots, slot) == slots.end())
			slots.push_back(slot);
	}
	for (const auto page : pages)
		if (materialize_slot_locked(slot_for(page)) < 0 ||
		    reload_page_from_backing_locked(page) < 0)
			return -1;
	for (const auto slot : slots)
		if (update_slot_protection_locked(slot) < 0)
			return -1;
	return 0;
}

int NativeAddressSpace::retry(
	std::uintptr_t address, bool execute, bool write)
{
	std::lock_guard lock{mutex_};
	if (retired_.load(std::memory_order_acquire)) {
		errno = ESHUTDOWN;
		return -1;
	}
	const auto page = address & ~(guest_page_size - 1);
	const auto mapping = mappings_.find(page);
	if (mapping == mappings_.end() ||
	    !materialized_slots_.contains(slot_for(address)))
		return 0;
	const auto required = execute ? PROT_EXEC : write ? PROT_WRITE : PROT_READ;
	const auto intended = shadow_data_protection(mapping->second.protection);
	if ((intended & required) == 0)
		return 0;
	/* The slot union already contains every intended data permission.  Retry
	 * only reports whether Linux may retry the access; it never changes host
	 * VM protections.  In particular, execute can never be authorized here.
	 */
	return 1;
}

#if UML_MEMORY_ALLOCATION_TESTING
std::size_t NativeAddressSpace::mapping_count_for_testing() const noexcept
{
	std::lock_guard lock{mutex_};
	return retired_.load(std::memory_order_acquire) ? 0 : mappings_.size();
}

std::size_t
NativeAddressSpace::materialized_slot_count_for_testing() const noexcept
{
	std::lock_guard lock{mutex_};
	return retired_.load(std::memory_order_acquire) ? 0 :
		materialized_slots_.size();
}

std::size_t NativeAddressSpace::direct_mapping_count_for_testing() const
	noexcept
{
	std::lock_guard lock{mutex_};
	return retired_.load(std::memory_order_acquire) ? 0 :
		direct_slots_.size();
}
#endif
#endif

NativeAddressSpace::~NativeAddressSpace()
{
	retire();
}

void NativeAddressSpace::retire() noexcept
{
	std::lock_guard lock{mutex_};
	if (retired_.exchange(true, std::memory_order_acq_rel))
		return;
#if DARWIN_UML_GUEST_PAGE_SIZE == 4096
	active_kernel_stacks_.clear();
	for (const auto slot : materialized_slots_)
		(void)mprotect(reinterpret_cast<void *>(slot), host_page_size(),
			PROT_READ | PROT_WRITE);
	for (const auto slot : materialized_slots_)
		(void)flush_slot_locked(slot);
	mappings_.clear();
	backing_index_.clear();
	slot_protections_.clear();
	materialized_slots_.clear();
	direct_mappings_.clear();
	backing_index_ready_ = false;
#endif
	if (auto *registry = host_ownership_registry(); registry != nullptr)
		registry->retire(ownership_id_);
	direct_slots_.clear();
}

namespace {

class NativeAddressSpaceRegistry final {
public:
	[[nodiscard]] std::shared_ptr<NativeAddressSpace> current() noexcept
	{
		for (;;) {
			const auto retirement_sequence = domain_retirement_sequence();
			std::uint64_t domain;
			if (!resolve_current_domain(domain)) {
				errno = ESRCH;
				return {};
			}
			try {
				{
					std::shared_lock lock{mutex_};
					if (const auto found = spaces_.find(domain);
					    found != spaces_.end()) {
						if (domain_retirement_sequence() !=
						    retirement_sequence)
							continue;
						return found->second;
					}
				}
				std::lock_guard lock{mutex_};
				if (domain_retirement_sequence() != retirement_sequence)
					continue;
				if (const auto found = spaces_.find(domain);
				    found != spaces_.end())
					return found->second;
				auto space = std::make_shared<NativeAddressSpace>();
				/* The shared retirement sequence advances before retire() acquires
				 * mutex_. A delayed resolver therefore cannot publish an old domain;
				 * retirement that begins after this check removes the entry. */
				if (domain_retirement_sequence() != retirement_sequence)
					continue;
				spaces_.emplace(domain, space);
				return space;
			} catch (const std::bad_alloc &) {
				errno = ENOMEM;
				return {};
			} catch (...) {
				errno = EIO;
				return {};
			}
		}
	}

	void retire(std::uint64_t domain) noexcept
	{
		std::shared_ptr<NativeAddressSpace> retired;
		{
			std::lock_guard lock{mutex_};
			if (const auto found = spaces_.find(domain);
			    found != spaces_.end()) {
				retired = std::move(found->second);
				spaces_.erase(found);
			}
		}
		if (retired)
			retired->retire();
	}

#if UML_MEMORY_ALLOCATION_TESTING
	[[nodiscard]] std::size_t size_for_testing() const noexcept
	{
		std::shared_lock lock{mutex_};
		return spaces_.size();
	}
#endif

private:
	mutable WordSharedMutex mutex_;
	std::unordered_map<std::uint64_t,
		std::shared_ptr<NativeAddressSpace>> spaces_;
};

void retire_native_domain(std::uint64_t domain) noexcept;

std::atomic<NativeAddressSpaceRegistry *> published_native_address_space_registry{};
std::atomic_flag native_address_space_registry_initialization = ATOMIC_FLAG_INIT;

NativeAddressSpaceRegistry *native_address_space_registry() noexcept
{
	if (auto *registry = published_native_address_space_registry.load(
		    std::memory_order_acquire); registry != nullptr)
		return registry;
	while (native_address_space_registry_initialization.test_and_set(
		       std::memory_order_acquire))
		std::this_thread::yield();
	struct InitializationUnlock final {
		~InitializationUnlock()
		{
			native_address_space_registry_initialization.clear(
				std::memory_order_release);
		}
	};
	[[maybe_unused]] InitializationUnlock unlock;
	if (auto *registry = published_native_address_space_registry.load(
		    std::memory_order_acquire); registry != nullptr)
		return registry;
	try {
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::native_domain_registry);
		auto *registry = new (std::nothrow) NativeAddressSpaceRegistry;
		if (registry != nullptr) {
			/* Publish the retirement callback first. A concurrent retirement
			 * then waits for this initializer and cannot miss the registry. */
			set_domain_retire_observer(&retire_native_domain);
			published_native_address_space_registry.store(
				registry, std::memory_order_release);
		}
		return registry;
	} catch (...) {
		return nullptr;
	}
}

void retire_native_domain(std::uint64_t domain) noexcept
{
	if (auto *registry = native_address_space_registry(); registry != nullptr)
		registry->retire(domain);
}

} // namespace

std::shared_ptr<NativeAddressSpace> native_address_space() noexcept
{
	try {
		errno = 0;
		if (auto *registry = native_address_space_registry(); registry != nullptr)
			return registry->current();
		errno = ENOMEM;
		return {};
	} catch (const std::bad_alloc &) {
		errno = ENOMEM;
		return {};
	} catch (...) {
		errno = EIO;
		return {};
	}
}

#if UML_MEMORY_ALLOCATION_TESTING
namespace testing {

std::size_t native_domain_count() noexcept
{
	if (auto *registry = native_address_space_registry(); registry != nullptr)
		return registry->size_for_testing();
	return 0;
}

std::size_t native_host_mapping_count() noexcept
{
	if (auto *registry = host_ownership_registry(); registry != nullptr)
		return registry->size_for_testing();
	return std::numeric_limits<std::size_t>::max();
}

} // namespace testing
#endif

} // namespace uml::memory
