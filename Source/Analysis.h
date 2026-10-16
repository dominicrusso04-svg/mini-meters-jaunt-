#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace ns
{

//==============================================================================
// Simple RBJ biquad, used on both the audio and GUI side.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void setLowpass (double fs, double f, double q)
    {
        const double w = juce::MathConstants<double>::twoPi * f / fs;
        const double c = std::cos (w), al = std::sin (w) / (2.0 * q), a0 = 1.0 + al;
        b0 = (1.0 - c) * 0.5 / a0; b1 = (1.0 - c) / a0; b2 = b0;
        a1 = -2.0 * c / a0; a2 = (1.0 - al) / a0;
    }

    void setHighpass (double fs, double f, double q)
    {
        const double w = juce::MathConstants<double>::twoPi * f / fs;
        const double c = std::cos (w), al = std::sin (w) / (2.0 * q), a0 = 1.0 + al;
        b0 = (1.0 + c) * 0.5 / a0; b1 = -(1.0 + c) / a0; b2 = b0;
        a1 = -2.0 * c / a0; a2 = (1.0 - al) / a0;
    }

    inline double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void reset() noexcept { z1 = z2 = 0; }
};

//==============================================================================
// Lock-free stereo sample FIFO: audio thread pushes, GUI thread pulls.
class SampleFifo
{
public:
    explicit SampleFifo (int capacity)
        : fifo (capacity), bufL ((size_t) capacity), bufR ((size_t) capacity) {}

    void push (const float* l, const float* r, int n) noexcept
    {
        int s1, z1, s2, z2;
        fifo.prepareToWrite (n, s1, z1, s2, z2);
        if (z1 > 0) { std::copy (l, l + z1, bufL.data() + s1); std::copy (r, r + z1, bufR.data() + s1); }
        if (z2 > 0) { std::copy (l + z1, l + z1 + z2, bufL.data() + s2); std::copy (r + z1, r + z1 + z2, bufR.data() + s2); }
        fifo.finishedWrite (z1 + z2);
    }

    int pull (float* l, float* r, int maxN) noexcept
    {
        int s1, z1, s2, z2;
        fifo.prepareToRead (maxN, s1, z1, s2, z2);
        if (z1 > 0) { std::copy (bufL.data() + s1, bufL.data() + s1 + z1, l); std::copy (bufR.data() + s1, bufR.data() + s1 + z1, r); }
        if (z2 > 0) { std::copy (bufL.data() + s2, bufL.data() + s2 + z2, l + z1); std::copy (bufR.data() + s2, bufR.data() + s2 + z2, r + z1); }
        fifo.finishedRead (z1 + z2);
        return z1 + z2;
    }

private:
    juce::AbstractFifo fifo;
    std::vector<float> bufL, bufR;
};

//==============================================================================
// EBU R128 / ITU-R BS.1770 loudness. Runs on the audio thread so integrated
// loudness keeps counting even while the window is closed.
class LoudnessMeter
{
public:
    std::atomic<float> momentary { -200.0f }, shortTerm { -200.0f }, integrated { -200.0f };

    void prepare (double fs)
    {
        // K-weighting stage 1: high shelf
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / fs);
            const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            for (auto& s : shelf)
            {
                s.b0 = (Vh + Vb * K / Q + K * K) / a0;
                s.b1 = 2.0 * (K * K - Vh) / a0;
                s.b2 = (Vh - Vb * K / Q + K * K) / a0;
                s.a1 = 2.0 * (K * K - 1.0) / a0;
                s.a2 = (1.0 - K / Q + K * K) / a0;
            }
        }
        // K-weighting stage 2: high pass
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / fs);
            const double a0 = 1.0 + K / Q + K * K;
            for (auto& h : hp)
            {
                h.b0 = 1.0; h.b1 = -2.0; h.b2 = 1.0;
                h.a1 = 2.0 * (K * K - 1.0) / a0;
                h.a2 = (1.0 - K / Q + K * K) / a0;
            }
        }
        blockLen = std::max (1, (int) std::lround (fs * 0.1));
        resetState();
    }

    void requestReset() noexcept { resetFlag.store (true); }

    // r may be nullptr for a mono input
    void process (const float* l, const float* r, int n) noexcept
    {
        if (resetFlag.exchange (false))
            resetState();

        for (int i = 0; i < n; ++i)
        {
            const double kl = hp[0].process (shelf[0].process (l[i]));
            double e = kl * kl;
            if (r != nullptr)
            {
                const double kr = hp[1].process (shelf[1].process (r[i]));
                e += kr * kr;
            }
            acc += e;
            if (++count >= blockLen)
                finishBlock();
        }
    }

    static float toLufs (double e) noexcept
    {
        return e > 1.0e-20 ? (float) (-0.691 + 10.0 * std::log10 (e)) : -200.0f;
    }

private:
    static constexpr int ringSize = 30;   // 30 x 100 ms = 3 s (short-term)
    static constexpr int numBins = 800;   // -70 .. +10 LUFS in 0.1 dB steps

    Biquad shelf[2], hp[2];
    int blockLen = 4800, count = 0;
    double acc = 0;
    std::array<double, ringSize> ring {};
    int ringPos = 0, filled = 0;
    std::array<double, numBins> histEnergy {};
    std::array<int, numBins> histCount {};
    std::atomic<bool> resetFlag { false };

    void resetState() noexcept
    {
        for (auto& s : shelf) s.reset();
        for (auto& h : hp) h.reset();
        acc = 0; count = 0; ringPos = 0; filled = 0;
        ring.fill (0.0); histEnergy.fill (0.0); histCount.fill (0);
        momentary = -200.0f; shortTerm = -200.0f; integrated = -200.0f;
    }

    double meanLast (int k) const noexcept
    {
        k = std::min (k, filled);
        if (k <= 0) return 0.0;
        double s = 0;
        for (int i = 1; i <= k; ++i)
            s += ring[(size_t) ((ringPos - i + ringSize) % ringSize)];
        return s / k;
    }

    void finishBlock() noexcept
    {
        ring[(size_t) ringPos] = acc / count;
        ringPos = (ringPos + 1) % ringSize;
        filled = std::min (filled + 1, ringSize);
        acc = 0; count = 0;

        const double m = meanLast (4);
        momentary.store (toLufs (m), std::memory_order_relaxed);
        shortTerm.store (toLufs (meanLast (ringSize)), std::memory_order_relaxed);

        if (filled >= 4)
        {
            const double lm = toLufs (m);
            if (lm > -70.0)
            {
                const int bin = juce::jlimit (0, numBins - 1, (int) ((lm + 70.0) * 10.0));
                histEnergy[(size_t) bin] += m;
                histCount[(size_t) bin] += 1;
            }
            integrated.store (computeIntegrated(), std::memory_order_relaxed);
        }
    }

    float computeIntegrated() const noexcept
    {
        double e = 0; long long c = 0;
        for (int i = 0; i < numBins; ++i) { e += histEnergy[(size_t) i]; c += histCount[(size_t) i]; }
        if (c == 0) return -200.0f;

        const double relGate = toLufs (e / (double) c) - 10.0;
        const int start = juce::jlimit (0, numBins, (int) std::ceil ((relGate + 70.0) * 10.0));
        e = 0; c = 0;
        for (int i = start; i < numBins; ++i) { e += histEnergy[(size_t) i]; c += histCount[(size_t) i]; }
        return c > 0 ? toLufs (e / (double) c) : -200.0f;
    }
};

//==============================================================================
// Tempo-synced scope: while the host is playing, every sample is written into
// a bin based on its position inside a 1/2/4/8-beat window. The result is a
// waveform that stays locked to the grid.
class BeatScope
{
public:
    static constexpr int bins = 512;

    std::array<std::atomic<float>, bins> mn {}, mx {};
    std::atomic<int> beatsPerWindow { 4 };
    std::atomic<bool> active { false };
    std::atomic<float> phase { 0.0f };

    void clear() noexcept
    {
        for (int i = 0; i < bins; ++i) { mn[(size_t) i].store (0.0f); mx[(size_t) i].store (0.0f); }
    }

    void process (const float* l, const float* r, int n, bool valid,
                  double ppq, double bpm, double fs) noexcept
    {
        active.store (valid, std::memory_order_relaxed);
        if (! valid || bpm <= 0.0 || fs <= 0.0) { lastBin = -1; return; }

        const double win = (double) beatsPerWindow.load (std::memory_order_relaxed);
        const double inc = bpm / 60.0 / fs;

        double ph = std::fmod (ppq, win);
        if (ph < 0) ph += win;
        const double binScale = bins / win;

        for (int i = 0; i < n; ++i, ph += inc)
        {
            if (ph >= win) ph -= win;
            const int b = std::min (bins - 1, (int) (ph * binScale));
            const float m = r != nullptr ? 0.5f * (l[i] + r[i]) : l[i];

            if (b != lastBin)
            {
                flush();
                lastBin = b;
                curMin = curMax = m;
            }
            else
            {
                curMin = std::min (curMin, m);
                curMax = std::max (curMax, m);
            }
        }
        flush();

        if (ph >= win) ph -= win;
        phase.store ((float) (ph / win), std::memory_order_relaxed);
    }

private:
    int lastBin = -1;
    float curMin = 0, curMax = 0;

    void flush() noexcept
    {
        if (lastBin < 0) return;
        mn[(size_t) lastBin].store (curMin, std::memory_order_relaxed);
        mx[(size_t) lastBin].store (curMax, std::memory_order_relaxed);
    }
};

} // namespace ns
