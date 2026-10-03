// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>

namespace uml::device {

/*
 * A bound Unix-socket path that must not outlive its process.
 *
 * The destructor removes the path on an ordinary teardown.  An adopted path is
 * additionally published to the handler installed by
 * install_socket_path_signal_cleanup(), because a terminating signal never
 * unwinds this object and would otherwise leave the path behind.  A stale path
 * is not merely untidy: the console socket server refuses to replace an
 * existing path, so the next run of the same command fails to start.
 */
class SocketPath final {
public:
	explicit SocketPath(std::filesystem::path path);
	~SocketPath();

	SocketPath(const SocketPath &) = delete;
	SocketPath &operator=(const SocketPath &) = delete;

	/* Records the bound path's device and inode and publishes it to the
	 * signal handler.  Call once, immediately after bind() succeeds. */
	void adopt();

private:
	std::filesystem::path path_;
	int slot_{-1};
};

/*
 * Removes every adopted socket path whose device and inode still match what
 * was bound, so a path another process has since recreated is left alone.
 * Async-signal-safe: it reads preallocated storage and calls only lstat() and
 * unlink().
 */
void remove_registered_socket_paths() noexcept;

/*
 * Installs SIGHUP/SIGINT/SIGQUIT/SIGTERM handlers that remove adopted socket
 * paths and then re-raise with the default disposition, leaving the wait status
 * a shell or supervisor observes unchanged.  A signal the caller already
 * ignores keeps that disposition.
 */
void install_socket_path_signal_cleanup();

} // namespace uml::device
