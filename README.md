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

- **LAN Discovery**: UDP broadcast lets peers find each other without manual IP entry
- **Role Negotiation**: automatic host/client role assignment during the connection handshake
- **Peer Validation**: configurable shared secret and version checking before a connection is accepted
- **Async TCP Sessions**: full-duplex message passing built on standalone ASIO
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
┌──────────────────────▼──────────────────────────────┐
│                  NetLink::Impl                      │
│  ┌──────────────┐  ┌───────────────┐  ┌──────────┐  │
│  │  Discovery   │  │  Connection   │  │Signaling │  │
│  │  Service     │  │  Service      │  │Service   │  │
│  └──────┬───────┘  └──────┬────────┘  └─────┬────┘  │
│         │                 │                 │       │
│  ┌──────▼─────────────────▼─────────────────▼────┐  │
│  │              TCP Transport Layer              │  │
│  │      TCPClient / TCPServer / TCPSession       │  │
│  └───────────────────────────────────────────────┘  │
│  ┌──────────────────┐  ┌─────────────────────────┐  │
│  │ PeerValidation   │  │  NetworkInformation     │  │
│  │ Service          │  │  (adapter enumeration)  │  │
│  └──────────────────┘  └─────────────────────────┘  │
└─────────────────────────────────────────────────────┘
```

### Internal Services

| Module | Responsibility |
|--------|----------------|
| `DiscoveryService` | UDP broadcast - advertises presence and collects peer announcements |
| `ConnectionService` | Orchestrates the full connection lifecycle with timeout and retry logic |
| `SignalingService` | UDP control messages for handshake and disconnect coordination |
| `PeerValidationService` | Validates shared secret before a connection is accepted |
| `RemoteCommunication` | Dedicated async send/receive threads; dispatches typed `Message` objects |
| `NetworkInformation` | Windows adapter enumeration via `iphlpapi`; fires adapter-change events |
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
| `NetLinkConfig` | `localDisplayName`, `discoveryPort` (default 5555), `broadcastAddress`, `secret` |
| `NetLinkCallbacks` | Four `std::function` callbacks (see below) |

### Callbacks

```cpp
netlink::NetLinkCallbacks cb;
cb.onRemoteDiscovered      = [](const netlink::Endpoint &e)         { /* peer found on LAN */ };
cb.onConnectionChanged     = [](netlink::ConnectionEvent ev)         { /* state machine update */ };
cb.onMessageReceived       = [](const netlink::Message &msg)         { /* handle inbound data */ };
cb.onNetworkAdapterChanged = [](const netlink::NetworkAdapter &a)    { /* adapter hotplug event */ };
```

### Typical usage

```cpp
#include <NetLink/NetLink.h>

netlink::NetLink net;

// 1. Configure
netlink::NetLinkConfig cfg;
cfg.localDisplayName = "MyApp";
cfg.secret           = "shared-secret";

netlink::NetLinkCallbacks cb;
cb.onRemoteDiscovered  = [&](const netlink::Endpoint &e) {
    net.connectTo(e);   // connect to the first peer we find
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
| `init()` | Initialize sockets and enumerate network adapters |
| `shutdown()` | Tear down all services; safe to call multiple times |
| `startDiscovery()` | Begin broadcasting and listening for peers |
| `stopDiscovery()` | Stop discovery without closing an active session |
| `getPotentialEndpoints()` | Snapshot of currently validated remote peers |
| `connectTo(endpoint)` | (Client) initiate a connection to a discovered peer |
| `respondToConnection(accepted)` | (Host) accept or reject a pending inbound connection |
| `disconnect()` | Close the active TCP session |
| `getConnectionState()` | Query the current `ConnectionState` |
| `send(message)` | Send a `Message` to the connected peer |
| `send(type, payload)` | Convenience overload — constructs a `Message` inline |
| `getAvailableAdapters()` | List all network adapters with their priority hints |
| `setActiveAdapter(id)` | Switch the active network adapter by ID |

## Integrating via CPM

```cmake
include(cmake/cpm.cmake)   # or however CPM is loaded in your project

CPMAddPackage(
    NAME    NetLink
    GITHUB_REPOSITORY Diversiam90815/NetLink
    VERSION 0.1.0
)

target_link_libraries(YourTarget PRIVATE NetLink)
```

NetLink's test suite is excluded from consumer builds automatically. To opt back in:

```cmake
set(NETLINK_BUILD_TESTS ON CACHE BOOL "" FORCE)
```

## Requirements

- C++23 compiler
- CMake 4.0+
- Windows 10+ (`_WIN32_WINNT=0x0A00`)

## Dependencies

Fetched automatically at configure time via [CPM](https://github.com/cpm-cmake/CPM.cmake) — no manual installation required.

| Library | Version | Role |
|---------|---------|------|
| [ASIO](https://github.com/chriskohlhoff/asio) | 1.30.2 | Standalone async I/O |
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
| **Pimpl** | `NetLink` exposes zero implementation headers — `struct Impl` is defined only in `src/NetLinkImpl.h` |
| **Factory / Strategy** | `ITransportFactory` → `TCPTransportFactory` decouples transport creation from connection logic |
| **Observer / Callbacks** | `NetLinkCallbacks` wires application code to async events without coupling to internals |
| **Active Object** | `ThreadBase` utility backs dedicated send and receive threads in `RemoteCommunication` |

## Platform

Windows 10+ only. Network adapter enumeration and socket initialization rely on `iphlpapi`, WinSock2, and related Windows APIs. Cross-platform support is not a current goal.

## License

MIT — see [LICENSE](LICENSE).
