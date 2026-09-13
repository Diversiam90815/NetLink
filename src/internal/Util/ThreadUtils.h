/*
  ==============================================================================
	Module:         ThreadUtils
	Description:    Small helpers for owning worker threads
  ==============================================================================
*/

#pragma once

#include <thread>


// Joins the thread, or detaches it when called from that very thread
inline void joinOrDetach(std::thread &thread)
{
	if (!thread.joinable())
		return;

	if (thread.get_id() == std::this_thread::get_id())
		thread.detach();
	else
		thread.join();
}
