/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2018
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

#pragma once


namespace tracktion_engine
{

ModifierNode::ModifierNode (std::unique_ptr<Node> inputNode,
                            tracktion_engine::Modifier::Ptr modifierToProcess,
                            double sampleRateToUse, int blockSizeToUse,
                            const TrackMuteState* trackMuteStateToUse,
                            tracktion_graph::PlayHeadState& playHeadStateToUse, bool rendering)
    : input (std::move (inputNode)),
      modifier (std::move (modifierToProcess)),
      trackMuteState (trackMuteStateToUse),
      playHeadState (&playHeadStateToUse),
      isRendering (rendering)
{
    jassert (input != nullptr);
    jassert (modifier != nullptr);
    initialiseModifier (sampleRateToUse, blockSizeToUse);
}

ModifierNode::ModifierNode (std::unique_ptr<Node> inputNode,
                            tracktion_engine::Modifier::Ptr modifierToProcess,
                            double sampleRateToUse, int blockSizeToUse,
                            std::shared_ptr<InputProvider> contextProvider)
    : input (std::move (inputNode)),
      modifier (std::move (modifierToProcess)),
      audioRenderContextProvider (std::move (contextProvider))
{
    jassert (input != nullptr);
    jassert (modifier != nullptr);
    initialiseModifier (sampleRateToUse, blockSizeToUse);
}

ModifierNode::~ModifierNode()
{
    if (isInitialised && ! modifier->baseClassNeedsInitialising())
        modifier->baseClassDeinitialise();
}

//==============================================================================
tracktion_graph::NodeProperties ModifierNode::getNodeProperties()
{
    auto props = input->getNodeProperties();

    props.numberOfChannels = juce::jmax (props.numberOfChannels, modifier->getAudioInputNames().size());
    props.hasAudio = props.hasAudio || modifier->getAudioInputNames().size() > 0;
    props.hasMidi  = props.hasMidi || modifier->getMidiInputNames().size() > 0;
    props.nodeID = (size_t) modifier->itemID.getRawID();

    return props;
}

void ModifierNode::prepareToPlay (const tracktion_graph::PlaybackInitialisationInfo& info)
{
    juce::ignoreUnused (info);
    jassert (sampleRate == info.sampleRate);
    
    auto props = getNodeProperties();

    if (props.latencyNumSamples > 0)
        automationAdjustmentTime = -tracktion_graph::sampleToTime (props.latencyNumSamples, sampleRate);
}

void ModifierNode::process (ProcessContext& pc)
{
    auto inputBuffers = input->getProcessedOutput();
    auto& inputAudioBlock = inputBuffers.audio;
    
    auto& outputBuffers = pc.buffers;
    auto& outputAudioBlock = outputBuffers.audio;

    // Copy the inputs to the outputs, then process using the
    // output buffers as that will be the correct size
    {
        const auto numInputChannelsToCopy = std::min (inputAudioBlock.getNumChannels(), outputAudioBlock.getNumChannels());
        
        if (numInputChannelsToCopy > 0)
        {
            jassert (inputAudioBlock.getNumFrames() == outputAudioBlock.getNumFrames());
            choc::buffer::copy (outputAudioBlock, inputAudioBlock.getChannelRange ({ 0, numInputChannelsToCopy }));
        }
    }

    // Setup audio buffers
    auto outputAudioBuffer = tracktion_graph::createAudioBuffer (outputAudioBlock);

    // Then MIDI buffers
    midiMessageArray.copyFrom (inputBuffers.midi);
    bool shouldProcess = getBoolParamValue (*modifier->enabledParam);
    
    if (playHeadState != nullptr && playHeadState->didPlayheadJump())
        midiMessageArray.isAllNotesOff = true;
    
    if (trackMuteState != nullptr)
    {
        if (! trackMuteState->shouldTrackContentsBeProcessed())
        {
            shouldProcess = shouldProcess && trackMuteState->shouldTrackBeAudible();
        
            if (trackMuteState->wasJustMuted())
                midiMessageArray.isAllNotesOff = true;
        }
    }

    // Process the plugin
    if (shouldProcess)
        modifier->applyToBuffer (getPluginRenderContext (pc.referenceSampleRange.getStart(), outputAudioBuffer));
    
    // Then copy the buffers to the outputs
    outputBuffers.midi.copyFrom (midiMessageArray);
}

//==============================================================================
void ModifierNode::initialiseModifier (double sampleRateToUse, int blockSizeToUse)
{
    sampleRate = sampleRateToUse;
    modifier->baseClassInitialise (sampleRate, blockSizeToUse);
    isInitialised = true;
}

PluginRenderContext ModifierNode::getPluginRenderContext (int64_t referenceSamplePosition, juce::AudioBuffer<float>& destBuffer)
{
    if (audioRenderContextProvider != nullptr)
    {
        tracktion_engine::PluginRenderContext rc (audioRenderContextProvider->getContext());
        rc.destBuffer = &destBuffer;
        rc.bufferStartSample = 0;
        rc.bufferNumSamples = destBuffer.getNumSamples();
        rc.bufferForMidiMessages = &midiMessageArray;
        rc.midiBufferOffset = 0.0;
        
        return rc;
    }

    jassert (playHeadState != nullptr);
    auto& playHead = playHeadState->playHead;
    
    return { &destBuffer,
             juce::AudioChannelSet::canonicalChannelSet (destBuffer.getNumChannels()),
             0, destBuffer.getNumSamples(),
             &midiMessageArray, 0.0,
             tracktion_graph::sampleToTime (playHead.referenceSamplePositionToTimelinePosition (referenceSamplePosition), sampleRate) + automationAdjustmentTime,
             playHead.isPlaying(), playHead.isUserDragging(), isRendering, false };
}

}
