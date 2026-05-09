/*
  ==============================================================================
	Module:         ConnectionConfig
	Description:    Configuration and timeout constants for ConnectionService
  ==============================================================================
*/

#pragma once


namespace netlink
{

namespace ConnectionTimeouts
{
constexpr const char *Connection = "connection";
constexpr const char *Invitation = "invitation";
constexpr const char *ReadyFlag	 = "ready_flag";
} // namespace ConnectionTimeouts


struct ConnectionConfig
{
	const int invitationTimeoutMs{5000};
	const int connectionTimeoutMs{10000};
	const int readyFlagTimeoutMs{3000};
};

} // namespace netlink
