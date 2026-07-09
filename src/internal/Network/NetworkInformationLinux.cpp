/*
  ==============================================================================
	Module:         NetworkInformation (Linux backend)
	Description:    Information about the local Network setup
  ==============================================================================
*/

#include "NetworkInformation.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <netlink/attr.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/netlink.h>
#include <linux/nl80211.h>

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


// ---------------------------------------------------------------------------
// nl80211 SSID lookup for the interface currently associated with an AP.
// Mirrors what `iw dev <if> link` does: scan results, find the BSS marked as
// associated, then extract the SSID information element (tag 0) from the
// raw information-elements blob.
// ---------------------------------------------------------------------------

struct Nl80211Session
{
	nl_sock *sock{nullptr};
	int		 driverId{-1};

	Nl80211Session()
	{
		sock = nl_socket_alloc();
		if (!sock)
			return;

		if (genl_connect(sock) != 0)
		{
			nl_socket_free(sock);
			sock = nullptr;
			return;
		}

		driverId = genl_ctrl_resolve(sock, "nl80211");
	}

	~Nl80211Session()
	{
		if (sock)
			nl_socket_free(sock);
	}

	bool ok() const { return sock && driverId >= 0; }
};


struct ScanCallbackContext
{
	std::string ssid;
	bool		found{false};
};


int handleScanResults(nl_msg *msg, void *arg)
{
	auto			 *ctx	 = static_cast<ScanCallbackContext *>(arg);
	genlmsghdr		 *gnlh	 = static_cast<genlmsghdr *>(nlmsg_data(nlmsg_hdr(msg)));
	nlattr			 *tb[NL80211_ATTR_MAX + 1];

	nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), nullptr);

	if (!tb[NL80211_ATTR_BSS])
		return NL_SKIP;

	nlattr *bss[NL80211_BSS_MAX + 1];
	if (nla_parse_nested(bss, NL80211_BSS_MAX, tb[NL80211_ATTR_BSS], nullptr) != 0)
		return NL_SKIP;

	if (!bss[NL80211_BSS_STATUS])
		return NL_SKIP;

	uint32_t status = nla_get_u32(bss[NL80211_BSS_STATUS]);
	if (status != NL80211_BSS_STATUS_ASSOCIATED && status != NL80211_BSS_STATUS_IBSS_JOINED)
		return NL_SKIP;

	if (!bss[NL80211_BSS_INFORMATION_ELEMENTS])
		return NL_SKIP;

	const uint8_t *ie	 = static_cast<const uint8_t *>(nla_data(bss[NL80211_BSS_INFORMATION_ELEMENTS]));
	int			   ieLen = nla_len(bss[NL80211_BSS_INFORMATION_ELEMENTS]);

	int			   pos	 = 0;
	while (pos + 2 <= ieLen)
	{
		uint8_t id	 = ie[pos];
		uint8_t len	 = ie[pos + 1];

		if (pos + 2 + len > ieLen)
			break;

		if (id == 0) // SSID information element
		{
			ctx->ssid.assign(reinterpret_cast<const char *>(ie + pos + 2), len);
			ctx->found = true;
			break;
		}

		pos += 2 + len;
	}

	return NL_SKIP;
}


int finishHandler(nl_msg *, void *arg)
{
	*static_cast<int *>(arg) = 1;
	return NL_SKIP;
}


std::string queryAssociatedSsid(int ifIndex)
{
	Nl80211Session session;
	if (!session.ok())
		return {};

	nl_msg *msg = nlmsg_alloc();
	if (!msg)
		return {};

	genlmsg_put(msg, 0, 0, session.driverId, 0, NLM_F_DUMP, NL80211_CMD_GET_SCAN, 0);
	nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);

	nl_cb			   *cb	   = nl_cb_alloc(NL_CB_DEFAULT);
	ScanCallbackContext ctx;
	int					done   = 0;

	nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, handleScanResults, &ctx);
	nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, finishHandler, &done);
	nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, finishHandler, &done);

	nl_send_auto(session.sock, msg);
	nlmsg_free(msg);

	while (!done)
	{
		if (nl_recvmsgs(session.sock, cb) < 0)
			break;
	}

	nl_cb_put(cb);

	return ctx.found ? ctx.ssid : std::string{};
}


bool isWirelessInterface(const std::string &ifName)
{
	return fileExists("/sys/class/net/" + ifName + "/wireless");
}

} // namespace


struct NetworkInformation::Impl
{
	using AddrList = std::unique_ptr<ifaddrs, void (*)(ifaddrs *)>;

	AddrList				mAddrList{nullptr, &freeifaddrs};

	bool					getNetworkInformationFromOS();
	void					saveAdapter(std::vector<NetworkAdapterInternal> &adapters, const ifaddrs *ifa, const int ID, const std::unordered_set<std::string> &defaultRouteIfNames);

	std::string				sockaddrToString(const sockaddr *sa) const;
	AdapterTypes			filterAdapterType(const std::string &ifName, unsigned int flags) const;
	AdapterPriorityInternal determinePriority(bool isDefaultRoute, AdapterTypes type, unsigned int flags) const;

	bool					getDefaultInterfaces(std::unordered_set<std::string> &ifNames);
	std::string				getHostName(const sockaddr *ip, socklen_t ipLength);
	std::string				getWifiSsid(const std::string &ifName) const;
	std::string				getNetworkGatename(AdapterTypes type, const std::string &ifName, const std::string &address);
	std::string				getNetworkName(AdapterTypes type, const std::string &ifName, const std::string &address);
};


NetworkInformation::NetworkInformation() : mImpl(std::make_unique<Impl>()) {}


NetworkInformation::~NetworkInformation()
{
	deinit();
}


bool NetworkInformation::init()
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

	int ID = 1; // Giving each network adapter an ID

	for (auto *ifa = mImpl->mAddrList.get(); ifa; ifa = ifa->ifa_next, ++ID)
	{
		mImpl->saveAdapter(mNetworkAdapters, ifa, ID, defaultRouteIfNames);
	}
}


void NetworkInformation::Impl::saveAdapter(std::vector<NetworkAdapterInternal> &adapters, const ifaddrs *ifa, const int ID, const std::unordered_set<std::string> &defaultRouteIfNames)
{
	if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
		return;

	std::string	adapterName		= ifa->ifa_name ? ifa->ifa_name : "";
	std::string	addressString	= sockaddrToString(ifa->ifa_addr);
	std::string	subnetMaskString = ifa->ifa_netmask ? sockaddrToString(ifa->ifa_netmask) : std::string{};
	AdapterTypes type			= filterAdapterType(adapterName, ifa->ifa_flags);
	std::string	networkName		= getNetworkName(type, adapterName, addressString);
	const bool	isDefaultRoute	= defaultRouteIfNames.find(adapterName) != defaultRouteIfNames.end();

	AdapterPriorityInternal visibility = determinePriority(isDefaultRoute, type, ifa->ifa_flags);

	adapters.emplace_back(adapterName, networkName, addressString, subnetMaskString, ID, isDefaultRoute, type, visibility);
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


std::string NetworkInformation::Impl::getHostName(const sockaddr *ip, socklen_t ipLength)
{
	char nameBuffer[NI_MAXHOST];

	int	 result = getnameinfo(ip, ipLength, nameBuffer, NI_MAXHOST, nullptr, 0, NI_NAMEREQD);

	if (result != 0)
		return {};

	return std::string(nameBuffer);
}


std::string NetworkInformation::Impl::getWifiSsid(const std::string &ifName) const
{
	std::string networkName = "WiFi";

	unsigned	 ifIndex	 = if_nametoindex(ifName.c_str());
	if (ifIndex == 0)
		return networkName;

	std::string ssid = queryAssociatedSsid(static_cast<int>(ifIndex));
	if (!ssid.empty())
		networkName = ssid;

	return networkName;
}


std::string NetworkInformation::Impl::getNetworkGatename(AdapterTypes type, const std::string &ifName, const std::string &address)
{
	std::string networkName = (type == AdapterTypes::Virtual) ? "Virtual Ethernet" : "Ethernet";

	std::ifstream file("/proc/net/route");
	if (!file.is_open())
	{
		if (!address.empty())
			networkName += " (" + address + ")";
		return networkName;
	}

	std::string line;
	std::getline(file, line); // header

	while (std::getline(file, line))
	{
		std::istringstream iss(line);
		std::string		   iface, destination, gatewayHex;

		if (!(iss >> iface >> destination >> gatewayHex))
			continue;

		if (iface != ifName || destination != "00000000")
			continue;

		unsigned long gatewayValue = std::strtoul(gatewayHex.c_str(), nullptr, 16);

		in_addr		  gatewayAddr{};
		gatewayAddr.s_addr = static_cast<in_addr_t>(gatewayValue);

		sockaddr_in gatewaySockaddr{};
		gatewaySockaddr.sin_family = AF_INET;
		gatewaySockaddr.sin_addr	= gatewayAddr;

		std::string host = getHostName(reinterpret_cast<const sockaddr *>(&gatewaySockaddr), sizeof(gatewaySockaddr));

		if (!host.empty())
		{
			networkName += " via " + host;
			return networkName;
		}

		break;
	}

	if (!address.empty())
		return networkName + " (" + address + ")";

	return networkName;
}


std::string NetworkInformation::Impl::getNetworkName(AdapterTypes type, const std::string &ifName, const std::string &address)
{
	std::string networkName = "";

	if (type == AdapterTypes::WiFi)
		networkName = getWifiSsid(ifName);
	else if (type == AdapterTypes::Ethernet)
		networkName = getNetworkGatename(type, ifName, address);

	return networkName;
}


} // namespace netlink
