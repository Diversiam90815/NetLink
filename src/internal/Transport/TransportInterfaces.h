/*
  ==============================================================================
	Module:         TransportInterfaces
	Description:    Protocol-agnostic interfaces for transport modules
  ==============================================================================
*/

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "Messaging/MessageTypes.h"
#include "Socket/IPv4Address.h"


namespace netlink
{

//	An established, message oriented connection to one remote peer
class ISession
{
public:
	using MessageReceivedCallback																					= std::function<void(InternalMessage message)>;
	using DisconnectedCallback																						= std::function<void(const std::string &reason)>;
	using pointer																									= std::shared_ptr<ISession>;

	virtual ~ISession()																								= default;

	virtual bool			 isConnected() const																	= 0;

	// Returns once the message was handed to the transport, or false if the session cannot deliver it.
	virtual bool			 sendMessage(const InternalMessage &message, DeliveryMode mode)							= 0;

	// Delivers received messages until stopReadAsync()/close(). onDisconnected fires once if the connection is lost
	virtual void			 startReadAsync(MessageReceivedCallback onMessage, DisconnectedCallback onDisconnected) = 0;

	// Returns once no callback is running anymore
	virtual void			 stopReadAsync()																		= 0;

	virtual int				 getBoundPort() const																	= 0;
	virtual net::IPv4Address getRemoteAddress() const																= 0;
	virtual int				 getRemotePort() const																	= 0;

	virtual void			 close()																				= 0;
};


// Accepts inbound connections
class IServer
{
public:
	using SessionHandler									 = std::function<void(ISession::pointer session)>;

	virtual ~IServer()										 = default;

	// Must be set before start(). Called on the accept thread for every inbound connection.
	virtual void setSessionHandler(SessionHandler handler)	 = 0;

	// Listens on localAddress with an OS assigned port and starts accepting. Returns false if listening failed.
	virtual bool start(const net::IPv4Address &localAddress) = 0;

	// Stops accepting. Established sessions stay alive.
	virtual void stop()										 = 0;

	virtual int	 getBoundPort() const						 = 0;
};


// Initiates outbound connections
class IClient
{
public:
	using ConnectHandler																						  = std::function<void(ISession::pointer session)>;
	using ConnectTimeoutHandler																					  = std::function<void()>;

	virtual ~IClient()																							  = default;

	// Must be set before connect(). Exactly one of them fires per connect() unless the client is destroyed first.
	virtual void setConnectHandler(ConnectHandler handler)														  = 0;
	virtual void setConnectTimeoutHandler(ConnectTimeoutHandler handler)										  = 0;

	// Connects asynchronously from localAddress (the selected adapter; empty = OS choice). A previous pending attempt is cancelled.
	virtual void connect(const net::IPv4Address &localAddress, const net::IPv4Address &host, unsigned short port) = 0;
};

} // namespace netlink
