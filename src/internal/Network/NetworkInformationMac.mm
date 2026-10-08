/*
  ==============================================================================
	Module:         NetworkInformation (macOS backend)
	Description:    Information about the local Network setup
  ==============================================================================
*/

#include "NetworkInformation.h"
#include "NetLinkLog.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_media.h>
#include <net/route.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <cstring>
#include <unordered_set>


namespace netlink
{

namespace
{

// BSD routing-socket sockaddrs are padded to word boundaries.
size_t roundUpToWord(size_t length)
{
	return length > 0 ? (1 + ((length - 1) | (sizeof(long) - 1))) : sizeof(long);
}


} // namespace


struct NetworkInformation::Impl
{
	using AddrList = std::unique_ptr<ifaddrs, void (*)(ifaddrs *)>;

	AddrList										mAddrList{nullptr, &freeifaddrs};

	bool											getNetworkInformationFromOS();
	void											saveAdapter(std::vector<NetworkAdapterInternal> &adapters, const ifaddrs *ifa, const std::unordered_set<std::string> &defaultRouteIfNames);

	std::string										sockaddrToString(const sockaddr *sa) const;
	AdapterTypes									filterAdapterType(const std::string &ifName, unsigned int flags) const;
	AdapterPriorityInternal							determinePriority(bool isDefaultRoute, AdapterTypes type, unsigned int flags) const;

	bool											getDefaultInterfaces(std::unordered_set<std::string> &ifNames);
};


NetworkInformation::NetworkInformation() : mImpl(std::make_unique<Impl>()) {}


NetworkInformation::~NetworkInformation()
{
	deinit();
}


bool NetworkInformation::init() const
{
	return mImpl->getNetworkInformationFromOS();
}


void NetworkInformation::deinit()
{
	mImpl->mAddrList.reset();
	mNetworkAdapters.clear();
}


bool NetworkInformation::Impl::getNetworkInformationFromOS()
{
	ifaddrs *raw{nullptr};

	if (getifaddrs(&raw) != 0)
	{
		NETLINK_LOG_ERROR("getifaddrs failed!");
		return false;
	}

	mAddrList.reset(raw);
	return true;
}


void NetworkInformation::processAdapter()
{
	mNetworkAdapters.clear();

	std::unordered_set<std::string> defaultRouteIfNames;

	if (!mImpl->getDefaultInterfaces(defaultRouteIfNames))
	{
		NETLINK_LOG_WARNING("Could not get list of default route interfaces!");
		defaultRouteIfNames.clear();
	}

	for (auto *ifa = mImpl->mAddrList.get(); ifa; ifa = ifa->ifa_next)
	{
		mImpl->saveAdapter(mNetworkAdapters, ifa, defaultRouteIfNames);
	}
}


void NetworkInformation::Impl::saveAdapter(std::vector<NetworkAdapterInternal> &adapters, const ifaddrs *ifa, const std::unordered_set<std::string> &defaultRouteIfNames)
{
	if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
		return;

	std::string	adapterName		 = ifa->ifa_name ? ifa->ifa_name : "";
	std::string	addressString	 = sockaddrToString(ifa->ifa_addr);
	std::string	subnetMaskString = ifa->ifa_netmask ? sockaddrToString(ifa->ifa_netmask) : std::string{};
	AdapterTypes type			 = filterAdapterType(adapterName, ifa->ifa_flags);
	std::string	networkName		 = networkNameOf(type, addressString);
	const bool	isDefaultRoute	 = defaultRouteIfNames.find(adapterName) != defaultRouteIfNames.end();

	AdapterPriorityInternal visibility = determinePriority(isDefaultRoute, type, ifa->ifa_flags);

	adapters.emplace_back(adapterName, networkName, addressString, subnetMaskString, makeAdapterId(if_nametoindex(adapterName.c_str()), addressString), isDefaultRoute, type,
						  visibility);
}


std::string NetworkInformation::Impl::sockaddrToString(const sockaddr *sa) const
{
	char addressBuffer[INET6_ADDRSTRLEN] = {0};

	if (sa->sa_family == AF_INET)
	{
		auto *sockaddr_ipv4 = reinterpret_cast<const sockaddr_in *>(sa);
		inet_ntop(AF_INET, &(sockaddr_ipv4->sin_addr), addressBuffer, sizeof(addressBuffer));
	}
	else if (sa->sa_family == AF_INET6)
	{
		auto *sockaddr_ipv6 = reinterpret_cast<const sockaddr_in6 *>(sa);
		inet_ntop(AF_INET6, &(sockaddr_ipv6->sin6_addr), addressBuffer, sizeof(addressBuffer));
	}

	return std::string(addressBuffer);
}


netlink::AdapterTypes NetworkInformation::Impl::filterAdapterType(const std::string &ifName, unsigned int flags) const
{
	if (flags & IFF_LOOPBACK)
		return AdapterTypes::Loopback;

	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return AdapterTypes::Other;

	ifmediareq ifmr{};
	std::strncpy(ifmr.ifm_name, ifName.c_str(), sizeof(ifmr.ifm_name) - 1);

	AdapterTypes type = AdapterTypes::Virtual;

	if (ioctl(sock, SIOCGIFMEDIA, &ifmr) == 0)
	{
		if (IFM_TYPE(ifmr.ifm_active) == IFM_IEEE80211)
			type = AdapterTypes::WiFi;
		else if (IFM_TYPE(ifmr.ifm_active) == IFM_ETHER)
			type = AdapterTypes::Ethernet;
	}

	close(sock);
	return type;
}


netlink::AdapterPriorityInternal NetworkInformation::Impl::determinePriority(bool isDefaultRoute, AdapterTypes type, unsigned int flags) const
{
	// Preferred device should be
	//	- Real
	//	- UP (currently operational)
	//	- preferably default route
	//	- IPv4 enabled (as currently we just support IPv4)

	if (type == AdapterTypes::Loopback)
		return netlink::AdapterPriorityInternal::Suppressed;

	if (!(flags & IFF_UP) || !(flags & IFF_RUNNING))
		return netlink::AdapterPriorityInternal::Available;

	if (!isDefaultRoute)
		return netlink::AdapterPriorityInternal::Available;

	if (type != AdapterTypes::Ethernet && type != AdapterTypes::WiFi)
		return netlink::AdapterPriorityInternal::Available;

	return netlink::AdapterPriorityInternal::Preferred;
}


bool NetworkInformation::Impl::getDefaultInterfaces(std::unordered_set<std::string> &ifNames)
{
	ifNames.clear();

	int	   mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_DUMP, 0};
	size_t needed  = 0;

	if (sysctl(mib, 6, nullptr, &needed, nullptr, 0) < 0)
	{
		NETLINK_LOG_WARNING("Could not size routing table!");
		return false;
	}

	std::vector<char> buffer(needed);
	if (sysctl(mib, 6, buffer.data(), &needed, nullptr, 0) < 0)
	{
		NETLINK_LOG_WARNING("Could not read routing table!");
		return false;
	}

	char *next = buffer.data();
	char *end  = buffer.data() + needed;

	while (next + sizeof(rt_msghdr) <= end)
	{
		auto *rtm = reinterpret_cast<rt_msghdr *>(next);

		if (rtm->rtm_msglen == 0)
			break;

		if (rtm->rtm_flags & RTF_GATEWAY)
		{
			char	   *sa		   = reinterpret_cast<char *>(rtm + 1);
			bool		isDefault  = false;

			for (int i = 0; i < RTAX_MAX; ++i)
			{
				if (!(rtm->rtm_addrs & (1 << i)))
					continue;

				auto *sockAddr = reinterpret_cast<sockaddr *>(sa);
				size_t saLen   = sockAddr->sa_len ? sockAddr->sa_len : sizeof(long);

				if (i == RTAX_DST && sockAddr->sa_family == AF_INET)
				{
					auto *sin = reinterpret_cast<sockaddr_in *>(sockAddr);
					if (sin->sin_addr.s_addr == 0)
						isDefault = true;
				}

				sa += roundUpToWord(saLen);
			}

			if (isDefault)
			{
				char ifNameBuf[IF_NAMESIZE] = {0};
				if (if_indextoname(rtm->rtm_index, ifNameBuf))
					ifNames.insert(ifNameBuf);
			}
		}

		next += rtm->rtm_msglen;
	}

	return true;
}


} // namespace netlink
