/*
  ==============================================================================
	Module:         TCPSession
	Description:    Message based session on top of a connected TCP stream
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "Transport/TransportInterfaces.h"
#include "Socket/TcpStream.h"


namespace netlink
{

class TCPSession final : public ISession
{
public:
	explicit TCPSession(net::TcpStream stream);
	~TCPSession() override;
	TCPSession(const TCPSession &)			  = delete;
	TCPSession &operator=(const TCPSession &) = delete;

	bool		isConnected() const override;

	bool		sendMessage(const InternalMessage &message, DeliveryMode mode) override;

	void		startReadAsync(MessageReceivedCallback onMessage, DisconnectedCallback onDisconnected) override;
	void		stopReadAsync() override;

	int			getBoundPort() const override;
	std::string getRemoteAddress() const override;
	int			getRemotePort() const override;

	void		close() override;

private:
	// Everything the read thread touches
	struct State;

	static void			   readLoop(const std::shared_ptr<State>			 &state,
									const std::shared_ptr<std::atomic<bool>> &stopFlag,
									const MessageReceivedCallback			 &onMessage,
									const DisconnectedCallback				 &onDisconnected);

	// Requests the read loop to stop and hands out its thread for joining
	std::thread			   requestReadStop();

	std::shared_ptr<State> mState;
};

} // namespace netlink
