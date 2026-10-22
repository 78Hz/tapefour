#include "TapeEngine.h"

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
    for (int t = 0; t < numTracks; ++t)
    {
        hasPrev[t] = false;
        for (auto& b : hfLoss[t]) b.reset();
        for (auto& b : antiAlias[t]) b.reset();
    }
}

void TapeEngine::setTape (Tape* t) noexcept
{
    tape = t;
    pos = t != nullptr ? t->position.load() : 0.0;
    windVel = 0;
    mode = (int) Mode::stop;
    rtz = false;
    for (auto& h : hasPrev) h = false;
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

void TapeEngine::beginBlock (int n, const bool* armed, int deviceLatencySamples) noexcept
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

    // Entering record: start the encoders fresh, like a punch-in.
    if (m == Mode::rec && lastMode != (int) Mode::rec)
        for (int t = 0; t < numTracks; ++t) { encoder[t].reset(); hasPrev[t] = false; }

    auto speedSel = juce::jlimit (1, 2, params.speedSel.load());
    auto nominal = speedSel * (1.0 + juce::jlimit (-5, 5, params.pitchStep.load()) * 0.024);
    nominalSpeed = (float) nominal;
    auto advNom = nominal * cellRate / sr;
    latencyCells = (deviceLatencySamples + params.latencyOffsetMs.load() * sr / 1000.0) * advNom;

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
    auto form = params.formulation.load();
    auto xt = params.xtalk.load();
    auto xtGain = xt > 0.0f ? dbToGain (-80.0f + xt * 50.0f) : 0.0f;
    auto hissAmt = params.hiss.load();
    auto hissGain = hissAmt > 0.0f ? dbToGain (-80.0f + hissAmt * 40.0f - (form == 1 ? 4.0f : 0.0f) - (speedSel == 2 ? 3.0f : 0.0f)) : 0.0f;
    auto hfAmt = params.hf.load() * (form == 1 ? 0.6f : 1.0f);
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
                auto w = hissNoise[t].next();
                auto& p = pink[t];
                p[0] = 0.99765f * p[0] + w * 0.0990460f;
                p[1] = 0.96300f * p[1] + w * 0.2965164f;
                p[2] = 0.57000f * p[2] + w * 1.0526913f;
                auto pk = (p[0] + p[1] + p[2] + w * 0.1848f) * 0.18f;
                v += hissGain * 2.7f * (0.5f * w + 0.5f * pk);   // 2.7: calibrates the mix to hissGain RMS
            }

            v = hfLoss[t][1].process (hfLoss[t][0].process (v));

            if (nrOn)
                v = decoder[t].process (v);

            o[i] = v;
        }
    }
}

void TapeEngine::writeTrack (int t, const float* in, int n) noexcept
{
    if (tape == nullptr || ! blockHasRecording)
        return;

    n = std::min (n, maxBlock);
    auto nrOn = params.nrOn.load();
    auto sat = params.sat.load();
    auto spd = nominalSpeed.load();
    const auto satNorm = 1.0f / std::tanh (2.5f);

    auto aaCut = (float) std::min (0.45 * spd * cellRate, 0.45 * sr);
    if (std::abs (aaCut - lastAaCut) > 1.0f)
    {
        for (auto& tr : antiAlias) { tr[0].setLowPass (sr, aaCut, 0.54); tr[1].setLowPass (sr, aaCut, 1.31); }
        lastAaCut = aaCut;
    }

    for (int i = 0; i < n; ++i)
    {
        if (! (recording[(size_t) i] && armedNow[t]))
        {
            hasPrev[t] = false;
            continue;
        }

        auto v = in[i];
        if (nrOn)
            v = encoder[t].process (v);
        v = (1.0f - sat) * v + sat * std::tanh (2.5f * v) * satNorm;
        v = juce::jlimit (-1.0f, 1.0f, v);
        v = antiAlias[t][1].process (antiAlias[t][0].process (v));

        writeSpan (t, posBuf[(size_t) i] - latencyCells, v, nrOn, spd);
    }
}

void TapeEngine::writeSpan (int t, double p, float v, bool nr, float spd) noexcept
{
    if (! hasPrev[t] || p <= prevPos[t] || p - prevPos[t] > 64.0)
    {
        prevPos[t] = p;
        prevVal[t] = v;
        hasPrev[t] = true;
        return;
    }

    auto a = prevPos[t], b = p;
    auto c0 = (int64_t) std::floor (a) + 1, c1 = (int64_t) std::floor (b);
    for (auto c = c0; c <= c1; ++c)
    {
        auto f = (float) (((double) c - a) / (b - a));
        tape->writeCell (t, c, prevVal[t] + (v - prevVal[t]) * f, nr, spd);
    }
    prevPos[t] = b;
    prevVal[t] = v;
}

void TapeEngine::endBlock() noexcept
{
    if (tape != nullptr)
        tape->position.store (pos);
    lastMode = mode.load();
}
} // namespace tf
