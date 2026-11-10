#include "Plate.h"

#include <condition_variable>
#include <thread>

namespace tf::plate
{
namespace
{
    constexpr double pi = juce::MathConstants<double>::pi, twoPi = juce::MathConstants<double>::twoPi;

    // EMT 140: steel, 2 x 1 m, 0.5 mm (the size commonly given; EMT's literature, as found, doesn't);
    // its decay at 500 Hz from about 1 s (the damper closest) to about 5 s (free).
    // EMT 240: a gold alloy foil 18 um thick, 270 x 290 mm, 0.7 to 5 s at 500 Hz (its data sheet); the
    // alloy's stiffness and density aren't published: pure gold's are used.
    // SMALL STEEL: not a particular unit; a cheaper plate of the same steel, 1 x 0.5 m.
    const PlateInfo plates[numPlates] {
        { "EMT 140 STEEL",     "EMT 140",     "STEEL 2 x 1 M, 0.5 MM",        2.0,  1.0,  0.0005, 2.0e11, 7850.0,  0.30, 1.0, 5.0, 0.42, 0.55 },
        { "EMT 240 GOLD FOIL", "EMT 240",     "GOLD FOIL 29 x 27 CM, 18 UM",  0.29, 0.27, 18.0e-6, 7.9e10, 19300.0, 0.42, 0.7, 5.0, 0.40, 0.55 },
        { "SMALL STEEL",       "SMALL STEEL", "STEEL 1 x 0.5 M, 0.5 MM",      1.0,  0.5,  0.0005, 2.0e11, 7850.0,  0.30, 0.8, 3.0, 0.42, 0.55 } };

    // The network's delays (samples at 48 kHz, for the EMT 140; smaller plates, shorter).
    constexpr int baseLines[Network::lines] { 1031, 1327, 1523, 1801, 2053, 2381, 2713, 3083 };
    constexpr int baseDiffusers[4] { 142, 107, 379, 277 };
    constexpr double plateScale[numPlates] { 1.0, 0.55, 0.72 };

    /** The tension, as where it takes over from the bending stiffness (Hz): chosen, not measured. */
    double tensionHz (double condition) { return 15.0 + 45.0 * condition; }
    /** The reference the levels are set at: modes, network and engines match at this decay. */
    constexpr double referenceDecay = 2.0, targetEnergy = 0.3;

    void hadamard8 (float* v) noexcept
    {
        for (int h = 1; h < 8; h <<= 1)
            for (int i = 0; i < 8; i += h << 1)
                for (int j = i; j < i + h; ++j)
                {
                    const float a = v[j], b = v[j + h];
                    v[j] = a + b;
                    v[j + h] = a - b;
                }
        for (int i = 0; i < 8; ++i) v[i] *= 0.35355339f;
    }
}

const PlateInfo& plateInfo (int p) { return plates[std::clamp (p, 0, numPlates - 1)]; }

double kappa (const PlateInfo& p)
{
    const double D = p.youngs * p.h * p.h * p.h / (12.0 * (1.0 - p.poisson * p.poisson));
    return std::sqrt (D / (p.density * p.h));
}

double modesPerHz (const PlateInfo& p) { return p.lx * p.ly / (2.0 * kappa (p)); }

double decayAt (double hz, double t500, double condition)
{
    // Highs go faster (the air, the metal, the damper): an aged plate, sooner.
    const double corner = 2000.0 + 3000.0 * condition;
    return t500 * (1.0 + 500.0 / corner) / (1.0 + hz / corner);
}

//==============================================================================
void Modes::build (int p, double cond, double rate, int budget)
{
    plate = p;
    condition = cond;
    const auto& info = plateInfo (p);
    const double k = kappa (info);
    const double c2 = twoPi * k * tensionHz (cond);   // c^2 = T / (rho h)
    double top = std::min ({ 8000.0, 1.15 * budget / modesPerHz (info), rate * 0.45 });
    // the rate they run at: as low as leaves room above the top mode (at most a quarter)
    decim = std::clamp ((int) (rate / (2.4 * top)), 1, 4);
    sampleRate = rate / decim;
    const double hostRate = rate;
    rate = sampleRate;

    struct M { double hz; int m, n; };
    std::vector<M> all;
    const double kMax = std::sqrt (twoPi * top / k);
    const int mMax = (int) (kMax * info.lx / pi) + 1, nMax = (int) (kMax * info.ly / pi) + 1;
    for (int m = 1; m <= mMax; ++m)
        for (int n = 1; n <= nMax; ++n)
        {
            const double wx = m * pi / info.lx, wy = n * pi / info.ly, kk = wx * wx + wy * wy;
            const double f = std::sqrt (k * k * kk * kk + c2 * kk) / twoPi;
            if (f >= 20.0 && f <= top) all.push_back ({ f, m, n });
        }
    std::sort (all.begin(), all.end(), [] (const M& a, const M& b) { return a.hz < b.hz; });
    if ((int) all.size() > budget) { all.resize ((size_t) budget); top = all.back().hz; }
    topHz = top;
    count = (int) all.size();
    padded = (count + 7) / 8 * 8;
    for (auto* v : { &cosw, &hz, &gin, &kx, &ky, &a1, &a2, &goutL, &goutR, &y1, &y2 }) v->assign ((size_t) padded, 0.0f);

    double energy = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const auto& md = all[(size_t) i];
        const double w = twoPi * md.hz / rate;
        cosw[(size_t) i] = (float) std::cos (w);
        hz[(size_t) i] = (float) md.hz;
        kx[(size_t) i] = (float) (md.m * pi);
        ky[(size_t) i] = (float) (md.n * pi);
        const double atDriver = std::sin (md.m * pi * info.driverX) * std::sin (md.n * pi * info.driverY);
        gin[(size_t) i] = (float) (atDriver * std::sin (w));   // so every mode rings at the same height
        // its energy at the reference decay, heard at an average place (a shape's mean square is 1/4)
        const double r = std::exp (-6.9078 / (decayAt (md.hz, referenceDecay, cond) * rate));
        energy += atDriver * atDriver * 0.25 / (2.0 * (1.0 - r * r));
    }
    // the scale that puts the modes at the target level, folded into the driver's gains
    // (a plate's energy is as dense in every hertz: the modes have their share of the whole band, the
    // tail above them the rest, so the two engines come out at the same level)
    // (and an impulse through the resampling comes out with 1/decim of the energy it would have at the full rate)
    const double share = (top - 20.0) / (0.5 * hostRate);
    const float scale = energy > 0.0 ? (float) std::sqrt (targetEnergy * share * decim / energy) : 0.0f;
    for (int i = 0; i < count; ++i) gin[(size_t) i] *= scale;
    tailGain = (float) (targetEnergy / (0.5 * hostRate));   // energy per hertz, for the tail to match
    const double edge = std::min (top * 1.12, 0.45 * sampleRate);
    for (auto* b : { &inLp1, &outLp1L, &outLp1R }) b->setLowPass (hostRate, edge, 0.5412);
    for (auto* b : { &inLp2, &outLp2L, &outLp2R }) b->setLowPass (hostRate, edge, 1.3066);
}

void Modes::setDecay (double t500, double cond)
{
    for (int i = 0; i < count; ++i)
    {
        const double r = std::exp (-6.9078 / (decayAt (hz[(size_t) i], t500, cond) * sampleRate));
        a1[(size_t) i] = (float) (2.0 * r * cosw[(size_t) i]);
        a2[(size_t) i] = (float) (r * r);
    }
}

void Modes::setPickups (float lx, float ly, float rx, float ry)
{
    for (int i = 0; i < count; ++i)
    {
        goutL[(size_t) i] = std::sin (kx[(size_t) i] * lx) * std::sin (ky[(size_t) i] * ly);
        goutR[(size_t) i] = std::sin (kx[(size_t) i] * rx) * std::sin (ky[(size_t) i] * ry);
    }
}

//==============================================================================
void Network::build (const PlateInfo& info, double rate)
{
    const int p = (int) (&info - plates);
    const double s = plateScale[std::clamp (p, 0, numPlates - 1)] * rate / 48000.0;
    for (int k = 0; k < lines; ++k)
    {
        len[k] = std::max (16, (int) (baseLines[k] * s));
        buf[k].assign ((size_t) len[k], 0.0f);
        pos[k] = 0;
    }
    for (int k = 0; k < 4; ++k) { diff[k].assign ((size_t) std::max (8, (int) (baseDiffusers[k] * s)), 0.0f); diffPos[k] = 0; }
    reset();
}

void Network::reset()
{
    for (int k = 0; k < lines; ++k) { std::fill (buf[k].begin(), buf[k].end(), 0.0f); lp[k] = 0.0f; }
    for (auto& d : diff) std::fill (d.begin(), d.end(), 0.0f);
    for (auto& a : apState) a = 0.0f;
}

void Network::setDecay (double t500, double cond, double rate)
{
    // Jot's absorption: each line loses as much as its length's worth of the decay, more at the top.
    const double tLow = decayAt (50.0, t500, cond), tHigh = decayAt (12000.0, t500, cond);
    for (int k = 0; k < lines; ++k)
    {
        const double gLow = std::pow (10.0, -3.0 * len[k] / (rate * tLow)), gHigh = std::pow (10.0, -3.0 * len[k] / (rate * tHigh));
        pole[k] = (float) ((gLow - gHigh) / (gLow + gHigh));
        g0[k] = (float) (gLow * (1.0 - pole[k]));
    }
}

void Network::process (float in, float& outL, float& outR) noexcept
{
    // Dispersion: first-order allpasses delay the lows more, so highs come through first.
    float x = in;
    for (auto& st : apState)
    {
        const float y = -apCoef * x + st;
        st = x + apCoef * y;
        x = y;
    }
    // Diffusion: a few Schroeder allpasses.
    for (int k = 0; k < 4; ++k)
    {
        auto& d = diff[k];
        const float delayed = d[(size_t) diffPos[k]];
        const float v = x + 0.62f * delayed;
        d[(size_t) diffPos[k]] = v;
        x = delayed - 0.62f * v;
        if (++diffPos[k] >= (int) d.size()) diffPos[k] = 0;
    }
    float o[lines];
    for (int k = 0; k < lines; ++k)
    {
        lp[k] = g0[k] * buf[k][(size_t) pos[k]] + pole[k] * lp[k];
        o[k] = lp[k];
    }
    outL = 0.35f * (o[0] - o[1] + o[2] + o[3] - o[4] + o[5] - o[6] - o[7]);
    outR = 0.35f * (o[0] + o[1] - o[2] + o[3] + o[4] - o[5] - o[6] + o[7]);
    hadamard8 (o);
    for (int k = 0; k < lines; ++k)
    {
        buf[k][(size_t) pos[k]] = o[k] + ((k & 1) ? -x : x) * 0.35f;
        if (++pos[k] >= len[k]) pos[k] = 0;
    }
}

//==============================================================================
/** Builds the modes off the audio thread when the plate, its condition or the rate change. */
class Processor::Builder
{
public:
    explicit Builder (Processor& p) : owner (p), thread ([this] { run(); }) {}
    ~Builder()
    {
        { std::lock_guard<std::mutex> g (m); quit = true; }
        cv.notify_all();
        thread.join();
        delete owner.retired.exchange (nullptr);
        delete owner.incoming.exchange (nullptr);
    }
    void poke() { cv.notify_all(); }

private:
    void run()
    {
        int builtPlate = -1;
        double builtCondition = -1.0, builtRate = 0.0;
        for (;;)
        {
            {
                std::unique_lock<std::mutex> g (m);
                cv.wait_for (g, std::chrono::milliseconds (40));
                if (quit) return;
            }
            delete owner.retired.exchange (nullptr);
            const int p = owner.wantPlate.load();
            const double c = owner.wantCondition.load(), r = owner.wantRate.load();
            if (p == builtPlate && std::abs (c - builtCondition) < 0.004 && juce::exactlyEqual (r, builtRate)) continue;
            if (owner.incoming.load() != nullptr) continue;   // the last one isn't taken yet
            auto made = std::make_unique<Modes>();
            made->build (p, c, r, modeBudget);
            builtPlate = p;
            builtCondition = c;
            builtRate = r;
            owner.incoming = made.release();
        }
    }

    Processor& owner;
    std::mutex m;
    std::condition_variable cv;
    bool quit = false;
    std::thread thread;
};

//==============================================================================
Processor::Processor()
    : juce::AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
{
    pEngine = addChoice ("engine", "Engine", { "SIMULATED", "LIGHT" }, 0);
    juce::StringArray names;
    for (auto& p : plates) names.add (p.label);
    pPlate = addChoice ("plate", "Plate", names, 0);
    pDamping = addFloat ("damping", "Damping", 0.0f, 1.0f, 0.45f);
    pCondition = addFloat ("condition", "Condition", 0.0f, 1.0f, 0.7f);
    pPredelay = addFloat ("predelay", "Pre-delay", 0.0f, 80.0f, 12.0f);
    pStereo = addBool ("stereo", "Stereo pickups", true);
    pWidth = addFloat ("width", "Width", 0.0f, 1.0f, 1.0f);
    pBass = addFloat ("bass", "Bass", -10.0f, 10.0f, 0.0f);
    pTreble = addFloat ("treble", "Treble", -10.0f, 10.0f, 0.0f);
    pLx = addFloat ("lx", "Pickup L across", 0.02f, 0.98f, 0.18f);
    pLy = addFloat ("ly", "Pickup L down", 0.02f, 0.98f, 0.28f);
    pRx = addFloat ("rx", "Pickup R across", 0.02f, 0.98f, 0.78f);
    pRy = addFloat ("ry", "Pickup R down", 0.02f, 0.98f, 0.74f);
    builder = std::make_unique<Builder> (*this);
}

Processor::~Processor()
{
    builder.reset();
}

juce::AudioParameterFloat* Processor::addFloat (const juce::String& id, const juce::String& name, float lo, float hi, float def)
{
    auto* p = new juce::AudioParameterFloat (juce::ParameterID { id, 1 }, name, juce::NormalisableRange<float> (lo, hi), def);
    addParameter (p);
    params[id] = p;
    return p;
}

juce::AudioParameterBool* Processor::addBool (const juce::String& id, const juce::String& name, bool def)
{
    auto* p = new juce::AudioParameterBool (juce::ParameterID { id, 1 }, name, def);
    addParameter (p);
    params[id] = p;
    return p;
}

juce::AudioParameterChoice* Processor::addChoice (const juce::String& id, const juce::String& name, const juce::StringArray& c, int def)
{
    auto* p = new juce::AudioParameterChoice (juce::ParameterID { id, 1 }, name, c, def);
    addParameter (p);
    params[id] = p;
    return p;
}

juce::RangedAudioParameter* Processor::param (const juce::String& id) const
{
    auto it = params.find (id);
    return it != params.end() ? it->second : nullptr;
}

float Processor::get (const juce::String& id) const
{
    auto* p = param (id);
    return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f;
}

void Processor::set (const juce::String& id, float value)
{
    if (auto* p = param (id))
        p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (p->getNormalisableRange().start, p->getNormalisableRange().end, value)));
}

int Processor::plateIndex() const { return std::clamp (pPlate->getIndex(), 0, numPlates - 1); }
bool Processor::simulated() const { return pEngine->getIndex() == 0; }

double Processor::decay500() const
{
    const auto& p = plateInfo (plateIndex());
    return p.maxDecay * std::pow (p.minDecay / p.maxDecay, (double) pDamping->get());
}

double Processor::damperMm() const { return 3.0 * std::pow (100.0 / 3.0, 1.0 - (double) pDamping->get()); }

//==============================================================================
void Processor::prepareToPlay (double rate, int block)
{
    sr = rate;
    wantPlate = plateIndex();
    wantCondition = (double) pCondition->get();
    wantRate = rate;
    // The first modes now (so the plate rings from the first block); changes later on the builder.
    auto first = std::make_unique<Modes>();
    first->build (plateIndex(), pCondition->get(), rate, modeBudget);
    delete incoming.exchange (nullptr);
    modes = std::move (first);
    fading.reset();
    fadeLeft = 0;
    lastDecay = lastCondition = -1.0;
    for (auto& v : lastPick) v = -1.0f;
    buildNetwork (plateIndex());
    pre.assign ((size_t) (rate * 0.1) + 8, 0.0f);
    prePos = 0;
    inScratch.assign ((size_t) std::max (block, 4096), 0.0f);
    l2.assign (inScratch.size(), 0.0f);
    r2.assign (inScratch.size(), 0.0f);
    hpL.setHighPass (rate, 80.0, 0.7071);   // the recovery amplifier turns over about here (EMT 240: 100 Hz)
    hpR = hpL;
    lastBass = lastTreble = 99.0f;
    lastTop = -1.0f;
    for (auto* b : { &hpL, &hpR, &bassL, &bassR, &trebleL, &trebleR, &topL, &topR, &top2L, &top2R }) b->reset();
}

void Processor::buildNetwork (int plate)
{
    // Its level at the reference decay, measured: so LIGHT sits where the modes do, and the tail
    // above them carries on at their level.
    network.build (plateInfo (plate), sr);
    network.setDecay (referenceDecay, 0.7, sr);
    double e = 0.0;
    for (int i = 0; i < (int) (sr * 3.0); ++i)
    {
        float a, b;
        network.process (i == 0 ? 1.0f : 0.0f, a, b);
        e += 0.5 * ((double) a * a + (double) b * b);
    }
    networkEnergy = e;
    network.reset();
    networkPlate = plate;
}

void Processor::processModes (Modes& md, const float* in, float* l, float* r, int n, float gain) noexcept
{
    float* __restrict Y1 = md.y1.data();
    float* __restrict Y2 = md.y2.data();
    const float* __restrict A1 = md.a1.data();
    const float* __restrict A2 = md.a2.data();
    const float* __restrict GI = md.gin.data();
    const float* __restrict GL = md.goutL.data();
    const float* __restrict GR = md.goutR.data();
    const int padded = md.padded, decim = md.decim;
    const float up = (float) decim;
    for (int s = 0; s < n; ++s)
    {
        const float x = md.inLp2.process (md.inLp1.process (in[s]));
        if (++md.phase < decim)
        {
            // between the modes' samples: zeros into the output's low-pass
            l[s] += gain * md.outLp2L.process (md.outLp1L.process (0.0f));
            r[s] += gain * md.outLp2R.process (md.outLp1R.process (0.0f));
            continue;
        }
        md.phase = 0;
        float accL[8] {}, accR[8] {};
        for (int i = 0; i < padded; i += 8)
            for (int j = 0; j < 8; ++j)
            {
                const int k = i + j;
                const float y = A1[k] * Y1[k] - A2[k] * Y2[k] + GI[k] * x;
                accL[j] += GL[k] * y;
                accR[j] += GR[k] * y;
                Y2[k] = Y1[k];
                Y1[k] = y;
            }
        const float yl = ((accL[0] + accL[1]) + (accL[2] + accL[3])) + ((accL[4] + accL[5]) + (accL[6] + accL[7]));
        const float yr = ((accR[0] + accR[1]) + (accR[2] + accR[3])) + ((accR[4] + accR[5]) + (accR[6] + accR[7]));
        l[s] += gain * md.outLp2L.process (md.outLp1L.process (yl * up));
        r[s] += gain * md.outLp2R.process (md.outLp1R.process (yr * up));
    }
}

void Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const int total = buffer.getNumSamples();
    auto* L = buffer.getWritePointer (0);
    auto* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : L;
    const int plate = plateIndex();
    const double cond = pCondition->get();
    wantPlate = plate;
    wantCondition = cond;
    builder->poke();

    // New modes (another plate, or condition): crossfade to them.
    if (incoming.load() != nullptr && retired.load() == nullptr)
    {
        if (fading != nullptr) retired = fading.release();
        else
        {
            fading = std::move (modes);
            modes.reset (incoming.exchange (nullptr));
            fadeLeft = fadeLength;
            lastDecay = -1.0;
            for (auto& v : lastPick) v = -1.0f;
        }
    }
    if (fading != nullptr && fadeLeft <= 0 && retired.load() == nullptr) retired = fading.release();
    // (until the builder has a new plate's modes, the old ones carry on)

    // A different plate's network is a different size (its buffers are small): rebuilt here, once.
    if (networkPlate != plate)
    {
        buildNetwork (plate);
        lastDecay = -1.0;
    }

    const double t500 = decay500() * (0.75 + 0.25 * cond);   // an aged plate doesn't ring as long
    if (std::abs (t500 - lastDecay) > 0.002 || std::abs (cond - lastCondition) > 0.002)
    {
        lastDecay = t500;
        lastCondition = cond;
        if (modes != nullptr) modes->setDecay (t500, cond);
        network.setDecay (t500, cond, sr);
    }
    const bool stereo = pStereo->get();
    const float pk[4] { pLx->get(), pLy->get(), pRx->get(), pRy->get() };
    bool moved = false;
    for (int i = 0; i < 4; ++i) moved = moved || ! juce::exactlyEqual (pk[i], lastPick[i]);
    if (modes != nullptr && moved)
    {
        std::copy (pk, pk + 4, lastPick);
        modes->setPickups (pk[0], pk[1], pk[2], pk[3]);
    }

    const float top = modes != nullptr ? (float) modes->topHz : 1000.0f;
    if (std::abs (top - lastTop) > 1.0f)
    {
        lastTop = top;
        for (auto* b : { &topL, &topR }) b->setHighPass (sr, top, 0.5412);
        for (auto* b : { &top2L, &top2R }) b->setHighPass (sr, top, 1.3066);
    }
    if (! juce::exactlyEqual (pBass->get(), lastBass) || ! juce::exactlyEqual (pTreble->get(), lastTreble))
    {
        lastBass = pBass->get();
        lastTreble = pTreble->get();
        for (auto* b : { &bassL, &bassR }) b->setLowShelf (sr, 200.0, lastBass);
        for (auto* b : { &trebleL, &trebleR }) b->setHighShelf (sr, 4000.0, lastTreble);
    }

    const bool sim = simulated() && modes != nullptr;
    // the network's level: for LIGHT, the target; under the modes, the modes' energy per hertz
    const float netGain = (float) std::sqrt ((sim ? modes->tailGain * sr * 0.5 : targetEnergy) / std::max (1.0e-9, networkEnergy));
    const int preLen = (int) pre.size();
    const int delay = juce::jlimit (0, preLen - 1, (int) (pPredelay->get() * 0.001 * sr));
    const float width = pWidth->get();
    float peak = 0.0f;

    for (int at = 0; at < total; at += (int) inScratch.size())
    {
        const int n = std::min ((int) inScratch.size(), total - at);
        float* l = L + at;
        float* r = R + at;
        for (int i = 0; i < n; ++i)
        {
            pre[(size_t) prePos] = 0.5f * (l[i] + r[i]);
            inScratch[(size_t) i] = pre[(size_t) ((prePos - delay + preLen) % preLen)];
            if (++prePos >= preLen) prePos = 0;
            l2[(size_t) i] = r2[(size_t) i] = 0.0f;
        }
        if (sim)
        {
            const float fadeIn = fading != nullptr ? 1.0f - (float) fadeLeft / fadeLength : 1.0f;
            processModes (*modes, inScratch.data(), l2.data(), r2.data(), n, fadeIn);
            if (fading != nullptr && fadeLeft > 0)
            {
                processModes (*fading, inScratch.data(), l2.data(), r2.data(), n, 1.0f - fadeIn);
                fadeLeft -= n;
            }
        }
        for (int i = 0; i < n; ++i)
        {
            float nl, nr;
            network.process (inScratch[(size_t) i], nl, nr);
            if (sim)
            {
                nl = top2L.process (topL.process (nl));
                nr = top2R.process (topR.process (nr));
            }
            float yl = l2[(size_t) i] + netGain * nl, yr = r2[(size_t) i] + netGain * nr;
            if (! stereo) yr = yl;
            const float mid = 0.5f * (yl + yr), side = 0.5f * (yl - yr) * width;
            yl = mid + side;
            yr = mid - side;
            yl = trebleL.process (bassL.process (hpL.process (yl)));
            yr = trebleR.process (bassR.process (hpR.process (yr)));
            l[i] = yl;
            r[i] = yr;
            peak = std::max (peak, std::abs (yl) + std::abs (yr));
        }
    }
    levelHold = std::max (peak * 0.5f, levelHold * 0.97f);
    level = levelHold;
    modesShown = sim ? modes->count : 0;
    topShown = sim ? modes->topHz : 0.0;
    for (int c = 2; c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, total);
}

//==============================================================================
std::vector<float> Processor::click (int plate, bool sim, double decay, double condition, double rate, double seconds, float px, float py, std::function<bool()> stop)
{
    Processor p;
    p.set ("plate", (float) plate);
    p.set ("engine", sim ? 0.0f : 1.0f);
    p.set ("condition", (float) condition);
    p.set ("predelay", 0.0f);
    p.set ("stereo", 0.0f);
    p.set ("lx", px);
    p.set ("ly", py);
    // the damping that gives this decay on this plate
    const auto& info = plateInfo (plate);
    const double t = juce::jlimit (info.minDecay, info.maxDecay, decay / (0.75 + 0.25 * condition));
    p.set ("damping", (float) (std::log (t / info.maxDecay) / std::log (info.minDecay / info.maxDecay)));
    p.prepareToPlay (rate, 512);
    const int total = (int) (seconds * rate);
    std::vector<float> out;
    out.reserve ((size_t) total);
    juce::AudioBuffer<float> b (2, 512);
    juce::MidiBuffer m;
    for (int at = 0; at < total; at += 512)
    {
        if (stop && stop()) break;
        const int n = std::min (512, total - at);
        b.setSize (2, n, false, false, true);
        b.clear();
        if (at == 0) { b.setSample (0, 0, 1.0f); b.setSample (1, 0, 1.0f); }
        p.processBlock (b, m);
        for (int i = 0; i < n; ++i) out.push_back (b.getSample (0, i));
    }
    return out;
}

//==============================================================================
std::unique_ptr<juce::XmlElement> Processor::toXml (bool) const
{
    auto x = std::make_unique<juce::XmlElement> ("TFPLATE");
    x->setAttribute ("version", 1);
    for (auto& [id, p] : params) x->setAttribute (id, (double) p->convertFrom0to1 (p->getValue()));
    return x;
}

void Processor::fromXml (const juce::XmlElement& x)
{
    if (x.hasTagName ("FX"))
    {
        // A plate from before the model: its size as the damper (bigger, longer), its damping as treble.
        const double size = x.getDoubleAttribute ("size", 0.62), damp = x.getDoubleAttribute ("damping", 0.45);
        set ("plate", 0.0f);
        set ("damping", (float) juce::jlimit (0.0, 1.0, 0.85 - 0.8 * size));
        set ("treble", (float) juce::jlimit (-10.0, 10.0, 4.0 - 10.0 * damp));
        set ("predelay", (float) x.getDoubleAttribute ("predelay", 12.0));
        return;
    }
    if (! x.hasTagName ("TFPLATE")) return;
    for (auto& [id, p] : params)
        if (x.hasAttribute (id))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) x.getDoubleAttribute (id)));
}

void Processor::getStateInformation (juce::MemoryBlock& mb)
{
    auto x = toXml (true);
    writePresetAttributes (*x);
    copyXmlToBinary (*x, mb);
}

void Processor::setStateInformation (const void* data, int size)
{
    if (auto x = getXmlFromBinary (data, size))
    {
        fromXml (*x);
        readPresetAttributes (*x);
    }
}

const presets::Kind& presetKind()
{
    static const presets::Kind kind { "Plate", "tfpl", "TFPLATE", "PLATE", [] (const juce::XmlElement& s)
    {
        const auto& p = plateInfo ((int) s.getDoubleAttribute ("plate"));
        return juce::String (p.name) + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 "))
               + (s.getDoubleAttribute ("stereo", 1) > 0.5 ? "STEREO" : "MONO");
    } };
    return kind;
}

const presets::Kind& Processor::presetKind() const { return plate::presetKind(); }
} // namespace tf::plate
