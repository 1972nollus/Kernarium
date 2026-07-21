/* SPDX-License-Identifier: GPL-2.0 */
#include "../include/uml_darwin_runtime.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

extern "C" int uml_slirp_net_open(
	void *, unsigned char mac[6], unsigned int *mtu);
extern "C" int uml_slirp_net_read(void *, void *buffer, size_t capacity);
extern "C" int uml_slirp_net_write(
	void *, const void *buffer, size_t length);
extern "C" void uml_slirp_net_stop(void);

struct arp_frame {
	uint8_t destination[6];
	uint8_t source[6];
	uint16_t ethernet_type;
	uint16_t hardware_type;
	uint16_t protocol_type;
	uint8_t hardware_length;
	uint8_t protocol_length;
	uint16_t operation;
	uint8_t sender_hardware[6];
	uint8_t sender_protocol[4];
	uint8_t target_hardware[6];
	uint8_t target_protocol[4];
} __attribute__((packed));

int main(void)
{
	static const uint8_t broadcast[6] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};
	static const uint8_t guest_address[4] = { 10, 0, 2, 15 };
	static const uint8_t gateway_address[4] = { 10, 0, 2, 2 };
	struct timespec delay = { .tv_nsec = 10000000 };
	uint8_t packet[2048] = { };
	struct arp_frame *request = (struct arp_frame *)packet;
	uint8_t mac[6];
	unsigned int mtu;
	int attempt;
	int length;

	if (uml_slirp_net_open(NULL, mac, &mtu) != 0) {
		fprintf(stderr, "network open failed\n");
		return 1;
	}
	memcpy(request->destination, broadcast, sizeof(broadcast));
	memcpy(request->source, mac, sizeof(request->source));
	request->ethernet_type = htons(0x0806);
	request->hardware_type = htons(1);
	request->protocol_type = htons(0x0800);
	request->hardware_length = 6;
	request->protocol_length = 4;
	request->operation = htons(1);
	memcpy(request->sender_hardware, mac,
	       sizeof(request->sender_hardware));
	memcpy(request->sender_protocol, guest_address,
	       sizeof(request->sender_protocol));
	memcpy(request->target_protocol, gateway_address,
	       sizeof(request->target_protocol));
	if (uml_slirp_net_write(NULL, packet, 64) != 0) {
		fprintf(stderr, "network transmit failed\n");
		return 1;
	}
	for (attempt = 0; attempt < 200; attempt++) {
		struct arp_frame *reply;

		(void)nanosleep(&delay, NULL);
		length = uml_slirp_net_read(NULL, packet, sizeof(packet));
		if (length < (int)sizeof(*reply))
			continue;
		reply = (struct arp_frame *)packet;
		if (reply->ethernet_type != htons(0x0806) ||
		    reply->operation != htons(2) ||
		    memcmp(reply->sender_protocol, gateway_address,
			   sizeof(gateway_address)) != 0)
			continue;
		printf("PASS: user-mode NAT answered gateway ARP; "
		       "guest mtu=%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
		       mtu, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		uml_slirp_net_stop();
		return 0;
	}
	fprintf(stderr, "no gateway ARP response\n");
	uml_slirp_net_stop();
	return 1;
}
