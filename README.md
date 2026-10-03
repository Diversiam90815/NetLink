# NetLink

[![Windows Build](https://github.com/Diversiam90815/NetLink/actions/workflows/windows.yml/badge.svg)](https://github.com/Diversiam90815/NetLink/actions/workflows/windows.yml)
[![macOS Build](https://github.com/Diversiam90815/NetLink/actions/workflows/macos.yml/badge.svg)](https://github.com/Diversiam90815/NetLink/actions/workflows/macos.yml)
[![Linux Build](https://github.com/Diversiam90815/NetLink/actions/workflows/linux.yml/badge.svg)](https://github.com/Diversiam90815/NetLink/actions/workflows/linux.yml)
[![Tests](https://github.com/Diversiam90815/NetLink/actions/workflows/tests.yml/badge.svg)](https://github.com/Diversiam90815/NetLink/actions/workflows/tests.yml)
[![Static Analysis](https://github.com/Diversiam90815/NetLink/actions/workflows/static-analysis.yml/badge.svg)](https://github.com/Diversiam90815/NetLink/actions/workflows/static-analysis.yml)

A C++23 static library for **LAN peer discovery and reliable peer-to-peer messaging over UDP**, designed to be embedded
in any application as a zero-friction CMake dependency.

## Overview

NetLink provides a single-header public API that hides all networking complexity behind a clean facade. Applications
register callbacks, call `init()`, and let NetLink handle UDP broadcast discovery, peer validation, the connection
handshake, reliable (acknowledged and retransmitted) messaging over a single UDP socket, and network adapter management.

Key design goals:

- **Single-header API**: consumers include only `<NetLink/NetLink.h>`
- **Pimpl isolation**: implementation details never leak into consumer translation units
- **Library-first**: tests are excluded from consumer builds automatically via `PROJECT_IS_TOP_LEVEL`

## Features

- **LAN Discovery**: UDP broadcast lets peers find each other without manual IP entry, scoped to the selected adapter's
  subnet
- **Peer Expiry**: a peer that stops announcing is dropped and reported via `onRemoteLost`
- **Peer Validation**: shared-secret and protocol-version checking before a connection is accepted
- **Reliable UDP Channel**: one dedicated UDP socket carries validation, the connection flow and application data. Every
  packet has a unique key and is confirmed by a `Data → DataAck → AckAck` exchange; anything unconfirmed is
  retransmitted. One acknowledgement datagram confirms everything that arrived since the last one
- **Congestion and Flow Control**: the sending rate adapts to the path (a lost packet is resent as soon as later ones
  are acknowledged), and an application that falls behind slows its sender down instead of piling up memory
- **Ordered, Exactly-Once Delivery**: `DeliveryMode::ReliableOrdered` messages arrive once and in send order, up to 16
  MiB (larger messages are fragmented transparently)
- **Unreliable Mode**: `DeliveryMode::UnreliableSequenced` for high-rate state updates: no acknowledgements, stale
  messages are dropped
- **Backpressure**: a bounded send queue per peer with a configurable `OverflowPolicy` (`DropNewest` / `DropOldest`);
  `send()` can wait for room instead of failing
- **Connection Loss Detection**: heartbeats supervise an idle session; a remote that stops acknowledging or goes silent
  is reported as `ConnectionState::Disconnected`; a declined invitation carries the remote's reason
- **Typed Messages**: opaque `Message` envelope with a `uint32_t` type tag and binary payload
- **Network Adapter Management**: enumerates adapters with priority hints; supports live adapter switching
- **Callback Model**: four event callbacks covering discovery, connection state, messages, and adapter changes

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                    Public API                       │
│              include/NetLink/NetLink.h              │
└──────────────────────┬──────────────────────────────┘
                       │  Pimpl
┌──────────────────────▼───────────────────────────────┐
│        NetLinkCore (wires all services)              │
│  ┌──────────────┐  ┌───────────────┐                 │
│  │  Discovery   │  │  Connection   │                 │
│  │  Service     │  │  Service      │                 │
│  └──────┬───────┘  └──────┬────────┘                 │
│         │         ┌───────▼──────────────────────┐   │
│         │         │ PeerChannel (1 UDP socket)   │   │
│         │         │  ReliableLink per peer       │   │
│         │         │  MessageAssembler per stream │   │
│         │         │  HeartbeatService            │   │
│         │         └───────┬──────────────────────┘   │
│  ┌──────▼─────────────────▼───────────────────────┐  │
│  │ IDatagramSocket · UdpSocket                    │  │
│  │ platform shim: Winsock / POSIX                 │  │
│  └────────────────────────────────────────────────┘  │
│  ┌──────────────────┐  ┌──────────────────────────┐  │
│  │ PeerValidation   │  │  NetworkInformation      │  │
│  │ Service          │  │  (adapter enumeration)   │  │
│  └──────────────────┘  └──────────────────────────┘  │
└──────────────────────────────────────────────────────┘
```

### Internal Services

| Module                  | Responsibility                                                                                        |
|-------------------------|-------------------------------------------------------------------------------------------------------|
| `DiscoveryService`      | UDP broadcast - advertises presence and collects peer announcements                                   |
| `ConnectionService`     | Orchestrates the connection lifecycle (invite → answer → ready flags) with per-state timeouts         |
| `PeerChannel`           | Owns the dedicated UDP socket, its I/O thread and the delivery thread that runs the callbacks         |
| `ReliableLink`          | Per-peer reliability: `seq`, stream IDs, `Data`/`DataAck`/`AckAck`, retransmit, ordering, windows     |
| `FragmentationService`  | Splits messages larger than one datagram into fragments                                               |
| `MessageAssembler`      | Puts the fragments of one stream back together, copying every byte once                               |
| `HeartbeatService`      | Keeps an idle session alive and detects a silent peer                                                 |
| `PeerValidationService` | Validates shared secret and protocol version before a connection is accepted                          |
| `NetworkInformation`    | Adapter enumeration (Windows / Linux / macOS backends); fires adapter-change events                   |
| Socket layer            | `UdpSocket` with `std::expected` error handling; OS specifics isolated in `Socket/Platform`           |
| `TimeoutService`        | Configurable timeout management across all async operations                                           |
| `IDeadlineTimer`        | Interruptible wait until a point in time; one implementation per platform (`Util/Timing`)             |

### Reliable UDP channel

Every datagram starts with a 20-byte header (magic, version, a bit-packed flags byte, source and destination
stream ID, 64-bit sequence number), extended by 4 bytes for fragments and by 4 bytes for the message type on the
packet that starts a message:

```
flags: bit 0-2 kind (Data, DataAck, AckAck, Heartbeat) · 3 reliable · 4 fragmented · 5 last fragment · 6 application channel
```

- **Sequencing**: every packet carries a 64-bit `seq` that never wraps, plus a random **stream ID**:
  the stream ID identifies one lifetime of a peer's stream, so a restarted peer (whose `seq` starts at 1 again)
  is detected and stale packets are ignored.
- **Three-way confirmation**: the sender retransmits `Data` until the `DataAck` arrives; the receiver retransmits the
  `DataAck` until the `AckAck` arrives. Duplicates are acknowledged again but delivered once. Acknowledgements are
  batched: one `DataAck` lists every seq that arrived since the last one (as ranges) and one `AckAck` confirms them, so
  the exchange costs two small datagrams per batch instead of two per packet. Both also carry the highest seq up to
  which everything is confirmed, which makes a lost acknowledgement harmless: the next one covers it.
- **Channels**: control signals (validation, connection flow) and application messages are two ordered streams with
  their own seqs and a 1024-packet send/receive window each. Application data can neither delay a control signal nor
  hold it back at the receiver.
- **Loss recovery**: a packet is resent as soon as three packets sent after it are acknowledged (fast retransmit),
  otherwise after its retransmission timeout (RFC 6298, backing off while nothing is acknowledged).
- **Congestion control**: a congestion window limits the packets in flight. It doubles per round trip until the first
  loss, then grows by one packet per round trip and halves with every round of losses.
- **Flow control**: every `DataAck` tells the sender whether the receiver's application keeps up. While it does not,
  the sender pauses the application channel and only asks again every 50 ms.
- **Fragmentation**: messages above one datagram (1200 bytes on the wire, below the Ethernet MTU) are split into
  fragments, each with its own `seq`. Fragments are sent straight out of the message and appended straight into the
  reassembled one: a payload byte is copied once on each side.
- **Loss of the peer**: data that stays unacknowledged for 5 s without any acknowledgement arriving, a session peer
  that stays silent for 5 s, or a peer restart ends the session.

### Threads

| Thread            | Does                                                                                              |
|-------------------|---------------------------------------------------------------------------------------------------|
| I/O thread        | Reads the socket, acknowledges, retransmits, sends heartbeats. Never runs a callback.              |
| Delivery thread   | Hands control signals to the connection and validation services                                   |
| Event thread      | Runs the application's callbacks, one at a time                                                   |
| Timeout thread    | Fires the timeouts of the connection and validation flow                                          |

A callback that takes long therefore delays the next callback, but neither acknowledgements nor control signals.

## Public API

All types live in the `netlink` namespace. Single include:

```cpp
#include <NetLink/NetLink.h>
```

### Types at a glance

| Type               | Description                                                                                                                                             |
|--------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------|
| `Endpoint`         | `IPAddress`, `port`, `displayName` : identifies a remote peer                                                                                           |
| `Message`          | `type` (`uint32_t`) + `data` (`vector<uint8_t>`) : opaque message envelope                                                                              |
| `NetworkAdapter`   | Adapter metadata: name, network, IPv4, ID, `AdapterPriority`                                                                                            |
| `ConnectionState`  | `None` · `Hosting` · `Searching` · `PendingInbound` · `Connected` · `Disconnected` · `Error`                                                            |
| `NetLinkConfig`    | `localDisplayName`, `discoveryPort` (default 5555), `broadcastAddress`, `secret`, `applicationVersion`, `sendQueueCapacity`, `sendQueueOverflow`        |
| `DeliveryMode`     | `ReliableOrdered` (default: acknowledged, retransmitted, in order, up to 16 MiB) · `UnreliableSequenced` (one datagram, may drop, stale ones discarded) |
| `OverflowPolicy`   | `DropNewest` (default: `send()` returns false when the queue is full) · `DropOldest` (the oldest unsent message is discarded)                           |
| `NetLinkCallbacks` | Five `std::function` callbacks (see below)                                                                                                              |

### Callbacks

```cpp
netlink::NetLinkCallbacks cb;
cb.onRemoteDiscovered      = [](const netlink::Endpoint &e)          { /* compatible peer found and validated */ };
cb.onRemoteLost            = [](const netlink::Endpoint &e)          { /* peer stopped announcing */ };
cb.onConnectionChanged     = [](netlink::ConnectionEvent ev)         { /* state machine update */ };
cb.onMessageReceived       = [](const netlink::Message &msg)         { /* handle inbound data */ };
cb.onNetworkAdapterChanged = [](const netlink::NetworkAdapter &a)    { /* adapter hotplug event */ };
```

Callbacks run one at a time on NetLink's event thread, never while internal locks are held. Calling back into NetLink
from a callback (e.g. `respondToConnection()` or `disconnect()`) is safe; destroying the `NetLink` instance inside a
callback is not. Marshal onto your own thread (e.g. a game loop) if required.

### Typical usage

```cpp
#include <NetLink/NetLink.h>

netlink::NetLink net;

// 1. Configure
netlink::NetLinkConfig cfg;
cfg.localDisplayName   = "MyApp";
cfg.secret             = "shared-secret";
cfg.applicationVersion = "1.4.0";   // peers must agree on major.minor

netlink::NetLinkCallbacks cb;
cb.onRemoteDiscovered  = [&](const netlink::Endpoint &e) {
    net.connectTo(e);   // connect to the first compatible peer we find
};
cb.onConnectionChanged = [](netlink::ConnectionEvent ev) {
    if (ev.state == netlink::ConnectionState::Connected) {
        // session is up — ready to send
    }
};
cb.onMessageReceived   = [](const netlink::Message &msg) {
    // msg.type  → identifies the message kind (application-defined)
    // msg.data  → raw payload bytes
};

// 2. Init & discover
net.configure(cfg, cb);
net.init();
net.startDiscovery();

// 3. Send (once connected)
netlink::Message m;
m.type = 1;
m.data = {0x01, 0x02, 0x03};
net.send(m);

// 4. Tear down
net.shutdown();
```

### `NetLink` method reference

| Method                          | Description                                                                                                                                                                                 |
|---------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `configure(config, callbacks)`  | Register configuration and callbacks — call before `init()`                                                                                                                                 |
| `init()`                        | Enumerate network adapters, select the preferred one if none is active, and bind sockets                                                                                                    |
| `shutdown()`                    | Tear down all services; safe to call multiple times                                                                                                                                         |
| `startDiscovery()`              | Begin broadcasting and listening for peers                                                                                                                                                  |
| `stopDiscovery()`               | Stop discovery without closing an active session                                                                                                                                            |
| `getPotentialEndpoints()`       | Snapshot of currently validated remote peers                                                                                                                                                |
| `connectTo(endpoint)`           | (Client) initiate a connection to a discovered peer                                                                                                                                         |
| `respondToConnection(accepted)` | (Host) accept or reject a pending inbound connection                                                                                                                                        |
| `disconnect()`                  | Close the active session (the remote is notified)                                                                                                                                           |
| `getConnectionState()`          | Query the current `ConnectionState`                                                                                                                                                         |
| `send(message, mode, timeout)`  | Send a `Message` to the connected peer (`mode` defaults to `ReliableOrdered`); false when not connected, when an unreliable message exceeds one datagram, or when the send queue refused it. With a `timeout`, a reliable message waits that long for room in a full queue before it is refused |
| `send(type, payload, mode, timeout)` | Convenience overload — constructs a `Message` inline                                                                                                                                   |
| `getAvailableAdapters()`        | List all network adapters with their priority hints                                                                                                                                         |
| `setActiveAdapter(id)`          | Switch the active network adapter by ID                                                                                                                                                     |
| `getActiveAdapterID()`          | ID of the currently active adapter (0 if none)                                                                                                                                              |

## Integrating via CPM

```cmake
include(cmake/cpm.cmake)   # or however CPM is loaded in your project

CPMAddPackage(
        NAME NetLink
        GITHUB_REPOSITORY Diversiam90815/NetLink
        VERSION 0.3.0
)

target_link_libraries(YourTarget PRIVATE NetLink::NetLink)
```

NetLink's test suite is excluded from consumer builds automatically. To opt back in:

```cmake
set(NETLINK_BUILD_TESTS ON CACHE BOOL "" FORCE)
```

## Requirements

- C++23 compiler
- CMake 4.0+
- Windows 10+, Linux (libnl-3 for Wi-Fi information) or macOS

## Dependencies

Fetched automatically at configure time via [CPM](https://github.com/cpm-cmake/CPM.cmake).

| Library                                                 | Version | Role                                       |
|---------------------------------------------------------|---------|--------------------------------------------|
| [nlohmann/json](https://github.com/nlohmann/json)       | 3.11.3  | Discovery and control signal serialization |
| [GoogleTest](https://github.com/google/googletest)      | 1.15.2  | Unit testing (standalone builds only)      |
| [Google Benchmark](https://github.com/google/benchmark) | 1.9.4   | Benchmarks (standalone builds only)        |

## Standalone Build

```bash
cmake -B build -S . && cmake --build build
```

Run tests:

```bash
ctest --test-dir build
```

## Benchmarks

`NetLinkBenchmarks` measures the library's production code as it ships: the unmodified `NetLink` target, real UDP
sockets on loopback, and services wired the same way `NetLinkCore` wires them. Each benchmark answers one question:

| Benchmark | Question | Time is |
|---|---|---|
| `BM_UdpSocket_RoundTrip` | What is the operating system's floor for a round trip? | one datagram there and back (µs) |
| `BM_UdpSocket_FanIn` | How much does the OS drop when many senders flood one socket? | all senders' bursts sent (ms) |
| `BM_ReliableLink_Transfer` | What does the reliability protocol cost in CPU, with and without loss? | one message, no sockets (µs) |
| `BM_Fragmentation_Reassemble` | What does reassembling a large message cost? | one message (µs) |
| `BM_PeerChannel_Throughput` | How many messages and MiB/s get through a channel? | a batch until all arrived (ms) |
| `BM_PeerChannel_RoundTrip` | How long does a reliable request/reply take? | one request and its reply (µs) |
| `BM_PeerChannel_FanIn` | Does a hub keep up with many peers sending at once? | all messages arrived or delivery stopped (ms) |
| `BM_TimeoutService_StartCancel` | What does arming and cancelling a timeout cost with many active? | one arm + cancel (µs) |
| `BM_TimeoutService_FireLatency` | How quickly does a due timeout fire with many active? | arming until the callback ran (µs) |
| `BM_TimeoutService_TimerResolution` | How precisely do short timeouts fire? | the whole timeout (ms) |
| `BM_TimeoutService_MassExpiry` | What happens when thousands of timeouts expire together? | until every callback ran (ms) |
| `BM_TaskQueue_Latency` / `_Throughput` | How fast is the event queue? | one hand-off (µs) / 100k tasks (ms) |
| `BM_PeerValidation_Validate` | How long does validating a peer take, per check? | both peers validated each other (µs) |
| `BM_PeerValidation_Swarm` | Does validation scale when many peers appear at once? | all peers validated (ms) |
| `BM_ConnectionService_Establish` | How long does session setup take, idle and on a saturated channel? | invitation until both sides are connected (ms) |
| `BM_ConnectionService_InvitationStorm` | What if many peers invite one host at once? | every peer answered (ms) |
| `BM_DiscoveryRegistry_Announcement` | What does one discovery announcement cost with many known peers? | one announcement (ns) |
| `BM_NetLinkCore_Throughput` / `_RoundTrip` / `_Connect` | The same, end to end through the public API | as for `PeerChannel` / connect (µs) |


```bash
python build.py --benchmark                                      # everything, Release, JSON written to build/<arch>/benchmarks/results
python build.py --benchmark --benchmark-filter=TimeoutService    # one module (regex on the name)
python build.py --benchmark --benchmark-repetitions=5            # every benchmark 5 times: mean, median, stddev, cv
```

The binary accepts all Google Benchmark flags. To compare two runs, use `compare.py` from Google Benchmark's `tools/`
folder. The target is built with the project like the tests (`NETLINK_BUILD_BENCHMARKS`, ON for top-level builds), but it
is never run by CI or ctest.

## Design Highlights

| Pattern                  | Where applied                                                                                                                                                                                  |
|--------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Pimpl**                | `NetLink` exposes zero implementation headers — `struct Impl` is defined only in `src/NetLink.cpp`                                                                                             |
| **Pure protocol core**   | `ReliableLink`, `MessageAssembler` and `HeartbeatService` contain no sockets or threads and take the time as a parameter, so loss, duplication and reordering are tested deterministically     |
| **Seams for testing**    | `IDatagramSocket` lets the whole stack run on an in-memory network with configurable loss (`tests/Fakes`)                                                                                      |
| **Observer / Callbacks** | `NetLinkCallbacks` wires application code to async events without coupling to internals                                                                                                        |
| **Active Object**        | `ThreadBase` backs the single I/O thread of `PeerChannel` (receive, retransmit, heartbeats); callbacks run on separate threads fed by a `TaskQueue`                                            |
| **Platform seams**       | `IDeadlineTimer` and the socket `ReadWaiter` hide how each operating system waits precisely (high resolution waitable timer, `ppoll`, `kqueue`)                                                 |

## Platform

Windows, Linux and macOS. Platform specific code is confined to `src/internal/Network/NetworkInformation*`,
`src/internal/Socket/Platform/SocketPlatform*` and `src/internal/Util/Timing/DeadlineTimer*`; CMake selects the
matching backend.

## Compatibility

Set `NetLinkConfig::applicationVersion` to your application's own version: it is what
decides whether two instances can talk to each other. If you leave it empty, NetLink
advertises its own version instead, which is only useful until your application has
versioning of its own.

NetLink 0.3 replaced the TCP data connection by the reliable UDP channel. Its wire protocol is not compatible with
0.2: peers of both versions do not validate each other.

NetLink 0.4 changed the wire protocol of that channel (batched acknowledgements, separate streams for control signals
and application messages, congestion and flow control). Peers of 0.3 and 0.4 ignore each other's channel packets and
therefore never validate each other.

Two peers are compatible when the **major and minor** components of that version match.
The patch and trailing build number are ignored, so builds from different commits of the
same release interoperate. A peer reporting an incompatible version is never offered
through `onRemoteDiscovered`.

Discovery is scoped to the selected adapter's subnet: announcements go to that subnet's
directed broadcast, and announcements arriving from other subnets are ignored. A peer that
has not announced for three announcement intervals (6 s by default) is dropped and reported
through `onRemoteLost`.

## License

MIT — see [LICENSE](LICENSE).
