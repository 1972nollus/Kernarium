/* SPDX-License-Identifier: GPL-2.0 */
#include "../include/uml_darwin_runtime.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libslirp.h>

#define UML_NET_FRAME_SIZE 2048
#define UML_NET_RING_SIZE 256
#define UML_NET_MAX_POLL_FDS 512

struct queued_frame {
	size_t length;
	unsigned char data[UML_NET_FRAME_SIZE];
};

struct slirp_timer {
	SlirpTimerId id;
	void *callback_opaque;
	int64_t expires_ms;
	struct slirp_timer *next;
};

struct slirp_poll_state {
	struct pollfd descriptors[UML_NET_MAX_POLL_FDS + 1];
	nfds_t count;
};

static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t receive_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t transmit_lock = PTHREAD_MUTEX_INITIALIZER;
static struct queued_frame receive_ring[UML_NET_RING_SIZE];
static struct queued_frame transmit_ring[UML_NET_RING_SIZE];
static unsigned int receive_head;
static unsigned int receive_tail;
static unsigned int transmit_head;
static unsigned int transmit_tail;
static struct slirp_timer *timer_list;
static Slirp *network_slirp;
static int wake_pipe[2] = { -1, -1 };
static int receive_pipe[2] = { -1, -1 };
static int network_started;

static int64_t monotonic_nanoseconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static void wake_network_thread(void *opaque)
{
	unsigned char byte = 1;

	(void)opaque;
	if (wake_pipe[1] >= 0)
		(void)write(wake_pipe[1], &byte, sizeof(byte));
}

static slirp_ssize_t send_packet_to_guest(const void *data, size_t length,
					  void *opaque)
{
	unsigned int next;

	(void)opaque;
	if (length == 0 || length > UML_NET_FRAME_SIZE)
		return -1;
	pthread_mutex_lock(&receive_lock);
	next = (receive_head + 1) % UML_NET_RING_SIZE;
	if (next == receive_tail) {
		pthread_mutex_unlock(&receive_lock);
		return 0;
	}
	receive_ring[receive_head].length = length;
	memcpy(receive_ring[receive_head].data, data, length);
	receive_head = next;
	if (receive_pipe[1] >= 0) {
		unsigned char byte = 1;

		(void)write(receive_pipe[1], &byte, sizeof(byte));
	}
	pthread_mutex_unlock(&receive_lock);
	return (slirp_ssize_t)length;
}

static void report_guest_error(const char *message, void *opaque)
{
	static const char prefix[] = "Darwin user network: ";

	(void)opaque;
	(void)write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
	(void)write(STDERR_FILENO, message, strlen(message));
	(void)write(STDERR_FILENO, "\n", 1);
}

static int64_t slirp_clock_get_ns(void *opaque)
{
	(void)opaque;
	return monotonic_nanoseconds();
}

static void *new_slirp_timer(SlirpTimerId id, void *callback_opaque,
				     void *opaque)
{
	struct slirp_timer *timer;

	(void)opaque;
	timer = calloc(1, sizeof(*timer));
	if (timer == NULL)
		return NULL;
	timer->id = id;
	timer->callback_opaque = callback_opaque;
	timer->expires_ms = -1;
	timer->next = timer_list;
	timer_list = timer;
	return timer;
}

static void free_slirp_timer(void *timer_pointer, void *opaque)
{
	struct slirp_timer **link = &timer_list;
	struct slirp_timer *timer = timer_pointer;

	(void)opaque;
	while (*link != NULL) {
		if (*link == timer) {
			*link = timer->next;
			free(timer);
			return;
		}
		link = &(*link)->next;
	}
}

static void modify_slirp_timer(void *timer_pointer, int64_t expires_ms,
				       void *opaque)
{
	struct slirp_timer *timer = timer_pointer;

	(void)opaque;
	if (timer != NULL)
		timer->expires_ms = expires_ms;
}

static uint32_t timer_timeout(uint32_t timeout)
{
	struct slirp_timer *timer;
	int64_t now_ms = monotonic_nanoseconds() / 1000000LL;

	for (timer = timer_list; timer != NULL; timer = timer->next) {
		int64_t remaining;

		if (timer->expires_ms < 0)
			continue;
		remaining = timer->expires_ms - now_ms;
		if (remaining <= 0)
			return 0;
		if ((uint64_t)remaining < timeout)
			timeout = (uint32_t)remaining;
	}
	return timeout;
}

static void handle_expired_timers(void)
{
	for (;;) {
		struct slirp_timer *timer;
		int64_t now_ms = monotonic_nanoseconds() / 1000000LL;

		for (timer = timer_list; timer != NULL; timer = timer->next) {
			SlirpTimerId id;
			void *callback_opaque;

			if (timer->expires_ms < 0 || timer->expires_ms > now_ms)
				continue;
			id = timer->id;
			callback_opaque = timer->callback_opaque;
			timer->expires_ms = -1;
			slirp_handle_timer(network_slirp, id, callback_opaque);
			break;
		}
		if (timer == NULL)
			return;
	}
}

static void ignore_poll_socket(slirp_os_socket descriptor, void *opaque)
{
	(void)descriptor;
	(void)opaque;
}

static short poll_events_from_slirp(int events)
{
	short result = 0;

	if (events & SLIRP_POLL_IN)
		result |= POLLIN;
	if (events & SLIRP_POLL_OUT)
		result |= POLLOUT;
	if (events & SLIRP_POLL_PRI)
		result |= POLLPRI;
	if (events & SLIRP_POLL_ERR)
		result |= POLLERR;
	if (events & SLIRP_POLL_HUP)
		result |= POLLHUP;
	return result;
}

static int slirp_events_from_poll(short events)
{
	int result = 0;

	if (events & POLLIN)
		result |= SLIRP_POLL_IN;
	if (events & POLLOUT)
		result |= SLIRP_POLL_OUT;
	if (events & POLLPRI)
		result |= SLIRP_POLL_PRI;
	if (events & POLLERR)
		result |= SLIRP_POLL_ERR;
	if (events & POLLHUP)
		result |= SLIRP_POLL_HUP;
	return result;
}

static int add_slirp_poll_descriptor(slirp_os_socket descriptor, int events,
				     void *opaque)
{
	struct slirp_poll_state *state = opaque;
	nfds_t index = state->count;

	if (index >= UML_NET_MAX_POLL_FDS)
		return -1;
	state->descriptors[index].fd = descriptor;
	state->descriptors[index].events = poll_events_from_slirp(events);
	state->descriptors[index].revents = 0;
	state->count++;
	return (int)index;
}

static int get_slirp_poll_events(int index, void *opaque)
{
	struct slirp_poll_state *state = opaque;

	if (index < 0 || (nfds_t)index >= state->count)
		return 0;
	return slirp_events_from_poll(state->descriptors[index].revents);
}

static void drain_wake_pipe(void)
{
	unsigned char buffer[64];

	while (read(wake_pipe[0], buffer, sizeof(buffer)) > 0)
		;
}

static void process_guest_packets(void)
{
	unsigned char frame[UML_NET_FRAME_SIZE];

	for (;;) {
		size_t length;

		pthread_mutex_lock(&transmit_lock);
		if (transmit_tail == transmit_head) {
			pthread_mutex_unlock(&transmit_lock);
			return;
		}
		length = transmit_ring[transmit_tail].length;
		memcpy(frame, transmit_ring[transmit_tail].data, length);
		transmit_tail = (transmit_tail + 1) % UML_NET_RING_SIZE;
		pthread_mutex_unlock(&transmit_lock);
		slirp_input(network_slirp, frame, (int)length);
	}
}

static void *network_thread_main(void *opaque)
{
	(void)opaque;
	for (;;) {
		struct slirp_poll_state state = { };
		uint32_t timeout = 1000;
		nfds_t wake_index;
		int result;

		process_guest_packets();
		slirp_pollfds_fill_socket(network_slirp, &timeout,
					    add_slirp_poll_descriptor, &state);
		timeout = timer_timeout(timeout);
		wake_index = state.count;
		state.descriptors[wake_index].fd = wake_pipe[0];
		state.descriptors[wake_index].events = POLLIN;
		state.descriptors[wake_index].revents = 0;
		state.count++;
		result = poll(state.descriptors, state.count, (int)timeout);
		if (state.descriptors[wake_index].revents & POLLIN)
			drain_wake_pipe();
		slirp_pollfds_poll(network_slirp, result < 0,
				     get_slirp_poll_events, &state);
		handle_expired_timers();
	}
}

static const SlirpCb slirp_callbacks = {
	.send_packet = send_packet_to_guest,
	.guest_error = report_guest_error,
	.clock_get_ns = slirp_clock_get_ns,
	.timer_free = free_slirp_timer,
	.timer_mod = modify_slirp_timer,
	.notify = wake_network_thread,
	.timer_new_opaque = new_slirp_timer,
	.register_poll_socket = ignore_poll_socket,
	.unregister_poll_socket = ignore_poll_socket,
};

static int configure_nonblocking(int descriptor)
{
	int flags = fcntl(descriptor, F_GETFL);

	if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;
	return 0;
}

int darwin_uml_net_open(unsigned char mac[6], unsigned int *mtu)
{
	static const unsigned char default_mac[6] = {
		0x52, 0x54, 0x00, 0x12, 0x34, 0x56
	};
	SlirpConfig config = { };
	pthread_t thread;
	int result = 0;

	pthread_mutex_lock(&start_lock);
	if (network_started)
		goto complete;
	if (pipe(wake_pipe) != 0 || pipe(receive_pipe) != 0 ||
	    configure_nonblocking(wake_pipe[0]) != 0 ||
	    configure_nonblocking(wake_pipe[1]) != 0 ||
	    configure_nonblocking(receive_pipe[0]) != 0 ||
	    configure_nonblocking(receive_pipe[1]) != 0) {
		result = -errno;
		goto failed;
	}
	config.version = 6;
	config.in_enabled = true;
	config.restricted = 0;
	(void)inet_pton(AF_INET, "10.0.2.0", &config.vnetwork);
	(void)inet_pton(AF_INET, "255.255.255.0", &config.vnetmask);
	(void)inet_pton(AF_INET, "10.0.2.2", &config.vhost);
	(void)inet_pton(AF_INET, "10.0.2.15", &config.vdhcp_start);
	(void)inet_pton(AF_INET, "10.0.2.3", &config.vnameserver);
	config.vhostname = "uml-darwin";
	config.if_mtu = 1500;
	config.if_mru = 1500;
	network_slirp = slirp_new(&config, &slirp_callbacks, NULL);
	if (network_slirp == NULL) {
		result = -EIO;
		goto failed;
	}
	if (pthread_create(&thread, NULL, network_thread_main, NULL) != 0) {
		result = -errno;
		slirp_cleanup(network_slirp);
		network_slirp = NULL;
		goto failed;
	}
	(void)pthread_detach(thread);
	network_started = 1;

complete:
	memcpy(mac, default_mac, sizeof(default_mac));
	*mtu = 1500;
	pthread_mutex_unlock(&start_lock);
	return 0;

failed:
	if (wake_pipe[0] >= 0)
		close(wake_pipe[0]);
	if (wake_pipe[1] >= 0)
		close(wake_pipe[1]);
	if (receive_pipe[0] >= 0)
		close(receive_pipe[0]);
	if (receive_pipe[1] >= 0)
		close(receive_pipe[1]);
	wake_pipe[0] = -1;
	wake_pipe[1] = -1;
	receive_pipe[0] = -1;
	receive_pipe[1] = -1;
	pthread_mutex_unlock(&start_lock);
	return result != 0 ? result : -EIO;
}

int darwin_uml_net_irq_fd(void)
{
	return network_started ? receive_pipe[0] : -1;
}

int darwin_uml_net_read(void *buffer, unsigned long capacity)
{
	size_t length;

	pthread_mutex_lock(&receive_lock);
	if (receive_tail == receive_head) {
		pthread_mutex_unlock(&receive_lock);
		return 0;
	}
	length = receive_ring[receive_tail].length;
	if (length > capacity) {
		receive_tail = (receive_tail + 1) % UML_NET_RING_SIZE;
		pthread_mutex_unlock(&receive_lock);
		return -EMSGSIZE;
	}
	memcpy(buffer, receive_ring[receive_tail].data, length);
	receive_tail = (receive_tail + 1) % UML_NET_RING_SIZE;
	pthread_mutex_unlock(&receive_lock);
	if (receive_pipe[0] >= 0) {
		unsigned char byte;

		(void)read(receive_pipe[0], &byte, sizeof(byte));
	}
	return (int)length;
}

int darwin_uml_net_write(const void *buffer, unsigned long length)
{
	unsigned int next;

	if (!network_started || length == 0 || length > UML_NET_FRAME_SIZE)
		return -EINVAL;
	pthread_mutex_lock(&transmit_lock);
	next = (transmit_head + 1) % UML_NET_RING_SIZE;
	if (next == transmit_tail) {
		pthread_mutex_unlock(&transmit_lock);
		return -EAGAIN;
	}
	transmit_ring[transmit_head].length = (size_t)length;
	memcpy(transmit_ring[transmit_head].data, buffer, length);
	transmit_head = next;
	pthread_mutex_unlock(&transmit_lock);
	wake_network_thread(NULL);
	return 0;
}
