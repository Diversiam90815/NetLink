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


/**
 * @brief	Interface for a session handling an established connection.
 */
class ISession
{
public:
	using MessageReceivedCallback										 = std::function<void(netlink::InternalMessage &message)>;
	using pointer														 = std::shared_ptr<ISession>;

	virtual ~ISession()													 = default;

	virtual bool		isConnected() const								 = 0;

	virtual bool		sendMessage(netlink::InternalMessage &message)	 = 0;

	virtual void		startReadAsync(MessageReceivedCallback callback) = 0;
	virtual void		stopReadAsync()									 = 0;

	virtual int			getBoundPort() const							 = 0;
	virtual std::string getRemoteAddress() const						 = 0;
	virtual int			getRemotePort() const							 = 0;
};


/**
 * @brief	Interface for a server accepting inbound connections.
 */
class IServer
{
public:
	using SessionHandler								   = std::function<void(ISession::pointer session)>;

	virtual ~IServer()									   = default;

	virtual void startAccept()							   = 0;

	virtual int	 getBoundPort() const					   = 0;

	virtual void setSessionHandler(SessionHandler handler) = 0;

	virtual void respondToConnectionRequest(bool accepted) = 0;
};


/**
 * @brief	Interface for a client initiating outbound connections.
 */
class IClient
{
public:
	using ConnectHandler												 = std::function<void(ISession::pointer session)>;
	using ConnectTimeoutHandler											 = std::function<void()>;

	virtual ~IClient()													 = default;

	virtual void connect(const std::string &host, unsigned short port)	 = 0;

	virtual void setConnectHandler(ConnectHandler handler)				 = 0;

	virtual void setConnectTimeoutHandler(ConnectTimeoutHandler handler) = 0;
};
