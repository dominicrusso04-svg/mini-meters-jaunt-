#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Analysis.h"

class NanoStripProcessor : public juce::AudioProcessor
{
public:
    NanoStripProcessor();
    ~NanoStripProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Shared with the editor
    ns::SampleFifo fifo { 1 << 17 };
    std::atomic<bool> editorOpen { false };
    std::atomic<double> currentSampleRate { 44100.0 };
    ns::LoudnessMeter loudness;
    ns::BeatScope beatScope;

    // UI settings saved with the project (panels, colours, window size...)
    juce::ValueTree uiState { "NanoStripUI" };

private:
    bool wasPlaying = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NanoStripProcessor)
};
