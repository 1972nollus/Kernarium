// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_statistics.hpp"

#include "memory/guest_ram_commitment.hpp"

#include <cstddef>
#include <cstring>
#include <mach/mach.h>
#include <mutex>

namespace uml::runtime {

namespace {

/* Leaf: nothing is taken under it. */
std::mutex ram_retirement_mutex;

constexpr std::string_view carrier_prefix = "uml-vcpu-";
constexpr std::string_view carrier_suffix = "-carrier";

} // namespace

bool is_vcpu_carrier_name(std::string_view name) noexcept
{
	if (!name.starts_with(carrier_prefix) || !name.ends_with(carrier_suffix) ||
	    name.size() <= carrier_prefix.size() + carrier_suffix.size())
		return false;
	name.remove_prefix(carrier_prefix.size());
	name.remove_suffix(carrier_suffix.size());
	for (const char digit : name)
		if (digit < '0' || digit > '9')
			return false;
	return true;
}

GuestCpuTime guest_cpu_time() noexcept
{
	thread_act_array_t threads = nullptr;
	mach_msg_type_number_t count = 0;
	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
		return {};
	GuestCpuTime total;
	total.listed = true;
	for (mach_msg_type_number_t index = 0; index < count; ++index) {
		thread_extended_info_data_t information {};
		mach_msg_type_number_t information_count = THREAD_EXTENDED_INFO_COUNT;
		if (thread_info(threads[index], THREAD_EXTENDED_INFO,
			    reinterpret_cast<thread_info_t>(&information),
			    &information_count) == KERN_SUCCESS &&
		    is_vcpu_carrier_name(std::string_view {information.pth_name,
			    ::strnlen(information.pth_name,
				    sizeof(information.pth_name))})) {
			++total.carriers;
			total.nanoseconds +=
				information.pth_user_time + information.pth_system_time;
		}
		(void)mach_port_deallocate(mach_task_self(), threads[index]);
	}
	(void)vm_deallocate(mach_task_self(),
		reinterpret_cast<vm_address_t>(threads),
		count * sizeof(thread_act_t));
	return total;
}

std::optional<GuestRamUsage> guest_ram_usage() noexcept
{
	/* A hand-back in progress holds the lock: the RAM is being given back,
	 * and the reader does not wait for it. */
	const std::unique_lock lock {ram_retirement_mutex, std::try_to_lock};
	if (!lock.owns_lock())
		return std::nullopt;
	const ::uml::memory::GuestRamCommitment *const commitment =
		::uml::memory::guest_ram_commitment();
	if (commitment == nullptr)
		return std::nullopt;
	const std::uint64_t ram = commitment->size();
	const std::uint64_t released = commitment->released_bytes();
	return GuestRamUsage {.ram_bytes = ram,
		.held_bytes = released < ram ? ram - released : 0U};
}

GuestRamRetirementScope::GuestRamRetirementScope() noexcept
{
	ram_retirement_mutex.lock();
}

GuestRamRetirementScope::~GuestRamRetirementScope()
{
	ram_retirement_mutex.unlock();
}

} // namespace uml::runtime
