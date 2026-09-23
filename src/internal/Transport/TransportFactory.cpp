/*
  ==============================================================================
	Module:         TransportFactory
	Description:    Abstract factory for creating transport components
  ==============================================================================
*/

#include "TransportFactory.h"
#include "TCP/TCPTransportFactory.h"


std::unique_ptr<netlink::ITransportFactory> netlink::createTransportFactory(TransportKind kind)
{
	switch (kind)
	{
	case TransportKind::Tcp:
		return std::make_unique<TCPTransportFactory>();
		// @TODO: create UCP transport once implemented
	}

	return std::make_unique<TCPTransportFactory>();
}
