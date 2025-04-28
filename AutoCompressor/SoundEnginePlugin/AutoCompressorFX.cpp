/*******************************************************************************
The content of this file includes portions of the AUDIOKINETIC Wwise Technology
released in source code form as part of the SDK installer package.

Commercial License Usage

Licensees holding valid commercial licenses to the AUDIOKINETIC Wwise Technology
may use this file in accordance with the end user license agreement provided
with the software or, alternatively, in accordance with the terms contained in a
written agreement between you and Audiokinetic Inc.

Apache License Usage

Alternatively, this file may be used under the Apache License, Version 2.0 (the
"Apache License"); you may not use this file except in compliance with the
Apache License. You may obtain a copy of the Apache License at
http://www.apache.org/licenses/LICENSE-2.0.

Unless required by applicable law or agreed to in writing, software distributed
under the Apache License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES
OR CONDITIONS OF ANY KIND, either express or implied. See the Apache License for
the specific language governing permissions and limitations under the License.

  Copyright (c) 2025 Audiokinetic Inc.
*******************************************************************************/

#include "AutoCompressorFX.h"
#include "../AutoCompressorConfig.h"

#include <AK/AkWwiseSDKVersion.h>

AK::IAkPlugin* CreateAutoCompressorFX(AK::IAkPluginMemAlloc* in_pAllocator)
{
    return AK_PLUGIN_NEW(in_pAllocator, AutoCompressorFX());
}

AK::IAkPluginParam* CreateAutoCompressorFXParams(AK::IAkPluginMemAlloc* in_pAllocator)
{
    return AK_PLUGIN_NEW(in_pAllocator, AutoCompressorFXParams());
}

AK_IMPLEMENT_PLUGIN_FACTORY(AutoCompressorFX, AkPluginTypeEffect, AutoCompressorConfig::CompanyID, AutoCompressorConfig::PluginID)

AutoCompressorFX::AutoCompressorFX()
    : m_pParams(nullptr)
    , m_pAllocator(nullptr)
    , m_pContext(nullptr)
{
}

AutoCompressorFX::~AutoCompressorFX()
{
}

AKRESULT AutoCompressorFX::Init(AK::IAkPluginMemAlloc* in_pAllocator, AK::IAkEffectPluginContext* in_pContext, AK::IAkPluginParam* in_pParams, AkAudioFormat& in_rFormat)
{
    m_pParams = (AutoCompressorFXParams*)in_pParams;
    m_pAllocator = in_pAllocator;
    m_pContext = in_pContext;
    objectID = in_pContext->GetAudioNodeID();
    sampleRate = in_rFormat.uSampleRate;

    g_SharedBuffer->addToObjectList(objectID);

    return AK_Success;
}

AKRESULT AutoCompressorFX::Term(AK::IAkPluginMemAlloc* in_pAllocator)
{
    g_SharedBuffer->removeFromObjectList(objectID);
    AK_PLUGIN_DELETE(in_pAllocator, this);
    return AK_Success;
}

AKRESULT AutoCompressorFX::Reset()
{
    return AK_Success;
}

AKRESULT AutoCompressorFX::GetPluginInfo(AkPluginInfo& out_rPluginInfo)
{
    out_rPluginInfo.eType = AkPluginTypeEffect;
    out_rPluginInfo.bIsInPlace = true;
	out_rPluginInfo.bCanProcessObjects = false;
    out_rPluginInfo.uBuildVersion = AK_WWISESDK_VERSION_COMBINED;
    return AK_Success;
}

#define AK_LINTODB( __lin__ ) (log10f(__lin__) * 20.f)

void AutoCompressorFX::Execute(AkAudioBuffer* io_pBuffer)
{
    const AkUInt32 uNumChannels = io_pBuffer->NumChannels();
    AkReal32 RMSsum[2] = { 0.0f, 0.0f };                        
    AkReal32 thresholdDB = m_pParams->RTPC.fThreshold;            
    AkReal32 maxRatio = m_pParams->RTPC.fRatio;
    AkReal32 priority = m_pParams->RTPC.fPriority;
    AkReal32 kneeDB = 1.0f;                      
    AkReal32 gainDB[2] = { 0.0f, 0.0f };
    auto& oldSBRMS = g_SharedBuffer->lastbuffer_mRMS;
    auto& newSBRMS = g_SharedBuffer->newbuffer_mRMS;
    AkReal32 movingSBRMS[2]{ 0.0f, 0.0f };                                              // a number representing the current mRMS of shared buffer
    AkReal32 rmsDiff[2] = { g_SharedBuffer->diff_mRMS[0], g_SharedBuffer->diff_mRMS[1] };
    AkUInt16 refCount = static_cast<AkUInt16>(g_SharedBuffer.use_count() - 1);          // number of instances of this plugin

    
    g_SharedBuffer->addToPriorityList(priority);
    g_SharedBuffer->resizeSharedBuffer(io_pBuffer);
    g_SharedBuffer->addToSharedBuffer(io_pBuffer);
    g_SharedBuffer->numBuffersCalculated.fetch_add(1, std::memory_order_relaxed);

    // Calculate trueRatio from Priority
    auto& prioList = g_SharedBuffer->priorityList;
    auto& maxPrio = g_SharedBuffer->maxPriority;
    auto& minPrio = g_SharedBuffer->minPriority;
    AkReal32 percentile = static_cast<AkReal32>(g_SharedBuffer->getRatioPercentile(priority));
    AkReal32 realRatio = (percentile * (maxRatio - 1)) + 1;


    AkUInt16 uFramesProcessed;
    for (AkUInt32 i = 0; i < uNumChannels; ++i)
    {
        AkReal32* AK_RESTRICT pBuf = (AkReal32* AK_RESTRICT)io_pBuffer->GetChannel(i);

        uFramesProcessed = 0;
        auto& frame = uFramesProcessed;
        const auto& maxFrames = io_pBuffer->uValidFrames;

        while (uFramesProcessed < io_pBuffer->uValidFrames)
        {
            // Determine the RMS the compression is based on, using data from the previous buffer tick.
            // Also makes the difference of RMS between buffers smoother
            {
                // Estimate current SBRMS (somewhere between oldSBRMS and newSBRMS, based on the % of progress through the total amount of frames in the buffer)
                
                movingSBRMS[i] = oldSBRMS[i] + ((frame / maxFrames) * (newSBRMS[i] - oldSBRMS[i]));
                

                // update new slope
                AkReal32 mySlope = newSBRMS[i] - oldSBRMS[i];
                if (abs(mySlope - rmsDiff[i]) < 0.0001) // if difference is negligible, rmsDiff matches it
                {
                    rmsDiff[i] = mySlope;
                }
                else //shift rmsSlope toward the next RMS, at 50% strength
                {
                    rmsDiff[i] += static_cast<AkReal32>((mySlope - rmsDiff[i]) * (0.5));
                }

                // Update current RMS to follow rmsDiff/slope
                movingSBRMS[i] += (rmsDiff[i] / maxFrames);
            }
            
      
            // TODO: Apply anti-aliasing to Ratio 

            // TOFIX: DSP section
            {
                // Entire formula: https://www.desmos.com/calculator/eu6xlluw9h
                // Calculate Compression DSP in DB
                AkReal32 x = AK_LINTODB(movingSBRMS[i]);        // X = the DB of the Shared Buffer signal
                AkReal32 y = x;                                 // base case, e.g. when no compression is needed, when shared buffer is below threshold and knee.

                if (x > thresholdDB + (kneeDB / 2))             // if shared buffer exceeds both threshold and knee
                {
                    y = ((x - thresholdDB) / realRatio) + thresholdDB;
                }
                else if (x > thresholdDB - (kneeDB / 2))        // if shared buffer is above the lower bound of knee
                {
                    AkReal32 m = ((1 / realRatio) - 1) / (2 * kneeDB);
                    y = x + (m * powf(x - (thresholdDB - (kneeDB / 2)), 2));
                }
                
                gainDB[i] = y - x;

                // TODO: apply %mix depending on myRMS vs SBRMS

                // Execute DSP in linear
                pBuf[uFramesProcessed] *= AK_DBTOLIN(gainDB[i]);
            }

#ifndef AK_OPTIMIZED
            if (m_pContext->CanPostMonitorData())
            {
                if (i < 2)
                {
                    RMSsum[i] += powf(pBuf[uFramesProcessed], 2);
                }
            }
#endif

            ++uFramesProcessed;
        }
    }
    
    // Once all plugin instances have submitted calculations, reset/update them
    if (g_SharedBuffer->numBuffersCalculated >= refCount)
    {
        g_SharedBuffer->diff_mRMS[0] = rmsDiff[0];
        g_SharedBuffer->diff_mRMS[1] = rmsDiff[1];
        g_SharedBuffer->calculatemRMS(sampleRate / 100);
        g_SharedBuffer->calculatePriorityMinMax();
        g_SharedBuffer->resetSharedBufferAndPriorityList();
        g_SharedBuffer->numBuffersCalculated.store(0, std::memory_order_relaxed);
    }


#ifndef AK_OPTIMIZED
    if (m_pContext->CanPostMonitorData())
    {
        myRMS[0] = AK_LINTODB(sqrtf(RMSsum[0]/io_pBuffer->uValidFrames));
        myRMS[1] = AK_LINTODB(sqrtf(RMSsum[1] / io_pBuffer->uValidFrames));
        std::ostringstream reformat1, reformat2;
        reformat1 << std::fixed << std::setprecision(2) << AK_LINTODB(movingSBRMS[1]);
        reformat2 << std::fixed << std::setprecision(2) << gainDB[1];
        std::stringstream sstream1;
        sstream1 << reformat1.str() << ", " << reformat2.str();

        std::string monitorData[1] = { sstream1.str() };
        m_pContext->PostMonitorData((void*)monitorData, sizeof(monitorData));

    }

#endif
}

AKRESULT AutoCompressorFX::TimeSkip(AkUInt32 in_uFrames)
{
    return AK_DataReady;
}

