#include "TapeEngine.h"

#include <complex>

namespace tf
{
//==============================================================================
void Compander::configure (double sampleRate, int newType, bool encoder)
{
    type = juce::jlimit (0, 2, newType);
    isEncoder = encoder;
    atk = (float) std::exp (-1.0 / (0.002 * sampleRate));
    rel = (float) std::exp (-1.0 / (0.060 * sampleRate));

    if (type == 0)
    {
        emphasis.setHighShelf (sampleRate, 2000.0, encoder ? 10.0 : -10.0);
        ref = 0.1f;
        maxBoost = 31.6f;
    }
    else
    {
        auto fc = type == 1 ? 1500.0 : 700.0;
        lpCoef = (float) std::exp (-juce::MathConstants<double>::twoPi * fc / sampleRate);
        ref = 0.03f;
        maxBoost = type == 1 ? 3.16f : 10.0f;
    }
    reset();
}

void Compander::reset() noexcept
{
    env2 = ref * ref;
    lpState = 0;
    emphasis.reset();
}

inline float Compander::detect (float v) noexcept
{
    auto sq = v * v;
    auto c = sq > env2 ? atk : rel;
    env2 = sq + c * (env2 - sq);
    return std::sqrt (env2) + 1.0e-9f;
}

float Compander::process (float x) noexcept
{
    if (type == 0)
    {
        if (isEncoder)
        {
            auto xe = emphasis.process (x);
            auto g = juce::jlimit (1.0f / maxBoost, maxBoost, 1.0f / std::sqrt (detect (xe) / ref));
            return xe * g;
        }

        auto g = juce::jlimit (0.001f, maxBoost, detect (x) / ref);
        return emphasis.process (x * g);
    }

    lpState = x * (1.0f - lpCoef) + lpState * lpCoef;
    auto hf = x - lpState;
    auto e = detect (hf);

    if (isEncoder)
        return lpState + hf * juce::jlimit (1.0f, maxBoost, 1.0f / std::sqrt (e / ref));

    return lpState + hf * juce::jlimit (1.0f / maxBoost, 1.0f, e / ref);
}

//==============================================================================
namespace
{
    // The hiss generator: half white, half pink.
    inline float hissSample (Noise& n, float* p) noexcept
    {
        auto w = n.next();
        p[0] = 0.99765f * p[0] + w * 0.0990460f;
        p[1] = 0.96300f * p[1] + w * 0.2965164f;
        p[2] = 0.57000f * p[2] + w * 1.0526913f;
        auto pk = (p[0] + p[1] + p[2] + w * 0.1848f) * 0.18f;
        return 0.5f * w + 0.5f * pk;
    }

    // IEC 61672 A-weighting, as a cascade, normalised to 0 dB at 1 kHz by the caller.
    struct AWeight
    {
        Biquad hp20a, lp12k;
        float a1 = 0, a2 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;   // two first-order high-passes
        explicit AWeight (double sr)
        {
            hp20a.setHighPass (sr, 20.6, 0.5);
            lp12k.setLowPass (sr, 12194.0, 0.5);
            a1 = (float) std::exp (-juce::MathConstants<double>::twoPi * 107.7 / sr);
            a2 = (float) std::exp (-juce::MathConstants<double>::twoPi * 737.9 / sr);
        }
        float process (float x) noexcept
        {
            x = hp20a.process (x);
            auto h1 = a1 * (y1 + x - x1); x1 = x; y1 = h1;
            auto h2 = a2 * (y2 + h1 - x2); x2 = h1; y2 = h2;
            return lp12k.process (h2);
        }
    };

    float aWeightedRms (double sr, const std::function<float()>& source)
    {
        AWeight a (sr);
        const int n = (int) (sr * 3.0), skip = (int) (sr * 0.5);
        double sum = 0;
        for (int i = 0; i < n; ++i)
        {
            auto y = a.process (source());
            if (i >= skip) sum += (double) y * y;
        }
        return (float) std::sqrt (sum / (n - skip));
    }
}

float TapeEngine::magnitudeAt (const Biquad& b, double f, double sampleRate)
{
    auto w = juce::MathConstants<double>::twoPi * f / sampleRate;
    std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
    auto num = (double) b.b0 + (double) b.b1 * z1 + (double) b.b2 * z2;
    auto den = 1.0 + (double) b.a1 * z1 + (double) b.a2 * z2;
    return (float) std::abs (num / den);
}

float TapeEngine::tanhThirdHarmonicPoint()
{
    static const float point = []
    {
        auto h3ratio = [] (double a)
        {
            const int n = 4096;
            double re1 = 0, im1 = 0, re3 = 0, im3 = 0;
            for (int i = 0; i < n; ++i)
            {
                auto ph = juce::MathConstants<double>::twoPi * i / n;
                auto y = std::tanh (a * std::sin (ph));
                re1 += y * std::cos (ph);       im1 += y * std::sin (ph);
                re3 += y * std::cos (3 * ph);   im3 += y * std::sin (3 * ph);
            }
            return std::hypot (re3, im3) / std::hypot (re1, im1);
        };
        double lo = 0.05, hi = 2.0;
        for (int k = 0; k < 60; ++k)
        {
            auto mid = 0.5 * (lo + hi);
            (h3ratio (mid) < 0.03 ? lo : hi) = mid;
        }
        return (float) (0.5 * (lo + hi));
    }();
    return point;
}

void TapeEngine::calibrateHiss()
{
    for (int eq = 0; eq < 2; ++eq)
    {
        Noise n (12345u);
        float p[3] {};
        Biquad shape;
        if (eq == 1) shape.setHighShelf (sr, 2200.0, -4.7);   // 70 us playback EQ: less treble hiss than 120 us
        auto rms = aWeightedRms (sr, [&] { return shape.process (hissSample (n, p)); });

        // Normalise to the A-weighting's own gain at 1 kHz.
        double ph = 0;
        auto ref = aWeightedRms (sr, [&] { ph += juce::MathConstants<double>::twoPi * 1000.0 / sr; return (float) std::sin (ph); });
        hissCal[eq] = std::max (1.0e-6f, rms / (ref * std::sqrt (2.0f)));
    }
}

TapeEngine::BiasEffect TapeEngine::biasEffect (int iecType, int switchPos) noexcept
{
    // Bias current per position, relative to NORMAL: Type II tapes take about 150%, Type IV about 250%.
    static constexpr float biasLevelDb[] { 0.0f, 3.5f, 8.0f };
    auto d = biasLevelDb[juce::jlimit (0, 2, switchPos)] - biasLevelDb[positionForIec (iecType)];
    BiasEffect e { d, 0.0f, 0.0f, 0.0f, 0.0f, iecType >= 4 && switchPos == 0 };
    if (d > 0.0f)
    {
        // Over-bias: the field reaches deeper and wipes the short wavelengths. Dull, a little cleaner.
        e.treble10k = std::max (-16.0f, -2.0f * d);
        e.midLevel  = -0.25f * d;
        e.mol       = std::min (1.5f, 0.25f * d);
    }
    else if (d < 0.0f)
    {
        // Under-bias: treble rises towards its peak and then falls; distortion and grain rise fast.
        auto u = -d;
        e.treble10k = u <= 4.0f ? 0.75f * u : 3.0f - (u - 4.0f);
        e.midLevel  = u > 3.0f ? -0.5f * (u - 3.0f) : 0.0f;
        e.mol       = -1.5f * u;
        e.extraModNoise = 1.2f * u;
    }
    return e;
}

void TapeEngine::setEqMismatch (Biquad& b, double fs, double recUs, double playUs) noexcept
{
    // The playback amplifier's response is (1 + s tau) / s: playing with tauP what wanted tauR
    // leaves (1 + s tauP) / (1 + s tauR). Bilinear transform, first order.
    auto k = 2.0 * fs, tp = playUs * 1.0e-6, tr = recUs * 1.0e-6;
    b.set (1.0 + tp * k, 1.0 - tp * k, 0.0, 1.0 + tr * k, 1.0 - tr * k, 0.0);
}

/** Sets up everything that comes from the cassette itself (headroom, treble saturation, noise)
    and from the tape switch while recording (bias). */
void TapeEngine::configureModel (int m, int speedSel, int switchPos)
{
    const auto& tm = tapeModels[juce::jlimit (0, numTapeModels - 1, m)];
    const auto bias = biasEffect (tm.iecType, switchPos);

    // tanh(k x) / k reaches 3% third harmonic at the tape's MOL (400 Hz), moved by any bias mismatch.
    auto molPeak = tapeRefPeak * dbToGain (tm.mol400 + bias.mol);
    satK = tanhThirdHarmonicPoint() / molPeak;

    // Full saturation of tanh sits this far above its 3% point...
    auto satHeadroomDb = 20.0f * std::log10 (4.0f / (juce::MathConstants<float>::pi * tanhThirdHarmonicPoint()));
    // ...so treble is lifted into the saturator by enough to make 10 kHz saturate at the tape's SOL,
    // and bass by enough to make 100 Hz reach 3% at its MOL. Shorter wavelengths at the slower speed
    // are what limit treble, so at 3 3/4 ips the lift starts an octave higher.
    auto hiTarget = std::max (0.0f, tm.mol400 + satHeadroomDb - tm.sol10k);
    auto loTarget = std::max (0.0f, tm.mol400 - tm.mol100);
    auto hiFc = std::min (4000.0 * speedSel, sr * 0.2);

    auto fit = [this] (bool high, double fc, double f, float targetDb)
    {
        Biquad b;
        double g = targetDb;
        for (int it = 0; it < 8 && targetDb != 0.0f; ++it)
        {
            if (high) b.setHighShelf (sr, fc, g, 0.7); else b.setLowShelf (sr, fc, g, 0.7);
            auto got = 20.0 * std::log10 (magnitudeAt (b, f, sr));
            g += targetDb - got;
        }
        return targetDb != 0.0f ? g : 0.0;
    };
    auto hiDb = fit (true, hiFc, 10000.0, hiTarget);
    auto loDb = fit (false, 160.0, 100.0, loTarget);

    for (int t = 0; t < numTracks; ++t)
    {
        emphHi[t].setHighShelf (sr, hiFc, hiDb, 0.7);
        deemphHi[t].setHighShelf (sr, hiFc, -hiDb, 0.7);
        emphLo[t].setLowShelf (sr, 160.0, loDb, 0.7);
        deemphLo[t].setLowShelf (sr, 160.0, -loDb, 0.7);
        for (auto* b : { &emphHi[t], &deemphHi[t], &emphLo[t], &deemphLo[t] }) b->reset();
        auto biasDb = fit (true, hiFc, 10000.0, bias.treble10k);
        biasHi[t].setHighShelf (sr, hiFc, biasDb, 0.7);
        biasHi[t].reset();
        if (tm.eqMicroseconds == 70) hissShape[t].setHighShelf (sr, 2200.0, -4.7);
        else                         hissShape[t] = Biquad();
    }

    sensGain = dbToGain (tm.sensitivity);
    modDepth = dbToGain (tm.modNoise);
    biasMidGain = dbToGain (bias.midLevel);
    recModDepth = bias.extraModNoise > 0.0f
                      ? std::sqrt (std::max (0.0f, std::pow (10.0f, (tm.modNoise + bias.extraModNoise) / 10.0f) - std::pow (10.0f, tm.modNoise / 10.0f)))
                      : 0.0f;
    eraseBleed = bias.eraseBleed;
    cfgModel = m;
    cfgSpeed = speedSel;
    cfgPos = switchPos;
}

void TapeEngine::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    maxBlock = std::max (maxBlockSize, 4096);
    posBuf.assign ((size_t) maxBlock, 0.0);
    playing.assign ((size_t) maxBlock, 0);
    recording.assign ((size_t) maxBlock, 0);
    for (auto& r : raw) r.assign ((size_t) maxBlock, 0.0f);

    companderType = -1;
    lastHfCut = lastAaCut = -1;
    cfgModel = cfgSpeed = cfgPos = -1;
    hissEqKey = -1;
    for (auto& k : sigEqKey) k = -1;
    calibrateHiss();

    // Modulation noise: a slow random wobble of the signal's level, unit RMS.
    auto a = std::exp (-juce::MathConstants<double>::twoPi * 1000.0 / sr);
    modCoef = (float) a;
    modNorm = (float) (1.0 / std::sqrt ((1.0 - a) / (1.0 + a) / 3.0));
    for (int t = 0; t < numTracks; ++t)
    {
        hasPrev[t] = false;
        for (auto& b : hfLoss[t]) b.reset();
        for (auto& b : antiAlias[t]) b.reset();
        for (auto& b : preAA[t]) b.reset();
        modLp[t] = 0;
    }
}

void TapeEngine::setTape (Tape* t, bool newTape) noexcept
{
    tape = t;
    pos = t != nullptr ? t->position.load() : 0.0;
    // A tape going in sets the switch to its own type; moving it after that is up to you.
    if (t != nullptr && newTape) params.tapeSwitch = t->naturalPosition();
    windVel = 0;
    mode = (int) Mode::stop;
    rtz = false;
    for (auto& h : hasPrev) h = false;
    for (auto& c : replacedCount) c = 0;
}

void TapeEngine::configureCompanders (int type)
{
    for (int t = 0; t < numTracks; ++t)
    {
        encoder[t].configure (sr, type, true);
        decoder[t].configure (sr, type, false);
    }
    companderType = type;
}

void TapeEngine::beginBlock (int n, const bool* armed) noexcept
{
    n = std::min (n, maxBlock);
    bool anyArmed = false;
    for (int t = 0; t < numTracks; ++t) { armedNow[t] = armed[t]; anyArmed = anyArmed || armed[t]; }

    if (tape == nullptr)
    {
        mode = (int) Mode::stop;
        std::fill_n (playing.begin(), n, (uint8_t) 0);
        std::fill_n (recording.begin(), n, (uint8_t) 0);
        blockHasPlayback = blockHasRecording = false;
        return;
    }

    auto m = (Mode) mode.load();
    switch ((Command) pendingCommand.exchange (0))
    {
        case Command::play: m = Mode::play; rtz = false; break;
        case Command::stop: m = Mode::stop; rtz = false; break;
        case Command::rec:
            if (anyArmed) { m = Mode::rec; rtz = false; }
            else noArmWarning = true;
            break;
        case Command::ff:   m = Mode::ff;  rtz = false; break;
        case Command::rew:  m = Mode::rew; rtz = false; break;
        case Command::rtz:
            if (pos > tape->counterZero.load() + 1.0) { m = Mode::rew; rtz = true; }
            break;
        case Command::none: default: break;
    }

    if (m == Mode::rec && ! anyArmed)
        m = Mode::play;

    auto ct = juce::jlimit (0, 2, params.nrType.load());
    if (ct != companderType)
        configureCompanders (ct);

    auto spdSel = juce::jlimit (1, 2, params.speedSel.load());
    auto sw = juce::jlimit (0, 2, params.tapeSwitch.load());
    if (tape->getModel() != cfgModel || spdSel != cfgSpeed || sw != cfgPos)
        configureModel (tape->getModel(), spdSel, sw);

    // Entering record: start the encoders fresh, like a punch-in.
    if (m == Mode::rec && lastMode != (int) Mode::rec)
        for (int t = 0; t < numTracks; ++t) { encoder[t].reset(); hasPrev[t] = false; }

    auto speedSel = juce::jlimit (1, 2, params.speedSel.load());
    auto nominal = speedSel * (1.0 + juce::jlimit (-5, 5, params.pitchStep.load()) * 0.024);
    nominalSpeed = (float) nominal;
    auto advNom = nominal * cellRate / sr;
    for (auto& w : wroteThisBlock) w = false;

    auto wowD = params.wow.load() * 0.005;
    auto flD  = params.flutter.load() * 0.003;
    auto drD  = params.drift.load() * 0.01;
    auto windTarget = 24.0 * cellRate / sr;
    auto windSmooth = 1.0 - std::exp (-1.0 / (0.35 * sr));
    auto len = (double) tape->getLengthCells();
    auto zero = tape->counterZero.load();

    const double inc[4] { 0.55 / sr, 1.3 / sr, 6.3 / sr, 11.7 / sr };
    blockHasPlayback = blockHasRecording = false;

    for (int i = 0; i < n; ++i)
    {
        if (m == Mode::play || m == Mode::rec)
        {
            for (int k = 0; k < 4; ++k) { ph[k] += inc[k]; if (ph[k] >= 1.0) ph[k] -= 1.0; }

            if (--driftCounter <= 0)
            {
                driftCounter = (int) (sr * 2.0);
                driftTarget = rng.next() * drD;
            }
            driftVal += (driftTarget - driftVal) * 2.0e-5;

            auto tp = juce::MathConstants<double>::twoPi;
            auto mod = 1.0 + wowD * (0.7 * std::sin (tp * ph[0]) + 0.3 * std::sin (tp * ph[1]))
                           + flD * (0.6 * std::sin (tp * ph[2]) + 0.4 * std::sin (tp * ph[3]))
                           + driftVal;
            pos += advNom * mod;
            windVel = 0;
            playing[(size_t) i] = 1;
            recording[(size_t) i] = m == Mode::rec ? 1 : 0;
            blockHasPlayback = true;
            blockHasRecording = blockHasRecording || m == Mode::rec;
        }
        else
        {
            auto target = m == Mode::ff ? windTarget : (m == Mode::rew ? -windTarget : 0.0);
            windVel += (target - windVel) * windSmooth;
            if (m == Mode::stop) windVel = 0;
            pos += windVel;
            playing[(size_t) i] = 0;
            recording[(size_t) i] = 0;
        }

        if (pos >= len)
        {
            pos = len;
            if (m != Mode::stop) { m = Mode::stop; endOfTape = true; }
        }
        if (pos <= 0.0)
        {
            pos = 0.0;
            if (m == Mode::rew) m = Mode::stop;
        }
        if (rtz.load() && m == Mode::rew && pos <= zero)
        {
            pos = zero;
            m = Mode::stop;
            rtz = false;
        }

        posBuf[(size_t) i] = pos;
    }

    if (m == Mode::stop) windVel = 0;
    mode = (int) m;
}

void TapeEngine::readAll (float* const* out, int n) noexcept
{
    n = std::min (n, maxBlock);

    for (int t = 0; t < numTracks; ++t)
    {
        auto* r = raw[t].data();
        for (int i = 0; i < n; ++i)
            r[i] = (tape != nullptr && playing[(size_t) i]) ? tape->readInterpolated (t, posBuf[(size_t) i]) : 0.0f;
    }

    // Character settings for this block
    auto speedSel = params.speedSel.load();
    const auto* tm = tape != nullptr ? &tape->getModelInfo() : &tapeModels[defaultTapeModel];
    auto xt = params.xtalk.load();
    auto xtGain = xt > 0.0f ? dbToGain (-80.0f + xt * 50.0f) : 0.0f;

    // Hiss at the tape's measured A-weighted level below Dolby level, trimmed by the HISS control.
    // The faster speed spreads the same noise over a wider band: about 3 dB less in the audible range.
    auto hissAmt = params.hiss.load();
    auto refRms = tapeRefPeak / std::sqrt (2.0f);
    auto hissGain = hissAmt > 0.0f
                        ? refRms * dbToGain (-tm->snr + hissOffsetDb (hissAmt) - (speedSel == 2 ? 3.0f : 0.0f))
                              / hissCal[tm->eqMicroseconds == 70 ? 1 : 0]
                        : 0.0f;
    auto hfAmt = params.hf.load();
    auto hfCut = 20000.0f * std::pow (0.25f, hfAmt) * (speedSel == 2 ? 1.6f : 1.0f);
    hfCut = std::min (hfCut, (float) (sr * 0.45));
    if (std::abs (hfCut - lastHfCut) > 1.0f)
    {
        for (auto& tr : hfLoss) { tr[0].setLowPass (sr, hfCut, 0.54); tr[1].setLowPass (sr, hfCut, 1.31); }
        lastHfCut = hfCut;
    }
    auto drop = params.drop.load();
    auto dropProb = drop * drop * 2.0f / (float) sr;
    auto nrOn = params.nrOn.load();

    // Playback EQ: the switch picks 120 or 70 us. Hiss follows the tape's own formulation,
    // the recording follows the position it was recorded at.
    auto sw = juce::jlimit (0, 2, params.tapeSwitch.load());
    auto eqUs = [] (int pos) { return pos == 0 ? 120 : 70; };
    const int playUs = eqUs (sw);
    {
        auto key = tm->eqMicroseconds * 1000 + playUs;
        if (key != hissEqKey)
        {
            for (auto& b : hissEq) setEqMismatch (b, sr, tm->eqMicroseconds, playUs);
            hissEqKey = key;
        }
    }

    for (int t = 0; t < numTracks; ++t)
    {
        auto* o = out[t];
        auto* r = raw[t].data();
        const float* left  = t > 0 ? raw[t - 1].data() : nullptr;
        const float* right = t < numTracks - 1 ? raw[t + 1].data() : nullptr;

        for (int i = 0; i < n; ++i)
        {
            if (! playing[(size_t) i])
            {
                o[i] = 0.0f;
                continue;
            }

            auto v = r[i];
            if (xtGain > 0.0f)
                v += xtGain * ((left != nullptr ? left[i] : 0.0f) + (right != nullptr ? right[i] : 0.0f));

            // modulation noise rides on the signal; sensitivity sets the tape's output level
            modLp[t] = modRng[t].next() * (1.0f - modCoef) + modLp[t] * modCoef;
            v *= sensGain * (1.0f + modDepth * modNorm * modLp[t]);

            // playback EQ against the EQ this stretch was recorded with
            if (tape != nullptr)
            {
                auto recUs = eqUs (tape->recordedPosition (t, tape->blockAt (posBuf[(size_t) i])));
                auto key = recUs * 1000 + playUs;
                if (key != sigEqKey[t]) { setEqMismatch (sigEq[t], sr, recUs, playUs); sigEqKey[t] = key; }
                if (recUs != playUs) v = sigEq[t].process (v);
            }

            // dropouts
            if (dropProb > 0.0f && dropTimer[t] <= 0 && hissNoise[t].uniform01() < dropProb)
            {
                dropTarget[t] = 0.08f + 0.3f * rng.uniform01();
                dropTimer[t] = (int) ((0.015 + 0.06 * rng.uniform01()) * sr);
            }
            if (dropTimer[t] > 0 && --dropTimer[t] == 0)
                dropTarget[t] = 1.0f;
            dropGain[t] += (dropTarget[t] - dropGain[t]) * 0.01f;
            v *= dropGain[t];

            // hiss: half white, half pink
            if (hissGain > 0.0f)
            {
                auto h = hissShape[t].process (hissSample (hissNoise[t], pink[t]));
                v += hissGain * (tm->eqMicroseconds != playUs ? hissEq[t].process (h) : h);
            }

            v = hfLoss[t][1].process (hfLoss[t][0].process (v));

            if (nrOn)
                v = decoder[t].process (v);

            o[i] = v;
        }
    }
}

void TapeEngine::writeTrack (int t, const float* in, int n, double latencySamples) noexcept
{
    if (tape == nullptr || ! blockHasRecording)
        return;
    const double latencyCells = latencySamples * nominalSpeed.load() * cellRate / sr;

    n = std::min (n, maxBlock);
    auto nrOn = params.nrOn.load();
    auto drive = dbToGain (driveDb (params.sat.load()));
    auto invDrive = 1.0f / drive;
    auto spd = nominalSpeed.load();
    auto k = satK, invK = 1.0f / satK;
    auto sw = cfgPos;

    auto aaCut = (float) std::min (0.45 * spd * cellRate, 0.45 * sr);
    if (std::abs (aaCut - lastAaCut) > 1.0f)
    {
        for (auto& tr : antiAlias) { tr[0].setLowPass (sr, aaCut, 0.54); tr[1].setLowPass (sr, aaCut, 1.31); }
        for (auto& tr : preAA)     { tr[0].setLowPass (sr, aaCut, 0.54); tr[1].setLowPass (sr, aaCut, 1.31); }
        lastAaCut = aaCut;
    }

    for (int i = 0; i < n; ++i)
    {
        if (! (recording[(size_t) i] && armedNow[t]))
        {
            if (hasPrev[t]) finishPunch (t);
            continue;
        }

        auto v = in[i];
        if (nrOn)
            v = encoder[t].process (v);

        // The tape: record EQ lifts bass and treble into the magnetic saturation, playback takes them back out.
        v = preAA[t][1].process (preAA[t][0].process (v * drive));
        v = emphHi[t].process (emphLo[t].process (v));
        v = std::tanh (k * v) * invK;
        v = deemphLo[t].process (deemphHi[t].process (v)) * invDrive;

        // bias against this tape: treble and level, and the grain of under-bias
        v = biasHi[t].process (v) * biasMidGain;
        if (recModDepth > 0.0f)
        {
            recModLp[t] = modRng[t].next() * (1.0f - modCoef) + recModLp[t] * modCoef;
            v *= 1.0f + recModDepth * modNorm * recModLp[t];
        }
        v = antiAlias[t][1].process (antiAlias[t][0].process (v));

        writeSpan (t, posBuf[(size_t) i] - latencyCells, v, nrOn, spd, sw);
    }
}

void TapeEngine::writeSpan (int t, double p, float v, bool nr, float spd, int pos) noexcept
{
    wroteThisBlock[t] = true;

    if (! hasPrev[t] || p <= prevPos[t] || p - prevPos[t] > 64.0)
    {
        // A new run of recording: a punch-in (or the head jumped, which counts as one).
        if (hasPrev[t]) finishPunch (t);
        eraseLp[t] = 0.0f;
        prevPos[t] = p;
        prevVal[t] = v;
        hasPrev[t] = true;
        runCells[t] = 0;
        replacedCount[t] = 0;
        return;
    }

    runNr[t] = nr;
    runSpeed[t] = spd;
    runPos[t] = pos;
    const auto fadeCells = std::max (1.0, punchFadeSeconds * cellRate * spd);
    // A NORMAL-position erase head can't fully erase metal tape: the old recording,
    // mostly its bass, survives about 30 dB down under the new one.
    const float bleedCoef = eraseBleed ? std::exp (-juce::MathConstants<float>::twoPi * 250.0f / (cellRate * std::max (0.5f, spd))) : 0.0f;

    auto a = prevPos[t], b = p;
    auto c0 = (int64_t) std::floor (a) + 1, c1 = (int64_t) std::floor (b);
    for (auto c = c0; c <= c1; ++c)
    {
        auto f = (float) (((double) c - a) / (b - a));
        auto val = prevVal[t] + (v - prevVal[t]) * f;
        const auto old = tape->readCell (t, c);
        if (eraseBleed)
        {
            eraseLp[t] = old * (1.0f - bleedCoef) + eraseLp[t] * bleedCoef;
            val += 0.0316f * eraseLp[t];
        }

        // Fade in over what was on the tape.
        if ((double) runCells[t] < fadeCells)
        {
            auto g = (float) (((double) runCells[t] + 1.0) / fadeCells);
            val = old + (val - old) * g;
        }
        ++runCells[t];

        auto& r = replaced[t][replacedHead[t]];
        r.cell = c;
        r.old = old;
        replacedHead[t] = (replacedHead[t] + 1) % punchRing;
        replacedCount[t] = std::min (replacedCount[t] + 1, punchRing);

        tape->writeCell (t, c, val, nr, spd, pos);
    }
    prevPos[t] = b;
    prevVal[t] = v;
}

void TapeEngine::finishPunch (int t) noexcept
{
    // Punch-out: blend the end of the new recording back into what it replaced.
    hasPrev[t] = false;
    if (tape == nullptr || replacedCount[t] == 0)
        return;

    const auto fadeCells = (int) std::max (1.0, punchFadeSeconds * cellRate * runSpeed[t]);
    const auto k = std::min ({ fadeCells, replacedCount[t], (int) std::min<int64_t> (runCells[t], punchRing) });
    for (int j = 0; j < k; ++j)
    {
        // j = 0 is the last cell written: almost all old; j = k-1 is almost all new.
        const auto& r = replaced[t][(replacedHead[t] - 1 - j + punchRing) % punchRing];
        const auto g = (float) (j + 0.5f) / (float) k;
        const auto now = tape->readCell (t, r.cell);
        tape->writeCell (t, r.cell, r.old + (now - r.old) * g, runNr[t], runSpeed[t], runPos[t]);
    }
    replacedCount[t] = 0;
}

void TapeEngine::endBlock() noexcept
{
    // A track that was recording and wrote nothing this block has been punched out.
    for (int t = 0; t < numTracks; ++t)
        if (hasPrev[t] && ! wroteThisBlock[t])
            finishPunch (t);

    if (tape != nullptr)
        tape->position.store (pos);
    lastMode = mode.load();
}
} // namespace tf
