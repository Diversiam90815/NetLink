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
	int invitationTimeoutMs{5000};
	int connectionTimeoutMs{10000};
	int readyFlagTimeoutMs{3000};
};

} // namespace netlink
