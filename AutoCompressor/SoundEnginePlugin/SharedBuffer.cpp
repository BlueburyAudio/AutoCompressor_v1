#include "SharedBuffer.h"

#define refCount static_cast<int>(GlobalManager::getGlobalSharedBuffer().use_count() - 2)		// number of instances of this plugin

void SharedBuffer::addToObjectList(AkUniqueID objectID)
{
	std::lock_guard<std::mutex> lock(mtx);
	auto iterator = std::find(objectList.begin(), objectList.end(), objectID);
	if (iterator == objectList.end())
	{
		objectList.push_back(objectID);
	}
	
}

void SharedBuffer::removeFromObjectList(AkUniqueID objectID)
{
	std::lock_guard<std::mutex> lock(mtx);
	auto iterator = std::find(objectList.begin(), objectList.end(), objectID);
	if (iterator != objectList.end())
	{
		objectList.erase(iterator);
	}
}

void SharedBuffer::resetSharedBufferAndPriorityList()
{
	std::lock_guard<std::mutex> lock(mtx);
	{
		sharedBuffer.clear();
		priorityList.clear();
	}
}

void SharedBuffer::resizeSharedBuffer(AkAudioBuffer* sourceBuffer)
{
	std::lock_guard<std::mutex> lock(mtx);
	if (sourceBuffer->NumChannels() != sharedBuffer.size() || sourceBuffer->uValidFrames != sharedBuffer[0].size())
	{
		sharedBuffer.resize(sourceBuffer->NumChannels(), std::vector<AkReal32>(sourceBuffer->uValidFrames));
	}
}

void SharedBuffer::addToSharedBuffer(AkAudioBuffer* sourceBuffer)
{
	std::lock_guard<std::mutex> lock(mtx);
	AkUInt16 numChannels = static_cast<AkUInt16>(sharedBuffer.size());
	AkUInt32 numFrames = static_cast<AkUInt32>(sharedBuffer[0].size());


	for (AkUInt32 channel = 0; channel < numChannels; channel++)
	{
		AkUInt32 frame = 0;
		numFrames = static_cast<AkUInt32>(AkMin(sharedBuffer[channel].size(), sourceBuffer->uValidFrames));
		auto& thisChannel = sharedBuffer[channel];
		AkReal32* AK_RESTRICT sourceChannel = (AkReal32 * AK_RESTRICT)sourceBuffer->GetChannel(channel);
		while (frame < numFrames)
		{
			AkReal32& thisSample = thisChannel[frame];

			thisSample += sourceBuffer->GetChannel(channel)[frame];

			frame++;
		}
	}

	// errorMsg = std::to_string(refCount);

}

void SharedBuffer::calculatemRMS(AkUInt32 frames10ms)
{
	std::lock_guard<std::mutex> lock(mtx);
	std::vector<AkReal32> currentRMS = { newbuffer_mRMS[0], newbuffer_mRMS[1] };
	AkUInt16 numChannels = static_cast<AkUInt16>(sharedBuffer.size());
	AkUInt32 numFrames = static_cast<AkUInt32>(sharedBuffer[0].size());

	// update lastbuffer_mRMS
	lastbuffer_mRMS[0] = newbuffer_mRMS[0];
	lastbuffer_mRMS[1] = newbuffer_mRMS[1];


	// calculated new mRMS
	if (!sharedBuffer.empty())
	{
		for (AkUInt16 channel = 0; channel < numChannels; ++channel)
		{
			AkUInt16 frame = 0;
			std::vector<AkReal32>& currentChannel = sharedBuffer[channel];
			numFrames = static_cast<AkUInt32>(currentChannel.size());

			while (frame < numFrames)
			{

				AkReal32& currentSample = currentChannel[frame];
				currentRMS[channel] = sqrtf(
					(
						(powf(currentRMS[channel], 2) * ((frames10ms * numChannels) - 1)    // a fake sum of previous frames' squares
							) + powf(currentSample, 2)                                      // add square of new sample
						) / (frames10ms * numChannels)                                      // divide by frames to get new average of squares
				);                                                                          // square root everything

				frame++;
			}
		}
	}

	// update newbuffer_mRMS
	newbuffer_mRMS[0] = currentRMS[0];
	newbuffer_mRMS[1] = currentRMS[1];
}

void SharedBuffer::addToPriorityList(AkReal32 priority)
{
	std::lock_guard<std::mutex> lock(mtx);
	priorityList.push_back(priority);
}

void SharedBuffer::calculatePriorityMinMax()
{
	std::lock_guard<std::mutex> lock(mtx);
	using pairtype = std::pair<AkUniqueID, AkReal32>;

	if (!priorityList.empty())
	{
		// Find Minimum
		auto iterator = std::min_element(priorityList.begin(), priorityList.end());
		minPriority = *iterator;

		// Find Maximum
		iterator = std::max_element(priorityList.begin(), priorityList.end());
		maxPriority = *iterator;
	}
	else {
		minPriority = 1.0f;
		maxPriority = 1.0f;
	}
}

float SharedBuffer::getRatioPercentile(AkReal32 ratio) const
{

	if (minPriority == maxPriority)
	{
		return 1 - static_cast<float>(1 / refCount);
	}

	return 1 - static_cast<float>((ratio - minPriority) / (maxPriority - minPriority));
}
