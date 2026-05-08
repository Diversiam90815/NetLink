/*
  ==============================================================================
	Module:         ConnectionFlow
	Description:    Orchestrating the connection
  ==============================================================================
*/

#include <string>
#include <functional>
#include <memory>

#include "ConnectionPhase.h"
#include "Signaling/SignalingService.h"
#include "TCP/TCPServer.h"
#include "TCP/TCPClient.h"
#include "Discovery/DiscoveryEndpoint.h"
#include "Signaling/RoleNegotiation.h"


namespace netlink
{

struct ConnectionFlowCallbacks
{
	// inbound request arrived. App must call respondToConnection
	std::function<void(const DiscoveryEndpoint &remote)> onConnectionRequested;

	// TCP fully established. Session is ready for messaging
	std::function<void(ITCPSession::pointer session)>	 onConnected;

	// Remote declined or disconnected
	std::function<void(const std::string &reason)>		 onConnectionFailed;

	// Remote initiated a disconnect
	std::function<void()>								 onDisconnected;
};



class ConnectionFlow
{
public:
	ConnectionFlow(asio::io_context &io_context, SignalingService &signaling);

	void			setCallbacks(ConnectionFlowCallbacks cb) { mCallbacks = std::move(cb); }
	void			setLocalIP(const std::string &ip) { mLocalIP = ip; }

	// Called by NetLink::connectTo()
	void			requestConnection(const DiscoveryEndpoint &remote);

	// Called by NetLink::respondToConnection()
	void			respondToConnection(bool accepted);

	// Called by NetLink::disconnect()
	void			disconnect();

	ConnectionPhase getPhase() const { return mPhase; }

private:
	void					   onSignalConnectRequested(const SignalPacket &pkt);
	void					   onSignalConnectAccepted(const SignalPacket &pkt);
	void					   onSignalConnectDeclined(const SignalPacket &pkt);
	void					   onSignalDisconnect(const SignalPacket &pkt);

	void					   beginTCPEstablishment();
	void					   reset();


	asio::io_context		  &mIoContext;
	SignalingService		  &mSignaling;

	ConnectionFlowCallbacks	   mCallbacks;
	ConnectionPhase			   mPhase{ConnectionPhase::Idle};

	DiscoveryEndpoint		   mRemote{};
	std::string				   mLocalIP{};

	std::unique_ptr<TCPServer> mServer;
	std::unique_ptr<TCPClient> mClient;
};


} // namespace netlink
