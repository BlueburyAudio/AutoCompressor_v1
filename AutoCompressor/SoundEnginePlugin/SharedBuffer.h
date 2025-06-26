#pragma once

#include <vector>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <string>
#include <sstream>
#include <shared_mutex>
#include <AK/SoundEngine/Common/AkCommonDefs.h>

class SharedBuffer
{
public:

	std::vector<AkUniqueID> objectList;
	std::vector<std::vector<AkReal32>> sharedBuffer;		// 2-D array imitating a buffer's channels (outer vector) and frames (inner vector)
	std::atomic<AkInt16> numBuffersCalculated = 0;
	std::vector<AkReal32> priorityList;						// used for calculating min and max Priority, of previous buffer
	//std::atomic<AkInt16> signalCounter = 0;

	AkReal32 minPriority = 1.0f;							// Current minimum of Priority ranks
	AkReal32 maxPriority = 1.0f;							// Current maximum of Priority ranks
	
	AkReal32 lastbuffer_mRMS[2] = { 0.0f, 0.0f };			// The moving RMS of the last L and R samples of the previous buffer
	AkReal32 newbuffer_mRMS[2] = { 0.0f, 0.0f };
	AkReal32 diff_mRMS[2] = { 0.0f, 0.0f };					// the "slope" of the RMS of the previous buffer
	AkReal32 lastRMSAdded[2] = { 0.1f, 0.1f };				// for debugging?
	

	std::string errorMsg = "Default Error Message";
	
	void resetSharedBufferAndPriorityList();				// resets everything, including numBuffersCalculated
	void resizeSharedBuffer(AkAudioBuffer* sourceBuffer);
	void addToSharedBuffer(AkAudioBuffer* sourceBuffer);
	void calculatemRMS(AkUInt32 frames10ms);				// in linear.  applies calcs to newbuffer_mRMS
	void addToPriorityList(AkReal32 priority);
	void calculatePriorityMinMax();							// applies calcs to minPriority and maxPriority
	float getRatioPercentile(AkReal32 priority) const;		// returns new Ratio based on minPrio and maxPrio, a percentile in decimal form
	void addToObjectList(AkUniqueID objectID);
	void removeFromObjectList(AkUniqueID objectID);
	void removeFromPriorityList(AkReal32 priority);

private:
	std::mutex mtx;
	std::shared_mutex smtx;
};

class GlobalManager
{
public:
	static std::shared_ptr<SharedBuffer> getGlobalSharedBuffer()
	{
		static std::shared_ptr<SharedBuffer> globalSharedBuffer = std::make_shared<SharedBuffer>();
		return globalSharedBuffer;
	}
};