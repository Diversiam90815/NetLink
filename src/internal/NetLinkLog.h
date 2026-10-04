/*
  ==============================================================================
	Module:         NetLinkLog
	Description:    Internal logging: lines go to the sink of the thread that
					writes them, which hands them to the application's onLog
  ==============================================================================
*/

#pragma once

#include <format>
#include <functional>
#include <string>

#include "NetLink/NetLink.h"


namespace netlink::internal
{

using LogSink = std::function<void(LogLevel level, std::string &&text)>;

// Where the log lines of the calling thread go from now on. Without a sink nothing is logged, and nothing is formatted.
void setThreadLogSink(LogSink sink);

bool hasThreadLogSink();
void writeLog(LogLevel level, std::string &&text);


// Gives the calling thread a sink for as long as the scope lives, unless it has one already
class LogScope
{
public:
	explicit LogScope(const LogSink &sink) : mOwns(sink && !hasThreadLogSink())
	{
		if (mOwns)
			setThreadLogSink(sink);
	}

	~LogScope()
	{
		if (mOwns)
			setThreadLogSink({});
	}

	LogScope(const LogScope &)			  = delete;
	LogScope &operator=(const LogScope &) = delete;

private:
	bool mOwns;
};

} // namespace netlink::internal


#define NETLINK_LOG(level, ...)                                                                  \
	do                                                                                           \
	{                                                                                            \
		if (::netlink::internal::hasThreadLogSink())                                             \
			::netlink::internal::writeLog(::netlink::LogLevel::level, std::format(__VA_ARGS__)); \
	} while (false)

#define NETLINK_LOG_DEBUG(...)	 NETLINK_LOG(Debug, __VA_ARGS__)
#define NETLINK_LOG_INFO(...)	 NETLINK_LOG(Info, __VA_ARGS__)
#define NETLINK_LOG_WARNING(...) NETLINK_LOG(Warning, __VA_ARGS__)
#define NETLINK_LOG_ERROR(...)	 NETLINK_LOG(Error, __VA_ARGS__)
