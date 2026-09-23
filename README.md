# NetLink

[![Windows Build](https://github.com/Diversiam90815/NetLink/actions/workflows/windows.yml/badge.svg)](https://github.com/Diversiam90815/NetLink/actions/workflows/windows.yml)
[![macOS Build](https://github.com/Diversiam90815/NetLink/actions/workflows/macos.yml/badge.svg)](https://github.com/Diversiam90815/NetLink/actions/workflows/macos.yml)
[![Linux Build](https://github.com/Diversiam90815/NetLink/actions/workflows/linux.yml/badge.svg)](https://github.com/Diversiam90815/NetLink/actions/workflows/linux.yml)
[![Tests](https://github.com/Diversiam90815/NetLink/actions/workflows/tests.yml/badge.svg)](https://github.com/Diversiam90815/NetLink/actions/workflows/tests.yml)
[![Static Analysis](https://github.com/Diversiam90815/NetLink/actions/workflows/static-analysis.yml/badge.svg)](https://github.com/Diversiam90815/NetLink/actions/workflows/static-analysis.yml)

A C++23 static library for **LAN peer discovery and peer-to-peer TCP communication**, designed to be embedded in any application as a zero-friction CMake dependency.

## Overview

NetLink provides a single-header public API that hides all networking complexity behind a clean facade. Applications register callbacks, call `init()`, and let NetLink handle UDP broadcast discovery, connection role negotiation, async TCP sessions, and network adapter management.

Key design goals:
- **Single-header API**: consumers include only `<NetLink/NetLink.h>`
- **Pimpl isolation**: implementation details never leak into consumer translation units
- **Library-first**: tests are excluded from consumer builds automatically via `PROJECT_IS_TOP_LEVEL`

## Features

- **LAN Discovery**: UDP broadcast lets peers find each other without manual IP entry, scoped to the selected adapter's subnet
- **Peer Expiry**: a peer that stops announcing is dropped and reported via `onRemoteLost`
- **Role Negotiation**: automatic host/client role assignment during the connection handshake
- **Peer Validation**: shared-secret and protocol-version checking before a connection is accepted
- **TCP Sessions**: full-duplex, length-framed message passing on a dependency-free socket layer (Winsock / POSIX)
- **Pluggable Transports**: the data transport is selected via `NetLinkConfig::transport`; every send carries a `DeliveryMode` so reliable-UDP transports can be added without API changes
- **Connection Loss Detection**: a dropped TCP connection is reported as `ConnectionState::Disconnected`; a declined invitation carries the remote's reason
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
│  ┌──────────────┐  ┌───────────────┐  ┌───────────┐  │
│  │  Discovery   │  │  Connection   │  │ Signaling │  │
│  │  Service     │  │  Service      │  │ Service   │  │
│  └──────┬───────┘  └──────┬────────┘  └──────┬────┘  │
│         │                 │                  │       │
│         │         IServer / IClient          │       │
│         │          ┌──────▼──────┐           │       │
│         │          │TCP Transport│           │       │
│         │          └──────┬──────┘           │       │
│  ┌──────▼─────────────────▼──────────────────▼────┐  │
│  │ IDatagramSocket (Discovery, Signaling)         │  │
│  │ UdpSocket · TcpListener · TcpStream            │  │
│  │ platform shim: Winsock / POSIX                 │  │
│  └────────────────────────────────────────────────┘  │
│  ┌──────────────────┐  ┌──────────────────────────┐  │
│  │ PeerValidation   │  │  NetworkInformation      │  │
│  │ Service          │  │  (adapter enumeration)   │  │
│  └──────────────────┘  └──────────────────────────┘  │
└──────────────────────────────────────────────────────┘
```

### Internal Services

| Module | Responsibility |
|--------|----------------|
| `DiscoveryService` | UDP broadcast - advertises presence and collects peer announcements |
| `ConnectionService` | Orchestrates the full connection lifecycle with timeout and retry logic |
| `SignalingService` | UDP control messages for handshake and disconnect coordination |
| `PeerValidationService` | Validates shared secret and protocol version before a connection is accepted |
| `RemoteCommunication` | Dedicated async send/receive threads; dispatches typed `Message` objects |
| `NetworkInformation` | Adapter enumeration (Windows / Linux / macOS backends); fires adapter-change events |
| Socket layer | `UdpSocket`, `TcpListener`, `TcpStream` with `std::expected` error handling; OS specifics isolated in `Socket/Platform` |
| `TimeoutService` | Configurable timeout and retry management across all async operations |

## Public API

All types live in the `netlink` namespace. Single include:

```cpp
#include <NetLink/NetLink.h>
```

### Types at a glance

| Type | Description |
|------|-------------|
| `Endpoint` | `IPAddress`, `port`, `displayName` : identifies a remote peer |
| `Message` | `type` (`uint32_t`) + `data` (`vector<uint8_t>`) : opaque message envelope |
| `NetworkAdapter` | Adapter metadata: name, network, IPv4, ID, `AdapterPriority` |
| `ConnectionState` | `None` · `Hosting` · `Searching` · `PendingInbound` · `Connected` · `Disconnected` · `Error` |
| `NetLinkConfig` | `localDisplayName`, `discoveryPort` (default 5555), `broadcastAddress`, `secret`, `transport`, `applicationVersion` |
| `DeliveryMode` | `ReliableOrdered` (default) · `UnreliableSequenced` — TCP delivers both reliably |
| `TransportKind` | `Tcp` |
| `NetLinkCallbacks` | Five `std::function` callbacks (see below) |

### Callbacks

```cpp
netlink::NetLinkCallbacks cb;
cb.onRemoteDiscovered      = [](const netlink::Endpoint &e)          { /* compatible peer found and validated */ };
cb.onRemoteLost            = [](const netlink::Endpoint &e)          { /* peer stopped announcing */ };
cb.onConnectionChanged     = [](netlink::ConnectionEvent ev)         { /* state machine update */ };
cb.onMessageReceived       = [](const netlink::Message &msg)         { /* handle inbound data */ };
cb.onNetworkAdapterChanged = [](const netlink::NetworkAdapter &a)    { /* adapter hotplug event */ };
```

Callbacks run one at a time on NetLink's event thread, never while internal locks are held. Calling back into NetLink from a callback (e.g. `respondToConnection()` or `disconnect()`) is safe; destroying the `NetLink` instance inside a callback is not. Marshal onto your own thread (e.g. a game loop) if required.

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

| Method | Description |
|--------|-------------|
| `configure(config, callbacks)` | Register configuration and callbacks — call before `init()` |
| `init()` | Enumerate network adapters, select the preferred one if none is active, and bind sockets |
| `shutdown()` | Tear down all services; safe to call multiple times |
| `startDiscovery()` | Begin broadcasting and listening for peers |
| `stopDiscovery()` | Stop discovery without closing an active session |
| `getPotentialEndpoints()` | Snapshot of currently validated remote peers |
| `connectTo(endpoint)` | (Client) initiate a connection to a discovered peer |
| `respondToConnection(accepted)` | (Host) accept or reject a pending inbound connection |
| `disconnect()` | Close the active TCP session |
| `getConnectionState()` | Query the current `ConnectionState` |
| `send(message, mode)` | Send a `Message` to the connected peer (`mode` defaults to `ReliableOrdered`) |
| `send(type, payload, mode)` | Convenience overload — constructs a `Message` inline |
| `getAvailableAdapters()` | List all network adapters with their priority hints |
| `setActiveAdapter(id)` | Switch the active network adapter by ID |
| `getActiveAdapterID()` | ID of the currently active adapter (0 if none) |

## Integrating via CPM

```cmake
include(cmake/cpm.cmake)   # or however CPM is loaded in your project

CPMAddPackage(
    NAME    NetLink
    GITHUB_REPOSITORY Diversiam90815/NetLink
    VERSION 0.2.0
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
- Windows 10+, Linux (libnl-3 for WiFi information) or macOS

## Dependencies

Fetched automatically at configure time via [CPM](https://github.com/cpm-cmake/CPM.cmake).

| Library | Version | Role |
|---------|---------|------|
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | Discovery packet serialization |
| [GoogleTest](https://github.com/google/googletest) | 1.15.2 | Unit testing (standalone builds only) |

## Standalone Build

```bash
cmake -B build -S . && cmake --build build
```

Run tests:

```bash
ctest --test-dir build
```

## Design Highlights

| Pattern | Where applied |
|---------|---------------|
| **Pimpl** | `NetLink` exposes zero implementation headers — `struct Impl` is defined only in `src/NetLink.cpp` |
| **Factory / Strategy** | `ITransportFactory` → `TCPTransportFactory` decouples transport creation from connection logic; a reliable-UDP transport plugs in the same way |
| **Seams for testing** | `IDatagramSocket` lets Discovery/Signaling run on an in-memory network with configurable loss (`tests/Fakes`) |
| **Observer / Callbacks** | `NetLinkCallbacks` wires application code to async events without coupling to internals |
| **Active Object** | `ThreadBase` utility backs dedicated send and receive threads in `RemoteCommunication` |

## Platform

Windows, Linux and macOS. Platform specific code is confined to `src/internal/Network/NetworkInformation*` and `src/internal/Socket/Platform/SocketPlatform*`; CMake selects the matching backend.


## Compatibility

Set `NetLinkConfig::applicationVersion` to your application's own version: it is what
decides whether two instances can talk to each other. If you leave it empty, NetLink
advertises its own version instead, which is only useful until your application has
versioning of its own.

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
