// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The session record (runtime/session_record): the console descriptors the
 * kernel arguments name, the kernel output it keeps, the panic message it
 * finds, and the record file through a session's begin, its first end, a
 * kernel crash and an exit() of the process, which runs in a child.
 */
#include "session_record.hpp"
#include "require.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using uml::runtime::SessionEnd;
using uml::testing::require;

std::string read_file(const std::string &path)
{
	std::string text;
	if (FILE *file = std::fopen(path.c_str(), "r"); file != nullptr) {
		char buffer[4096];
		std::size_t count;
		while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
			text.append(buffer, count);
		std::fclose(file);
	}
	return text;
}

bool has(const std::string &text, const char *part)
{
	return text.find(part) != std::string::npos;
}

std::string temporary_record()
{
	char path[] = "/tmp/uml-session-record-XXXXXX";
	const int descriptor = mkstemp(path);
	require(descriptor >= 0, "no temporary record");
	close(descriptor);
	return path;
}

std::string tail()
{
	std::vector<char> buffer(8192);
	return {buffer.data(), uml::runtime::kernel_output_tail(buffer)};
}

void test_names()
{
	require(uml::runtime::session_end_name(SessionEnd::running) == "running");
	require(uml::runtime::session_end_name(SessionEnd::guest_panic) ==
		"guest-panic");
	require(uml::runtime::session_end_name(
		SessionEnd::hibernation_restore_failed) ==
		"hibernation-restore-failed");
	require(uml::runtime::session_end_name(SessionEnd::process_exit) ==
		"process-exit");
	require(uml::runtime::session_end_name(SessionEnd::host_fault) ==
		"host-fault");
}

void test_console_descriptors()
{
	const std::vector<std::string> arguments{"console=ttyS0",
		"ssl0=fd:7,fd:7", "con=fd:0,fd:1", "con1=fd:3", "ssl=pts",
		"conx=fd:4", "ssl1=fd:-1", "ssl2=fd:9x", "mem=512M"};
	int found[4] = {-1, -1, -1, -1};
	require(uml::runtime::console_output_descriptors(arguments, found) == 3,
		"the console descriptors were miscounted");
	require(found[0] == 7 && found[1] == 1 && found[2] == 3,
		"a console's output descriptor was misread");
	int one[1] = {-1};
	require(uml::runtime::console_output_descriptors(arguments, one) == 1 &&
		one[0] == 7, "a short list was overrun");
}

void test_panic_message()
{
	using uml::runtime::panic_message;
	require(panic_message("no panic here\n").empty());
	require(panic_message("x\nDarwin UML panic: first\nDarwin UML panic: "
		"Attempted to kill init! exitcode=0x0000000b\nmore\n") ==
		"Attempted to kill init! exitcode=0x0000000b",
		"the last panic line was not the message");
	require(panic_message("Darwin UML panic: unterminated") ==
		"unterminated");
}

void test_kept_output()
{
	uml::runtime::reset_session_record();
	int console[2];
	require(pipe(console) == 0);
	int other[2];
	require(pipe(other) == 0);
	const std::vector<std::string> arguments{"ssl0=fd:" +
		std::to_string(console[1])};
	require(uml::runtime::begin_session_record({}, arguments),
		"a session without a record did not begin");
	require(uml::runtime::kernel_write(console[1], "console line\n", 13) ==
		13, "the console write did not reach its descriptor");
	require(uml::runtime::kernel_write(other[1], "elsewhere\n", 10) == 10);
	std::string kept = tail();
	require(kept == "console line\n", "the kept output is wrong: " + kept);
	/* More than the record keeps: the oldest bytes go. */
	std::string long_output(5000, 'a');
	long_output += "the end\n";
	for (std::size_t written = 0; written < long_output.size();) {
		const auto count = uml::runtime::kernel_write(console[1],
			long_output.data() + written,
			std::min<std::size_t>(512, long_output.size() - written));
		require(count > 0);
		written += static_cast<std::size_t>(count);
		char drain[512];
		(void)read(console[0], drain, sizeof(drain));
	}
	kept = tail();
	require(kept.size() == 4096 && kept.ends_with("aaaathe end\n"),
		"the kept output did not keep the newest bytes");
	for (const int descriptor : {console[0], console[1], other[0], other[1]})
		close(descriptor);
	uml::runtime::reset_session_record();
}

void test_record_lifecycle()
{
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	std::string record = read_file(path);
	require(record.starts_with("uml-session 2\nmemory at ") &&
		record.size() > 256 && record[255] == '\n' &&
		record.substr(256).starts_with("state running\nend running\n"),
		"a begun session is not running: " + record);
	uml::runtime::end_session_record(SessionEnd::powered_off, 0);
	record = read_file(path);
	require(has(record, "state ended\nend powered-off\nstatus 0\n") &&
		has(record, "\nended ") && !has(record, "kernel-output"),
		"a power-off was recorded wrong: " + record);
	uml::runtime::end_session_record(SessionEnd::kernel_exit, 143);
	require(read_file(path).substr(256) == record.substr(256),
		"a second end replaced the first");

	/* A restart begins the next session in the same file. */
	require(uml::runtime::begin_session_record(path, {}));
	require(has(read_file(path), "state running\n"));
	const char line[] = "Darwin: the kernel says goodbye\n";
	(void)uml::runtime::kernel_write(STDERR_FILENO, line, sizeof(line) - 1);
	uml::runtime::end_session_record(SessionEnd::kernel_exit, 143,
		"exit\nwith\x01" "control");
	record = read_file(path);
	require(has(record, "end kernel-exit\nstatus 143\n") &&
		has(record, "message exit?with?control\n") &&
		has(record, "kernel-output\n") &&
		has(record, "the kernel says goodbye\n"),
		"a kernel exit was recorded wrong: " + record);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

void test_memory_checkpoints()
{
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	const std::string begun = read_file(path);
	const std::string slot = begun.substr(0, 256);
	require(has(slot, " footprint ") && has(slot, " peak "),
		"the memory slot has no footprint: " + slot);
	/* A checkpoint rewrites the slot and nothing after it. */
	constexpr int checkpoints = 500;
	const auto start = std::chrono::steady_clock::now();
	for (int index = 0; index < checkpoints; ++index)
		uml::runtime::checkpoint_session_memory();
	const auto each = std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - start).count() / checkpoints;
	std::printf("a memory checkpoint takes %.1f us\n", each);
	require(each < 5000.0, "a memory checkpoint is far too slow");
	const std::string checked = read_file(path);
	require(checked.size() == begun.size() &&
		checked.substr(256) == begun.substr(256),
		"a checkpoint changed the session's state");
	uml::runtime::end_session_record(SessionEnd::powered_off, 0);
	uml::runtime::checkpoint_session_memory();
	const std::string ended = read_file(path);
	require(ended.substr(256).starts_with("state ended\nend powered-off\n"),
		"a checkpoint after the end replaced it: " + ended);
	/* The next session reuses the record's descriptor number. */
	const std::string next = temporary_record();
	require(uml::runtime::begin_session_record(next, {}));
	require(has(read_file(next), "\nstate running\n") &&
		has(read_file(path), "\nend powered-off\n"),
		"the next session's record was written to the wrong file");
	uml::runtime::reset_session_record();
	unlink(path.c_str());
	unlink(next.c_str());
}

void *crash_on_a_thread(void *)
{
	uml::runtime::record_kernel_crash();
	static char name[64];
	pthread_getname_np(pthread_self(), name, sizeof(name));
	return name;
}

void test_kernel_crash()
{
	for (const bool panicked : {true, false}) {
		uml::runtime::reset_session_record();
		const std::string path = temporary_record();
		require(uml::runtime::begin_session_record(path, {}));
		const char panic[] = "[ 1.0] Kernel panic - not syncing: sysrq "
			"triggered crash\nDarwin UML panic: sysrq triggered crash\n";
		const char other[] = "[ 1.0] an ordinary line\n";
		if (panicked)
			(void)uml::runtime::kernel_write(STDERR_FILENO, panic,
				sizeof(panic) - 1);
		else
			(void)uml::runtime::kernel_write(STDERR_FILENO, other,
				sizeof(other) - 1);
		pthread_t thread;
		require(pthread_create(&thread, nullptr, crash_on_a_thread,
			nullptr) == 0);
		void *result = nullptr;
		require(pthread_join(thread, &result) == 0);
		const std::string name{static_cast<const char *>(result)};
		const std::string record = read_file(path);
		if (panicked) {
			require(name == "panic: sysrq triggered crash",
				"the panicking thread was named " + name);
			require(has(record, "end guest-panic\nstatus 6\n") &&
				has(record, "message sysrq triggered crash\n") &&
				has(record, "Kernel panic - not syncing"),
				"a panic was recorded wrong: " + record);
		} else {
			require(name.empty(), "a crash without a panic named its "
				"thread " + name);
			require(has(record, "end kernel-abort\n") &&
				!has(record, "message "),
				"a kernel abort was recorded wrong: " + record);
		}
		uml::runtime::reset_session_record();
		unlink(path.c_str());
	}
}

void test_host_fault()
{
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	const char registers[] = "Darwin host fault: signal=11 esr=0x92000046 "
		"far=0 pc=0x1a2b3c sp=0x16f000 lr=0x1a2b00\n";
	const char line[] = "Darwin host fault: taken on a host thread which "
		"owns no virtual CPU; the guest is not the faulting agent\n";
	(void)uml::runtime::kernel_write(STDERR_FILENO, registers,
		sizeof(registers) - 1);
	require(has(read_file(path), "state running\n"),
		"the registers alone ended the session");
	(void)uml::runtime::kernel_write(STDERR_FILENO, line, sizeof(line) - 1);
	const std::string record = read_file(path);
	require(has(record, "end host-fault\nstatus 11\n") &&
		has(record, "message signal=11 esr=0x92000046 far=0 pc=0x1a2b3c") &&
		has(record, "owns no virtual CPU"),
		"a host fault was recorded wrong: " + record);
	uml::runtime::record_kernel_crash();
	require(read_file(path).substr(256) == record.substr(256),
		"a later crash replaced the fault");
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

void test_process_exit_in_a_child()
{
	const std::string path = temporary_record();
	const pid_t child = fork();
	require(child >= 0);
	if (child == 0) {
		uml::runtime::reset_session_record();
		if (!uml::runtime::begin_session_record(path, {}))
			_exit(2);
		std::exit(0);
	}
	int status = 0;
	require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		WEXITSTATUS(status) == 0, "the child did not exit");
	const std::string record = read_file(path);
	require(has(record, "end process-exit\n") &&
		has(record, "message exit() was called while the guest ran\n"),
		"an exit() while the guest ran was recorded wrong: " + record);
	unlink(path.c_str());
}

void test_unopenable_record()
{
	uml::runtime::reset_session_record();
	require(!uml::runtime::begin_session_record(
		"/nonexistent-directory/session", {}),
		"a record that cannot be opened was reported open");
	uml::runtime::end_session_record(SessionEnd::powered_off, 0);
}

} // namespace

int main()
{
	try {
		/* The child forks before this process has a thread of its own. */
		test_process_exit_in_a_child();
		test_names();
		test_console_descriptors();
		test_panic_message();
		test_kept_output();
		test_record_lifecycle();
		test_memory_checkpoints();
		test_kernel_crash();
		test_host_fault();
		test_unopenable_record();
	} catch (const std::exception &error) {
		std::fprintf(stderr, "session record test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
	std::puts("session record tests passed");
	return EXIT_SUCCESS;
}
