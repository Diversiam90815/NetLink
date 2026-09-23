/*
  ==============================================================================
	Module:         TCPTransportFactory
	Description:    Concrete transport factory for TCP-based connections
  ==============================================================================
*/

#pragma once

#include "Transport/TransportFactory.h"
#include "TCPServer.h"
#include "TCPClient.h"


namespace netlink
{

class TCPTransportFactory : public ITransportFactory
{
public:
	std::unique_ptr<IServer> createServer() override { return std::make_unique<TCPServer>(); }
	std::unique_ptr<IClient> createClient() override { return std::make_unique<TCPClient>(); }
};

} // namespace netlink
