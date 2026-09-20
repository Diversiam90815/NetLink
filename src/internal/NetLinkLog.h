/*
  ==============================================================================
	Module:         NetLinkLog
	Description:    Internal logging abstraction
  ==============================================================================
*/

#pragma once

#ifdef NETLINK_LOGGING_ENABLED

#include "Logger.h"

#define NETLINK_LOG_DEBUG(...)	 LOG_DEBUG(__VA_ARGS__)
#define NETLINK_LOG_INFO(...)	 LOG_INFO(__VA_ARGS__)
#define NETLINK_LOG_WARNING(...) LOG_WARNING(__VA_ARGS__)
#define NETLINK_LOG_ERROR(...)	 LOG_ERROR(__VA_ARGS__)

#else

namespace netlink::internal
{
template <typename... Args>
int logArgumentsUnused(const Args &...);
} // namespace netlink::internal

#define NETLINK_LOG_DEBUG(...)	 ((void)sizeof(::netlink::internal::logArgumentsUnused(__VA_ARGS__)))
#define NETLINK_LOG_INFO(...)	 ((void)sizeof(::netlink::internal::logArgumentsUnused(__VA_ARGS__)))
#define NETLINK_LOG_WARNING(...) ((void)sizeof(::netlink::internal::logArgumentsUnused(__VA_ARGS__)))
#define NETLINK_LOG_ERROR(...)	 ((void)sizeof(::netlink::internal::logArgumentsUnused(__VA_ARGS__)))

#endif
