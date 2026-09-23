/*
  ==============================================================================
	Module:         TCPSession
	Description:    Message based session on top of a connected TCP stream
  ==============================================================================
*/

#include "TCPSession.h"

#include <atomic>
#include <mutex>
#include <thread>

#include "Messaging/MessageFramer.h"
#include "NetLinkConstants.h"
#include "NetLinkLog.h"
#include "Util/ThreadUtils.h"


namespace netlink
{

struct TCPSession::State
{
	explicit State(net::TcpStream s) : stream(std::move(s)) {}

	net::TcpStream					   stream;
	std::atomic<bool>				   connected{true};

	std::mutex						   sendMutex;

	std::mutex						   readMutex; // guards readThread / readStopFlag
	std::thread						   readThread;
	std::shared_ptr<std::atomic<bool>> readStopFlag;
};


TCPSession::TCPSession(net::TcpStream stream) : mState(std::make_shared<State>(std::move(stream))) {}


TCPSession::~TCPSession()
{
	close();
}


bool TCPSession::isConnected() const
{
	return mState->connected.load();
}


bool TCPSession::sendMessage(const InternalMessage &message, DeliveryMode /*mode: TCP is always reliable & ordered*/)
{
	if (!isConnected())
		return false;

	if (message.data.size() > internal::MaxMessagePayload)
	{
		NETLINK_LOG_ERROR("Message of type {} exceeds the maximum payload ({} > {} bytes)", message.type, message.data.size(), internal::MaxMessagePayload);
		return false;
	}

	const auto					frame = MessageFramer::serialize(message);

	std::lock_guard<std::mutex> lock(mState->sendMutex);

	if (auto sent = mState->stream.sendAll(frame, internal::TcpSendTimeout); !sent)
	{
		NETLINK_LOG_ERROR("TCPSession: sending failed ({}), dropping connection", net::toString(sent.error()));

		// The stream is unusable now, shutting it down lets the read loop report the disconnect.
		mState->connected.store(false);
		mState->stream.shutdown();
		return false;
	}

	return true;
}


void TCPSession::startReadAsync(MessageReceivedCallback onMessage, DisconnectedCallback onDisconnected)
{
	stopReadAsync();

	std::lock_guard<std::mutex> lock(mState->readMutex);

	auto						stopFlag = std::make_shared<std::atomic<bool>>(false);
	mState->readStopFlag				 = stopFlag;
	mState->readThread					 = std::thread([state = mState, stopFlag, onMessage = std::move(onMessage), onDisconnected = std::move(onDisconnected)]()
													   { readLoop(state, stopFlag, onMessage, onDisconnected); });
}


void TCPSession::stopReadAsync()
{
	std::thread thread = requestReadStop();
	joinOrDetach(thread);
}


std::thread TCPSession::requestReadStop()
{
	std::lock_guard<std::mutex> lock(mState->readMutex);

	if (mState->readStopFlag)
		mState->readStopFlag->store(true);

	return std::move(mState->readThread);
}


int TCPSession::getBoundPort() const
{
	return mState->stream.localAddress().port;
}


net::IPv4Address TCPSession::getRemoteAddress() const
{
	return mState->stream.remoteAddress().ip;
}


int TCPSession::getRemotePort() const
{
	return mState->stream.remoteAddress().port;
}


void TCPSession::close()
{
	// Stop flag first, so the read loop does not report the local shutdown as a lost connection
	std::thread thread = requestReadStop();

	mState->connected.store(false);
	mState->stream.shutdown();

	joinOrDetach(thread);
}


void TCPSession::readLoop(const std::shared_ptr<State>			   &state,
						  const std::shared_ptr<std::atomic<bool>> &stopFlag,
						  const MessageReceivedCallback			   &onMessage,
						  const DisconnectedCallback			   &onDisconnected)
{
	FrameDecoder		 decoder;
	std::vector<uint8_t> buffer(internal::PackageBufferSize);

	auto				 reportDisconnect = [&](const std::string &reason)
	{
		state->connected.store(false);
		state->stream.shutdown();

		// A local close()/stopReadAsync() is not a lost connection
		if (stopFlag->load())
			return;

		NETLINK_LOG_WARNING("TCPSession: connection to {} lost: {}", state->stream.remoteAddress().toString(), reason);

		if (onDisconnected)
			onDisconnected(reason);
	};

	while (!stopFlag->load())
	{
		auto received = state->stream.receiveSome(buffer, internal::SocketPollInterval);

		if (!received)
		{
			if (received.error() == net::SocketError::Timeout)
				continue;

			reportDisconnect(std::string(net::toString(received.error())));
			return;
		}

		decoder.feed(std::span<const uint8_t>(buffer.data(), *received));

		while (auto message = decoder.next())
		{
			if (stopFlag->load())
				return;

			if (onMessage)
				onMessage(std::move(*message));
		}

		if (decoder.hasError())
		{
			reportDisconnect("protocol error: invalid frame length");
			return;
		}
	}
}

} // namespace netlink
