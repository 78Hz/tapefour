#pragma once

#include <array>
#include <cmath>
#include <cstdint>

/*
    Noise reduction, modelled on the published designs (docs/noise-reduction.md has the sources):

      B   a sliding-band stage: a fixed high-pass and a variable high-pass (a capacitor and a
          FET's channel) in the side path, the side path added to the main path when encoding
          and subtracted in a feedback loop when decoding. The FET is driven by the rectified,
          integrated side-path output, so above the threshold the variable filter's corner moves
          up the band until the side path's output is held roughly constant: loud signals are
          left alone, and the noise reduction carries on above them.
      C   two such stages in series, the low-level stage working 20 dB below the high-level
          stage and reaching further down in frequency, plus spectral skewing (a treble cut
          before the encoder, restored after the decoder) and anti-saturation (a treble shelf in
          the high-level stage's main path that only acts once its side path has backed off).
      2:1 a feed-forward compander: emphasis, a gain cell controlled by a log-domain RMS
          detector that hears a band-limited, treble-weighted copy, 2:1 on record and 1:2 on
          playback, then de-emphasis.

    Levels: 1.0 is the peak of a sine at Dolby level (the tape's 0 dB). Plain C++, no JUCE,
    so the models can be measured on their own (Tests/NoiseReductionTest.cpp).
*/
namespace tf::nr
{
// A one-pole filter in topology-preserving (trapezoidal) form: it stays well behaved when its
// corner moves every sample, and its output is linear in the input, so a decoder's feedback
// loop can be solved exactly.
struct OnePole
{
    float s = 0.0f, G = 0.0f;   // state; g / (1 + g), g = tan (pi fc / sr)

    void setCorner (double fc, double sr) noexcept
    {
        const auto g = std::tan (3.14159265358979 * std::fmin (fc, sr * 0.49) / sr);
        G = (float) (g / (1.0 + g));
    }
    // The high-pass output is a * x + b for the current sample: a = 1 - G, b = -(1 - G) * s.
    float hpA() const noexcept { return 1.0f - G; }
    float hpB() const noexcept { return -(1.0f - G) * s; }
    // Advance with the input that was actually used; returns the high-pass output.
    float tickHp (float x) noexcept
    {
        const auto v = (x - s) * G;
        const auto lp = v + s;
        s = lp + v;
        return x - lp;
    }
    float tickLp (float x) noexcept
    {
        const auto v = (x - s) * G;
        const auto lp = v + s;
        s = lp + v;
        return lp;
    }
    void reset() noexcept { s = 0.0f; }
};

/** One sliding-band stage (the B-type circuit; Dolby C is two of them). */
struct SlidingStage
{
    struct Design
    {
        float sideGain = 2.23f;       // the side path's gain at high frequencies, low level (1 + g = maximum lift)
        float fixedHz = 1620.0f;      // the fixed high-pass in the side path
        float thresholdDolby = 0.0f;  // side-path level (peak re Dolby level) above which the FET conducts
        float hzPerUnit = 0.0f;       // how far the variable corner moves per unit of control above threshold
        float lawPower = 1.0f;        // the FET's conductance against its control voltage above pinch-off
        float controlHz = 0.0f;       // the control amplifier's own high-pass (it listens hardest to the treble); 0 = flat
        float controlLowGain = 0.0f;  // how much of the control amplifier's input it hears below controlHz (a shelf, not a cut)
        bool slidingCorner = true;    // the FET's conductance adds to the fixed filter's: one corner that slides up
        float secondPole = 0.0f;      // with slidingCorner: a second high-pass sliding with it, at this ratio (the slope steepens)
        float attackMs = 100.0f, fastAttackMs = 1.0f, decayMs = 100.0f;
        float fastRatio = 2.0f;       // the side signal this far above the integrator takes the fast path (the diode)
        float clipDolby = 0.0f;       // the overshoot diodes: the side path can't exceed this (0 = none)
        // Anti-saturation (C high-level stage): a treble shelf in the main path.
        float antiSatHz = 0.0f, antiSatDb = 0.0f;
    };

    void prepare (const Design& d, double sampleRate, bool encoder) noexcept;
    void reset() noexcept;
    float encode (float x) noexcept;
    float decode (float y) noexcept;

    Design design;
    double sr = 48000.0;
    bool isEncoder = true;
    OnePole fixed, variable, controlHp;
    float env = 0.0f, quick = 0.0f, aAttack = 0, aFast = 0, aDecay = 0, aQuick = 0, lastCorner = -1.0f;
    // anti-saturation shelf in the main path: main = x - k * lp(x)
    OnePole shelf;
    float shelfK = 0.0f;

private:
    float sideOut (float sideIn, bool advance) noexcept;
    void control (float side) noexcept;
};

/** The 2:1 compander. */
struct Compander21
{
    struct Design
    {
        // Signal emphasis: a treble shelf, lifting by emphasisDb between its two corners.
        float emphasisLowHz = 1600.0f, emphasisHighHz = 6400.0f;
        // Detector: band-limited and treble-weighted, then RMS.
        float detectorHpHz = 30.0f, detectorLpHz = 10000.0f, detectorTiltLowHz = 400.0f, detectorTiltHighHz = 4000.0f;
        float rmsMs = 20.0f;          // the RMS detector's averaging time (sets the recovery rate in dB/s)
        float referenceDolby = 0.5f;  // the level (re Dolby level) that passes at unity gain
        float maxGainDb = 40.0f;      // the gain cell's range either way
    };

    void prepare (const Design& d, double sampleRate, bool encoder) noexcept;
    void reset() noexcept;
    float process (float x) noexcept;

    Design design;
    double sr = 48000.0;
    bool isEncoder = true;
    OnePole emphLp, emphLp2, detHp, detLp, tiltLp, tiltLp2;
    float emphK = 0.0f, tiltK = 0.0f, ms = 0.0f, aRms = 0.0f, ref2 = 0.25f, maxGain = 100.0f;
    float lastOut = 0.0f;

private:
    float detector (float v) noexcept;
};

/** Spectral skewing (Dolby C): a steep treble shelf down before the encoder, exactly back up
    after the decoder (the same filter inverted). */
struct Skew
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    bool active = false;
    void prepare (float cornerHz, float depthDb, float q, double sampleRate, bool isCut) noexcept;
    float process (float x) noexcept;
    void reset() noexcept { z1 = z2 = 0; }
};

/** A complete encoder or decoder: type 0 the 2:1 compander, 1 B, 2 C. */
class NoiseReduction
{
public:
    void configure (double sampleRate, int type, bool encoder) noexcept;
    void reset() noexcept;
    float process (float x) noexcept;   // x in engine units: tapeRefPeak = Dolby level
    int getType() const noexcept { return type; }

    static SlidingStage::Design bStage();
    static SlidingStage::Design cHighStage();
    static SlidingStage::Design cLowStage();
    static Compander21::Design twoToOne();
    static void cSkew (float& hz, float& db, float& q);

    static constexpr float dolbyPeak = 0.2512f;   // tapeRefPeak (Common.h)

private:
    int type = 1;
    bool isEncoder = true;
    SlidingStage b, cHigh, cLow;
    Skew skew;
    Compander21 comp;
};
} // namespace tf::nr
