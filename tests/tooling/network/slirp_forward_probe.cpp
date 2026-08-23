/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The host-forward specification is the only user-facing grammar the user
 * network exposes, and a silently misread forward is worse than a refused one:
 * the guest port simply never answers. Pin every accepted form, every default,
 * and the malformed shapes that must be refused.
 */
#include "provider.hpp"

#include <cstdio>
#include <string_view>

namespace {

int failures;

void expect_rejected(std::string_view specification)
{
	uml::network::HostForward forward{};

	if (!uml::network::parse_host_forward(specification, forward))
		return;
	fprintf(stderr, "accepted malformed forward '%.*s'\n",
		static_cast<int>(specification.size()), specification.data());
	failures++;
}

void expect_parsed(std::string_view specification, bool udp,
		   std::string_view host_address, unsigned int host_port,
		   std::string_view guest_address, unsigned int guest_port)
{
	uml::network::HostForward forward{};

	if (!uml::network::parse_host_forward(specification, forward)) {
		fprintf(stderr, "rejected valid forward '%.*s'\n",
			static_cast<int>(specification.size()),
			specification.data());
		failures++;
		return;
	}
	if (forward.udp == udp && forward.host_address == host_address &&
	    forward.host_port == host_port &&
	    forward.guest_address == guest_address &&
	    forward.guest_port == guest_port)
		return;
	fprintf(stderr,
		"forward '%.*s' parsed as %s %s:%u-%s:%u\n",
		static_cast<int>(specification.size()), specification.data(),
		forward.udp ? "udp" : "tcp", forward.host_address.c_str(),
		static_cast<unsigned int>(forward.host_port),
		forward.guest_address.c_str(),
		static_cast<unsigned int>(forward.guest_port));
	failures++;
}

} // namespace

int main(void)
{
	/* The short form is the one an operator types: TCP, bound to loopback so
	 * the forward is not reachable off the machine, aimed at the DHCP lease. */
	expect_parsed("5555-5555", false, "127.0.0.1", 5555, "10.0.2.15", 5555);
	expect_parsed("8022-22", false, "127.0.0.1", 8022, "10.0.2.15", 22);
	expect_parsed("tcp:5555-5555", false, "127.0.0.1", 5555, "10.0.2.15",
		      5555);
	expect_parsed("udp:5353-5353", true, "127.0.0.1", 5353, "10.0.2.15",
		      5353);
	/* Binding off loopback has to be spelled out, never inferred. */
	expect_parsed("tcp:0.0.0.0:5555-10.0.2.15:5555", false, "0.0.0.0", 5555,
		      "10.0.2.15", 5555);
	expect_parsed("192.168.64.1:80-10.0.2.15:8080", false, "192.168.64.1",
		      80, "10.0.2.15", 8080);
	expect_parsed("udp:127.0.0.1:69-69", true, "127.0.0.1", 69, "10.0.2.15",
		      69);

	expect_rejected("");
	expect_rejected("5555");
	expect_rejected("-5555");
	expect_rejected("5555-");
	expect_rejected("0-5555");
	expect_rejected("5555-0");
	expect_rejected("65536-5555");
	expect_rejected("5555-65536");
	expect_rejected("sctp:5555-5555");
	expect_rejected("tcp:5555");
	expect_rejected("nothost:5555-5555");
	expect_rejected("127.0.0.1:-5555");
	expect_rejected("5555x-5555");
	expect_rejected("5555-5555x");
	expect_rejected("::1:5555-5555");

	if (failures != 0) {
		fprintf(stderr, "%d host-forward parse expectations failed\n",
			failures);
		return 1;
	}
	printf("PASS: host-forward specifications parse with loopback and "
	       "DHCP-lease defaults and refuse malformed forms\n");
	return 0;
}
