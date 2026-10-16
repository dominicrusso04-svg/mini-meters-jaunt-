#include "PluginProcessor.h"
#include "PluginEditor.h"

NanoStripProcessor::NanoStripProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    beatScope.clear();
}

void NanoStripProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    loudness.prepare (sampleRate);
}

bool NanoStripProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

void NanoStripProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n   = buffer.getNumSamples();
    const int nIn = getTotalNumInputChannels();

    for (int ch = nIn; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, n);

    if (n == 0 || nIn == 0)
        return;

    // Audio passes through untouched; we only look at it.
    const float* L = buffer.getReadPointer (0);
    const float* R = nIn > 1 ? buffer.getReadPointer (1) : nullptr;

    bool playing = false, hasPpq = false;
    double ppq = 0.0, bpm = 120.0;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (auto p = pos->getPpqPosition()) { ppq = *p; hasPpq = true; }
            if (auto b = pos->getBpm()) bpm = *b;
        }
    }

    // Fresh integrated loudness each time playback starts
    if (playing && ! wasPlaying)
        loudness.requestReset();
    wasPlaying = playing;

    loudness.process (L, R, n);
    beatScope.process (L, R, n, playing && hasPpq, ppq, bpm, getSampleRate());

    if (editorOpen.load (std::memory_order_relaxed))
        fifo.push (L, R != nullptr ? R : L, n);
}

juce::AudioProcessorEditor* NanoStripProcessor::createEditor()
{
    return new NanoStripEditor (*this);
}

void NanoStripProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = uiState.createXml())
        copyXmlToBinary (*xml, destData);
}

void NanoStripProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        auto vt = juce::ValueTree::fromXml (*xml);
        if (vt.isValid() && vt.hasType (uiState.getType()))
            uiState.copyPropertiesFrom (vt, nullptr);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NanoStripProcessor();
}
