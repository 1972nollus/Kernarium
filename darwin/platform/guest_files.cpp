// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_files.hpp"

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace uml::platform {

namespace {

struct Record final {
	dev_t device;
	ino_t inode;
	bool recorded;
};

std::mutex registryMutex;
std::vector<Record> registry;

[[nodiscard]] bool flushDescriptor(int descriptor) noexcept
{
	/* fsync alone leaves the write in the device's own cache on Darwin. The
	 * caller asked for durability across a host that may not come back, so
	 * ask for the barrier first and accept the weaker flush only where the
	 * filesystem cannot provide one. */
	int result;
	do {
		result = ::fcntl(descriptor, F_FULLFSYNC);
	} while (result < 0 && errno == EINTR);
	if (result >= 0)
		return true;
	do {
		result = ::fsync(descriptor);
	} while (result < 0 && errno == EINTR);
	return result >= 0;
}

[[nodiscard]] bool stillNames(int descriptor, const Record &record) noexcept
{
	struct stat details {};
	return ::fstat(descriptor, &details) == 0 &&
		details.st_dev == record.device &&
		details.st_ino == record.inode;
}

} // namespace

void record_guest_file(int descriptor, int openFlags) noexcept
{
	if (descriptor < 0 || (openFlags & O_ACCMODE) == O_RDONLY)
		return;
	struct stat details {};
	if (::fstat(descriptor, &details) != 0)
		return;
	const auto index = static_cast<std::size_t>(descriptor);
	try {
		std::lock_guard lock(registryMutex);
		if (registry.size() <= index)
			registry.resize(index + 1U, Record {});
		registry[index] = {details.st_dev, details.st_ino, true};
	} catch (...) {
		/* An unrecorded descriptor is flushed by the guest's own writeback
		 * instead of by a host request; losing the record must not fail the
		 * open that the guest is performing. */
	}
}

std::size_t forget_guest_files() noexcept
{
	std::lock_guard lock(registryMutex);
	std::size_t forgotten = 0;
	for (const Record &record : registry)
		forgotten += record.recorded ? 1U : 0U;
	/* The storage goes with the records: a descriptor table that once grew
	 * large must not keep its vector. */
	std::vector<Record>().swap(registry);
	return forgotten;
}

GuestFileSyncResult sync_guest_files() noexcept
{
	GuestFileSyncResult result {};
	std::lock_guard lock(registryMutex);
	for (std::size_t index = 0; index < registry.size(); ++index) {
		Record &record = registry[index];
		if (!record.recorded)
			continue;
		const auto descriptor = static_cast<int>(index);
		if (!stillNames(descriptor, record)) {
			record = Record {};
			++result.retired;
			continue;
		}
		if (flushDescriptor(descriptor))
			++result.flushed;
		else
			++result.failed;
	}
	return result;
}

} // namespace uml::platform
