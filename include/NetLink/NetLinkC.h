/*
==============================================================================
	Module:         NetLinkC
	Description:    Plain C API of the NetLink library, for hosts that cannot
					use the C++ API (C#, Swift, Python, ...)
  ==============================================================================
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#if defined(_WIN32)
#if defined(NETLINK_C_EXPORTS)
#define NL_API __declspec(dllexport)
#else
#define NL_API __declspec(dllimport)
#endif
#else
#define NL_API __attribute__((visibility("default")))
#endif


#ifdef __cplusplus
extern "C"
{
#endif

// --- Limits ---------------------------------------------

// Size of the text fields below, including the terminating zero. All texts are UTF-8; a longer one is cut off at a
// character boundary. Lists are not limited by it: the functions that list something return how many there are.
#define NL_MAX_STRING 256


// --- Types ----------------------------------------------

// One NetLink instance
typedef struct NLNetLink NLNetLink;


// Values of netlink::Lane
typedef enum NLLane
{
	NL_LANE_RELIABLE = 0,
	NL_LANE_BULK	 = 1,
	NL_LANE_MEDIA	 = 2,
} NLLane;


// Values of netlink::SendResult
typedef enum NLSendResult
{
	NL_SEND_QUEUED		  = 0,
	NL_SEND_QUEUE_FULL	  = 1,
	NL_SEND_TOO_LARGE	  = 2,
	NL_SEND_NOT_CONNECTED = 3,
	NL_SEND_NOT_RUNNING	  = 4,
	NL_SEND_INVALID		  = 5, // a null handle, or data missing for a size above zero
} NLSendResult;


// Values of netlink::DisconnectReason
typedef enum NLDisconnectReason
{
	NL_DISCONNECT_LOCAL		   = 0,
	NL_DISCONNECT_REMOTE	   = 1,
	NL_DISCONNECT_DECLINED	   = 2,
	NL_DISCONNECT_INCOMPATIBLE = 3,
	NL_DISCONNECT_LOST		   = 4,
	NL_DISCONNECT_NETWORK	   = 5,
	NL_DISCONNECT_SHUTDOWN	   = 6,
} NLDisconnectReason;


// Values of netlink::AdapterPriority
typedef enum NLAdapterPriority
{
	NL_ADAPTER_SUPPRESSED = 1,
	NL_ADAPTER_AVAILABLE  = 2,
	NL_ADAPTER_PREFERRED  = 3,
} NLAdapterPriority;


// Values of netlink::LogLevel
typedef enum NLLogLevel
{
	NL_LOG_DEBUG   = 0,
	NL_LOG_INFO	   = 1,
	NL_LOG_WARNING = 2,
	NL_LOG_ERROR   = 3,
} NLLogLevel;


typedef struct NLPeerInfo
{
	uint64_t id;
	uint16_t port;
	char	 displayName[NL_MAX_STRING];
	char	 address[NL_MAX_STRING];
	char	 appVersion[NL_MAX_STRING];
} NLPeerInfo;


typedef struct NLNetworkAdapter
{
	uint64_t id;
	int32_t	 priority; // NLAdapterPriority
	char	 adapterName[NL_MAX_STRING];
	char	 networkName[NL_MAX_STRING];
	char	 ipv4[NL_MAX_STRING];
} NLNetworkAdapter;


typedef struct NLPeerStats
{
	uint64_t rttMicroseconds;
	uint64_t bytesQueued;
	uint64_t bytesSent;
	uint64_t bytesReceived;
	uint64_t retransmissions;
	uint64_t mediaSent;
	uint64_t mediaDropped;
	uint64_t mediaIncomplete;
	float	 mediaLoss;
} NLPeerStats;


// See netlink::NetLinkConfig. Zero for a number keeps NetLink's default; the texts may be null except appId.
typedef struct NLConfig
{
	const char *displayName;
	const char *appId;
	const char *appVersion;
	uint16_t	discoveryPort;
	uint64_t	sendQueueBytes;
	uint32_t	maxSendRate;
	uint32_t	peerTimeoutMs;
} NLConfig;


// See netlink::NetLinkCallbacks: they run one at a time on NetLink's event thread. Pointers passed to a callback are
// only valid while it runs. Every callback may be null; a null onConnectionRequest accepts every request.
typedef struct NLCallbacks
{
	void *context; // passed back to every callback

	void (*onPeerDiscovered)(void *context, const NLPeerInfo *peer);
	void (*onPeerLost)(void *context, uint64_t peer);
	void (*onConnectionRequest)(void *context, const NLPeerInfo *peer);
	void (*onConnected)(void *context, const NLPeerInfo *peer);
	void (*onDisconnected)(void *context, uint64_t peer, int32_t reason);					// NLDisconnectReason
	void (*onMessage)(void *context, uint64_t from, int32_t lane, uint32_t type, const uint8_t *data, size_t size);
	void (*onNetworkAdapterChanged)(void *context, const NLNetworkAdapter *adapter);
	void (*onLog)(void *context, int32_t level, const char *message);						// NLLogLevel
} NLCallbacks;


// --- Lifecycle ------------------------------------------

// The library version, e.g. "0.6.0.12"
NL_API const char *nl_version(void);

NL_API NLNetLink  *nl_create(void);

// Stops the instance if it runs. Must not be called from a callback.
NL_API void		   nl_destroy(NLNetLink *netlink);

NL_API bool		   nl_start(NLNetLink *netlink, const NLConfig *config, const NLCallbacks *callbacks);
NL_API void		   nl_stop(NLNetLink *netlink);


// --- Discovery ------------------------------------------

NL_API bool		   nl_start_discovery(NLNetLink *netlink);
NL_API void		   nl_stop_discovery(NLNetLink *netlink);

// Copies up to maxPeers of the peers discovered right now and returns how many there are (which can be more)
NL_API int32_t	   nl_get_peers(const NLNetLink *netlink, NLPeerInfo *peers, int32_t maxPeers);


// --- Sessions -------------------------------------------

NL_API bool		   nl_connect(NLNetLink *netlink, uint64_t peer);
NL_API void		   nl_accept(NLNetLink *netlink, uint64_t peer);
NL_API void		   nl_decline(NLNetLink *netlink, uint64_t peer);
NL_API void		   nl_disconnect(NLNetLink *netlink, uint64_t peer);

// Copies up to maxPeers and returns how many there are
NL_API int32_t	   nl_get_connected_peers(const NLNetLink *netlink, uint64_t *peers, int32_t maxPeers);


// --- Messaging ------------------------------------------

// Returns an NLSendResult. The data is copied before the call returns.
NL_API int32_t	   nl_send(NLNetLink *netlink, uint64_t peer, uint32_t type, const uint8_t *data, size_t size, int32_t lane, uint32_t timeoutMs);

// Returns how many connected peers took the message
NL_API size_t	   nl_broadcast(NLNetLink *netlink, uint32_t type, const uint8_t *data, size_t size, int32_t lane);

NL_API bool		   nl_get_stats(const NLNetLink *netlink, uint64_t peer, NLPeerStats *stats);


// --- Network adapters -----------------------------------

// Works before nl_start as well. Copies up to maxAdapters and returns how many there are.
NL_API int32_t	   nl_get_adapters(const NLNetLink *netlink, NLNetworkAdapter *adapters, int32_t maxAdapters);
NL_API bool		   nl_set_active_adapter(NLNetLink *netlink, uint64_t adapterId);
NL_API uint64_t	   nl_get_active_adapter_id(const NLNetLink *netlink);

#ifdef __cplusplus
}
#endif
