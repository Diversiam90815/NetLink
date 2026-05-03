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

	#define NETLINK_LOG_DEBUG(...)	 ((void)0)
	#define NETLINK_LOG_INFO(...)	 ((void)0)
	#define NETLINK_LOG_WARNING(...) ((void)0)
	#define NETLINK_LOG_ERROR(...)	 ((void)0)

#endif

