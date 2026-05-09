/*
  ==============================================================================
	Module:         TransportFactory
	Description:    Abstract factory for creating transport components
  ==============================================================================
*/

#pragma once

#include <memory>

#include "TransportInterfaces.h"


namespace asio
{
class io_context;
}


namespace netlink
{

class ITransportFactory
{
public:
	virtual ~ITransportFactory()												   = default;

	virtual std::unique_ptr<IServer> createServer(asio::io_context &ioContext) = 0;
	virtual std::unique_ptr<IClient> createClient(asio::io_context &ioContext) = 0;
};

} // namespace netlink
