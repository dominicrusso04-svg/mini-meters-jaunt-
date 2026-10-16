#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "Panels.h"

// Small pill-shaped toggle used in the top bar
class Chip : public juce::Component
{
public:
    Chip (juce::String t) : text (std::move (t)) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    juce::String text;
    bool on = true;
    bool isSwatch = false;
    juce::Colour colour { ns::theme::accent (0) };
    std::function<void()> onClick;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        if (isSwatch)
        {
            const float d = std::min (r.getWidth(), r.getHeight()) - 6.0f;
            g.setColour (colour);
            g.fillEllipse (r.withSizeKeepingCentre (d, d));
            return;
        }
        g.setColour (on ? colour.withAlpha (0.16f) : ns::theme::panel);
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (on ? colour.withAlpha (0.6f) : ns::theme::gridHi);
        g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);
        g.setColour (on ? colour : ns::theme::text);
        g.setFont (ns::theme::label (9.0f));
        g.drawText (text, getLocalBounds(), juce::Justification::centred, false);
    }

    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
};

class NanoStripEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit NanoStripEditor (NanoStripProcessor&);
    ~NanoStripEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void applyAccent();
    void saveSettings();

    NanoStripProcessor& proc;

    ns::WaveformPanel wave;
    ns::SpectrumPanel spectrum;
    ns::StereoPanel stereo;
    ns::LoudnessPanel loud;
    ns::ScopePanel scope;
    std::array<ns::Panel*, 5> panels;
    std::array<bool, 5> visible { true, true, true, true, true };

    std::array<std::unique_ptr<Chip>, 5> panelChips;
    Chip accentChip { "" };
    Chip fpsChip { "60 FPS" };
    int fps = 60;
    void setFps (int newFps);
    int accentIndex = 0;

    std::vector<float> bufL, bufR;
    double preparedRate = 0.0;
    bool restoring = false;
    int silentFrames = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NanoStripEditor)
};
