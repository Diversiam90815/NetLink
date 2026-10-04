/*
  ==============================================================================
	Module:         NetLinkLog
	Description:    Internal logging: lines go to the sink of the thread that
					writes them, which hands them to the application's onLog
  ==============================================================================
*/

#include "NetLinkLog.h"

#include <utility>


namespace
{

thread_local netlink::internal::LogSink threadSink;

} // namespace


void netlink::internal::setThreadLogSink(LogSink sink)
{
	threadSink = std::move(sink);
}


bool netlink::internal::hasThreadLogSink()
{
	return static_cast<bool>(threadSink);
}


void netlink::internal::writeLog(const LogLevel level, std::string &&text)
{
	if (threadSink)
		threadSink(level, std::move(text));
}
