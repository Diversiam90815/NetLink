/*
==============================================================================
	Module:         WinsockCommon
	Description:    Low-level WinSock2 common modules
  ==============================================================================
*/

#pragma once

#include <WinSock2.h>
#include <WS2tcpip.h>

#include "../ISocket.h"

#pragma comment(lib, "Ws2_32.lib")


namespace
{
std::mutex gWinsSocketMutex;
int		   gWinsSocketRefCount = 0;
} // namespace

struct WinsockScope
{
	WinsockScope()
	{
		std::lock_guard<std::mutex> lock(gWinsSocketMutex);

		if (gWinsSocketRefCount++ == 0)
		{
			WSADATA	  wsaData{};
			const int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);

			if (rc != 0)
				gWinsSocketRefCount = 0;
		}
	}

	~WinsockScope()
	{
		std::lock_guard<std::mutex> lock(gWinsSocketMutex);

		if (gWinsSocketRefCount > 0 && --gWinsSocketRefCount == 0)
			WSACleanup();
	}
	WinsockScope(const WinsockScope &)			  = delete;
	WinsockScope &operator=(const WinsockScope &) = delete;
};
