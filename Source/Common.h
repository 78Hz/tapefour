#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace tf
{
inline constexpr int numTracks = 4;

// The tape is stored as "cells": 32000 per second of tape travel at 1 7/8 ips.
// Recording at 3 3/4 ips lays the same sound over twice as many cells (better
// treble, as on a real deck); playing back at a different speed than it was
// recorded at shifts pitch naturally, because the cells are read faster or slower.
inline constexpr int cellRate = 32000;
inline constexpr int blockCells = 3200;   // metadata resolution: 0.1 s of tape

struct TapeTypeInfo { const char* name; int minutes; float fullPackMm; };
// fullPackMm: nominal, with average leaders (each cassette's own is Tape::fullPackMm(), Geometry.h).
inline constexpr TapeTypeInfo tapeTypes[] { { "C-46", 23, 42.8f },
                                            { "C-60", 30, 47.3f },
                                            { "C-90", 45, 47.9f } };
inline constexpr int numTapeTypes = 3;

/**
    A tape formulation, from published measurements of a real cassette
    (Roberson's comparison of retail tapes, dB re 400 Hz Dolby level, 1 7/8 ips).

    mol100 / mol400: level giving 3% third-harmonic distortion at 100 / 400 Hz.
    sol10k:          saturation output at 10 kHz (treble headroom).
    snr:             A-weighted bias noise below Dolby level, no noise reduction.
    modNoise:        modulation noise (noise that rides on the signal), dB re signal.
    sensitivity:     output at a given record level, re the IEC reference tape.
*/
struct TapeModel
{
    const char* name;          // printed on the shell, e.g. "SD"
    const char* line;          // the strap line
    const char* position;      // "TYPE I · NORMAL"
    int iecType;               // 1, 2 or 4
    int eqMicroseconds;        // playback EQ: 120 (normal) or 70 (high/metal)
    float mol100, mol400, sol10k, snr, modNoise, sensitivity;
    const char* basedOn;       // the real tape the numbers come from
    juce::uint32 accent;       // print colour, for the inlay and menus
};

inline constexpr TapeModel tapeModels[] {
    { "BASIC", "EVERYDAY RECORDING",   "TYPE I \xc2\xb7 NORMAL", 1, 120, 2.0f, 2.4f, -10.3f, 54.3f, -36.1f, -1.0f, "Sony HF", 0xfff2c230 },
    { "SD",    "STUDIO DYNAMIC",       "TYPE I \xc2\xb7 NORMAL", 1, 120, 3.8f, 6.2f,  -8.2f, 60.3f, -44.3f, -0.3f, "TDK AD", 0xffe8702a },
    { "XR",    "CHROME EXTRA RANGE", "TYPE II \xc2\xb7 HIGH", 2, 70, 3.4f, 4.4f, -11.2f, 60.9f, -47.2f, 1.1f, "TDK SA", 0xff3f7fe0 },
    { "MX",    "REFERENCE \xc2\xb7 MAXIMUM OUTPUT", "TYPE IV \xc2\xb7 METAL", 4, 70, 8.0f, 9.1f, -6.8f, 62.7f, -50.4f, 0.8f, "Maxell MX", 0xffc98a4b } };
inline constexpr int numTapeModels = 4;

/** The deck's tape switch: NORMAL, CHROME, METAL. */
inline constexpr int numTapePositions = 3;
inline int positionForIec (int iecType) noexcept { return iecType >= 4 ? 2 : iecType >= 2 ? 1 : 0; }
inline const char* positionName (int p) noexcept { return p == 2 ? "METAL" : p == 1 ? "CHROME" : "NORMAL"; }
inline const char* iecTypeName (int p) noexcept  { return p == 2 ? "TYPE IV" : p == 1 ? "TYPE II" : "TYPE I"; }
inline constexpr int defaultTapeModel = 1;   // tapes made before models existed play as SD

// Dolby level (the tape's 0 dB) sits at 0 VU on the meters: -12 dBFS peak.
inline constexpr float tapeRefPeak = 0.2512f;

enum class Mode : int { stop = 0, play, rec, ff, rew };

inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }

inline juce::String formatTime (double seconds)
{
    auto s = (int) std::max (0.0, std::floor (seconds));
    return juce::String (s / 60).paddedLeft ('0', 2) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
}

// Simple RBJ biquad that never allocates (safe to retune on the audio thread).
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    inline float process (float x) noexcept
    {
        auto y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void reset() noexcept { z1 = z2 = 0; }

    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2) noexcept
    {
        b0 = (float) (nb0 / na0); b1 = (float) (nb1 / na0); b2 = (float) (nb2 / na0);
        a1 = (float) (na1 / na0); a2 = (float) (na2 / na0);
    }

    void setLowShelf (double sr, double f, double db, double slope = 1.0) noexcept
    {
        auto A = std::pow (10.0, db / 40.0), w = juce::MathConstants<double>::twoPi * f / sr;
        auto cs = std::cos (w), sn = std::sin (w);
        auto alpha = sn / 2.0 * std::sqrt ((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);
        auto sa = 2.0 * std::sqrt (A) * alpha;
        set (A * ((A + 1) - (A - 1) * cs + sa), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - sa),
             (A + 1) + (A - 1) * cs + sa, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - sa);
    }

    void setHighShelf (double sr, double f, double db, double slope = 1.0) noexcept
    {
        auto A = std::pow (10.0, db / 40.0), w = juce::MathConstants<double>::twoPi * f / sr;
        auto cs = std::cos (w), sn = std::sin (w);
        auto alpha = sn / 2.0 * std::sqrt ((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);
        auto sa = 2.0 * std::sqrt (A) * alpha;
        set (A * ((A + 1) + (A - 1) * cs + sa), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - sa),
             (A + 1) - (A - 1) * cs + sa, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - sa);
    }

    void setPeak (double sr, double f, double q, double db) noexcept
    {
        auto A = std::pow (10.0, db / 40.0), w = juce::MathConstants<double>::twoPi * f / sr;
        auto alpha = std::sin (w) / (2.0 * q), cs = std::cos (w);
        set (1 + alpha * A, -2 * cs, 1 - alpha * A, 1 + alpha / A, -2 * cs, 1 - alpha / A);
    }

    void setLowPass (double sr, double f, double q = 0.7071) noexcept
    {
        f = juce::jlimit (10.0, sr * 0.49, f);
        auto w = juce::MathConstants<double>::twoPi * f / sr;
        auto alpha = std::sin (w) / (2.0 * q), cs = std::cos (w);
        set ((1 - cs) / 2, 1 - cs, (1 - cs) / 2, 1 + alpha, -2 * cs, 1 - alpha);
    }

    void setHighPass (double sr, double f, double q = 0.7071) noexcept
    {
        auto w = juce::MathConstants<double>::twoPi * f / sr;
        auto alpha = std::sin (w) / (2.0 * q), cs = std::cos (w);
        set ((1 + cs) / 2, -(1 + cs), (1 + cs) / 2, 1 + alpha, -2 * cs, 1 - alpha);
    }
};

// Fast xorshift noise, audio-thread safe.
struct Noise
{
    uint32_t state = 0x12345678u;
    explicit Noise (uint32_t seed = 0x12345678u) : state (seed ? seed : 1u) {}
    inline float next() noexcept
    {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return (float) (int32_t) state * (1.0f / 2147483648.0f);
    }
    inline float uniform01() noexcept { return next() * 0.5f + 0.5f; }
};
} // namespace tf
