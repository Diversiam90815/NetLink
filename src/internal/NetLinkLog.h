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

#elif defined(NETLINK_DEBUG_STDERR)
#include <format>
#include <cstdio>
#include <thread>
	#define NETLINK_LOG_DEBUG(...)	 std::fprintf(stderr, "%s
", std::format(__VA_ARGS__).c_str())
	#define NETLINK_LOG_INFO(...)	 std::fprintf(stderr, "%s
", std::format(__VA_ARGS__).c_str())
	#define NETLINK_LOG_WARNING(...) std::fprintf(stderr, "W %s
", std::format(__VA_ARGS__).c_str())
	#define NETLINK_LOG_ERROR(...)	 std::fprintf(stderr, "E %s
", std::format(__VA_ARGS__).c_str())
#else

	#define NETLINK_LOG_DEBUG(...)	 ((void)0)
	#define NETLINK_LOG_INFO(...)	 ((void)0)
	#define NETLINK_LOG_WARNING(...) ((void)0)
	#define NETLINK_LOG_ERROR(...)	 ((void)0)

#endif
