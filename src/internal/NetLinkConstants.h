/*
  ==============================================================================
	Module:         NetLinkConstants
	Description:    Constants defined for the NetLink lib
  ==============================================================================
*/

#pragma once

namespace netlink::internal
{

inline constexpr size_t		 PackageBufferSize = 65536; // 64 KB default
inline constexpr const char *DefaultSecret	   = "NETLINK";

} // namespace netlink::internal
