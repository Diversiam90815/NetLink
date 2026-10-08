# Benchmark baseline

Reference numbers for the v3 redesign. Every later phase is compared against the **Phase 0** column.

| | |
|---|---|
| Date | 2026-10-03 |
| Commit | Phase 0 changes on top of `0883ca2` (branch `Increase-performance`); the `0883ca2` column is that commit plus `BM_PeerChannel_FanOut` |
| Build type | Release (`-O3 -DNDEBUG`) |
| Compiler | MinGW-w64 g++ 15.2.0 (CLion bundled), CMake 4.3.1, Ninja 1.13.2 |
| CPU | AMD Ryzen 9 5900X, 12 cores / 24 threads |
| OS | Windows 11 Home 10.0.26300 |
| Network | UDP on loopback (127.0.0.1) |

Both columns were measured in one session, one binary after the other, with the flags this command passes to
`NetLinkBenchmarks`:

```bash
python build.py --benchmark --benchmark-filter "ReliableLink|PeerChannel|Engine|NetLinkCore|Fragmentation" --benchmark-repetitions 5
```

Times are the median of 5 repetitions, cv is the spread of the Phase 0 repetitions.

## Name map

Since Phase 3 the channel is the engine, and its benchmarks were renamed with it. The rows below keep the names they
were measured under.

| In this file | Since Phase 3 | What changed besides the name |
|---|---|---|
| `BM_PeerChannel_Throughput` | `BM_Engine_Throughput` | the peers have a session, opened over discovery on loopback |
| `BM_PeerChannel_Paced` | `BM_Engine_Paced` | |
| `BM_PeerChannel_RoundTrip` | `BM_Engine_RoundTrip` | |
| `BM_PeerChannel_Flush` | `BM_Engine_Flush` | |
| `BM_PeerChannel_FanIn` | `BM_Engine_FanIn` | |
| `BM_PeerChannel_FanOut` | `BM_Engine_FanOut` | |
| `BM_NetLinkCore_Throughput`, `_RoundTrip` | same | both instances run on 127.0.0.1 |
| `BM_NetLinkCore_Connect` | same | a session is one round trip now: no validation and no ready flags |

The benchmarks of the services Phase 3 removed are gone: `BM_ConnectionService_*`, `BM_PeerValidation_*`,
`BM_TimeoutService_*` and `BM_DiscoveryRegistry_*`.

## Reading the numbers

- **Compare back to back.** The benchmarks that use sockets moved by up to 30 % between two runs of the *same* binary
  in this session (`BM_PeerChannel_FanOut/receivers:32`: 159 ms and 211 ms; `FanIn/senders:1`: 3.6 ms and 5.4 ms),
  although the repetitions within one run agree to a few percent. A difference of that size between the two columns
  below is therefore not a change. To judge a change, build this baseline and the new state, and run both binaries one
  after the other.
- **The rows without sockets are stable** (`BM_ReliableLink_Transfer`, `BM_Fragmentation_Reassemble`): a few percent.
- **16 MiB messages got slower in Phase 0**, reproducibly (marked †). The receiver no longer sets aside the whole
  message when its first fragment arrives, but at most 1 MiB, and grows from there: a forged first fragment could
  otherwise make it allocate 16 MiB per stream. For these rows the `0883ca2` column is the number to get back to once
  the receiver reserves from a validated total length again (Phase 2).

## Results

| Benchmark | 0883ca2 | Phase 0 | Rate (Phase 0) | cv |
|---|---:|---:|---:|---:|
| `BM_NetLinkCore_Throughput/bytes:64/messages:20000` | 183 ms | 137 ms | 9 MiB/s | 18.2 % |
| `BM_NetLinkCore_Throughput/bytes:1024/messages:20000` | 196 ms | 188 ms | 104 MiB/s | 5.6 % |
| `BM_NetLinkCore_Throughput/bytes:65536/messages:256` | 115 ms | 116 ms | 138 MiB/s | 1.5 % |
| `BM_NetLinkCore_Throughput/bytes:1048576/messages:16` | 114 ms | 113 ms | 142 MiB/s | 1.8 % |
| `BM_NetLinkCore_RoundTrip/bytes:64` | 79.8 µs | 81.2 µs | | 3.4 % |
| `BM_NetLinkCore_RoundTrip/bytes:1024` | 81.1 µs | 79.4 µs | | 3.2 % |
| `BM_NetLinkCore_RoundTrip/bytes:65536` | 963 µs | 970 µs | | 3.6 % |
| `BM_NetLinkCore_Connect` | 185 µs | 188 µs | | 7.5 % |
| `BM_PeerChannel_Throughput/bytes:64/messages:20000` | 194 ms | 191 ms | 6 MiB/s | 0.9 % |
| `BM_PeerChannel_Throughput/bytes:1024/messages:20000` | 194 ms | 181 ms | 108 MiB/s | 9.9 % |
| `BM_PeerChannel_Throughput/bytes:65536/messages:256` | 116 ms | 82.9 ms | 193 MiB/s | 6.5 % |
| `BM_PeerChannel_Throughput/bytes:1048576/messages:16` | 111 ms | 102 ms | 157 MiB/s | 16.1 % |
| `BM_PeerChannel_Throughput/bytes:16777212/messages:2` † | 218 ms | 235 ms | 136 MiB/s | 2.2 % |
| `BM_PeerChannel_RoundTrip/bytes:64` | 82.8 µs | 81.5 µs | | 7.1 % |
| `BM_PeerChannel_RoundTrip/bytes:1024` | 76.3 µs | 78.1 µs | | 10.6 % |
| `BM_PeerChannel_RoundTrip/bytes:65536` | 947 µs | 984 µs | | 13.7 % |
| `BM_PeerChannel_Flush` | 44.1 µs | 41.8 µs | | 4.5 % |
| `BM_PeerChannel_FanIn/senders:1` | 5.16 ms | 3.62 ms | 138 k msg/s | 2.3 % |
| `BM_PeerChannel_FanIn/senders:8` | 9.26 ms | 10.5 ms | 381 k msg/s | 11.3 % |
| `BM_PeerChannel_FanIn/senders:32` | 52 ms | 47.3 ms | 339 k msg/s | 1.2 % |
| `BM_PeerChannel_FanIn/senders:128` | 234 ms | 221 ms | 290 k msg/s | 1.6 % |
| `BM_PeerChannel_FanOut/receivers:1` | 5.2 ms | 3.6 ms | 139 k msg/s | 2.7 % |
| `BM_PeerChannel_FanOut/receivers:8` | 52.5 ms | 39.8 ms | 100 k msg/s | 0.8 % |
| `BM_PeerChannel_FanOut/receivers:32` | 216 ms | 159 ms | 100 k msg/s | 2.3 % |
| `BM_ReliableLink_Transfer/bytes:1024/loss_pct:0` | 0.694 µs | 0.681 µs | 1474 MiB/s | 0.3 % |
| `BM_ReliableLink_Transfer/bytes:65536/loss_pct:0` | 8.54 µs | 8.09 µs | 7860 MiB/s | 2.6 % |
| `BM_ReliableLink_Transfer/bytes:1024/loss_pct:5` | 0.778 µs | 0.774 µs | 1321 MiB/s | 0.6 % |
| `BM_ReliableLink_Transfer/bytes:65536/loss_pct:5` | 25.9 µs | 26.7 µs | 2383 MiB/s | 0.8 % |
| `BM_ReliableLink_Transfer/bytes:1024/loss_pct:20` | 1.01 µs | 1.02 µs | 952 MiB/s | 1.4 % |
| `BM_ReliableLink_Transfer/bytes:65536/loss_pct:20` | 43.5 µs | 45 µs | 1412 MiB/s | 1.2 % |
| `BM_ReliableLink_Transfer/bytes:1048576/loss_pct:0` | 0.612 ms | 0.62 ms | 1641 MiB/s | 2.2 % |
| `BM_ReliableLink_Transfer/bytes:16777216/loss_pct:0` † | 8.78 ms | 15.7 ms | 1067 MiB/s | 11.6 % |
| `BM_ReliableLink_Transfer/bytes:1048576/loss_pct:5` | 1.38 ms | 1.37 ms | 724 MiB/s | 2.2 % |
| `BM_ReliableLink_Transfer/bytes:16777216/loss_pct:5` † | 20.3 ms | 27.8 ms | 592 MiB/s | 6.5 % |
| `BM_ReliableLink_Transfer/bytes:1048576/loss_pct:20` | 1.35 ms | 1.3 ms | 784 MiB/s | 1.9 % |
| `BM_ReliableLink_Transfer/bytes:16777216/loss_pct:20` † | 19.3 ms | 25.8 ms | 623 MiB/s | 6.0 % |
| `BM_Fragmentation_Reassemble/bytes:65536` | 1.55 µs | 1.54 µs | 42432 MiB/s | 1.3 % |
| `BM_Fragmentation_Reassemble/bytes:1048576` | 0.258 ms | 0.241 ms | 4167 MiB/s | 0.7 % |
| `BM_Fragmentation_Reassemble/bytes:16777216` † | 3.18 ms | 10.6 ms | 1536 MiB/s | 7.9 % |

`BM_PeerChannel_FanIn` and `BM_PeerChannel_FanOut` delivered 100 % of their messages and lost no link in every row.
