/*
  ==============================================================================
	Module:         TransportInterfaces
	Description:    Protocol-agnostic interfaces for transport modules
  ==============================================================================
*/

#pragma once

#include <functional>
#include <memory>

#include "Messaging/MessageTypes.h"


/**
 * @brief	Interface for a session handling an established connection.
 */
class ISession
{
public:
	using MessageReceivedCallback								  = std::function<void(netlink::InternalMessage &message)>;
	using pointer												  = std::shared_ptr<ISession>;

	virtual ~ISession()											  = default;

	/**
	 * @brief	True if socket is in a connected state.
	 */
	virtual bool isConnected() const							  = 0;

	/**
	 * @brief	Send a serialized multiplayer message (synchronous or async fire-and-forget).
	 * @return	true if queued / sent successfully.
	 */
	virtual bool sendMessage(netlink::InternalMessage &message)	  = 0;

	/**
	 * @brief	Begin asynchronous read loop invoking callback per complete message.
	 */
	virtual void startReadAsync(MessageReceivedCallback callback) = 0;

	/**
	 * @brief	Stop ongoing async read loop (callback will no longer fire).
	 */
	virtual void stopReadAsync()								  = 0;

	/**
	 * @brief	The bound local port (after connect / accept).
	 */
	virtual int	 getBoundPort() const							  = 0;
};


/**
 * @brief	Interface for a server accepting inbound connections.
 */
class IServer
{
public:
	using SessionHandler								   = std::function<void(ISession::pointer session)>;

	virtual ~IServer()									   = default;

	/**
	 * @brief	Begin accepting connections (continuously or single depending on implementation).
	 */
	virtual void startAccept()							   = 0;

	/**
	 * @brief	Local port currently bound.
	 */
	virtual int	 getBoundPort() const					   = 0;

	/**
	 * @brief	Register handler invoked with newly created session.
	 */
	virtual void setSessionHandler(SessionHandler handler) = 0;

	/**
	 * @brief	Respond to pending connection request (e.g., after an invitation flow).
	 */
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

	/**
	 * @brief	Begin async connect to host:port.
	 */
	virtual void connect(const std::string &host, unsigned short port)	 = 0;

	/**
	 * @brief	Set handler invoked on successful connection establishment.
	 */
	virtual void setConnectHandler(ConnectHandler handler)				 = 0;

	/**
	 * @brief	Set handler invoked if connection attempt times out.
	 */
	virtual void setConnectTimeoutHandler(ConnectTimeoutHandler handler) = 0;
};
