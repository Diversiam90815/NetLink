/*
  ==============================================================================
	Module:         ServiceWiring
	Description:    Connects services to their PeerChannel for benchmarks that run
					many service instances
  ==============================================================================
*/

#pragma once

#include <functional>
#include <string>

#include "BenchUtil.h"
#include "Channel/PeerChannel.h"
#include "ConnectionService/ConnectionService.h"
#include "PeerValidation/PeerValidationService.h"


namespace bench
{

// Channel <-> PeerValidationService (signals in, requests and answers out)
inline void wireValidation(netlink::PeerChannel &channel, netlink::PeerValidationService &validation)
{
	using netlink::RemoteRequest;

	netlink::ChannelValidationCallbacks signals;
	signals.onValidationRequestReceived = [&validation](const std::string &name, const RemoteRequest request) { validation.onRequestReceived(name, request); };
	signals.onSecretResponseReceived	= [&validation](const std::string &name, const std::string &secret)
	{ validation.onCheckResponseReceived(name, RemoteRequest::Secret, secret); };
	signals.onVersionResponseReceived = [&validation](const std::string &name, const std::string &version)
	{ validation.onCheckResponseReceived(name, RemoteRequest::Version, version); };
	signals.onValidationHandshakeReceived = [&validation](const std::string &name) { validation.onHandshakeReceived(name); };
	channel.setValidationCallbacks(std::move(signals));

	netlink::PeerValidationSendCallbacks send;
	send.sendRequest		 = [&channel](const std::string &name, const RemoteRequest request) { channel.sendValidationRequest(name, request); };
	send.sendSecretResponse	 = [&channel](const std::string &name, const std::string &value) { channel.sendSecretResponse(name, value); };
	send.sendVersionResponse = [&channel](const std::string &name, const std::string &value) { channel.sendVersionResponse(name, value); };
	send.sendHandshake		 = [&channel](const std::string &name) { channel.sendValidationHandshake(name); };
	validation.setSendCallbacks(std::move(send));
}


// Channel -> ConnectionService (connection signals and peer loss)
inline void wireConnection(netlink::PeerChannel &channel, netlink::ConnectionService &service)
{
	netlink::ChannelConnectionCallbacks signals;
	signals.onConnectRequested		 = [&service](const std::string &name) { service.onReceivedInvitation(name); };
	signals.onConnectRequestAnswered = [&service](const std::string &name, const bool accepted, const std::string &reason)
	{ service.onReceivedAnswerToInvite(name, accepted, reason); };
	signals.onDisconnectReceived = [&service](const std::string &name) { service.onDisconnectReceived(name); };
	signals.onReadyFlagReceived	 = [&service](const std::string &name) { service.onReadyFlagReceived(name); };
	channel.setConnectionCallbacks(std::move(signals));
	channel.setOnPeerLost([&service](const std::string &name, const std::string &reason) { service.onPeerLost(name, reason); });
}


// ConnectionService -> channel, as NetLinkCore::onConnectionStatus does: keepalive for the session, the session's
// unsent application traffic ends with it. Every update is then handed to report.
inline netlink::ConnectionServiceCallbacks connectionStatusHandler(netlink::PeerChannel &channel, std::function<void(const netlink::ConnectionStatusUpdate &)> report)
{
	using Type = netlink::ConnectionStatusUpdate::Type;

	netlink::ConnectionServiceCallbacks callbacks;
	callbacks.onStatusUpdate = [&channel, report = std::move(report)](const netlink::ConnectionStatusUpdate &update)
	{
		if (update.type == Type::Established)
			channel.setKeepAlive(update.endpoint.displayName, true);
		else if (update.type == Type::Closed)
		{
			channel.setKeepAlive(update.endpoint.displayName, false);
			channel.dropApplicationTraffic(update.endpoint.displayName);
		}

		report(update);
	};
	return callbacks;
}


// A remote that passed validation, as the connection service learns about it
inline netlink::ValidationResult validatedRemote(const std::string &name, const int channelPort)
{
	netlink::ValidationResult result;
	result.remoteEndpoint = DiscoveryEndpoint{.IPAddress = loopback(), .port = channelPort, .displayName = name};
	result.status		  = netlink::ValidationResult::Status::ReadyToConnect;
	result.canConnect	  = true;
	return result;
}

} // namespace bench
