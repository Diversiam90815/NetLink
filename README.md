# NetLink

[![Tests](https://github.com/Diversiam90815/NetLink/actions/workflows/tests.yml/badge.svg)](https://github.com/Diversiam90815/NetLink/actions/workflows/tests.yml)
[![Sanitizers](https://github.com/Diversiam90815/NetLink/actions/workflows/sanitizers.yml/badge.svg)](https://github.com/Diversiam90815/NetLink/actions/workflows/sanitizers.yml)
[![Static Analysis](https://github.com/Diversiam90815/NetLink/actions/workflows/static-analysis.yml/badge.svg)](https://github.com/Diversiam90815/NetLink/actions/workflows/static-analysis.yml)

A C++23 static library for **LAN peer discovery and peer-to-peer messaging over UDP**. One header, no dependencies,
Windows, Linux and macOS.

## Features

- **Discovery**: peers on the same subnet find each other by UDP broadcast, without any address being entered
- **Sessions with many peers**: up to 256 at once, opened in one round trip
- **Reliable messages** up to 16 MiB: delivered once and in order, fragmented and retransmitted as needed
- **Lanes**: `Reliable` for normal messages, `Bulk` for large transfers that must not hold up anything else, `Media`
  for data that is worthless when late
- **Fair and paced**: one send budget for all peers, congestion control per peer, and a slow receiver slows its
  senders down instead of piling up memory
- **Loss detection**: a peer that stops answering ends its session after `peerTimeout`
- **Network adapters**: lists them, lets the application choose, and notices when the address changes

## Quick start

```cpp
#include <NetLink/NetLink.h>

netlink::NetLink net;

netlink::NetLinkConfig cfg;
cfg.displayName = "Living room";
cfg.appId       = "com.example.my-app";   // only peers with the same appId see each other
cfg.appVersion  = "1.4.0";                // ... and the same major.minor

netlink::NetLinkCallbacks cb;
cb.onPeerDiscovered = [&](const netlink::PeerInfo &peer) { net.connect(peer.id); };
cb.onConnected      = [&](const netlink::PeerInfo &peer) {
    const std::vector<uint8_t> hello{1, 2, 3};
    net.send(peer.id, 1, hello);
};
cb.onMessage = [](netlink::PeerId from, netlink::Lane, netlink::Message &&msg) {
    // msg.type: what kind of message (application-defined), msg.data: the payload
};

net.start(cfg, cb);
net.startDiscovery();
// ...
net.stop();
```

## API

All types live in the `netlink` namespace; the only include is `<NetLink/NetLink.h>`.

| Method | Description |
|---|---|
| `start(config, callbacks)` / `stop()` | Start on the preferred network adapter; `stop()` tells every peer and ends all sessions |
| `startDiscovery()` / `stopDiscovery()` | Announce this peer. Peers that announce themselves are found either way |
| `peers()` / `connectedPeers()` | The peers that are discovered, and those a session exists with |
| `connect(peer)` | Ask a discovered peer for a session: `onConnected` or `onDisconnected` follows |
| `accept(peer)` / `decline(peer)` | Answer `onConnectionRequest`. Unanswered requests are declined after 30 s |
| `disconnect(peer)` | Send what is still waiting (up to 1 s), then end the session |
| `send(peer, type, data, lane, timeout)` | Queue a message. Returns `Queued`, `QueueFull`, `TooLarge`, `NotConnected` or `NotRunning` |
| `broadcast(type, data, lane)` | Queue one message for every connected peer |
| `stats(peer)` | Round-trip time, bytes queued / sent / received, retransmissions, Media loss |
| `getAvailableAdapters()` / `setActiveAdapter(id)` / `getActiveAdapterID()` | Network adapter selection. Switching ends every session |

| Callback | Called when |
|---|---|
| `onPeerDiscovered(PeerInfo)` / `onPeerLost(PeerId)` | A peer of this application announced itself / stopped announcing |
| `onConnectionRequest(PeerInfo)` | A peer asks for a session. Not set: every request is accepted |
| `onConnected(PeerInfo)` | A session is open |
| `onDisconnected(PeerId, DisconnectReason)` | A session, a `connect()` or a request ended: `Local`, `Remote`, `Declined`, `Incompatible`, `Lost`, `NetworkError`, `Shutdown` |
| `onMessage(PeerId, Lane, Message &&)` | A message arrived |
| `onNetworkAdapterChanged(NetworkAdapter)` | NetLink runs on this adapter now |
| `onLog(LogLevel, std::string_view)` | NetLink has something for the application's log. Not set: nothing is logged |

Callbacks run one at a time on NetLink's event thread. Every method may be called from any thread, callbacks
included; only destroying the `NetLink` instance inside a callback is not allowed.

`NetLinkConfig`: `displayName`, `appId` (required), `appVersion`, `discoveryPort` (5555), `sendQueueBytes` (64 MiB per
lane of a peer), `maxSendRate` (80,000 datagrams per second), `peerTimeout` (5 s).

## Usage notes

| Lane | Guarantee | Size | Use it for |
|---|---|---|---|
| `Reliable` | once, in order | 16 MiB | normal messages |
| `Bulk` | once, in order, sent last | 16 MiB | files and other large transfers |
| `Media` | sent once: may be lost or overtaken | 64 KiB | audio, video, positions |

- **Keep callbacks short.** While one runs, no other is delivered. The network does not wait for it; if more than
  32 MiB pile up, the peers are asked to hold `Reliable` and `Bulk` back until the application has caught up.
- **Large blobs**: send them as chunks of about 1 MiB on `Bulk`, followed by an end marker on the same lane. Give
  `send()` a timeout so it waits while the queue is full instead of returning `QueueFull`.
- **Media** is never retransmitted: a message is lost when one of its datagrams is. Keep frames at or below 9 KB
  (8 datagrams); at 1% datagram loss 92% of them arrive, of 64 KiB frames only 56%. If more is produced than
  `maxSendRate` lets out, only the newest 32 messages are kept. Adapt the bitrate to `stats()`: lower it when
  `mediaDropped` grows, `mediaLoss` exceeds a few percent or `rtt` rises.
- **Wi-Fi**: the default `maxSendRate` suits wired gigabit networks; on Wi-Fi start with 15,000 to 20,000.
- **Firewall**: NetLink receives UDP on the discovery port and on a port the operating system assigns. Allow inbound
  UDP for the application rather than for a port. On macOS the application needs `NSLocalNetworkUsageDescription` in
  its `Info.plist`.
- **Several instances** on one machine, and several applications on the same discovery port, work.
- **The `appId` is not security.** It keeps applications apart, nothing more: anyone on the network who knows it can
  connect, and nothing is encrypted.
- **Limits**: 256 sessions, 1024 discovered peers, display names up to 64 bytes, IPv4 only.

## How it works

NetLink runs two threads. The **I/O thread** owns the sockets, the sessions and every timer; application threads
only hand it messages and commands. The **event thread** runs the callbacks. Every 2 s an instance announces itself
on its subnet; a peer that was not heard for 6 s is forgotten, unless a session with it exists.

### Protocol

One UDP socket per instance carries everything. Datagrams are at most 1200 bytes, numbers are big endian.

```
u16 magic 0x4E4C · u8 version 3 · u8 flags · u32 srcStream · u32 dstStream · u64 seq
[fragment: u16 index, u16 count] [message start: u32 type] [fragment 0: u32 totalLength] · body

flags: bit 0-1 kind (Data, Ack, Ping, Beacon) · 2-3 lane (Control, Reliable, Bulk, Media) · 4 fragmented · 5 last fragment · 6 paused · 7 reserved
```

- **Data** carries a message or one fragment of it (1168 bytes per fragment). `Control`, `Reliable` and `Bulk` each
  count their own `seq` and are acknowledged; `Media` is not.
- **Ack** names the highest `seq` received without a gap and lists what waits behind one. Lost packets are sent
  again once three later ones are acknowledged, otherwise after a timeout. The pause bit asks the sender to hold a
  lane back.
- **Ping** asks for an Ack: sent to a peer that was quiet for 1 s, and to probe a paused lane.
- **Sessions** use the `Control` lane: `Hello` (the first packet of a link) is answered with `Accept` or `Decline`,
  `Close` ends a session. Stream IDs tie every packet to its session.
- **Beacon** is the discovery announcement: instance ID, a hash of the `appId`, the application version and the
  display name. A peer is reached at the address its beacon came from.

Bit 7 of the flags and an extension field in `Hello`, `Accept` and `Beacon` are reserved for encryption.

## Integration

```cmake
CPMAddPackage(
        NAME NetLink
        GITHUB_REPOSITORY Diversiam90815/NetLink
        VERSION 0.5.0
)

target_link_libraries(YourTarget PRIVATE NetLink::NetLink)
```

Requires a C++23 compiler and CMake 4.0+. Tests and benchmarks are only built when NetLink is the top-level project
(`NETLINK_BUILD_TESTS`, `NETLINK_BUILD_BENCHMARKS`).

```bash
cmake -B build -S . && cmake --build build     # standalone build
ctest --test-dir build                         # tests
python build.py --benchmark                    # benchmarks, Release; see --benchmark-filter and --benchmark-repetitions
```

## Compatibility

Two peers see each other when their `appId` is the same and the major and minor components of `appVersion` match;
patch and build number are ignored. With an empty `appVersion` NetLink uses its own version.

NetLink 0.5 changed the API and the wire protocol: it does not talk to 0.4 or earlier.

## License

MIT — see [LICENSE](LICENSE).
