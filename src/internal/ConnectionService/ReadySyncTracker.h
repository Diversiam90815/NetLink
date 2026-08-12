/*
  ==============================================================================
	Module:         ReadySyncTracker
	Description:    Tracks local/remote "ready" flag synchronization
  ==============================================================================
*/


#pragma once

#include <atomic>


namespace netlink
{

class ReadySyncTracker
{
public:
	ReadySyncTracker()	= default;
	~ReadySyncTracker() = default;

	void reset()
	{
		mLocalReady.store(false);
		mRemoteReady.store(false);
	}

	void setLocalReady(bool ready = true) { mLocalReady.store(ready); }
	void setRemoteReady(bool ready = true) { mRemoteReady.store(ready); }

	bool isLocalReady() const { return mLocalReady.load(); }
	bool isRemoteReady() const { return mRemoteReady.load(); }
	bool bothReady() const { return mLocalReady.load() && mRemoteReady.load(); }

private:
	std::atomic<bool> mLocalReady{false};
	std::atomic<bool> mRemoteReady{false};
};

} // namespace netlink
