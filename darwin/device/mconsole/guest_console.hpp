// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace uml::device {

enum class GuestSuspension {
	none,
	managementStop,
	system,
};

/*
 * UML's own management console is the only channel that reaches the guest
 * kernel while it runs, and the only way to make it write back pages it has
 * not yet handed to the block layer. Everything the host can flush by itself
 * stops at the block device.
 *
 * The guest binds the console as a datagram socket and answers each request at
 * the address it came from, so the host binds one reply socket per request.
 * Without a private directory, runs share /tmp and every name carries the
 * run's identity: the guest binds /tmp/uml-<umid>-mconsole and a request
 * /tmp/uml-mc-<pid>-<id>. That lets guests run side by side. A private
 * directory belongs to this process alone; the guest receives it as uml_dir=
 * and binds <directory>/mconsole, and a request binds <directory>/mc-<id>.
 */
struct GuestConsoleLocation final {
	/* Handed to the guest as uml_dir=; empty for the shared /tmp. */
	std::string directory;
	std::string console;
	/* A request's reply socket appends eight hexadecimal digits. */
	std::string client_prefix;
};

/*
 * Composes a location, dropping a private directory's trailing slashes as the
 * guest does. Every path it can name, the guest's and the widest reply
 * socket's, must fit sun_path. A private directory that is not absolute, or
 * for which one of them would not fit, yields nothing: never a truncated path,
 * and never /tmp in its place.
 */
[[nodiscard]] std::optional<GuestConsoleLocation>
compose_guest_console_location(
	std::optional<std::string_view> private_directory,
	std::string_view umid, pid_t pid);

/*
 * This process's location. The iOS sandbox refuses sockets in /tmp, so an iOS
 * device uses the application's own temporary directory, $TMPDIR. macOS keeps
 * /tmp, and so does the iOS Simulator, whose application directory is longer
 * than sun_path allows. Nothing, reported once on stderr, leaves the guest
 * without a console the host can reach.
 */
[[nodiscard]] const std::optional<GuestConsoleLocation> &
guest_console_location();

/*
 * The kernel arguments that put the guest's console where the host looks for
 * it: umid= always, and uml_dir= for a private directory. Every composition
 * root passes them. A caller that supplies either keeps its own value, and the
 * host then cannot reach that console.
 */
[[nodiscard]] std::vector<std::string> guest_management_console_arguments();

/*
 * Asks the guest to write its filesystems back and waits for it to accept the
 * request, then allows a bounded settling period for the writeback itself.
 *
 * True means the guest confirmed the request. It is not a completion proof:
 * the guest schedules the writeback and reports that it started, so the
 * settling period is a bound rather than a guarantee. False means no console
 * answered, which is the normal result where the guest could not bind one.
 */
[[nodiscard]] bool request_guest_writeback(unsigned int settle_ms) noexcept;

/*
 * Enters Linux system suspend, so devices, secondary CPUs, interrupts, and
 * timekeeping stop at a kernel-owned boundary, and sends the explicit wake
 * edge. Older kernels fall back to management-console stop/go.
 *
 * The returned mode records which resume command is required. A sent suspend
 * request that has not replied by the deadline still selects system wake:
 * timeout cannot prove that the kernel did not already enter suspend. None
 * means no request reached the console. Resuming is best effort and reports
 * nothing.
 */
[[nodiscard]] GuestSuspension suspend_guest(
	unsigned int writeback_settle_ms,
	unsigned int stop_settle_ms) noexcept;
void resume_guest(GuestSuspension suspension) noexcept;

/*
 * Removes the console socket at this run's path. A run calls it before its
 * guest binds the path: a guest removes its socket only when it shuts down,
 * not when its process is killed, which is how iOS ends an application, and
 * a private directory's fixed name would then refuse every later guest.
 * Adopting the path separately covers the terminating-signal paths that never
 * unwind.
 */
void release_guest_management_console() noexcept;

} // namespace uml::device
