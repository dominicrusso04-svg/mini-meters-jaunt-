#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "Analysis.h"

namespace ns
{

//==============================================================================
namespace theme
{
    const juce::Colour bg      { 0xff0a0c0f };
    const juce::Colour panel   { 0xff12151a };
    const juce::Colour grid    { 0xff1d2128 };
    const juce::Colour gridHi  { 0xff2a3039 };
    const juce::Colour text    { 0xff6f7987 };
    const juce::Colour textHi  { 0xffdde3ea };
    const juce::Colour warn    { 0xffffb347 };
    const juce::Colour hot     { 0xffff4d5e };

    // Waveform band colours: lows / mids / highs
    const juce::Colour bandLo  { 0xffff5a36 };
    const juce::Colour bandMid { 0xff6ef07a };
    const juce::Colour bandHi  { 0xff48b8ff };

    inline juce::Colour accent (int i)
    {
        static const juce::uint32 c[] = { 0xff3fe0d0, 0xffff5fa2, 0xffffb347, 0xffa6f04a, 0xff9b7bff, 0xffeef2f6 };
        return juce::Colour (c[(size_t) juce::jlimit (0, 5, i)]);
    }
    constexpr int numAccents = 6;

    inline juce::Font label (float h)  { return juce::Font (juce::FontOptions (h, juce::Font::bold)); }
    inline juce::Font number (float h)
    {
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), h, juce::Font::bold));
    }

    inline juce::String fmtDb (float v, int decimals = 1)
    {
        if (v <= -100.0f) return "-inf";
        return (v > 0.0f ? "+" : "") + juce::String (v, decimals);
    }
}

inline float gainToDb (float g) { return g > 1.0e-6f ? 20.0f * std::log10 (g) : -120.0f; }

//==============================================================================
class Panel : public juce::Component
{
public:
    explicit Panel (juce::String t) : title (std::move (t)) { setOpaque (true); }

    virtual void prepare (double fs) { sampleRate = fs; }
    virtual void feed (const float* L, const float* R, int n) = 0;
    virtual void frame() {}
    virtual void setFrameRate (int fps) { frameK = 60.0f / (float) fps; }
    virtual juce::String subtitle() const { return {}; }
    virtual void clicked() {}

    void setAccent (juce::Colour c) { accent = c; repaint(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::bg);
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::panel);
        g.fillRoundedRectangle (r, 5.0f);

        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (getLocalBounds().reduced (1));
            paintContent (g, r.reduced (6.0f, 5.0f));
        }

        auto t = title;
        auto sub = subtitle();
        if (sub.isNotEmpty()) t << "  " << sub;
        g.setFont (theme::label (9.5f));
        g.setColour (theme::text);
        g.drawText (t, r.reduced (7.0f, 4.0f).removeFromTop (12.0f), juce::Justification::topLeft, false);
    }

    void mouseDown (const juce::MouseEvent&) override { clicked(); repaint(); }

protected:
    virtual void paintContent (juce::Graphics&, juce::Rectangle<float>) = 0;

    juce::String title;
    juce::Colour accent = theme::accent (0);
    double sampleRate = 44100.0;
    float frameK = 1.0f;   // 1 at 60 fps, 2 at 30 fps
    // convert a per-frame smoothing amount tuned for 60 fps to the current rate
    float adj (float c) const { return 1.0f - std::pow (1.0f - c, frameK); }
};

//==============================================================================
// Scrolling waveform. Two colour modes:
//   SPECTRAL - coloured by low / mid / high energy
//   DUO      - two user-picked colours: outer (peaks) and inner core (RMS)
// Columns are drawn once into a cached image that works as a ring buffer, so
// each frame only paints the new slivers instead of the whole history.
class WaveformPanel : public Panel
{
public:
    WaveformPanel() : Panel ("WAVE") { cols.resize (4096); }

    int speed = 1;    // 0..3
    int mode = 0;     // 0 = spectral, 1 = duo
    int colA = 1, colB = 0;
    static constexpr float speeds[] = { 40.0f, 80.0f, 160.0f, 320.0f }; // columns / second

    static juce::Colour duoColour (int i)
    {
        static const juce::uint32 c[] = { 0xffeef2f6, 0xff3fe0d0, 0xffff5fa2, 0xffffb347,
                                          0xffa6f04a, 0xff9b7bff, 0xffff4d5e, 0xff48b8ff };
        return juce::Colour (c[(size_t) (i & 7)]);
    }
    static constexpr int numDuo = 8;

    void prepare (double fs) override
    {
        Panel::prepare (fs);
        lp.setLowpass (fs, 220.0, 0.707);
        hp.setHighpass (fs, 2800.0, 0.707);
        lp.reset(); hp.reset();
        updateSpc();
    }

    void setSpeed (int s) { speed = juce::jlimit (0, 3, s); updateSpc(); }
    void setMode (int m)  { mode = juce::jlimit (0, 1, m); dirty = true; }
    void setColours (int a, int b) { colA = a & 7; colB = b & 7; dirty = true; }

    void feed (const float* L, const float* R, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (L[i] + R[i]);
            const double lo = lp.process (m);
            const double hi = hp.process (m);
            const double mid = m - lo - hi;
            eLo += lo * lo; eMid += mid * mid; eHi += hi * hi; eAll += (double) m * m;
            cMin = std::min (cMin, m);
            cMax = std::max (cMax, m);

            if (++cCount >= spc)
            {
                auto& c = cols[(size_t) head];
                c.mn = cMin; c.mx = cMax;
                c.rms = (float) std::sqrt (eAll / cCount);
                c.spectral = spectralColour ((float) std::sqrt (eLo / cCount),
                                             (float) std::sqrt (eMid / cCount),
                                             (float) std::sqrt (eHi / cCount));
                head = (head + 1) % (int) cols.size();
                numCols = std::min (numCols + 1, (int) cols.size());
                ++pending;
                cMin = 1.0f; cMax = -1.0f; cCount = 0; eLo = eMid = eHi = eAll = 0;
            }
        }
    }

    bool hasNewData() const { return pending > 0 || dirty; }

    juce::String subtitle() const override { return juce::String (1 << speed) + "x"; }
    std::function<void()> onSettingChanged;

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto p = e.position;
        if (modeRect.contains (p))                 setMode (1 - mode);
        else if (mode == 1 && swARect.contains (p)) setColours (colA + 1, colB);
        else if (mode == 1 && swBRect.contains (p)) setColours (colA, colB + 1);
        else                                        setSpeed ((speed + 1) % 4);
        if (onSettingChanged) onSettingChanged();
        repaint();
    }

protected:
    void paintContent (juce::Graphics& g, juce::Rectangle<float> r) override
    {
        const auto area = r.toNearestInt();
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        const int W = std::max (1, area.getWidth()), H = std::max (1, area.getHeight());

        if (cache.isNull() || cacheW != W || cacheH != H || std::abs (cacheScale - scale) > 0.01f)
        {
            cacheW = W; cacheH = H; cacheScale = scale;
            cache = juce::Image (juce::Image::RGB, juce::roundToInt (W * scale), juce::roundToInt (H * scale), false);
            dirty = true;
        }

        if (dirty || pending >= W)
        {
            // full redraw of the visible history
            juce::Graphics ig (cache);
            ig.addTransform (juce::AffineTransform::scale (scale));
            ig.fillAll (theme::panel);
            const int count = std::min (W, numCols);
            writeX = 0;
            for (int i = count - 1; i >= 0; --i)
                drawColumn (ig, writeX++, colAt (i));
            writeX %= W;
            dirty = false;
        }
        else if (pending > 0)
        {
            juce::Graphics ig (cache);
            ig.addTransform (juce::AffineTransform::scale (scale));
            for (int i = pending - 1; i >= 0; --i)
            {
                drawColumn (ig, writeX, colAt (i));
                writeX = (writeX + 1) % W;
            }
        }
        pending = 0;

        // blit the ring: [writeX, W) on the left, [0, writeX) on the right
        const int sx = juce::roundToInt (writeX * scale);
        const int left = W - writeX;
        g.drawImage (cache, area.getX(), area.getY(), left, H, sx, 0, cache.getWidth() - sx, cache.getHeight());
        if (writeX > 0)
            g.drawImage (cache, area.getX() + left, area.getY(), writeX, H, 0, 0, sx, cache.getHeight());

        // soft fade on the oldest edge
        g.setGradientFill (juce::ColourGradient (theme::panel, r.getX(), 0, theme::panel.withAlpha (0.0f), r.getX() + 40.0f, 0, false));
        g.fillRect (r.withWidth (40.0f).expanded (0, 6));

        paintColourControls (g);
    }

private:
    struct Column { float mn = 0, mx = 0, rms = 0; juce::Colour spectral; };
    std::vector<Column> cols;
    int head = 0, numCols = 0, cCount = 0, spc = 512, pending = 0;
    float cMin = 1.0f, cMax = -1.0f;
    double eLo = 0, eMid = 0, eHi = 0, eAll = 0;
    Biquad lp, hp;

    juce::Image cache;
    int cacheW = 0, cacheH = 0, writeX = 0;
    float cacheScale = 1.0f;
    bool dirty = true;
    juce::Rectangle<float> modeRect, swARect, swBRect;

    void updateSpc() { spc = std::max (16, (int) (sampleRate / speeds[speed])); }

    // i = 0 is the newest column
    const Column& colAt (int i) const
    {
        const int size = (int) cols.size();
        return cols[(size_t) ((head - 1 - i + 2 * size) % size)];
    }

    void drawColumn (juce::Graphics& ig, int x, const Column& c)
    {
        const float cy = cacheH * 0.5f, half = cacheH * 0.5f;
        const float fx = (float) x;
        ig.setColour (theme::panel);
        ig.fillRect (fx, 0.0f, 1.0f, (float) cacheH);
        ig.setColour (theme::grid);
        ig.fillRect (fx, std::floor (cy), 1.0f, 1.0f);

        const juce::Colour outer = mode == 0 ? c.spectral.withAlpha (0.42f) : duoColour (colA).withAlpha (0.9f);
        const juce::Colour inner = mode == 0 ? c.spectral : duoColour (colB);

        const float top = cy - juce::jlimit (-1.0f, 1.0f, c.mx) * half;
        const float bot = cy - juce::jlimit (-1.0f, 1.0f, c.mn) * half;
        ig.setColour (outer);
        ig.fillRect (fx, top, 1.0f, std::max (1.0f, bot - top));

        const float rr = std::max (0.5f, std::min (1.0f, c.rms * 1.41f) * half);
        ig.setColour (inner);
        ig.fillRect (fx, cy - rr, 1.0f, 2.0f * rr);
    }

    void paintColourControls (juce::Graphics& g)
    {
        auto bar = getLocalBounds().toFloat().reduced (7.0f, 4.0f).removeFromTop (12.0f);
        g.setFont (theme::label (8.5f));

        if (mode == 1)
        {
            swBRect = bar.removeFromRight (12.0f);
            bar.removeFromRight (3.0f);
            swARect = bar.removeFromRight (12.0f);
            bar.removeFromRight (5.0f);
            for (auto [rect, col] : { std::pair { swARect, duoColour (colA) }, std::pair { swBRect, duoColour (colB) } })
            {
                g.setColour (col);
                g.fillEllipse (rect.reduced (1.5f));
                g.setColour (theme::bg.withAlpha (0.6f));
                g.drawEllipse (rect.reduced (1.5f), 1.0f);
            }
        }
        else
        {
            swARect = swBRect = {};
        }

        const juce::String label = mode == 0 ? "SPECTRAL" : "DUO";
        const float w = mode == 0 ? 54.0f : 30.0f;
        modeRect = bar.removeFromRight (w);
        g.setColour (theme::panel.withAlpha (0.85f));
        g.fillRoundedRectangle (modeRect, 6.0f);
        g.setColour (theme::gridHi);
        g.drawRoundedRectangle (modeRect.reduced (0.5f), 6.0f, 1.0f);
        g.setColour (theme::textHi.withAlpha (0.85f));
        g.drawText (label, modeRect, juce::Justification::centred, false);
    }

    static juce::Colour spectralColour (float lo, float mid, float hi)
    {
        // weight highs up a bit, they carry far less energy than lows
        mid *= 1.6f; hi *= 3.2f;
        const float sum = lo + mid + hi + 1.0e-9f;
        const float wl = lo / sum, wm = mid / sum, wh = hi / sum;
        float rC = wl * theme::bandLo.getFloatRed()   + wm * theme::bandMid.getFloatRed()   + wh * theme::bandHi.getFloatRed();
        float gC = wl * theme::bandLo.getFloatGreen() + wm * theme::bandMid.getFloatGreen() + wh * theme::bandHi.getFloatGreen();
        float bC = wl * theme::bandLo.getFloatBlue()  + wm * theme::bandMid.getFloatBlue()  + wh * theme::bandHi.getFloatBlue();
        const float mxC = std::max ({ rC, gC, bC, 1.0e-6f });
        return juce::Colour::fromFloatRGBA (rC / mxC, gC / mxC, bC / mxC, 1.0f);
    }
};

//==============================================================================
// Smooth FFT spectrum with peak-hold trace.
class SpectrumPanel : public Panel
{
public:
    SpectrumPanel() : Panel ("SPECTRUM") {}

    void setFrameRate (int fps) override
    {
        Panel::setFrameRate (fps);
        kAttack = adj (0.65f);
        kRelease = adj (0.16f);
    }
    float kAttack = 0.65f, kRelease = 0.16f;

    bool showPeak = true;
    static constexpr float tilt = 4.5f;            // dB / octave around 1 kHz
    static constexpr float dbMin = -84.0f, dbMax = 6.0f;

    void feed (const float* L, const float* R, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            ring[(size_t) ringPos] = 0.5f * (L[i] + R[i]);
            ringPos = (ringPos + 1) & (N - 1);
        }
        if (n > 0) fresh = true;
    }

    void frame() override
    {
        // one point per pixel, capped: beyond ~480 points the eye can't tell
        const int pts = juce::jlimit (2, 480, getWidth() - 12);
        if ((int) level.size() != pts || ! juce::exactlyEqual (mapRate, sampleRate))
        {
            level.assign ((size_t) pts, dbMin - 20.0f);
            peak.assign ((size_t) pts, dbMin - 20.0f);
            buildMap (pts);
        }

        if (fresh)
        {
            fresh = false;
            const int split = N - ringPos;
            std::copy (ring.begin() + ringPos, ring.end(), fftData.begin());
            std::copy (ring.begin(), ring.begin() + ringPos, fftData.begin() + split);
            std::fill (fftData.begin() + N, fftData.end(), 0.0f);
            window.multiplyWithWindowingTable (fftData.data(), (size_t) N);
            fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

            for (int x = 0; x < pts; ++x)
            {
                const auto& m = map[(size_t) x];
                float mag;
                if (m.interp)
                    mag = fftData[(size_t) m.i0] * (1.0f - m.t) + fftData[(size_t) m.i0 + 1] * m.t;
                else
                    mag = *std::max_element (fftData.begin() + m.i0, fftData.begin() + m.i1 + 1);

                const float db = 10.0f * std::log10 (mag * mag + 1.0e-12f) + m.offset;
                auto& lv = level[(size_t) x];
                lv = db > lv ? lv + (db - lv) * kAttack : lv + (db - lv) * kRelease;
            }
        }

        for (size_t x = 0; x < level.size(); ++x)
            peak[x] = level[x] > peak[x] ? level[x] : peak[x] - 0.22f * frameK;
    }

    juce::String subtitle() const override { return showPeak ? "PEAK" : ""; }
    void clicked() override { showPeak = ! showPeak; if (onSettingChanged) onSettingChanged(); }
    std::function<void()> onSettingChanged;

protected:
    void paintContent (juce::Graphics& g, juce::Rectangle<float> r) override
    {
        auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (dbMin, dbMax, db), dbMin, dbMax, r.getBottom(), r.getY() + 10.0f); };
        auto xFor = [&] (double f) { return r.getX() + (float) (std::log (f / 20.0) / std::log (1000.0)) * r.getWidth(); };

        // grid
        g.setFont (theme::label (8.5f));
        for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const float x = xFor (f);
            const bool major = (juce::exactlyEqual (f, 100.0) || juce::exactlyEqual (f, 1000.0) || juce::exactlyEqual (f, 10000.0));
            g.setColour (major ? theme::gridHi : theme::grid);
            g.drawVerticalLine ((int) x, r.getY() + 12.0f, r.getBottom());
        }
        for (float db = -72.0f; db <= 0.0f; db += 12.0f)
        {
            g.setColour (theme::grid.withAlpha (0.6f));
            g.drawHorizontalLine ((int) yFor (db), r.getX(), r.getRight());
        }

        if (level.size() < 2) return;
        const float dx = r.getWidth() / (float) level.size();

        juce::Path line;
        line.startNewSubPath (r.getX(), yFor (level[0]));
        for (size_t i = 1; i < level.size(); ++i)
            line.lineTo (r.getX() + (float) i * dx, yFor (level[i]));

        juce::Path fill (line);
        fill.lineTo (r.getRight(), r.getBottom());
        fill.lineTo (r.getX(), r.getBottom());
        fill.closeSubPath();

        g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.38f), 0, r.getY(),
                                                 accent.withAlpha (0.02f), 0, r.getBottom(), false));
        g.fillPath (fill);

        if (showPeak)
        {
            juce::Path pk;
            pk.startNewSubPath (r.getX(), yFor (peak[0]));
            for (size_t i = 1; i < peak.size(); ++i)
                pk.lineTo (r.getX() + (float) i * dx, yFor (peak[i]));
            g.setColour (theme::textHi.withAlpha (0.28f));
            g.strokePath (pk, juce::PathStrokeType (1.0f));
        }

        g.setColour (accent);
        g.strokePath (line, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved));

        if (r.getHeight() > 50)
        {
            g.setFont (theme::label (8.5f));
            g.setColour (theme::text.withAlpha (0.8f));
            for (double f : { 100.0, 1000.0, 10000.0 })
                g.drawText (f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f),
                            juce::Rectangle<float> (xFor (f) + 3.0f, r.getBottom() - 11.0f, 30.0f, 10.0f), juce::Justification::left);
        }
    }

private:
    static constexpr int order = 13, N = 1 << order;
    juce::dsp::FFT fft { order };
    juce::dsp::WindowingFunction<float> window { (size_t) N, juce::dsp::WindowingFunction<float>::hann, false };
    std::vector<float> ring = std::vector<float> ((size_t) N, 0.0f);
    std::vector<float> fftData = std::vector<float> ((size_t) N * 2, 0.0f);
    std::vector<float> level, peak;
    int ringPos = 0;
    bool fresh = false;

    struct MapPoint { int i0 = 0, i1 = 0; float t = 0, offset = 0; bool interp = true; };
    std::vector<MapPoint> map;
    double mapRate = 0.0;

    void buildMap (int pts)
    {
        mapRate = sampleRate;
        map.resize ((size_t) pts);
        const double binHz = sampleRate / N, nyq = sampleRate * 0.5;
        const float normDb = 20.0f * std::log10 (4.0f / (float) N);
        for (int x = 0; x < pts; ++x)
        {
            auto& m = map[(size_t) x];
            const double fa = std::min (nyq, freqAt ((double) x / pts));
            const double fb = std::min (nyq, freqAt ((double) (x + 1) / pts));
            const double ba = fa / binHz, bb = fb / binHz;
            m.interp = (bb - ba) < 1.0;
            if (m.interp)
            {
                const double bc = 0.5 * (ba + bb);
                m.i0 = juce::jlimit (0, N / 2 - 1, (int) bc);
                m.t = (float) (bc - m.i0);
            }
            else
            {
                m.i0 = juce::jlimit (0, N / 2, (int) std::ceil (ba));
                m.i1 = juce::jlimit (m.i0, N / 2, (int) bb);
            }
            const double fc = std::sqrt (std::max (1.0, fa * fb));
            m.offset = normDb + tilt * (float) std::log2 (fc / 1000.0);
        }
    }

    static double freqAt (double t) { return 20.0 * std::pow (1000.0, t); }
};

//==============================================================================
// Goniometer (stereo dot cloud) + phase correlation bar.
class StereoPanel : public Panel
{
public:
    StereoPanel() : Panel ("STEREO") {}

    void prepare (double fs) override
    {
        Panel::prepare (fs);
        corrCoef = (float) std::exp (-1.0 / (0.3 * fs));
    }

    void feed (const float* L, const float* R, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float l = L[i], r = R[i];
            ringL[(size_t) pos] = l; ringR[(size_t) pos] = r;
            framePeak = std::max (framePeak, std::max (std::abs (l), std::abs (r)));
            pos = (pos + 1) % numDots;
            sLR = corrCoef * sLR + (1.0f - corrCoef) * l * r;
            sLL = corrCoef * sLL + (1.0f - corrCoef) * l * l;
            sRR = corrCoef * sRR + (1.0f - corrCoef) * r * r;
        }
    }

    void frame() override
    {
        const float pk = framePeak;
        framePeak *= 1.0f - adj (0.1f);
        const float target = juce::jlimit (0.03f, 1.5f, pk);
        autoGain = target > autoGain ? target : autoGain + (target - autoGain) * adj (0.04f);

        const float denom = std::sqrt (sLL * sRR);
        const float c = denom > 1.0e-9f ? juce::jlimit (-1.0f, 1.0f, sLR / denom) : 0.0f;
        correlation += (c - correlation) * adj (0.25f);
    }

    juce::String subtitle() const override { return juce::String (correlation, 2); }

protected:
    void paintContent (juce::Graphics& g, juce::Rectangle<float> r) override
    {
        auto barArea = r.removeFromBottom (14.0f);
        r.removeFromTop (8.0f);
        const float rad = std::min (r.getWidth(), r.getHeight()) * 0.5f;
        const auto c = r.getCentre();

        // guides: M (vertical), S (horizontal), L / R diagonals
        g.setColour (theme::grid);
        g.drawLine (c.x, c.y - rad, c.x, c.y + rad);
        g.drawLine (c.x - rad, c.y, c.x + rad, c.y);
        const float d = rad * 0.7071f;
        g.setColour (theme::gridHi);
        g.drawLine (c.x - d, c.y - d, c.x + d, c.y + d, 1.0f);
        g.drawLine (c.x + d, c.y - d, c.x - d, c.y + d, 1.0f);
        g.setFont (theme::label (8.5f));
        g.setColour (theme::text.withAlpha (0.8f));
        g.drawText ("L", juce::Rectangle<float> (c.x - d - 10.0f, c.y - d - 10.0f, 10.0f, 10.0f), juce::Justification::centred);
        g.drawText ("R", juce::Rectangle<float> (c.x + d, c.y - d - 10.0f, 10.0f, 10.0f), juce::Justification::centred);

        // dots
        const float scale = rad * 0.92f / autoGain;
        dots.clear();
        dots.ensureStorageAllocated (numDots);
        for (int i = 0; i < numDots; ++i)
        {
            const float l = ringL[(size_t) i], rr = ringR[(size_t) i];
            const float mid = (l + rr) * 0.7071f, side = (rr - l) * 0.7071f;
            const float x = c.x + juce::jlimit (-rad, rad, side * scale);
            const float y = c.y - juce::jlimit (-rad, rad, mid * scale);
            dots.addWithoutMerging ({ x - 0.75f, y - 0.75f, 1.5f, 1.5f });
        }
        g.setColour (accent.withAlpha (0.7f));
        g.fillRectList (dots);   // one draw call for all dots

        // correlation bar (-1 .. +1)
        auto bar = barArea.reduced (2.0f, 4.0f);
        g.setColour (theme::grid);
        g.fillRoundedRectangle (bar, 2.0f);
        const float zero = bar.getCentreX();
        const float vx = zero + correlation * bar.getWidth() * 0.5f;
        const auto col = correlation < 0.0f ? theme::hot : (correlation < 0.3f ? theme::warn : accent);
        g.setColour (col.withAlpha (0.85f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (std::min (zero, vx), bar.getY(), std::max (zero, vx), bar.getBottom()));
        g.setColour (theme::textHi);
        g.fillRect (vx - 1.0f, bar.getY() - 2.0f, 2.0f, bar.getHeight() + 4.0f);
        g.setColour (theme::text);
        g.drawVerticalLine ((int) zero, bar.getY() - 2.0f, bar.getBottom() + 2.0f);
    }

private:
    static constexpr int numDots = 1536;
    std::array<float, numDots> ringL {}, ringR {};
    int pos = 0;
    float framePeak = 0.0f;
    float corrCoef = 0.9999f, sLR = 0, sLL = 0, sRR = 0;
    float correlation = 0.0f, autoGain = 0.5f;
    juce::RectangleList<float> dots;
};

//==============================================================================
// Peak / RMS bars + LUFS readouts. Click to reset.
class LoudnessPanel : public Panel
{
public:
    explicit LoudnessPanel (LoudnessMeter& m) : Panel ("LOUDNESS"), meter (m) {}

    void prepare (double fs) override
    {
        Panel::prepare (fs);
        rmsCoef = (float) std::exp (-1.0 / (0.3 * fs));
    }

    void feed (const float* L, const float* R, int n) override
    {
        const float* ch[2] = { L, R };
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
            {
                const float v = ch[c][i];
                framePeak[c] = std::max (framePeak[c], std::abs (v));
                ms[c] = rmsCoef * ms[c] + (1.0f - rmsCoef) * v * v;
            }
    }

    void frame() override
    {
        const float fall = 24.0f / 60.0f * frameK;
        for (int c = 0; c < 2; ++c)
        {
            const float pDb = gainToDb (framePeak[c]);
            framePeak[c] = 0.0f;
            peakDb[c] = std::max (pDb, peakDb[c] - fall);
            rmsDb[c] = gainToDb (std::sqrt (ms[c]));
            maxPeak[c] = std::max (maxPeak[c], pDb);
            if (pDb >= holdDb[c]) { holdDb[c] = pDb; holdFrames[c] = (int) (90 / frameK); }
            else if (--holdFrames[c] < 0) holdDb[c] -= fall;
        }
        lufsM = meter.momentary.load();
        lufsS = meter.shortTerm.load();
        lufsI = meter.integrated.load();
    }

    void clicked() override
    {
        meter.requestReset();
        maxPeak[0] = maxPeak[1] = -120.0f;
    }

    juce::String subtitle() const override { return "click = reset"; }

protected:
    void paintContent (juce::Graphics& g, juce::Rectangle<float> r) override
    {
        r.removeFromTop (12.0f);

        // bars
        auto bars = r.removeFromLeft (26.0f);
        r.removeFromLeft (10.0f);
        constexpr float lo = -60.0f, hi = 3.0f;
        auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (lo, hi, db), lo, hi, bars.getBottom(), bars.getY()); };

        juce::ColourGradient grad (accent, 0, bars.getBottom(), theme::hot, 0, bars.getY(), false);
        grad.addColour (juce::jmap (-12.0, (double) lo, (double) hi, 0.0, 1.0), accent);
        grad.addColour (juce::jmap (-4.0,  (double) lo, (double) hi, 0.0, 1.0), theme::warn);

        for (int c = 0; c < 2; ++c)
        {
            auto b = juce::Rectangle<float> (bars.getX() + (float) c * 10.0f, bars.getY(), 7.0f, bars.getHeight());
            g.setColour (theme::grid);
            g.fillRoundedRectangle (b, 1.5f);

            g.setGradientFill (grad);
            g.setOpacity (0.4f);
            g.fillRect (b.withTop (yFor (peakDb[c])));
            g.setOpacity (1.0f);
            g.fillRect (b.withTop (yFor (rmsDb[c])));

            g.setColour (holdDb[c] > -0.1f ? theme::hot : theme::textHi);
            g.fillRect (b.getX(), yFor (holdDb[c]) - 0.5f, b.getWidth(), 1.5f);
        }
        g.setColour (theme::gridHi);
        for (float db : { 0.0f, -6.0f, -12.0f, -24.0f, -48.0f })
            g.drawHorizontalLine ((int) yFor (db), bars.getRight() - 6.0f, bars.getRight());

        // readouts
        const float big = juce::jlimit (14.0f, 34.0f, r.getHeight() * 0.32f);
        const float small = juce::jlimit (9.0f, 12.0f, r.getHeight() * 0.11f);

        auto top = r.removeFromTop (big + 2.0f);
        g.setColour (lufsS <= -100.0f ? theme::text : theme::textHi);
        g.setFont (theme::number (big));
        g.drawText (lufsS <= -100.0f ? "--.-" : juce::String (lufsS, 1), top, juce::Justification::bottomLeft, false);

        g.setFont (theme::label (8.5f));
        g.setColour (theme::text);
        g.drawText ("LUFS SHORT-TERM", r.removeFromTop (11.0f), juce::Justification::topLeft, false);
        r.removeFromTop (3.0f);

        auto row = [&] (const juce::String& name, const juce::String& value, juce::Colour vc)
        {
            if (r.getHeight() < small) return;
            auto line = r.removeFromTop (small + 3.0f);
            g.setFont (theme::label (small * 0.85f));
            g.setColour (theme::text);
            g.drawText (name, line.removeFromLeft (36.0f), juce::Justification::centredLeft, false);
            g.setFont (theme::number (small));
            g.setColour (vc);
            g.drawText (value, line, juce::Justification::centredLeft, false);
        };

        auto lufsStr = [] (float v) { return v <= -100.0f ? juce::String ("--.-") : juce::String (v, 1); };
        const float mp = std::max (maxPeak[0], maxPeak[1]);

        row ("INT", lufsStr (lufsI), accent);
        row ("MOM", lufsStr (lufsM), theme::textHi);
        row ("PEAK", theme::fmtDb (mp), mp > -0.1f ? theme::hot : theme::textHi);
    }

private:
    LoudnessMeter& meter;
    float framePeak[2] { 0, 0 }, ms[2] { 0, 0 }, rmsCoef = 0.9999f;
    float peakDb[2] { -120, -120 }, rmsDb[2] { -120, -120 }, holdDb[2] { -120, -120 }, maxPeak[2] { -120, -120 };
    int holdFrames[2] { 0, 0 };
    float lufsM = -200, lufsS = -200, lufsI = -200;
};

//==============================================================================
// Oscilloscope. Locks to the song's beat while playing; otherwise uses a
// classic rising-edge trigger. Click to change the window length.
class ScopePanel : public Panel
{
public:
    explicit ScopePanel (BeatScope& b) : Panel ("SCOPE"), beat (b) {}

    int zoom = 2; // 0..3
    static constexpr int beatOpts[] = { 1, 2, 4, 8 };
    static constexpr float msOpts[] = { 5.0f, 10.0f, 20.0f, 40.0f };

    void setZoom (int z)
    {
        zoom = juce::jlimit (0, 3, z);
        beat.beatsPerWindow.store (beatOpts[zoom]);
        beat.clear();
    }

    void feed (const float* L, const float* R, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            ring[(size_t) pos] = 0.5f * (L[i] + R[i]);
            pos = (pos + 1) & (ringSize - 1);
        }
    }

    void frame() override
    {
        synced = beat.active.load();
        float pk = 0.0f;

        if (synced)
        {
            for (int i = 0; i < BeatScope::bins; ++i)
            {
                syncMin[(size_t) i] = beat.mn[(size_t) i].load (std::memory_order_relaxed);
                syncMax[(size_t) i] = beat.mx[(size_t) i].load (std::memory_order_relaxed);
                pk = std::max ({ pk, std::abs (syncMin[(size_t) i]), std::abs (syncMax[(size_t) i]) });
            }
            phase = beat.phase.load();
        }
        else
        {
            const int win = juce::jlimit (32, ringSize / 4, (int) (sampleRate * msOpts[zoom] / 1000.0));
            int start = (pos - win - 1 + ringSize) & (ringSize - 1);
            // look back for a rising zero crossing
            for (int k = 0; k < win * 2; ++k)
            {
                const int i = (pos - win - 1 - k + 2 * ringSize) & (ringSize - 1);
                const int p = (i - 1 + ringSize) & (ringSize - 1);
                if (ring[(size_t) p] < 0.0f && ring[(size_t) i] >= 0.0f) { start = i; break; }
            }
            trig.resize ((size_t) win);
            for (int k = 0; k < win; ++k)
            {
                trig[(size_t) k] = ring[(size_t) ((start + k) & (ringSize - 1))];
                pk = std::max (pk, std::abs (trig[(size_t) k]));
            }
        }

        const float target = juce::jlimit (0.05f, 1.0f, pk);
        autoGain = target > autoGain ? target : autoGain + (target - autoGain) * adj (0.05f);
    }

    juce::String subtitle() const override
    {
        if (synced)
        {
            const int b = beatOpts[zoom];
            return "SYNC " + (b >= 4 ? juce::String (b / 4) + (b == 4 ? " BAR" : " BARS")
                                     : juce::String (b) + (b == 1 ? " BEAT" : " BEATS"));
        }
        return juce::String ((int) msOpts[zoom]) + " MS";
    }

    void clicked() override { setZoom ((zoom + 1) % 4); if (onSettingChanged) onSettingChanged(); }
    std::function<void()> onSettingChanged;

protected:
    void paintContent (juce::Graphics& g, juce::Rectangle<float> r) override
    {
        r.removeFromTop (8.0f);
        const float cy = r.getCentreY(), half = r.getHeight() * 0.5f;
        const float gain = 0.9f / autoGain;
        g.setColour (theme::grid);
        g.drawHorizontalLine ((int) cy, r.getX(), r.getRight());

        if (synced)
        {
            const int beats = beatOpts[zoom];
            const int lines = beats <= 4 ? beats * 4 : beats; // 16ths up to a bar
            for (int i = 1; i < lines; ++i)
            {
                const bool major = beats <= 4 ? (i % 4 == 0) : (i % 4 == 0);
                g.setColour (major ? theme::gridHi : theme::grid.withAlpha (0.5f));
                g.drawVerticalLine ((int) (r.getX() + r.getWidth() * (float) i / (float) lines), r.getY(), r.getBottom());
            }

            const int w = std::max (1, (int) r.getWidth());
            juce::Path p;
            tops.resize ((size_t) w); bots.resize ((size_t) w);
            for (int x = 0; x < w; ++x)
            {
                const int b0 = x * BeatScope::bins / w;
                const int b1 = std::max (b0 + 1, (x + 1) * BeatScope::bins / w);
                float mn = 0, mx = 0;
                for (int b = b0; b < b1 && b < BeatScope::bins; ++b)
                {
                    mn = std::min (mn, syncMin[(size_t) b]);
                    mx = std::max (mx, syncMax[(size_t) b]);
                }
                tops[(size_t) x] = cy - juce::jlimit (-1.0f, 1.0f, mx * gain) * half;
                bots[(size_t) x] = cy - juce::jlimit (-1.0f, 1.0f, mn * gain) * half;
            }
            p.startNewSubPath (r.getX(), tops[0]);
            for (int x = 1; x < w; ++x) p.lineTo (r.getX() + (float) x, tops[(size_t) x]);
            for (int x = w - 1; x >= 0; --x) p.lineTo (r.getX() + (float) x, bots[(size_t) x] + 0.5f);
            p.closeSubPath();
            g.setColour (accent.withAlpha (0.85f));
            g.fillPath (p);

            // playhead + a dark gap just ahead of it
            const float px = r.getX() + phase * r.getWidth();
            g.setGradientFill (juce::ColourGradient (theme::panel, px, 0, theme::panel.withAlpha (0.0f), px + r.getWidth() * 0.12f, 0, false));
            g.fillRect (juce::Rectangle<float> (px, r.getY(), r.getWidth() * 0.12f, r.getHeight()));
            g.setColour (theme::textHi.withAlpha (0.8f));
            g.drawVerticalLine ((int) px, r.getY(), r.getBottom());
        }
        else if (trig.size() > 1)
        {
            juce::Path p;
            const float dx = r.getWidth() / (float) (trig.size() - 1);
            for (size_t k = 0; k < trig.size(); ++k)
            {
                const float y = cy - juce::jlimit (-1.0f, 1.0f, trig[k] * gain) * half;
                if (k == 0) p.startNewSubPath (r.getX(), y);
                else        p.lineTo (r.getX() + (float) k * dx, y);
            }
            g.setColour (accent.withAlpha (0.25f));
            g.strokePath (p, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved));
            g.setColour (accent);
            g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved));
        }
    }

private:
    BeatScope& beat;
    static constexpr int ringSize = 1 << 15;
    std::vector<float> ring = std::vector<float> ((size_t) ringSize, 0.0f);
    int pos = 0;
    std::vector<float> trig, tops, bots;
    std::array<float, BeatScope::bins> syncMin {}, syncMax {};
    bool synced = false;
    float phase = 0.0f, autoGain = 0.5f;
};

} // namespace ns
