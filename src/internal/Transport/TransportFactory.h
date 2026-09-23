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


// Creates the factory for the transport selected in NetLinkConfig
std::unique_ptr<ITransportFactory> createTransportFactory(TransportKind kind);

} // namespace netlink
