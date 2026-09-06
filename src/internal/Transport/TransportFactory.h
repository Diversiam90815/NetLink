/*
  ==============================================================================
	Module:         TransportFactory
	Description:    Abstract factory for creating transport components
  ==============================================================================
*/

#pragma once

#include <memory>

#include "TransportInterfaces.h"


namespace netlink
{

class ITransportFactory
{
public:
	virtual ~ITransportFactory()					= default;

	virtual std::unique_ptr<IServer> createServer() = 0;
	virtual std::unique_ptr<IClient> createClient() = 0;
};

} // namespace netlink
