// SPDX-License-Identifier: GPL-2.0-or-later
#include "session_record.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <time.h>
#include <unistd.h>

namespace uml::runtime {
namespace {

/* The kernel output a record keeps: enough for a panic's message, its stack
 * and the lines before it. */
constexpr std::size_t output_size = 4096;
constexpr std::size_t console_slots = 4;
/* Darwin's limit on a thread name, its terminator included. */
constexpr std::size_t thread_name_size = 64;
constexpr std::string_view panic_prefix{"Darwin UML panic: "};
/* What the kernel's handler (os-Darwin/signal.c) writes, one write each, when
 * a host thread that owns no vCPU faults: the registers, then this. */
constexpr std::string_view host_fault_prefix{"Darwin host fault: "};
constexpr std::string_view host_fault_registers{"Darwin host fault: signal="};
constexpr std::string_view host_fault_line{
	"Darwin host fault: taken on a host thread which owns no virtual CPU"};

std::atomic<char> output[output_size];
std::atomic<std::uint64_t> output_end{0};
std::atomic<int> console_descriptors[console_slots] = {-1, -1, -1, -1};

std::atomic<int> record_descriptor{-1};
/* True while no end has been recorded for the session that began last. */
std::atomic<bool> session_open{false};
std::atomic<std::int64_t> session_started{0};
std::atomic<bool> exit_hook_installed{false};
std::atomic<bool> crash_recorded{false};

/* The record and the output it quotes: static, so an end formats nothing on
 * a stack it does not know the size of. One end writes them, the first. */
char record_text[output_size + 1024];
char tail_text[output_size];

bool kept(int descriptor) noexcept
{
	if (descriptor == STDOUT_FILENO || descriptor == STDERR_FILENO)
		return true;
	for (const auto &console : console_descriptors)
		if (console.load(std::memory_order_relaxed) == descriptor)
			return true;
	return false;
}

void keep_output(const void *data, std::size_t length) noexcept
{
	const auto *bytes = static_cast<const char *>(data);
	if (length > output_size) {
		bytes += length - output_size;
		length = output_size;
	}
	const std::uint64_t start =
		output_end.fetch_add(length, std::memory_order_acq_rel);
	for (std::size_t index = 0; index < length; ++index)
		output[(start + index) % output_size].store(bytes[index],
			std::memory_order_relaxed);
}

/* Formats into a fixed buffer; what does not fit is dropped. */
class Text final {
public:
	Text(char *data, std::size_t size) noexcept : data_{data}, size_{size} {}

	Text &text(std::string_view value) noexcept
	{
		const std::size_t count = std::min(value.size(), size_ - used_);
		std::memcpy(data_ + used_, value.data(), count);
		used_ += count;
		return *this;
	}

	Text &number(std::int64_t value) noexcept
	{
		char digits[24];
		const auto result =
			std::to_chars(digits, digits + sizeof(digits), value);
		return text({digits, static_cast<std::size_t>(result.ptr - digits)});
	}

	/* `value` with every byte that is not printable ASCII, or a line feed
	 * where `lines` allows one, replaced; carriage returns are dropped. */
	Text &printable(std::string_view value, bool lines) noexcept
	{
		for (const char character : value) {
			if (used_ == size_)
				break;
			if (character == '\r')
				continue;
			const bool keep = (character >= ' ' && character < 0x7f) ||
				(lines && (character == '\n' || character == '\t'));
			data_[used_++] = keep ? character : '?';
		}
		return *this;
	}

	[[nodiscard]] std::size_t size() const noexcept
	{
		return used_;
	}

private:
	char *data_;
	std::size_t size_;
	std::size_t used_{};
};

std::int64_t now() noexcept
{
	timespec time{};
	(void)clock_gettime(CLOCK_REALTIME, &time);
	return static_cast<std::int64_t>(time.tv_sec);
}

void write_record(SessionEnd end, int status, std::string_view message,
	std::string_view tail) noexcept
{
	const int descriptor = record_descriptor.load(std::memory_order_acquire);
	if (descriptor < 0)
		return;
	Text record{record_text, sizeof(record_text)};
	record.text("uml-session 1\nstate ")
		.text(end == SessionEnd::running ? "running" : "ended")
		.text("\nend ").text(session_end_name(end))
		.text("\nstatus ").number(status)
		.text("\npid ").number(getpid())
		.text("\nstarted ")
		.number(session_started.load(std::memory_order_relaxed));
	if (end != SessionEnd::running)
		record.text("\nended ").number(now());
	if (!message.empty())
		record.text("\nmessage ").printable(message, false);
	record.text("\n");
	if (!tail.empty())
		record.text("kernel-output\n").printable(tail, true);
	ssize_t written;
	do {
		written = pwrite(descriptor, record_text, record.size(), 0);
	} while (written < 0 && errno == EINTR);
	if (written == static_cast<ssize_t>(record.size()))
		(void)ftruncate(descriptor, static_cast<off_t>(record.size()));
}

std::string_view output_tail() noexcept
{
	return {tail_text, kernel_output_tail(tail_text)};
}

/* Records the session's first end; `quote_output` adds the kernel's last
 * output, which only the first end reads into the static copy. */
void end_session(SessionEnd end, int status, std::string_view message,
	bool quote_output) noexcept
{
	if (!session_open.exchange(false, std::memory_order_acq_rel))
		return;
	write_record(end, status, message,
		quote_output ? output_tail() : std::string_view{});
}

void record_process_exit()
{
	end_session(SessionEnd::process_exit, 0,
		"exit() was called while the guest ran", true);
}

/* The signal number in a host-fault report's registers, else 0. */
int fault_signal(std::string_view registers) noexcept
{
	constexpr std::string_view key{"signal="};
	if (!registers.starts_with(key))
		return 0;
	int signal_number = 0;
	(void)std::from_chars(registers.data() + key.size(),
		registers.data() + registers.size(), signal_number);
	return signal_number;
}

void record_host_fault() noexcept
{
	if (crash_recorded.exchange(true, std::memory_order_acq_rel))
		return;
	if (!session_open.exchange(false, std::memory_order_acq_rel))
		return;
	const auto tail = output_tail();
	const auto registers = host_fault_message(tail);
	write_record(SessionEnd::host_fault, fault_signal(registers), registers,
		tail);
}

void name_thread(std::string_view message) noexcept
{
	char name[thread_name_size];
	Text text{name, sizeof(name) - 1};
	text.text("panic: ").printable(message, false);
	name[text.size()] = '\0';
	(void)pthread_setname_np(name);
}

} // namespace

std::string_view session_end_name(SessionEnd end) noexcept
{
	switch (end) {
	case SessionEnd::running:
		return "running";
	case SessionEnd::powered_off:
		return "powered-off";
	case SessionEnd::restarted:
		return "restarted";
	case SessionEnd::stopped:
		return "stopped";
	case SessionEnd::failed:
		return "failed";
	case SessionEnd::guest_panic:
		return "guest-panic";
	case SessionEnd::kernel_abort:
		return "kernel-abort";
	case SessionEnd::kernel_exit:
		return "kernel-exit";
	case SessionEnd::hibernation_restore_failed:
		return "hibernation-restore-failed";
	case SessionEnd::process_exit:
		return "process-exit";
	case SessionEnd::host_fault:
		return "host-fault";
	}
	return "unknown";
}

std::size_t console_output_descriptors(
	std::span<const std::string> kernel_arguments, std::span<int> descriptors)
	noexcept
{
	std::size_t found = 0;
	for (const auto &argument : kernel_arguments) {
		const std::string_view text{argument};
		const auto equals = text.find('=');
		if (equals == std::string_view::npos)
			continue;
		const auto name = text.substr(0, equals);
		const auto channel = name.starts_with("con") ? name.substr(3) :
			name.starts_with("ssl") ? name.substr(3) :
			std::string_view{"-"};
		if (!std::all_of(channel.begin(), channel.end(),
			    [](char c) { return c >= '0' && c <= '9'; }))
			continue;
		/* fd:IN,fd:OUT writes to OUT; fd:BOTH to BOTH. */
		auto value = text.substr(equals + 1);
		if (const auto comma = value.find(','); comma != value.npos)
			value = value.substr(comma + 1);
		if (!value.starts_with("fd:"))
			continue;
		value.remove_prefix(3);
		int descriptor = -1;
		const auto parsed = std::from_chars(value.data(),
			value.data() + value.size(), descriptor);
		if (parsed.ec != std::errc{} ||
		    parsed.ptr != value.data() + value.size() || descriptor < 0)
			continue;
		if (found < descriptors.size())
			descriptors[found] = descriptor;
		++found;
	}
	return std::min(found, descriptors.size());
}

bool begin_session_record(std::string_view path,
	std::span<const std::string> kernel_arguments) noexcept
{
	int consoles[console_slots] = {-1, -1, -1, -1};
	(void)console_output_descriptors(kernel_arguments, consoles);
	for (std::size_t slot = 0; slot < console_slots; ++slot)
		console_descriptors[slot].store(consoles[slot],
			std::memory_order_relaxed);
	if (path.empty())
		return true;
	const std::string name{path};
	int descriptor;
	do {
		descriptor = open(name.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC,
			0600);
	} while (descriptor < 0 && errno == EINTR);
	if (descriptor < 0) {
		const int error = errno;
		dprintf(STDERR_FILENO,
			"uml: the session record cannot be opened: %s\n",
			strerror(error));
		return false;
	}
	if (const int previous = record_descriptor.exchange(descriptor,
		    std::memory_order_acq_rel);
	    previous >= 0)
		(void)close(previous);
	if (!exit_hook_installed.exchange(true))
		(void)atexit(record_process_exit);
	session_started.store(now(), std::memory_order_relaxed);
	session_open.store(true, std::memory_order_release);
	write_record(SessionEnd::running, 0, {}, {});
	return true;
}

void end_session_record(SessionEnd end, int status,
	std::string_view message) noexcept
{
	end_session(end, status, message, end != SessionEnd::powered_off &&
		end != SessionEnd::restarted && end != SessionEnd::stopped);
}

void record_kernel_crash() noexcept
{
	/* A second vCPU that crashes while the first records leaves the first
	 * one's record and name alone. */
	if (crash_recorded.exchange(true, std::memory_order_acq_rel))
		return;
	const auto tail = output_tail();
	const auto message = panic_message(tail);
	if (!message.empty())
		name_thread(message);
	if (!session_open.exchange(false, std::memory_order_acq_rel))
		return;
	write_record(message.empty() ? SessionEnd::kernel_abort :
		SessionEnd::guest_panic, SIGABRT, message, tail);
}

ssize_t kernel_write(int descriptor, const void *buffer, std::size_t length)
	noexcept
{
	if (length != 0 && buffer != nullptr && kept(descriptor)) {
		keep_output(buffer, length);
		/* The fault ends the process as soon as the handler returns. */
		if (descriptor == STDERR_FILENO &&
		    std::string_view{static_cast<const char *>(buffer), length}
			    .starts_with(host_fault_line))
			record_host_fault();
	}
	return write(descriptor, buffer, length);
}

void *kernel_output_import(const char *name) noexcept
{
	if (name != nullptr && name[0] == '_')
		++name;
	return name != nullptr && std::strcmp(name, "write") == 0 ?
		reinterpret_cast<void *>(&kernel_write) : nullptr;
}

std::size_t kernel_output_tail(std::span<char> buffer) noexcept
{
	const std::uint64_t end = output_end.load(std::memory_order_acquire);
	const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(
		{end, static_cast<std::uint64_t>(output_size),
			static_cast<std::uint64_t>(buffer.size())}));
	for (std::size_t index = 0; index < count; ++index)
		buffer[index] = output[(end - count + index) % output_size].load(
			std::memory_order_relaxed);
	return count;
}

std::string_view panic_message(std::string_view output_text) noexcept
{
	const auto start = output_text.rfind(panic_prefix);
	if (start == std::string_view::npos)
		return {};
	auto message = output_text.substr(start + panic_prefix.size());
	if (const auto end = message.find('\n'); end != message.npos)
		message = message.substr(0, end);
	return message;
}

std::string_view host_fault_message(std::string_view output_text) noexcept
{
	const auto start = output_text.rfind(host_fault_registers);
	if (start == std::string_view::npos)
		return {};
	auto message = output_text.substr(start + host_fault_prefix.size());
	if (const auto end = message.find('\n'); end != message.npos)
		message = message.substr(0, end);
	return message;
}

void reset_session_record() noexcept
{
	session_open.store(false, std::memory_order_release);
	if (const int descriptor = record_descriptor.exchange(-1); descriptor >= 0)
		(void)close(descriptor);
	for (auto &console : console_descriptors)
		console.store(-1, std::memory_order_relaxed);
	output_end.store(0, std::memory_order_release);
	crash_recorded.store(false, std::memory_order_release);
}

} // namespace uml::runtime
