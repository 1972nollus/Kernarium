// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * darwin_uml_guest_shutdown(), the bridge import through which the guest
 * kernel says how its run ends: what the run's context records, what the host
 * logs, and what the record keeps across the end of a run.
 */
#include "platform/run_routing.hpp"
#include "require.hpp"
#include "runtime/context.hpp"
#include "uml_darwin_host.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using uml::runtime::Context;
using uml::runtime::GuestShutdown;
using uml::runtime::GuestShutdownKind;
using uml::runtime::RunScope;
using uml::testing::require;

constexpr std::size_t command_limit =
	uml::runtime::guest_shutdown_command_limit;

/* What the import writes to standard error while `operation` runs. */
template<typename Operation>
std::string logged(Operation &&operation)
{
	int pipe_descriptors[2];
	require(pipe(pipe_descriptors) == 0);
	std::fflush(stderr);
	const int saved = dup(STDERR_FILENO);
	require(saved >= 0);
	require(dup2(pipe_descriptors[1], STDERR_FILENO) >= 0);
	operation();
	require(dup2(saved, STDERR_FILENO) >= 0);
	close(saved);
	close(pipe_descriptors[1]);
	std::string text;
	char buffer[1024];
	for (;;) {
		const ssize_t count = read(pipe_descriptors[0], buffer, sizeof(buffer));
		if (count <= 0)
			break;
		text.append(buffer, static_cast<std::size_t>(count));
	}
	close(pipe_descriptors[0]);
	return text;
}

std::string command_of(const GuestShutdown &record)
{
	return std::string{record.command};
}

/* The import resolves the run's context through the active route, as the
 * kernel's call does. */
class RoutedContext final {
public:
	RoutedContext() noexcept = default;
	RoutedContext(const RoutedContext &) = delete;
	RoutedContext &operator=(const RoutedContext &) = delete;

	[[nodiscard]] Context &context() noexcept { return context_; }

private:
	Context context_;
	uml::routing::ActiveRouteScope route_{&context_, nullptr};
};

void test_each_kind_is_recorded_and_logged()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());
	require(context.guest_shutdown().kind == GuestShutdownKind::none);
	require(command_of(context.guest_shutdown()).empty());

	struct Case {
		int kind;
		GuestShutdownKind recorded;
		const char *line;
	};
	const Case cases[] = {
		{DARWIN_UML_SHUTDOWN_POWER_OFF, GuestShutdownKind::power_off,
			"guest powered off\n"},
		{DARWIN_UML_SHUTDOWN_HALT, GuestShutdownKind::halt,
			"guest halted\n"},
		{DARWIN_UML_SHUTDOWN_RESTART, GuestShutdownKind::restart,
			"guest requested restart\n"},
	};
	for (const auto &entry : cases) {
		const auto text = logged([&] {
			darwin_uml_guest_shutdown(entry.kind, nullptr);
		});
		require(text == entry.line, text);
		const auto record = context.guest_shutdown();
		require(record.kind == entry.recorded);
		require(command_of(record).empty());
	}
}

void test_restart_command_is_recorded_and_named()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());

	const auto text = logged([] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART,
			"recovery");
	});
	require(text == "guest requested restart (command 'recovery')\n", text);
	const auto record = context.guest_shutdown();
	require(record.kind == GuestShutdownKind::restart);
	require(command_of(record) == "recovery");

	/* An empty command is no command. */
	const auto empty = logged([] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "");
	});
	require(empty == "guest requested restart\n", empty);
	require(command_of(context.guest_shutdown()).empty());
}

void test_null_command_means_no_command()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());

	darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "bootloader");
	require(command_of(context.guest_shutdown()) == "bootloader");
	/* The next report replaces the whole record, its command included. */
	darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, nullptr);
	const auto record = context.guest_shutdown();
	require(record.kind == GuestShutdownKind::restart);
	require(command_of(record).empty());
}

void test_command_is_cut_at_255_bytes()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());
	static_assert(command_limit == 255);

	/* Exactly the limit stays whole; a longer command keeps its first 255
	 * bytes and the byte after them is not read into the record. */
	std::string exact;
	for (std::size_t index = 0; index < command_limit; ++index)
		exact.push_back(static_cast<char>('a' + index % 26));
	darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, exact.c_str());
	require(command_of(context.guest_shutdown()) == exact);

	const std::string longer = exact + "TAIL" + std::string(300, 'z');
	const auto text = logged([&] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART,
			longer.c_str());
	});
	const auto record = context.guest_shutdown();
	require(record.kind == GuestShutdownKind::restart);
	require(command_of(record) == exact);
	require(record.command[command_limit] == '\0');
	require(text == "guest requested restart (command '" + exact + "')\n",
		text);

	const std::string one_over = exact + "X";
	darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, one_over.c_str());
	require(command_of(context.guest_shutdown()) == exact);
}

void test_log_line_keeps_control_characters_out()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());

	const auto text = logged([] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART,
			"a\nb\x1b" "c\x7f" "d");
	});
	require(text == "guest requested restart (command 'a?b?c?d')\n", text);
	/* The record keeps what the guest sent. */
	require(command_of(context.guest_shutdown()) == "a\nb\x1b" "c\x7f" "d");
}

void test_later_report_replaces_the_earlier()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());

	darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "recovery");
	darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_HALT, nullptr);
	const auto record = context.guest_shutdown();
	require(record.kind == GuestShutdownKind::halt);
	require(command_of(record).empty());
}

void test_record_outlives_the_run_and_the_next_run_clears_it()
{
	RoutedContext routed;
	auto &context = routed.context();
	{
		RunScope run{context};
		require(run.entered());
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "recovery");
	}
	/* The owner reads the record after the run has returned. */
	auto record = context.guest_shutdown();
	require(record.kind == GuestShutdownKind::restart);
	require(command_of(record) == "recovery");
	{
		RunScope run{context};
		require(run.entered());
		record = context.guest_shutdown();
		require(record.kind == GuestShutdownKind::none);
		require(command_of(record).empty());
	}
	require(context.guest_shutdown().kind == GuestShutdownKind::none);
}

void test_record_is_readable_while_an_exitcall_never_returns()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());

	std::mutex mutex;
	std::condition_variable changed;
	bool reported = false;
	bool release = false;
	/* The kernel's thread reports and then hangs in an exitcall. */
	std::thread kernel{[&] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "recovery");
		std::unique_lock lock{mutex};
		reported = true;
		changed.notify_all();
		changed.wait(lock, [&] { return release; });
	}};
	{
		std::unique_lock lock{mutex};
		require(changed.wait_for(lock, 30s, [&] { return reported; }));
	}
	const auto record = context.guest_shutdown();
	require(record.kind == GuestShutdownKind::restart);
	require(command_of(record) == "recovery");
	{
		std::lock_guard lock{mutex};
		release = true;
	}
	changed.notify_all();
	kernel.join();
}

void test_an_unknown_kind_records_nothing()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());

	darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_HALT, nullptr);
	const auto text = logged([] {
		darwin_uml_guest_shutdown(0, "ignored");
		darwin_uml_guest_shutdown(99, nullptr);
	});
	require(text.find("guest reported shutdown kind 0") !=
		std::string::npos, text);
	require(text.find("guest reported shutdown kind 99") !=
		std::string::npos, text);
	const auto record = context.guest_shutdown();
	require(record.kind == GuestShutdownKind::halt);
	require(command_of(record).empty());
}

void test_without_a_run_context_the_report_is_only_logged()
{
	/* No route exists here: the kernel's thread resolves no context, so
	 * nothing is recorded, and the report still reaches the log. */
	const auto text = logged([] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "recovery");
	});
	require(text == "guest requested restart (command 'recovery')\n", text);
}

} // namespace

int main()
{
	/* First: later tests route a context of their own. */
	test_without_a_run_context_the_report_is_only_logged();
	test_each_kind_is_recorded_and_logged();
	test_restart_command_is_recorded_and_named();
	test_null_command_means_no_command();
	test_command_is_cut_at_255_bytes();
	test_log_line_keeps_control_characters_out();
	test_later_report_replaces_the_earlier();
	test_record_outlives_the_run_and_the_next_run_clears_it();
	test_record_is_readable_while_an_exitcall_never_returns();
	test_an_unknown_kind_records_nothing();
	std::puts("guest shutdown import: ok");
	return 0;
}
