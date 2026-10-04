/*
  ==============================================================================
	Module:         NetworkInformation (Windows backend)
	Description:    Information about the local Network setup
  ==============================================================================
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "NetworkInformation.h"
#include "NetLinkLog.h"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iptypes.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <unordered_set>
#include <algorithm>


namespace netlink
{

struct NetworkInformation::Impl
{
	// RAII helpers
	struct WinsockSession
	{
		bool ok{false};
		WinsockSession()
		{
			WSADATA wsa{};
			ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

			if (!ok)
				NETLINK_LOG_ERROR("WSAStartup failed!");
		}

		~WinsockSession()
		{
			if (ok)
				WSACleanup();
		}
	};

	using AdapterBuffer = std::unique_ptr<IP_ADAPTER_ADDRESSES, void (*)(IP_ADAPTER_ADDRESSES *)>;

	bool						   getNetworkInformationFromOS();
	static void saveAdapter(std::vector<NetworkAdapterInternal> &adapters, const PIP_ADAPTER_ADDRESSES adapter, const std::unordered_set<ULONG64> &defaultRouteLuidValues);

	static std::string			   sockaddrToString(SOCKADDR *sa);
	static std::string			   prefixLengthToSubnetMask(USHORT family, ULONG prefixLength);
	static AdapterTypes			   filterAdapterType(const DWORD Type);
	static AdapterPriorityInternal determinePriority(bool isDefaultRoute, bool IPv4Enabled, AdapterTypes type, IF_OPER_STATUS status);

	static bool					   getDefaultInterfaces(std::vector<NET_LUID> &pLUIDs);


	static std::string			   WStringToStdString(const std::wstring &wstr)
	{
		if (wstr.empty())
			return {};

		std::string str{};
		size_t		size{};
		str.resize(wstr.length());
		wcstombs_s(&size, &str[0], str.size() + 1, wstr.c_str(), wstr.size());
		return str;
	}


	AdapterBuffer					mAdapterAddresses{nullptr, [](IP_ADAPTER_ADDRESSES *p)
									  {
										  if (p)
											  free(p);
									  }};
	ULONG							mOutBufLen{0};

	std::unique_ptr<WinsockSession> mWinsockSession;
};


NetworkInformation::NetworkInformation() : mImpl(std::make_unique<Impl>()) {}


NetworkInformation::~NetworkInformation()
{
	deinit();
}


bool NetworkInformation::init() const
{
	mImpl->mWinsockSession = std::make_unique<Impl::WinsockSession>();

	if (!mImpl->mWinsockSession->ok)
		return false;

	return mImpl->getNetworkInformationFromOS();
}


void NetworkInformation::deinit()
{
	mImpl->mAdapterAddresses.reset();
	mNetworkAdapters.clear();
}


bool NetworkInformation::Impl::getNetworkInformationFromOS()
{
	ULONG flags	 = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS;

	// Get the required buffer size by a first call
	DWORD result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &mOutBufLen);

	if (result == ERROR_ACCESS_DENIED)
	{
		NETLINK_LOG_ERROR("Access denied: Running without admin privileges limits available information!");
		flags = 0;
	}
	else if (result != ERROR_BUFFER_OVERFLOW)
	{
		NETLINK_LOG_ERROR("GetAdapterAddresses failed with error {}", result);
		return false;
	}

	AdapterBuffer tmp(static_cast<IP_ADAPTER_ADDRESSES *>(malloc(mOutBufLen)),
					  [](IP_ADAPTER_ADDRESSES *p)
					  {
						  if (p)
							  free(p);
					  });

	if (!tmp)
	{
		NETLINK_LOG_ERROR("Allocation failed for adapter buffer ({} bytes)", mOutBufLen);
		return false;
	}

	// Get the actual data by a second call
	result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, tmp.get(), &mOutBufLen);

	if (result != NO_ERROR)
	{
		NETLINK_LOG_ERROR("GetAdapterAddresses failed with error : {}", result);
		return false;
	}

	mAdapterAddresses.reset(tmp.release());

	return true;
}


void NetworkInformation::processAdapter()
{
	mNetworkAdapters.clear();

	std::vector<NET_LUID> defaultRouteAdapters;

	if (!mImpl->getDefaultInterfaces(defaultRouteAdapters))
	{
		NETLINK_LOG_WARNING("Could not get list of default route adapters!");
		defaultRouteAdapters.clear();
	}

	std::unordered_set<ULONG64> defaultRouteLuidValues;
	defaultRouteLuidValues.reserve(defaultRouteAdapters.size());

	for (const auto &luid : defaultRouteAdapters)
		defaultRouteLuidValues.insert(luid.Value);

	for (auto *node = mImpl->mAdapterAddresses.get(); node; node = node->Next)
	{
		mImpl->saveAdapter(mNetworkAdapters, node, defaultRouteLuidValues);
	}
}


void NetworkInformation::Impl::saveAdapter(std::vector<NetworkAdapterInternal> &adapters, const PIP_ADAPTER_ADDRESSES adapter,
										   const std::unordered_set<ULONG64> &defaultRouteLuidValues)
{
	const std::string			adapterName = WStringToStdString(adapter->Description);

	PIP_ADAPTER_UNICAST_ADDRESS unicast		= adapter->FirstUnicastAddress;

	while (unicast)
	{
		if (unicast->Address.lpSockaddr->sa_family == AF_INET)
		{
			std::string							   addressString	= sockaddrToString(unicast->Address.lpSockaddr);
			std::string							   subnetMaskString = prefixLengthToSubnetMask(unicast->Address.lpSockaddr->sa_family, unicast->OnLinkPrefixLength);
			const AdapterTypes					   type				= filterAdapterType(adapter->IfType);
			std::string							   networkName		= networkNameOf(type, addressString);
			const bool							   isDefaultRoute	= defaultRouteLuidValues.contains(adapter->Luid.Value);
			const bool							   ipv4Enabled		= adapter->Flags & 0x80;
			const netlink::AdapterPriorityInternal visibility		= determinePriority(isDefaultRoute, ipv4Enabled, type, adapter->OperStatus);

			auto createdAdapter = NetworkAdapterInternal(adapterName, networkName, addressString, subnetMaskString, makeAdapterId(adapter->Luid.Value, addressString),
														 isDefaultRoute, type, visibility);

			adapters.push_back(createdAdapter);
		}

		unicast = unicast->Next;
	}
}


std::string NetworkInformation::Impl::sockaddrToString(SOCKADDR *sa)
{
	char addressBuffer[INET6_ADDRSTRLEN] = {0};

	if (sa->sa_family == AF_INET)
	{
		const auto sockaddr_ipv4 = reinterpret_cast<sockaddr_in *>(sa);
		inet_ntop(AF_INET, &(sockaddr_ipv4->sin_addr), addressBuffer, sizeof(addressBuffer));
	}
	else if (sa->sa_family == AF_INET6)
	{
		const auto sockaddr_ipv6 = reinterpret_cast<sockaddr_in6 *>(sa);
		inet_ntop(AF_INET6, &(sockaddr_ipv6->sin6_addr), addressBuffer, sizeof(addressBuffer));
	}

	return std::string(addressBuffer);
}


std::string NetworkInformation::Impl::prefixLengthToSubnetMask(const USHORT family, const ULONG prefixLength)
{
	if (family == AF_INET && prefixLength <= 32)
	{
		const DWORD	   mask = (prefixLength == 0) ? 0 : (~0U << (32 - prefixLength));
		struct in_addr maskAddr{};
		maskAddr.s_addr = htonl(mask);

		char maskBuffer[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &maskAddr, maskBuffer, INET_ADDRSTRLEN);

		return std::string(maskBuffer);
	}
	else if (family == AF_INET6 && prefixLength <= 128)
	{
		// IPv6 does not use subnet masks like IPv4 does. We might log the prefix length instead.
		return std::to_string(prefixLength);
	}

	return {};
}


netlink::AdapterTypes NetworkInformation::Impl::filterAdapterType(const DWORD Type)
{
	switch (Type)
	{
	case IF_TYPE_ETHERNET_CSMACD: return AdapterTypes::Ethernet;
	case IF_TYPE_IEEE80211: return AdapterTypes::WiFi;
	case IF_TYPE_SOFTWARE_LOOPBACK: return AdapterTypes::Loopback;
	case IF_TYPE_PROP_VIRTUAL: return AdapterTypes::Virtual;
	default: return AdapterTypes::Other;
	}
}


netlink::AdapterPriorityInternal NetworkInformation::Impl::determinePriority(const bool			  isDefaultRoute,
																			 const bool			  IPv4Enabled,
																			 const AdapterTypes	  type,
																			 const IF_OPER_STATUS status)
{
	// Preferred device should be
	//	- Real
	//	- UP (currently operational)
	//	- preferably default route
	//	- IPv4 enabled (as currently we just support IPv4)

	if (type == AdapterTypes::Loopback)
		return netlink::AdapterPriorityInternal::Suppressed;

	if (status != IfOperStatusUp)
		return netlink::AdapterPriorityInternal::Available;

	if (!isDefaultRoute || !IPv4Enabled)
		return netlink::AdapterPriorityInternal::Available;

	if (type != AdapterTypes::Ethernet && type != AdapterTypes::WiFi)
		return netlink::AdapterPriorityInternal::Available;

	return netlink::AdapterPriorityInternal::Preferred;
}


bool NetworkInformation::Impl::getDefaultInterfaces(std::vector<NET_LUID> &pLUIDs)
{
	MIB_IPFORWARD_TABLE2 *routingTable = nullptr;
	pLUIDs.clear();

	if (GetIpForwardTable2(AF_INET, &routingTable) != NO_ERROR)
	{
		NETLINK_LOG_WARNING("Could not retrieve Routing Table!");

		if (routingTable)
			FreeMibTable(routingTable);

		return false;
	}

	for (ULONG i = 0; i < routingTable->NumEntries; ++i)
	{
		if (routingTable->Table[i].DestinationPrefix.PrefixLength == 0)
			pLUIDs.push_back(routingTable->Table[i].InterfaceLuid);
	}

	FreeMibTable(routingTable);
	return true;
}


} // namespace netlink
