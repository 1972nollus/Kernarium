// SPDX-License-Identifier: GPL-2.0-or-later
// Run against the compiled kernel console implementation, not a modeled writer.
#include <array>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <util.h>

extern "C" int generic_console_write(int, const char *, int);

int main()
{
	alarm(5);
	int master, slave;
	if (openpty(&master, &slave, nullptr, nullptr, nullptr))
		return 2;
	termios state{};
	if (tcgetattr(slave, &state))
		return 3;
	cfmakeraw(&state);
	if (tcsetattr(slave, TCSANOW, &state))
		return 4;
	const int output = dup(slave);
	const int flags = fcntl(slave, F_GETFL);
	if (output < 0 || flags < 0 || fcntl(slave, F_SETFL, flags | O_NONBLOCK))
		return 5;

	std::array<char, 512> input{};
	for (size_t index = 0; index < input.size(); ++index)
		input[index] = 'A' + index % 26;
	input.back() = '\n';
	if (write(master, input.data(), input.size()) != static_cast<ssize_t>(input.size()))
		return 6;
	pollfd input_ready{slave, POLLIN, 0};
	if (poll(&input_ready, 1, 1000) != 1)
		return 7;

	constexpr char marker[] = "kernel-console-marker";
	std::string seen;
	std::thread drain([&] {
		while (seen.size() < sizeof(marker) - 1) {
			pollfd ready{master, POLLIN, 0};
			if (poll(&ready, 1, 2000) != 1)
				return;
			char buffer[1024];
			const auto count = read(master, buffer, sizeof(buffer));
			if (count <= 0)
				return;
			seen.append(buffer, count);
		}
	});
	const int written = generic_console_write(output, marker, sizeof(marker) - 1);
	drain.join();
	std::array<char, 1024> received{};
	const auto count = read(slave, received.data(), received.size());
	const bool correct = count == static_cast<ssize_t>(input.size()) &&
		!memcmp(received.data(), input.data(), input.size());
	const bool output_ok = written == sizeof(marker) - 1 && seen == marker;
	std::printf("input=%zd/512 preserved=%s output=%d/%zu correct=%s\n",
		count, correct ? "yes" : "no", written, sizeof(marker) - 1,
		output_ok ? "yes" : "no");
	close(output);
	close(slave);
	close(master);
	return correct && output_ok ? 0 : 1;
}
