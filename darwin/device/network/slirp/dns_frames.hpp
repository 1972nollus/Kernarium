// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * The guest frames the provider answers itself instead of handing them to
 * SLiRP: DNS datagrams to the virtual name server or the gateway, and the
 * name server's other traffic, which SLiRP would otherwise send to that
 * literal address on the host's network once its own DNS proxy is off.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace uml::network::dns {

using MacAddress = std::array<std::uint8_t, 6>;
using Ipv4Address = std::array<std::uint8_t, 4>;

/* The two ends of one guest datagram or segment, as the reply reverses them. */
struct Endpoints final {
	MacAddress guest_mac{};
	MacAddress server_mac{};
	Ipv4Address guest_address{};
	Ipv4Address server_address{};
	std::uint16_t guest_port{};
	std::uint16_t server_port{};
};

enum class FrameAction {
	pass,      /* SLiRP handles it */
	query,     /* a DNS datagram: `payload` goes to the resolver */
	reset,     /* a TCP segment to a port the name server does not serve */
	drop,      /* anything else addressed to the name server */
};

struct Classified final {
	FrameAction action{FrameAction::pass};
	Endpoints endpoints;
	std::span<const std::uint8_t> payload;
	/* For `reset`: the segment's sequence, acknowledgement and flags. */
	std::uint32_t sequence{};
	std::uint32_t acknowledgement{};
	std::uint8_t tcp_flags{};
	std::size_t segment_length{};
};

/*
 * `name_server` is SLiRP's virtual name server, which serves only DNS (UDP
 * here, TCP through SLiRP's guest forward). `gateway` answers DNS too, for
 * guests configured with the gateway as their server, and passes the rest.
 */
[[nodiscard]] Classified classify_guest_frame(
	std::span<const std::uint8_t> frame, const Ipv4Address &name_server,
	const Ipv4Address &gateway);

/* One Ethernet frame carrying `payload` from the server back to the guest.
 * The payload must fit one 1500-byte IP packet. */
[[nodiscard]] std::vector<std::uint8_t> udp_reply_frame(
	const Endpoints &endpoints, std::span<const std::uint8_t> payload,
	std::uint16_t identification);

/* The RFC 9293 reset for a classified `reset` segment, or nothing for a
 * segment that carries RST itself. */
[[nodiscard]] std::vector<std::uint8_t> tcp_reset_frame(
	const Classified &segment, std::uint16_t identification);

} // namespace uml::network::dns
