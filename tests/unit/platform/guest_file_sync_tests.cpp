// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_files.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

void require(bool condition)
{
	if (!condition)
		std::abort();
}

std::filesystem::path scratchDirectory()
{
	static const auto directory = [] {
		auto candidate = std::filesystem::temp_directory_path() /
			("uml-guest-file-sync-" + std::to_string(::getpid()));
		std::filesystem::create_directories(candidate);
		return candidate;
	}();
	return directory;
}

/* The registry is process-global, and each case here closes what it opened.
 * Draining first retires those stale records so a case reasons only about its
 * own descriptors. */
void drainRegistry()
{
	(void)uml::platform::sync_guest_files();
}

int openScratch(const char *name, int flags)
{
	const auto path = scratchDirectory() / name;
	const int descriptor = ::open(path.c_str(), flags | O_CREAT, 0600);
	require(descriptor >= 0);
	return descriptor;
}

void aWritableDescriptorIsFlushed()
{
	drainRegistry();
	const int descriptor = openScratch("writable", O_RDWR);
	uml::platform::record_guest_file(descriptor, O_RDWR);
	require(::write(descriptor, "guest", 5) == 5);
	const auto result = uml::platform::sync_guest_files();
	require(result.flushed == 1U);
	require(result.failed == 0U);
	require(::close(descriptor) == 0);
}

void aReadOnlyDescriptorIsNotRecorded()
{
	drainRegistry();
	const int descriptor = openScratch("read-only", O_RDONLY);
	uml::platform::record_guest_file(descriptor, O_RDONLY);
	const auto result = uml::platform::sync_guest_files();
	require(result.flushed == 0U);
	require(result.failed == 0U);
	require(::close(descriptor) == 0);
}

/* Closing needs no hook: the record remembers the file it described, so a
 * descriptor that no longer names it is retired instead of flushed. */
void aClosedDescriptorIsRetired()
{
	drainRegistry();
	const int descriptor = openScratch("closed", O_WRONLY);
	uml::platform::record_guest_file(descriptor, O_WRONLY);
	require(::close(descriptor) == 0);
	const auto result = uml::platform::sync_guest_files();
	require(result.flushed == 0U);
	require(result.failed == 0U);
	require(result.retired == 1U);
}

/* The host may reuse a descriptor number for something the guest never opened.
 * Flushing that would be wrong, so identity decides rather than the number. */
void aReusedDescriptorIsRetiredRatherThanFlushed()
{
	drainRegistry();
	const int first = openScratch("reused-first", O_RDWR);
	uml::platform::record_guest_file(first, O_RDWR);
	require(::close(first) == 0);
	const int second = openScratch("reused-second", O_RDWR);
	require(second == first);
	const auto result = uml::platform::sync_guest_files();
	require(result.flushed == 0U);
	require(result.failed == 0U);
	require(result.retired == 1U);
	require(::close(second) == 0);
}

void everyRecordedDescriptorIsFlushed()
{
	drainRegistry();
	const int first = openScratch("many-first", O_RDWR);
	const int second = openScratch("many-second", O_RDWR);
	uml::platform::record_guest_file(first, O_RDWR);
	uml::platform::record_guest_file(second, O_RDWR);
	const auto result = uml::platform::sync_guest_files();
	require(result.flushed == 2U);
	require(result.failed == 0U);
	require(::close(first) == 0);
	require(::close(second) == 0);
}

/* A guest can open something that has no stable storage behind it. That must
 * be reported rather than counted as durable, and must not stop the files
 * beside it from reaching disk. */
void anUnflushableDescriptorIsReportedWithoutStoppingTheRest()
{
	drainRegistry();
	int pair[2] {-1, -1};
	require(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	const int file = openScratch("beside-socket", O_RDWR);
	uml::platform::record_guest_file(pair[0], O_RDWR);
	uml::platform::record_guest_file(file, O_RDWR);
	const auto result = uml::platform::sync_guest_files();
	require(result.failed == 1U);
	require(result.flushed == 1U);
	require(::close(pair[0]) == 0);
	require(::close(pair[1]) == 0);
	require(::close(file) == 0);
}

void anEmptyRegistryFlushesNothing()
{
	const auto result = uml::platform::sync_guest_files();
	require(result.flushed == 0U);
	require(result.failed == 0U);
}

} // namespace

int main()
{
	anEmptyRegistryFlushesNothing();
	aWritableDescriptorIsFlushed();
	aReadOnlyDescriptorIsNotRecorded();
	aClosedDescriptorIsRetired();
	aReusedDescriptorIsRetiredRatherThanFlushed();
	everyRecordedDescriptorIsFlushed();
	anUnflushableDescriptorIsReportedWithoutStoppingTheRest();
	std::filesystem::remove_all(scratchDirectory());
	std::puts("guest file sync tests passed");
	return 0;
}
