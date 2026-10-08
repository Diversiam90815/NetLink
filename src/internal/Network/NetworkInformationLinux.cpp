/*
  ==============================================================================
	Module:         NetworkInformation (Linux backend)
	Description:    Information about the local Network setup
  ==============================================================================
*/

#include "NetworkInformation.h"
#include "NetLinkLog.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>


namespace netlink
{

namespace
{

bool fileExists(const std::string &path)
{
	struct stat st{};
	return ::stat(path.c_str(), &st) == 0;
}


bool isWirelessInterface(const std::string &ifName)
{
	return fileExists("/sys/class/net/" + ifName + "/wireless");
}

} // namespace


struct NetworkInformation::Impl
{
	using AddrList = std::unique_ptr<ifaddrs, void (*)(ifaddrs *)>;

	AddrList	 mAddrList{nullptr, &freeifaddrs};

	bool		 getNetworkInformationFromOS();
	void					saveAdapter(std::vector<NetworkAdapterInternal> &adapters, const ifaddrs *ifa, const std::unordered_set<std::string> &defaultRouteIfNames);

	std::string	 sockaddrToString(const sockaddr *sa) const;
	AdapterTypes filterAdapterType(const std::string &ifName, unsigned int flags) const;
	AdapterPriorityInternal determinePriority(bool isDefaultRoute, AdapterTypes type, unsigned int flags) const;

	bool					getDefaultInterfaces(std::unordered_set<std::string> &ifNames);
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

	std::string				adapterName		 = ifa->ifa_name ? ifa->ifa_name : "";
	std::string				addressString	 = sockaddrToString(ifa->ifa_addr);
	std::string				subnetMaskString = ifa->ifa_netmask ? sockaddrToString(ifa->ifa_netmask) : std::string{};
	AdapterTypes			type			 = filterAdapterType(adapterName, ifa->ifa_flags);
	std::string				networkName		 = networkNameOf(type, addressString);
	const bool				isDefaultRoute	 = defaultRouteIfNames.find(adapterName) != defaultRouteIfNames.end();

	AdapterPriorityInternal visibility		 = determinePriority(isDefaultRoute, type, ifa->ifa_flags);

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

	if (isWirelessInterface(ifName))
		return AdapterTypes::WiFi;

	if (!fileExists("/sys/class/net/" + ifName + "/device"))
		return AdapterTypes::Virtual;

	return AdapterTypes::Ethernet;
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

	std::ifstream file("/proc/net/route");
	if (!file.is_open())
	{
		NETLINK_LOG_WARNING("Could not open /proc/net/route!");
		return false;
	}

	std::string line;
	std::getline(file, line); // header

	while (std::getline(file, line))
	{
		std::istringstream iss(line);
		std::string		   iface, destination, gateway, flagsHex;

		if (!(iss >> iface >> destination >> gateway >> flagsHex))
			continue;

		if (destination != "00000000")
			continue;

		unsigned long flags = std::strtoul(flagsHex.c_str(), nullptr, 16);
		if (!(flags & 0x2)) // RTF_GATEWAY
			continue;

		ifNames.insert(iface);
	}

	return true;
}


} // namespace netlink
