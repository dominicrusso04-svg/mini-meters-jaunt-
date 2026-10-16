#include "PluginEditor.h"

namespace
{
    const char* panelNames[] = { "WAVE", "SPEC", "STEREO", "LOUD", "SCOPE" };
    constexpr int defaultW = 1080, defaultH = 170;
}

NanoStripEditor::NanoStripEditor (NanoStripProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      loud (p.loudness), scope (p.beatScope),
      panels { &wave, &spectrum, &stereo, &loud, &scope }
{
    restoring = true;
    auto& st = proc.uiState;

    const int mask = st.getProperty ("panels", 31);
    for (int i = 0; i < 5; ++i)
        visible[(size_t) i] = (mask >> i) & 1;

    accentIndex = juce::jlimit (0, ns::theme::numAccents - 1, (int) st.getProperty ("accent", 0));
    wave.setSpeed (st.getProperty ("waveSpeed", 1));
    wave.setMode (st.getProperty ("waveMode", 0));
    wave.setColours (st.getProperty ("waveColA", 1), st.getProperty ("waveColB", 0));
    spectrum.showPeak = st.getProperty ("specPeak", true);
    scope.setZoom (st.getProperty ("scopeZoom", 2));

    for (auto* panel : panels)
        addChildComponent (panel);

    wave.onSettingChanged = spectrum.onSettingChanged = scope.onSettingChanged = [this] { saveSettings(); };

    for (int i = 0; i < 5; ++i)
    {
        auto chip = std::make_unique<Chip> (panelNames[i]);
        chip->on = visible[(size_t) i];
        chip->onClick = [this, i]
        {
            visible[(size_t) i] = ! visible[(size_t) i];
            // keep at least one panel showing
            if (std::none_of (visible.begin(), visible.end(), [] (bool b) { return b; }))
                visible[(size_t) i] = true;
            panelChips[(size_t) i]->on = visible[(size_t) i];
            panelChips[(size_t) i]->repaint();
            resized();
            saveSettings();
        };
        addAndMakeVisible (*chip);
        panelChips[(size_t) i] = std::move (chip);
    }

    accentChip.isSwatch = true;
    accentChip.onClick = [this]
    {
        accentIndex = (accentIndex + 1) % ns::theme::numAccents;
        applyAccent();
        saveSettings();
    };
    addAndMakeVisible (accentChip);

    fpsChip.on = false;
    fpsChip.onClick = [this] { setFps (fps == 60 ? 30 : 60); saveSettings(); };
    addAndMakeVisible (fpsChip);
    applyAccent();

    bufL.resize (1 << 17);
    bufR.resize (1 << 17);

    // throw away anything that piled up while the window was closed
    while (proc.fifo.pull (bufL.data(), bufR.data(), (int) bufL.size()) > 0) {}
    proc.editorOpen = true;

    setResizable (true, true);
    setResizeLimits (360, 90, 3200, 900);
    setSize (st.getProperty ("w", defaultW), st.getProperty ("h", defaultH));
    restoring = false;

    setFps (st.getProperty ("fps", 60));
}

void NanoStripEditor::setFps (int newFps)
{
    fps = newFps == 30 ? 30 : 60;
    for (auto* panel : panels) panel->setFrameRate (fps);
    fpsChip.text = juce::String (fps) + " FPS";
    fpsChip.repaint();
    startTimerHz (fps);
}

NanoStripEditor::~NanoStripEditor()
{
    proc.editorOpen = false;
    stopTimer();
}

void NanoStripEditor::applyAccent()
{
    const auto c = ns::theme::accent (accentIndex);
    for (auto* panel : panels) panel->setAccent (c);
    for (auto& chip : panelChips) { chip->colour = c; chip->repaint(); }
    accentChip.colour = c;
    accentChip.repaint();
}

void NanoStripEditor::saveSettings()
{
    if (restoring) return;
    auto& st = proc.uiState;
    int mask = 0;
    for (int i = 0; i < 5; ++i) if (visible[(size_t) i]) mask |= 1 << i;
    st.setProperty ("panels", mask, nullptr);
    st.setProperty ("accent", accentIndex, nullptr);
    st.setProperty ("waveSpeed", wave.speed, nullptr);
    st.setProperty ("waveMode", wave.mode, nullptr);
    st.setProperty ("waveColA", wave.colA, nullptr);
    st.setProperty ("waveColB", wave.colB, nullptr);
    st.setProperty ("specPeak", spectrum.showPeak, nullptr);
    st.setProperty ("scopeZoom", scope.zoom, nullptr);
    st.setProperty ("fps", fps, nullptr);
    st.setProperty ("w", getWidth(), nullptr);
    st.setProperty ("h", getHeight(), nullptr);
}

void NanoStripEditor::paint (juce::Graphics& g)
{
    g.fillAll (ns::theme::bg);
    g.setFont (ns::theme::label (10.5f));
    g.setColour (ns::theme::textHi);
    g.drawText ("NANO", 10, 6, 40, 16, juce::Justification::centredLeft, false);
    g.setColour (ns::theme::accent (accentIndex));
    g.drawText ("STRIP", 41, 6, 50, 16, juce::Justification::centredLeft, false);
}

void NanoStripEditor::resized()
{
    auto area = getLocalBounds().reduced (6);
    auto top = area.removeFromTop (18);
    area.removeFromTop (5);

    // top bar: wordmark | panel chips | colour swatch
    top.removeFromLeft (84);
    accentChip.setBounds (top.removeFromRight (18));
    top.removeFromRight (6);
    fpsChip.setBounds (top.removeFromRight (52));
    top.removeFromRight (4);
    for (auto& chip : panelChips)
    {
        const int w = chip->text.length() * 7 + 16;
        chip->setBounds (top.removeFromLeft (w));
        top.removeFromLeft (4);
    }

    // panel layout: stereo stays square, loudness has a fixed width,
    // the rest share the remaining space by weight
    const int h = area.getHeight();
    const int gap = 5;
    const float weights[] = { 2.2f, 2.0f, 0.0f, 0.0f, 1.5f };
    int fixedW[] = { 0, 0, juce::jlimit (90, 260, h), juce::jlimit (140, 210, (int) (h * 1.15f)), 0 };

    int count = 0, fixedTotal = 0;
    float weightTotal = 0.0f;
    for (int i = 0; i < 5; ++i)
    {
        if (! visible[(size_t) i]) continue;
        ++count;
        fixedTotal += fixedW[i];
        weightTotal += weights[i];
    }

    const int free = std::max (0, area.getWidth() - fixedTotal - gap * (count - 1));
    int x = area.getX();
    int placed = 0;

    for (int i = 0; i < 5; ++i)
    {
        auto* panel = panels[(size_t) i];
        panel->setVisible (visible[(size_t) i]);
        if (! visible[(size_t) i]) continue;

        int w;
        if (weightTotal > 0.0f)
            w = fixedW[i] + (int) (free * weights[i] / weightTotal);
        else // only fixed-width panels: stretch them evenly
            w = fixedW[i] + free / count;

        if (++placed == count) w = area.getRight() - x; // absorb rounding
        panel->setBounds (x, area.getY(), w, h);
        x += w + gap;
    }

    saveSettings();
}

void NanoStripEditor::timerCallback()
{
    const double fs = proc.currentSampleRate.load();
    if (! juce::exactlyEqual (fs, preparedRate) && fs > 0)
    {
        preparedRate = fs;
        for (auto* panel : panels) panel->prepare (fs);
    }

    // Pull everything the audio thread sent. The waveform always listens so its
    // history is there when you re-enable it; hidden panels otherwise skip work.
    int n;
    float blockPeak = 0.0f;
    while ((n = proc.fifo.pull (bufL.data(), bufR.data(), (int) bufL.size())) > 0)
    {
        for (int i = 0; i < 5; ++i)
            if (i == 0 || visible[(size_t) i])
                panels[(size_t) i]->feed (bufL.data(), bufR.data(), n);

        const auto rl = juce::FloatVectorOperations::findMinAndMax (bufL.data(), n);
        const auto rr = juce::FloatVectorOperations::findMinAndMax (bufR.data(), n);
        blockPeak = std::max ({ blockPeak, std::abs (rl.getStart()), std::abs (rl.getEnd()),
                                std::abs (rr.getStart()), std::abs (rr.getEnd()) });
    }

    // After ~5 s of silence everything has settled: stop redrawing until sound returns.
    silentFrames = blockPeak > 1.0e-6f ? 0 : silentFrames + 1;
    if (silentFrames > 5 * fps)
        return;

    for (int i = 0; i < 5; ++i)
    {
        if (! visible[(size_t) i]) continue;
        panels[(size_t) i]->frame();
        if (i != 0 || wave.hasNewData())
            panels[(size_t) i]->repaint();
    }
}
