/*
  ==============================================================================
	Module:         NetworkInformation
	Description:    Information about the local Network setup
  ==============================================================================
*/

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iptypes.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <wlanapi.h>

#include <vector>
#include <unordered_set>
#include <memory>
#include <functional>

#include "NetLinkLog.h"


namespace netlink
{

using AdapterChangedCallback = std::function<void(const std::string &newIPv4)>;

enum class AdapterTypes
{
	Ethernet = 1,
	WiFi	 = 2,
	Loopback = 3,
	Virtual	 = 4,
	Other	 = 5
};


enum class AdapterPriorityInternal
{
	Suppressed = 1,
	Available  = 2,
	Preferred  = 3
};


struct NetworkAdapterInternal
{
	NetworkAdapterInternal() = default;

	NetworkAdapterInternal(const std::string	  &adapterName,
						   const std::string	  &networkName,
						   const std::string	  &ipv4,
						   const std::string	  &subnet,
						   const int			   id,
						   bool					   isDefaultRoute,
						   AdapterTypes			   type,
						   AdapterPriorityInternal priority)
		: AdapterName(adapterName), NetworkName(networkName), IPv4(ipv4), Subnet(subnet), ID(id), IsDefaultRoute(isDefaultRoute), Type(type), Priority(priority)
	{
		Eligible = filterSubnetMask();
	}


	bool					operator==(const NetworkAdapterInternal &other) const { return std::tie(AdapterName, Subnet) == std::tie(other.AdapterName, other.Subnet); }
	bool					operator!=(const NetworkAdapterInternal &other) const { return !(*this == other); }

	bool					isValid() const { return !AdapterName.empty() && !IPv4.empty() && ID != 0; }

	bool					filterSubnetMask() const { return Subnet == "255.255.255.0"; }

	std::string				AdapterName{};
	std::string				NetworkName{};
	std::string				IPv4{};
	std::string				Subnet{};
	int						ID{0};
	bool					IsDefaultRoute{false};
	bool					Eligible{false};
	AdapterTypes			Type{AdapterTypes::Other};
	AdapterPriorityInternal Priority{};
};



class NetworkInformation
{
public:
	NetworkInformation() = default;
	~NetworkInformation();

	bool									   init();

	void									   deinit();

	void									   processAdapter();

	bool									   setCurrentNetworkAdapter(const int adapterID);
	bool									   setCurrentNetworkAdapter(const NetworkAdapterInternal &adapter);
	const NetworkAdapterInternal			  &getCurrentNetworkAdapter() const;

	NetworkAdapterInternal					   isAdapterCurrentlyAvailable(const NetworkAdapterInternal &adapter);

	const std::vector<NetworkAdapterInternal> &getAvailableNetworkAdapters() const;

	void									   setOnAdapterChanged(AdapterChangedCallback cb) { mOnAdapterChanged = std::move(cb); }

private:
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

	struct IpForwardTable
	{
		MIB_IPFORWARD_TABLE2 *ptr{nullptr};
		~IpForwardTable()
		{
			if (ptr)
				FreeMibTable(ptr);
		}
	};

	struct WlanHandle
	{
		HANDLE h{};
		~WlanHandle()
		{
			if (h)
				WlanCloseHandle(h, nullptr);
		}
	};

	struct WlanQueryData
	{
		void *ptr{};
		~WlanQueryData()
		{
			if (ptr)
				WlanFreeMemory(ptr);
		}
	};

	using AdapterBuffer = std::unique_ptr<IP_ADAPTER_ADDRESSES, void (*)(IP_ADAPTER_ADDRESSES *)>;

	void					saveAdapter(const PIP_ADAPTER_ADDRESSES adapter, const int ID, std::unordered_set<ULONG64> &defaultRouteLuidValues);
	bool					getNetworkInformationFromOS();

	std::string				sockaddrToString(SOCKADDR *sa) const;
	std::string				prefixLengthToSubnetMask(USHORT family, ULONG prefixLength) const;
	AdapterTypes			filterAdapterType(const DWORD Type) const;
	AdapterPriorityInternal determinePriority(bool isDefaultRoute, bool IPv4Enabled, AdapterTypes type, IF_OPER_STATUS status);

	bool					getDefaultInterfaces(std::vector<NET_LUID> &pLUIDs);
	std::string				getHostName(const SOCKADDR *ip, const socklen_t ipLength);
	std::string				getWifiSsid(const AdapterTypes type, const NET_LUID luid);
	std::string				getNetworkGatename(const AdapterTypes type, const NET_LUID_LH luid, const std::string address);
	std::string				getNetworkName(const AdapterTypes type, const NET_LUID_LH luid, const std::string address);


	std::string				WStringToStdString(const std::wstring &wstr)
	{
		if (wstr.empty())
			return {};

		std::string str{};
		size_t		size{};
		str.resize(wstr.length());
		wcstombs_s(&size, &str[0], str.size() + 1, wstr.c_str(), wstr.size());
		return str;
	}


	AdapterBuffer						mAdapterAddresses{nullptr, [](IP_ADAPTER_ADDRESSES *p)
									  {
										  if (p)
											  free(p);
									  }};
	ULONG								mOutBufLen{0};

	std::unique_ptr<WinsockSession>		mWinsockSession;

	std::vector<NetworkAdapterInternal> mNetworkAdapters{};
	NetworkAdapterInternal				mCurrentNetworkAdapter{};

	AdapterChangedCallback				mOnAdapterChanged;
};


} // namespace netlink
