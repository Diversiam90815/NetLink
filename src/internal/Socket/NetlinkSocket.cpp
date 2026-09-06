/*
==============================================================================
	Module:         NetlinkSocket
	Description:    Socket wrapper for Netlink
  ==============================================================================
*/

#include "NetlinkSocket.h"

#if defined(_WIN32)
#include <WinSock2.h>
#endif

NetlinkSocket::NetlinkSocket(NetLink::SocketTransport transport) : mTransport(transport), mSocket(SocketFactory::create(transport)) {}


NetlinkSocket::NetlinkSocket(NetlinkSocket &&other) noexcept : mTransport(other.mTransport), mSocket(std::move(other.mSocket))
{
	other.mTransport = NetLink::SocketTransport::None;
}


NetlinkSocket &NetlinkSocket::operator=(NetlinkSocket &&other) noexcept
{
	if (this != &other)
	{
		mSocket			 = std::move(other.mSocket);
		mTransport		 = other.mTransport;
		other.mTransport = NetLink::SocketTransport::None;
	}
	return *this;
}


NetlinkSocket NetlinkSocket::createUDP()
{
	return NetlinkSocket(NetLink::SocketTransport::UDP);
}


NetlinkSocket NetlinkSocket::createTCP()
{
	return NetlinkSocket(NetLink::SocketTransport::TCP);
}


SocketBindResult NetlinkSocket::bind(const std::string &address, int port, const NetLink::BindOptions &options)
{
	if (!mSocket)
	{
		SocketBindResult result;
		result.port	  = port;
		result.IPv4	  = address;
		result.status = SocketBindResult::BindingStatus::SocketUnavailable;
		return result;
	}

	NetLink::SocketBindResult raw = mSocket->bind(address, port, options);
	return translate(raw, address, port);
}


NetlinkSocket NetlinkSocket::takeAcceptedConnection(int timeoutMS)
{
	NetlinkSocket result;

	if (!mSocket)
		return result;

	auto accepted = mSocket->acceptNew(timeoutMS);
	if (!accepted)
		return result; // timeout / error

	result.mTransport = NetLink::SocketTransport::TCP;
	result.mSocket	  = std::move(accepted);
	return result;
}


bool NetlinkSocket::connect(const std::string &address, int port, int timeoutMS)
{
	return mSocket && mSocket->connect(address, port, timeoutMS);
}


bool NetlinkSocket::listen(int backlog)
{
	return mSocket && mSocket->listen(backlog);
}


bool NetlinkSocket::accept(int timeoutMS)
{
	return mSocket && mSocket->accept(timeoutMS);
}


int NetlinkSocket::sendTo(const std::string &address, int port, const void *data, int size)
{
	return mSocket ? mSocket->sendTo(address, port, data, size) : -1;
}


int NetlinkSocket::sendTo(const std::string &address, int port, const std::vector<std::byte> &data)
{
	return sendTo(address, port, data.data(), static_cast<int>(data.size()));
}


bool NetlinkSocket::receive(ReceivedPacket &outPacket, int timeoutMS)
{
	if (!mSocket)
		return false;

	constexpr int buffersize = 64 * 1024;
	outPacket.data.resize(buffersize);

	std::string senderIP   = "";
	int			senderPort = 0;

	int			received   = mSocket->recvFrom(outPacket.data.data(), buffersize, senderIP, senderPort, timeoutMS);
	if (received <= 0)
		return false;

	outPacket.data.resize(received);
	outPacket.senderIP	 = std::move(senderIP);
	outPacket.senderPort = senderPort;
	return true;
}


bool NetlinkSocket::waitUntilReadable(int timeoutMS)
{
	return mSocket && mSocket->waitUntilReady(true, timeoutMS);
}


bool NetlinkSocket::waitUntilWritable(int timeoutMS)
{
	return mSocket && mSocket->waitUntilReady(false, timeoutMS);
}


void NetlinkSocket::close()
{
	if (mSocket)
		mSocket->close();
}


bool NetlinkSocket::isOpen() const
{
	return mSocket && mSocket->isOpen();
}


int NetlinkSocket::getBoundPort() const
{
	return mSocket ? mSocket->getBoundPort() : 0;
}


std::string NetlinkSocket::getRemoteAddress() const
{
	return mSocket ? mSocket->getRemoteAddress() : std::string{};
}


int NetlinkSocket::getRemotePort() const
{
	return mSocket ? mSocket->getRemotePort() : 0;
}


SocketBindResult NetlinkSocket::translate(const NetLink::SocketBindResult &raw, const std::string &address, int port)
{
	SocketBindResult result;
	result.port = raw.boundPort != 0 ? raw.boundPort : port;
	result.IPv4 = !raw.address.empty() ? raw.address : address;

	if (raw.success)
	{
		result.status = SocketBindResult::BindingStatus::Success;
		return result;
	}

#if defined(_WIN32)
	switch (raw.nativeError)
	{
	case WSAEADDRINUSE: result.status = SocketBindResult::BindingStatus::AddressInUse; break;
	case WSAEADDRNOTAVAIL: result.status = SocketBindResult::BindingStatus::AddressNotAvailable; break;
	case WSAEACCES: result.status = SocketBindResult::BindingStatus::PermissionDenied; break;
	case WSAEINVAL: result.status = SocketBindResult::BindingStatus::InvalidAddress; break;
	default: result.status = SocketBindResult::BindingStatus::UnknownError; break;
	}
#else
	// @TODO
#endif

	if (raw.nativeError == 0 && result.status == SocketBindResult::BindingStatus::UnknownError)
		result.status = SocketBindResult::BindingStatus::SocketUnavailable;

	return result;
}
