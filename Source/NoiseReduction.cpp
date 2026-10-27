#include "NoiseReduction.h"

#include <algorithm>

namespace tf::nr
{
namespace
{
    constexpr double pi = 3.14159265358979;
    float coef (double ms, double sr) { return (float) std::exp (-1.0 / (std::max (1.0e-3, ms) * 0.001 * sr)); }
    float dbToGain (float db) { return std::pow (10.0f, db / 20.0f); }
}

//==============================================================================
void SlidingStage::prepare (const Design& d, double sampleRate, bool encoder) noexcept
{
    design = d;
    sr = sampleRate;
    isEncoder = encoder;
    fixed.setCorner (d.fixedHz, sr);
    variable.setCorner (d.slidingCorner ? 0.001 : 1.0, sr);
    if (d.controlHz > 0.0f) controlHp.setCorner (d.controlHz, sr);
    aAttack = coef (d.attackMs, sr);
    aFast = coef (d.fastAttackMs, sr);
    aDecay = coef (d.decayMs, sr);
    aQuick = coef (3.0, sr);
    if (d.antiSatHz > 0.0f && d.antiSatDb < 0.0f)
    {
        shelf.setCorner (d.antiSatHz, sr);
        shelfK = 1.0f - dbToGain (d.antiSatDb);
    }
    else
        shelfK = 0.0f;
    reset();
}

void SlidingStage::reset() noexcept
{
    fixed.reset();
    variable.reset();
    controlHp.reset();
    shelf.reset();
    env = quick = 0.0f;
    lastCorner = -1.0f;
    control (0.0f);
}

void SlidingStage::control (float side) noexcept
{
    // The rectifier and integrator: about 100 ms either way for small changes; a big step
    // through the diode in a millisecond or so.
    const auto r = std::abs (design.controlHz > 0.0f ? controlHp.tickHp (side) * (1.0f - design.controlLowGain) + side * design.controlLowGain : side);
    // A step well above where the integrator is (its average over a few ms, so a steady tone's
    // own ripple never counts) charges it in about a millisecond, as the diode does.
    quick = r + aQuick * (quick - r);
    float a = aDecay;
    if (r > env) a = quick > design.fastRatio * env ? aFast : aAttack;
    env = r + a * (env - r);

    // The FET: no conduction below the threshold, then a channel conductance (and so a corner
    // frequency for the 4.7 nF capacitor) that grows with the control voltage above it.
    const auto over = std::max (0.0f, env - design.thresholdDolby);
    const auto moved = design.hzPerUnit * std::pow (over, design.lawPower);
    const auto corner = design.slidingCorner ? design.fixedHz + moved : 1.0f + moved;
    if (std::abs (corner - lastCorner) > 0.002f * lastCorner || lastCorner < 0.0f)
    {
        (design.slidingCorner ? fixed : variable).setCorner (corner, sr);
        if (design.slidingCorner && design.secondPole > 0.0f)
            variable.setCorner (std::max (0.001f, design.secondPole * moved), sr);
        lastCorner = corner;
    }
}

float SlidingStage::encode (float x) noexcept
{
    // side path: fixed high-pass, variable high-pass, gain, overshoot diodes
    auto s = design.sideGain * variable.tickHp (fixed.tickHp (x));
    if (design.clipDolby > 0.0f) s = design.clipDolby * std::tanh (s / design.clipDolby);
    const auto main = shelfK > 0.0f ? x * (1.0f - shelfK) + shelfK * shelf.tickLp (x) : x;
    control (s);
    return main + s;
}

float SlidingStage::decode (float y) noexcept
{
    // The same side path in a negative feedback loop round the output: y = main(x) + side(x).
    // Everything but the diodes is linear in x for this sample, so solve it, then refine
    // through the diodes with Newton's method.
    const auto af = fixed.hpA(), bf = fixed.hpB();
    const auto av = variable.hpA(), bv = variable.hpB();
    const auto A = design.sideGain * av * af, B = design.sideGain * (av * bf + bv);
    float mA = 1.0f, mB = 0.0f;
    if (shelfK > 0.0f)
    {
        // main = (1 - k) x + k * lp(x), lp = G x + (1 - G) s
        mA = 1.0f - shelfK + shelfK * shelf.G;
        mB = shelfK * (1.0f - shelf.G) * shelf.s;
    }
    auto x = (y - B - mB) / (A + mA);
    if (design.clipDolby > 0.0f)
    {
        const auto c = design.clipDolby;
        for (int i = 0; i < 4; ++i)
        {
            const auto u = (A * x + B) / c;
            const auto t = std::tanh (u);
            const auto f = mA * x + mB + c * t - y;
            const auto df = mA + A * (1.0f - t * t);
            x -= f / df;
        }
    }
    // advance every filter with the solved input, as the circuit settles
    auto s = design.sideGain * variable.tickHp (fixed.tickHp (x));
    if (design.clipDolby > 0.0f) s = design.clipDolby * std::tanh (s / design.clipDolby);
    if (shelfK > 0.0f) shelf.tickLp (x);
    control (s);
    return x;
}

//==============================================================================
void Compander21::prepare (const Design& d, double sampleRate, bool encoder) noexcept
{
    design = d;
    sr = sampleRate;
    isEncoder = encoder;
    // Emphasis: a first-order treble shelf between two corners, as a passive network gives.
    emphLp.setCorner (d.emphasisLowHz, sr);
    emphK = (float) (d.emphasisHighHz / d.emphasisLowHz);
    detHp.setCorner (d.detectorHpHz, sr);
    detLp.setCorner (d.detectorLpHz, sr);
    tiltLp.setCorner (d.detectorTiltLowHz, sr);
    tiltK = (float) (d.detectorTiltHighHz / d.detectorTiltLowHz);
    aRms = coef (d.rmsMs, sr);
    ref2 = d.referenceDolby * d.referenceDolby * 0.5f;   // mean square of a sine at the reference
    maxGain = dbToGain (d.maxGainDb);
    reset();
}

void Compander21::reset() noexcept
{
    emphLp.reset(); detHp.reset(); detLp.reset(); tiltLp.reset();
    ms = ref2 * 1.0e-6f;
    lastOut = 0.0f;
}

float Compander21::detector (float v) noexcept
{
    // band-limited, treble-weighted: the rumble and the ultrasonics the tape won't carry
    // don't steer the gain; the treble, where the tape is least certain, steers it most
    auto w = detLp.tickLp (detHp.tickHp (v));
    const auto lp = tiltLp.tickLp (w);
    w = lp + (w - lp) * tiltK;          // a first-order shelf up by tiltK
    ms = w * w + aRms * (ms - w * w);
    return std::max (ms, 1.0e-20f);
}

float Compander21::process (float x) noexcept
{
    // Both detectors hear the encoded signal (the compressor's output, the expander's input), so
    // the expander's gain is always exactly the inverse of the compressor's. With the detector on
    // its output, a gain of (level / reference)^-1/2 there makes the compressor 2:1.
    const auto m = std::max (ms, 1.0e-20f);   // what the detector has heard up to the last sample
    const auto gEnc = std::clamp (std::pow (m / ref2, -0.5f), 1.0f / maxGain, maxGain);
    if (isEncoder)
    {
        // emphasis: lp + (x - lp) * k, a shelf up by emphasisHigh / emphasisLow
        const auto lp = emphLp.tickLp (x);
        const auto e = lp + (x - lp) * emphK;
        const auto y = e * gEnc;
        detector (y);
        return y;
    }
    // decode: the inverse gain, then the inverse emphasis
    const auto v = x / gEnc;
    detector (x);
    // de-emphasis, the exact inverse of the shelf: e = lp + (x - lp) k  =>  x = (e + (k - 1) lp) / k,
    // with lp = G x + (1 - G) s, solved for x
    const auto G = emphLp.G, s = emphLp.s;
    const auto out = (v + (emphK - 1.0f) * (1.0f - G) * s) / (emphK - (emphK - 1.0f) * G);
    emphLp.tickLp (out);
    return out;
}

//==============================================================================
void Skew::prepare (float cornerHz, float depthDb, float q, double sampleRate, bool isCut) noexcept
{
    active = cornerHz > 0.0f && std::abs (depthDb) > 0.01f;
    reset();
    if (! active) return;
    // RBJ high shelf, down by depthDb: minimum phase, so its inverse (numerator and denominator
    // swapped) is stable and undoes it exactly.
    const double A = std::pow (10.0, -std::abs (depthDb) / 40.0), w = 2.0 * pi * cornerHz / sampleRate;
    const double cs = std::cos (w), sn = std::sin (w), alpha = sn / (2.0 * q), sa = 2.0 * std::sqrt (A) * alpha;
    double nb0 = A * ((A + 1) + (A - 1) * cs + sa), nb1 = -2 * A * ((A - 1) + (A + 1) * cs), nb2 = A * ((A + 1) + (A - 1) * cs - sa);
    double na0 = (A + 1) - (A - 1) * cs + sa, na1 = 2 * ((A - 1) - (A + 1) * cs), na2 = (A + 1) - (A - 1) * cs - sa;
    if (! isCut) { std::swap (nb0, na0); std::swap (nb1, na1); std::swap (nb2, na2); }
    b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0; a1 = na1 / na0; a2 = na2 / na0;
}

float Skew::process (float x) noexcept
{
    if (! active) return x;
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return (float) y;
}

//==============================================================================
SlidingStage::Design NoiseReduction::bStage()
{
    // Fitted to Dolby Laboratories' average encoder response (NE645/646 data sheet: 13 frequencies
    // from 200 Hz to 20 kHz, 9 levels from Dolby level to -40 dB): 0.37 dB RMS, nowhere 1 dB out.
    // The low-level response is one high-pass at 1.59 kHz with 10.5 dB of lift (Wireless World's
    // Dolby B circuit: 1.5 kHz); the timing is the published one (attack 1-100 ms by the size of
    // the step, decay 100 ms).
    SlidingStage::Design d;
    d.sideGain = 2.4415f;
    d.fixedHz = 1588.5f;
    d.thresholdDolby = 0.014320f;
    d.hzPerUnit = 92088.76f;
    d.lawPower = 0.383346f;
    d.controlHz = 12385.28f;
    d.controlLowGain = 0.242615f;
    d.slidingCorner = true;
    d.secondPole = 0.0f;
    d.attackMs = 100.0f;
    d.fastAttackMs = 1.0f;
    d.decayMs = 100.0f;
    d.clipDolby = 0.0f;
    return d;
}

// Dolby C: two of the B-type stage in series, reaching two octaves further down (corner 351 Hz),
// the low-level stage working 20 dB below the high-level one, and anti-saturation in the
// high-level stage's main path. Fitted to the Philips TEA0665's record-mode test points (all six
// inside its limits) and Dolby's description: about 15 dB near 400 Hz, 20 dB from 2 to 10 kHz,
// starting about 100 Hz.
SlidingStage::Design NoiseReduction::cHighStage()
{
    auto d = bStage();
    d.sideGain = 2.27804f;
    d.fixedHz = 351.04f;
    d.thresholdDolby = 0.0171922f;
    d.hzPerUnit = 97332.0f;
    d.antiSatHz = 4078.6f;
    d.antiSatDb = -5.98f;
    return d;
}

SlidingStage::Design NoiseReduction::cLowStage()
{
    auto d = bStage();
    d.sideGain = 2.27804f;
    d.fixedHz = 351.04f;
    d.thresholdDolby = 0.0016734f;
    d.hzPerUnit = 61820.1f;
    return d;
}

Compander21::Design NoiseReduction::twoToOne()
{
    // The 2:1 compander as dbx type II is built: a feed-forward 2:1 law steered by an RMS detector
    // that hears a band-limited, treble-weighted copy, with fixed treble emphasis on the signal.
    // What's published is the architecture and the recovery rate (60 to 200 dB/s for
    // unobtrusive compression, Blackmer's patent; 125 dB/s here). dbx never published the
    // emphasis networks' values; these are chosen to give type II's character (strong treble
    // emphasis, a detector that ignores rumble and ultrasonics). See docs/noise-reduction.md.
    Compander21::Design d;
    d.emphasisLowHz = 1600.0f;      // +12 dB shelf, 1.6 to 6.4 kHz
    d.emphasisHighHz = 6400.0f;
    d.detectorHpHz = 30.0f;
    d.detectorLpHz = 10000.0f;
    d.detectorTiltLowHz = 200.0f;   // +20 dB of treble weighting, 200 Hz to 2 kHz
    d.detectorTiltHighHz = 2000.0f;
    d.rmsMs = 34.7f;                // mean square decays at 4.343 / tau dB/s: 125 dB/s
    d.referenceDolby = 1.0f;        // Dolby level passes at unity gain
    d.maxGainDb = 40.0f;
    return d;
}

void NoiseReduction::cSkew (float& hz, float& db, float& q)
{
    // Spectral skewing: a selective cut above about 10 kHz (Dolby's description); the decoder
    // puts it back. Its exact depth isn't published: 10 dB, centred so 10 kHz is barely touched.
    hz = 15000.0f;
    db = 10.0f;
    q = 1.0f;
}

void NoiseReduction::configure (double sampleRate, int newType, bool encoder) noexcept
{
    type = std::clamp (newType, 0, 2);
    isEncoder = encoder;
    b.prepare (bStage(), sampleRate, encoder);
    cHigh.prepare (cHighStage(), sampleRate, encoder);
    cLow.prepare (cLowStage(), sampleRate, encoder);
    float hz, db, q;
    cSkew (hz, db, q);
    skew.prepare (hz, db, q, sampleRate, encoder);
    comp.prepare (twoToOne(), sampleRate, encoder);
}

void NoiseReduction::reset() noexcept
{
    b.reset(); cHigh.reset(); cLow.reset(); skew.reset(); comp.reset();
}

float NoiseReduction::process (float x) noexcept
{
    const auto v = x / dolbyPeak;
    float out;
    if (type == 0)
        out = comp.process (v);
    else if (type == 1)
        out = isEncoder ? b.encode (v) : b.decode (v);
    else if (isEncoder)
        out = cLow.encode (cHigh.encode (skew.process (v)));
    else
        out = skew.process (cHigh.decode (cLow.decode (v)));
    return out * dolbyPeak;
}
} // namespace tf::nr
