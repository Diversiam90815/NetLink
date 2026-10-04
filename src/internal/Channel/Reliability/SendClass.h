/*
  ==============================================================================
	Module:         SendClass
	Description:    The priority classes a link's outgoing data falls into
  ==============================================================================
*/

#pragma once

#include <cstddef>
#include <cstdint>


namespace netlink::channel
{

// The most urgent first. Acknowledgements and the heartbeat are not among them: they are never held back.
enum class SendClass : uint8_t
{
	Control,
	Unreliable,
	Application,
};

inline constexpr size_t SendClassCount = 3;

} // namespace netlink::channel
