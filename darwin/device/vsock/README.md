# Virtio vsock

`UML::VsockDevice` is the host end of the guest's virtio-vsock device (Virtio
device 19). It carries a byte stream between a host application and a service
inside the guest, adbd on port 5555 for Andarium, with no listening socket on
the host and no network between them. It reuses the common vhost-user transport
and, like battery and console, builds on it alone.

The host only opens connections. The guest is CID 3 and the host CID 2, and the
device offers stream sockets alone: no feature bit is set, so SEQPACKET and the
other optional behaviours are off. Every connection a guest tries to open is
refused with an RST, so no guest process can reach the application.

## Layout

| File | Owns |
|---|---|
| `protocol.hpp` | the wire records, CIDs, ops and flow-control constants |
| `virtio_vsock_device.{hpp,cpp}` | the `VhostUserDevice`: packets, state machine, flow control |
| `connection_table.{hpp,cpp}` | per-connection state, the credit counters, the host port allocator |
| `host_events.{hpp,cpp}` | the kqueue the transport polls: readiness of the host ends, and the wake |
| `endpoint.{hpp,cpp}` | the thread-safe open API and the gateway it reaches the device through |
| `in_process_service.{hpp,cpp}` | the service on a thread named `uml-vsock`, and its endpoint |

## Rules the device keeps

- **Stream data on a disabled ring stays.** `keepsRequestsOnDisabledRing()` is
  true. A transmit buffer is data the guest has handed over for good and waits
  for nothing, so it stays on a ring the guest disabled to suspend and runs
  after the resume. Receive and event buffers stay too, as the transport keeps
  every buffer the device only fills.
- **A request is never deferred.** `processQueue` returns `complete` or
  `retry`. Disabling a ring with a deferred request pending resets the device,
  and a reset ends every connection, so one deferral would end them all at each
  guest suspend. The guest never freezes the virtio-uml device on suspend (it
  disables its rings only), so connections survive both Android's autosuspend
  and the host's quiesce.
- **The event queue always retries.** The device sends no event: it lives for
  one boot, so the transport is never reset under the guest.
- **Every guest-initiated REQUEST is answered with an RST**, and so is a packet
  for another CID, of another socket type, or for no connection. An RST is
  never answered. The refusals are queued at most 256 deep.
- **The device is appended after every other Virtio device**, in the runtime
  and in the launcher, so that no other device's index moves: Android's vendor
  policy names the battery's sysfs path by its position.

## Connections

A connection is the pair (host port, guest port). The host port comes from the
range 49152-65535, cycling, and is unique among live connections. At most 32
connections are open or opening (`EMFILE` above). The host's end of each is a
socket the device reads and writes without blocking, with 256 KiB send and
receive buffers.

| Event | Action |
|---|---|
| open | queue REQUEST, wait for the answer |
| RESPONSE | established: hand the opener its descriptor, record the guest's `buf_alloc` and `fwd_cnt` |
| RST before the answer | the open fails with `ECONNREFUSED` |
| the opener gives up | REQUEST not sent yet: dropped quietly. Sent: an RST at once, and a response that arrives later finds no connection and is reset |
| RW | written to the host's end; what it does not take is held, at most 256 KiB |
| more than the credit granted, or RW after SHUTDOWN SEND | RST, and the host's end closes |
| CREDIT_REQUEST | CREDIT_UPDATE |
| SHUTDOWN SEND | after the held data, `shutdown(SHUT_WR)` on the host's end |
| SHUTDOWN RCV | the host's end is no longer read |
| host's end at end of stream | SHUTDOWN SEND to the guest |
| host's end broken (`EPIPE`, `ECONNRESET`) | RST, and the host's end closes |
| guest done sending and (done receiving, or the host's end at end of stream) | RST, and the entry is freed |
| device reset, guest restart, service stop | every host end closes (the application reads end of stream) and opens in flight fail with `ECONNRESET` |

Flow control follows Virtio 1.2, section 5.10.6.3:

- every packet the device sends carries `buf_alloc` = 256 KiB and the current
  `fwd_cnt`, the bytes the host's socket has accepted;
- a CREDIT_UPDATE is queued each time `fwd_cnt` has advanced 64 KiB since the
  guest was last told, in any packet;
- the device reads the host's end only while the guest has credit and a receive
  buffer, and reads at most what both allow into the buffer itself;
- control packets go before data, and data is shared round-robin among the
  connections that have some. A receive buffer is one descriptor of about 3.7 KiB
  that the guest driver allocates, so a packet carries up to that much.

The counters wrap at 32 bits as on the wire; `Credit` in `connection_table.hpp`
does the arithmetic and its tests cover the wrap.

## Readiness

`eventDescriptor()` is a kqueue. Both filters of a host descriptor are
registered once, dispatched: a filter that fired is disabled until the device
enables it, and while enabled it is level-triggered. The device enables the
read filter only while the connection could send (credit, not shut, not read
dry) and the write filter only while held data waits, so a guest without
credit or buffers does not spin the transport, and data that arrived meanwhile
is reported the moment the filter is enabled. One `EVFILT_USER` wakes the
transport from other threads (a new open, an abandoned one) and when a packet
the device just handled owes the guest a reply.

## Opening a connection

```cpp
uml::vsock::InProcessService started = uml::vsock::makeInProcessService();
// started.service goes to the guest as virtio_uml.device=<name>:19
int fd = started.endpoint->connect(5555, std::chrono::seconds(5));
```

`Endpoint::connect(port, timeout)` blocks up to the timeout and returns a
connected `SOCK_STREAM` descriptor the caller owns, or a negated errno:
`-ECONNREFUSED` (nothing listens, or listens no longer), `-ETIMEDOUT` (no
answer: no vsock driver in the guest, or a guest the host quiesced, which also
abandons the open), `-ECONNRESET` (the guest went away first), `-EMFILE`,
`-ENODEV` (the service ended) and `-EINVAL` (the port cannot be named).
`Endpoint::attach(stream, port, timeout)` is the same for a stream the caller
already has, such as an accepted TCP socket: the device owns it from the call,
closes it when the open fails, and returns 0 when the guest accepts.
`begin`/`beginAttach` return the `Opening` before waiting, for a caller that
must not block; `Opening::cancel()` gives an open up from another thread.

The endpoint outlives the device and then answers `-ENODEV`. The application's
end of a connection is a plain socket: closing it, or half-closing it with
`shutdown`, is seen by the guest as the stream's end or half-close.

## Tests

`tests/unit/vsock` drives the device through its queues with synthetic packets
and real socketpairs, no guest: the credit arithmetic, the open, refusal and
timeout paths, stalls in both directions, half-closes, resets, the table's
limit, the four rules above, and an echo run of several threads opening and
closing at once.
