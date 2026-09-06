/*
==============================================================================
	Module:         NetlinkSocket
	Description:    Socket wrapper for Netlink
  ==============================================================================
*/

#pragma once

#include <string>
#include <vector>
#include <cstddef>


#include "SocketFactory.h"


// Result of socket binding operation
struct SocketBindResult
{
	enum class BindingStatus
	{
		None				= 0,
		Success				= 1,
		PortBlocked			= 2,
		AddressInUse		= 3,
		AddressNotAvailable = 4,
		PermissionDenied	= 5,
		InvalidAddress		= 6,
		SocketUnavailable	= 7,
		UnknownError		= 9,
	};

	int			  port	 = 0;
	std::string	  IPv4	 = "";
	BindingStatus status = BindingStatus::None;

	bool		  succeeded() const { return status == BindingStatus::Success; }
	bool		  isPortBlocked() const { return status == BindingStatus::PortBlocked; }

	std::string	  getStatusString() const
	{
		switch (status)
		{
		case BindingStatus::None: return "None (default value)";
		case BindingStatus::Success: return "Success";
		case BindingStatus::PortBlocked: return "Port Blocked";
		case BindingStatus::AddressInUse: return "Address In Use";
		case BindingStatus::AddressNotAvailable: return "Address Not Available";
		case BindingStatus::PermissionDenied: return "Permission Denied";
		case BindingStatus::InvalidAddress: return "Invalid Address";
		case BindingStatus::SocketUnavailable: return "Socket Unavailable";
		case BindingStatus::UnknownError: return "Unknown Error";
		default: return "Unrecognized Error";
		}
	}
};


// Represents a received packet
struct ReceivedPacket
{
	std::vector<std::byte> data;
	std::string			   senderIP;
	int					   senderPort;
};


// The sole socket wrapper used accross the project
class NetlinkSocket
{
public:
	NetlinkSocket() = default;
	explicit NetlinkSocket(NetLink::SocketTransport transport);
	~NetlinkSocket()								= default;

	NetlinkSocket(const NetlinkSocket &)			= delete;
	NetlinkSocket &operator=(const NetlinkSocket &) = delete;

	NetlinkSocket(NetlinkSocket &&other) noexcept;
	NetlinkSocket			&operator=(NetlinkSocket &&other) noexcept;

	// Convenience factories
	static NetlinkSocket	 createUDP();
	static NetlinkSocket	 createTCP();

	SocketBindResult		 bind(const std::string &address, int port, const NetLink::BindOptions &options = {});

	NetlinkSocket			 takeAcceptedConnection(int timeoutMS = 250);

	bool					 connect(const std::string &address, int port, int timeoutMS);
	bool					 listen(int backlog);
	bool					 accept(int timeoutMS);
	bool					 isConnected() const { return mSocket && mSocket->isConnected(); }

	int						 sendTo(const std::string &address, int port, const void *data, int size);
	int						 sendTo(const std::string &address, int port, const std::vector<std::byte> &data);

	bool					 receive(ReceivedPacket &outPacket, int timeoutMS = 0);

	bool					 waitUntilReadable(int timeoutMS);
	bool					 waitUntilWritable(int timeoutMS);

	void					 close();
	bool					 isOpen() const;
	int						 getBoundPort() const;
	std::string				 getRemoteAddress() const;
	int						 getRemotePort() const;

	NetLink::SocketTransport getTransport() const { return mTransport; }

private:
	static SocketBindResult	 translate(const NetLink::SocketBindResult &raw, const std::string &address, int port);

	NetLink::SocketTransport mTransport = NetLink::SocketTransport::None;
	std::unique_ptr<ISocket> mSocket;
};
