/*
==============================================================================
	Module:         SocketFactory
	Description:    Creates platform- & UDP/TCP specific ISocket Instances
  ==============================================================================
*/

#include "SocketFactory.h"

#if defined(_WIN32)
#include "WinDatagramSocket.h"
// include WinStreamSocket
#elifdef
// include Mac/Linux UDP/TCP sockets
#endif //


std::unique_ptr<ISocket> SocketFactory::create(NetLink::SocketTransport transport)
{
	return std::unique_ptr<ISocket>();	// @TODO
}
