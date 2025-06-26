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

    sampleRate = in_rFormat.uSampleRate;

    if (in_pContext != nullptr)
    {
        objectID = in_pContext->GetAudioNodeID();
    }
    
    g_SharedBuffer->addToObjectList(objectID);
   
    return AK_Success;
}

AKRESULT AutoCompressorFX::Term(AK::IAkPluginMemAlloc* in_pAllocator)
{
    g_SharedBuffer->removeFromObjectList(objectID);
    g_SharedBuffer->removeFromPriorityList(priority);
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
    AkUInt32 frames10ms = static_cast<AkUInt32>(sampleRate / 100);
    AkReal32 thresholdDB = m_pParams->RTPC.fThreshold;      // unaffected by envelope     
    AkReal32 maxRatio = m_pParams->RTPC.fRatio;             // in 1:X, X is a float between 1 and 10
    priority = m_pParams->RTPC.fPriority;                   // a float between 1 and 10
    AkReal32 kneeDB = m_pParams->RTPC.fKnee;
    AkReal32 attack = max(epsilon, m_pParams->RTPC.fAttack);              // in seconds, between 0 and 6
    AkReal32 overshootA = static_cast<AkReal32>(max(epsilon, 0.3f));
    AkReal32 release = max(epsilon, m_pParams->RTPC.fRelease);             // in seconds, minimum of 0
    AkReal32 overshootR = static_cast<AkReal32>(max(epsilon, 0.01f));
    AkReal32 gainDB[2] = { 0.0f, 0.0f };
    AkReal32 peak_decay = static_cast<AkReal32>(3.0 / sampleRate);          // DB decreased every frame, positive (3 DB over 1 second)
    auto& oldSBRMS = g_SharedBuffer->lastbuffer_mRMS;
    auto& newSBRMS = g_SharedBuffer->newbuffer_mRMS;
    AkReal32 movingSBRMS[2]{ 0.0f, 0.0f };                                                  // the current mRMS of shared buffer, effectively the sidechain signal
    AkReal32 rmsDiff[2] = { g_SharedBuffer->diff_mRMS[0], g_SharedBuffer->diff_mRMS[1] };
    AkUInt16 refCount = static_cast<AkUInt16>(g_SharedBuffer->objectList.size());           // number of instances of this plugin

    g_SharedBuffer->addToPriorityList(priority);
    g_SharedBuffer->resizeSharedBuffer(io_pBuffer);
    g_SharedBuffer->addToSharedBuffer(io_pBuffer);
    g_SharedBuffer->numBuffersCalculated.fetch_add(1, std::memory_order_relaxed);

    // Calculate realRatio from Priority
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
            // Determine the RMS of sidechain signal (movingSBRMS), using data from the previous buffer tick.
            // Also makes the difference of RMS between buffers smoother
            {
                // Estimate current SBRMS (somewhere between oldSBRMS and newSBRMS, based on the % of progress through the total amount of frames in the buffer)
                
                movingSBRMS[i] = oldSBRMS[i] + ((frame / maxFrames) * (newSBRMS[i] - oldSBRMS[i]));
                

                // update new slope
                AkReal32 mySlope = newSBRMS[i] - oldSBRMS[i];
                if (abs(mySlope - rmsDiff[i]) < epsilon) // if difference is negligible, rmsDiff matches it
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

            AkReal32 inputDB = AK_LINTODB(movingSBRMS[i]);
            
            // Calculate myRMS
            myRMS[i] = sqrtf(
                (
                    (powf(myRMS[i], 2) * ((frames10ms * uNumChannels) - 1)    // a fake sum of previous frames' squares
                        ) + powf(pBuf[frame], 2)                                      // add square of new sample
                    ) / (frames10ms * uNumChannels)                                      // divide by frames to get new average of squares
            );

            // DSP section
            {
                // Entire formula: https://www.desmos.com/calculator/eu6xlluw9h
                // Calculate Compression DSP in DB
                auto& x = inputDB;        // X = the DB of the Shared Buffer signal
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

                // Apply Envelope
                // note: ADSR values are never 0, using epsilon as minimum
                // Formula found here: https://www.earlevel.com/main/2013/06/03/envelope-generators-adsr-code/
                {
                    env_target[i] = -1 * gainDB[i];
                    AkReal32 rate;
                    if (env_target[i] > env_output[i])
                    {
                        env_state = env_attack;
                    }
                    else if (env_state != env_idle)
                    {
                        env_state = env_release;
                    }

                    switch (env_state)
                    {
                    case env_idle:
                        break;
                    case env_attack:
                        rate = expf(-log((1 + overshootA) / overshootA) / (attack * sampleRate));
                        env_ratio[i] = static_cast<AkReal32>((env_ratio[i] * rate) + ((1.0 + overshootA) * (1.0 - rate)));
                        env_output[i] = env_ratio[i] * env_target[i];
                        env_outputPeak[i] = env_output[i];
                        if (env_ratio[i] >= 1.0)
                        {
                            env_ratio[i] = 1.0;
                            env_state = env_sustain;
                        }
                        break;
                    case env_sustain:
                        break;
                    case env_release:
                        rate = expf(-log((1 + overshootR) / overshootR) / (release * sampleRate));
                        env_ratio[i] = static_cast<AkReal32>((env_ratio[i] * rate) + ((-overshootR) * (1.0 - rate)));
                        env_output[i] = env_ratio[i] * env_outputPeak[i];
                        if (env_ratio[i] < 0.0)
                        {
                            env_ratio[i] = 0.0;
                            env_state = env_idle;
                        }
                    }
                    
                }
                
                // Find myRMS vs SBRMS %, in linear
                scPercent[i] = (movingSBRMS[i] == 0) ? 1.0f : std::clamp(myRMS[i] / movingSBRMS[i], 0.0f, 1.0f);
                

                // Execute DSP in linear
                mixOutput[i] = -env_output[i];
                pBuf[uFramesProcessed] *= std::clamp(AK_DBTOLIN(mixOutput[i]), 0.0f, 1.0f);
            }


            ++uFramesProcessed;
        }
    }
    
    // Monitor Data
#ifndef AK_OPTIMIZED
    if (m_pContext->CanPostMonitorData())
    {
        std::ostringstream reformat1, reformat2, reformat3, reformat4;
        reformat1 << std::fixed << std::setprecision(2) << g_SharedBuffer->newbuffer_mRMS[0];
        reformat2 << std::fixed << std::setprecision(2) << g_SharedBuffer->newbuffer_mRMS[1];
        reformat3 << std::fixed << std::setprecision(2) << env_output[0];
        reformat4 << std::fixed << std::setprecision(2) << env_output[1];
        std::stringstream sstream1, sstream2;
        sstream1 << reformat1.str() << ", " << reformat2.str();
        sstream2 << reformat3.str() << ", " << reformat4.str();
        std::string monitorData[2] = { sstream1.str(), sstream2.str() };
        m_pContext->PostMonitorData((void*)monitorData, sizeof(monitorData));

    }

#endif

    // Once all plugin instances have submitted calculations, reset/update them
    if (g_SharedBuffer->numBuffersCalculated >= refCount)
    {
        g_SharedBuffer->diff_mRMS[0] = rmsDiff[0];
        g_SharedBuffer->diff_mRMS[1] = rmsDiff[1];
        g_SharedBuffer->calculatemRMS(frames10ms);
        g_SharedBuffer->calculatePriorityMinMax();
        g_SharedBuffer->resetSharedBufferAndPriorityList();
        g_SharedBuffer->numBuffersCalculated.store(0, std::memory_order_relaxed);
    }
}

AKRESULT AutoCompressorFX::TimeSkip(AkUInt32 in_uFrames)
{
    return AK_DataReady;
}

