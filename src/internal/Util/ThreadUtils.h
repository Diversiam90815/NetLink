/*
  ==============================================================================
	Module:         ThreadUtils
	Description:    Small helpers for owning worker threads
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <thread>


// Joins the thread, or detaches it when called from that very thread
template <typename Thread>
void joinOrDetach(Thread &thread)
{
	if (!thread.joinable())
		return;

	if (thread.get_id() == std::this_thread::get_id())
		thread.detach();
	else
		thread.join();
}


namespace netlink::internal
{

// Threads the library started since the process began: every place that starts one counts it
inline std::atomic<int> threadsStarted{0};

} // namespace netlink::internal
