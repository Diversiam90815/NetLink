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
	std::unique_ptr<IServer> createServer(asio::io_context &ioContext) override { return std::make_unique<TCPServer>(ioContext); }

	std::unique_ptr<IClient> createClient(asio::io_context &ioContext) override { return std::make_unique<TCPClient>(ioContext); }
};

} // namespace netlink
