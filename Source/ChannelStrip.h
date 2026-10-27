#pragma once

#include "Common.h"

namespace tf
{
/**
    One channel strip's signal path: input trim, three-band EQ with the swept mid,
    the fader, and the pan into the stereo mix. Every control is smoothed so
    turning a knob never zips or clicks, and arming crossfades between the tape
    and the live input instead of switching.

    Audio thread only, apart from prepare().
*/
class ChannelStrip
{
public:
    struct Controls
    {
        float trimDb, low, mid, midFreqHz, high, fader, pan;
        bool live;   // armed: hear the input instead of the tape
        float noiseRms = 0.0f;   // the strip's own electronics: noise added at its input, before EQ and fader
    };

    void prepare (double sampleRate) noexcept;

    /** Mixes tape and live by the crossfade, then trim (on the live part), EQ and fader, into out. */
    void process (const float* tape, const float* live, float* out, int numSamples, const Controls&) noexcept;

    /** Adds out to the stereo mix with constant-power pan; gain scales it (a bounce destination
        fades out of the mix as it goes live, since you already hear its sources). */
    void addToMix (const float* chan, float* left, float* right, int numSamples, float pan, float gain) noexcept;

    /** How live the strip is right now: 0 = tape, 1 = input. */
    float liveAmount() const noexcept { return liveMix; }

    static constexpr int chunk = 32;            // EQ coefficients follow the knobs this often
    static constexpr double smoothSeconds = 0.03, crossfadeSeconds = 0.012;

private:
    void updateFilters (bool force) noexcept;

    double sr = 48000.0;
    Biquad eq[3];
    float target[4] {}, value[4] { 99, 99, 99, 99 }, applied[4] { 99, 99, 99, 99 };   // low, mid, midFreq (Hz), high
    float smoothCoef = 0.1f, liveStep = 0.001f;
    float liveMix = 0.0f, trimGain = 1.0f, faderGain = 0.0f, lastGainL = 0.0f, lastGainR = 0.0f;
    bool first = true;
    // each strip's own noise source: independent of the others, as separate circuits are
    static uint32_t nextSeed() noexcept { static std::atomic<uint32_t> n { 0 }; return 0x2545f491u + 0x9e3779b9u * ++n; }
    Noise noise { nextSeed() };
};
} // namespace tf
