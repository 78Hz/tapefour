#include "ChannelStrip.h"

namespace tf
{
void ChannelStrip::prepare (double sampleRate) noexcept
{
    sr = sampleRate;
    smoothCoef = (float) (1.0 - std::exp (-(double) chunk / (smoothSeconds * sr)));
    liveStep = (float) (1.0 / (crossfadeSeconds * sr));
    for (auto& b : eq) b.reset();
    first = true;
}

void ChannelStrip::updateFilters (bool force) noexcept
{
    // Shelves voiced like a cassette portastudio's: LOW is at full strength by 100 Hz (half at 250 Hz),
    // HIGH at nearly full strength by 10 kHz (half at 4.5 kHz), so both are heard well inside the tape's range.
    // Only recalculated when a knob has actually moved.
    auto moved = [&] (int i, float eps) { return force || std::abs (value[i] - applied[i]) > eps; };
    if (moved (0, 1.0e-4f)) { eq[0].setLowShelf (sr, 250.0, value[0] * 12.0); applied[0] = value[0]; }
    if (moved (1, 1.0e-4f) || moved (2, 0.05f))
    {
        eq[1].setPeak (sr, value[2], 0.8, value[1] * 12.0);
        applied[1] = value[1];
        applied[2] = value[2];
    }
    if (moved (3, 1.0e-4f)) { eq[2].setHighShelf (sr, 4500.0, value[3] * 12.0); applied[3] = value[3]; }
}

void ChannelStrip::process (const float* tape, const float* live, float* out, int n, const Controls& c) noexcept
{
    target[0] = c.low; target[1] = c.mid; target[2] = c.midFreqHz; target[3] = c.high;
    const auto trimTarget = dbToGain (c.trimDb);
    const auto faderTarget = c.fader;

    if (first)
    {
        for (int i = 0; i < 4; ++i) value[i] = target[i];
        liveMix = c.live ? 1.0f : 0.0f;
        trimGain = trimTarget;
        faderGain = faderTarget;
        updateFilters (true);
        first = false;
    }

    const auto liveTarget = c.live ? 1.0f : 0.0f;
    for (int start = 0; start < n; start += chunk)
    {
        const int len = std::min (chunk, n - start);

        // Knobs glide over ~30 ms; the mid frequency glides on a log scale, as the knob turns.
        for (int i = 0; i < 4; ++i)
        {
            if (i == 2)
                value[2] = std::exp (std::log (value[2]) + (std::log (target[2]) - std::log (value[2])) * smoothCoef);
            else
                value[i] += (target[i] - value[i]) * smoothCoef;
        }
        updateFilters (false);

        const auto trim0 = trimGain, trim1 = trimGain + (trimTarget - trimGain) * smoothCoef;
        const auto fad0 = faderGain, fad1 = faderGain + (faderTarget - faderGain) * smoothCoef;
        const auto inv = 1.0f / (float) len;

        for (int i = 0; i < len; ++i)
        {
            const auto k = start + i;
            const auto f = (float) i * inv;
            if (liveMix != liveTarget)
                liveMix = liveTarget > liveMix ? std::min (liveTarget, liveMix + liveStep) : std::max (liveTarget, liveMix - liveStep);

            // The trim only acts on the live input; the tape comes back at the level it was recorded.
            const auto x = tape[k] * (1.0f - liveMix) + live[k] * liveMix * (trim0 + (trim1 - trim0) * f);
            out[k] = eq[2].process (eq[1].process (eq[0].process (x))) * (fad0 + (fad1 - fad0) * f);
        }
        trimGain = trim1;
        faderGain = fad1;
    }
}

void ChannelStrip::addToMix (const float* c, float* left, float* right, int n, float pan, float gain) noexcept
{
    const auto angle = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    const auto gl = std::cos (angle) * gain, gr = std::sin (angle) * gain;
    const auto inv = 1.0f / (float) std::max (1, n);
    for (int i = 0; i < n; ++i)
    {
        const auto f = (float) i * inv;
        left[i]  += c[i] * (lastGainL + (gl - lastGainL) * f);
        right[i] += c[i] * (lastGainR + (gr - lastGainR) * f);
    }
    lastGainL = gl;
    lastGainR = gr;
}
} // namespace tf
