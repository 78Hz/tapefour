#include "FmDrums.h"

namespace tf::fmd
{
namespace
{
    constexpr double twoPi = 6.283185307179586;

    const ParamInfo params[numParams] {
        { "machine", "MACHINE", 0, numMachines - 1, 0 },
        { "pitch", "PITCH", 0, 127, 64 },
        { "decay", "DECAY", 0, 127, 64 },
        { "color", "COLOR", 0, 127, 64 },
        { "shape", "SHAPE", 0, 127, 64 },
        { "sweep", "SWEEP", 0, 127, 64 },
        { "contour", "CONTOUR", 0, 127, 64 },
        { "volume", "VOLUME", 0, 127, 100 },
        { "drive", "DRIVE", 0, 127, 0 },
        { "pan", "PAN", 0, 127, 64 },
        { "delay", "DELAY", 0, 127, 0 },
        { "reverb", "REVERB", 0, 127, 0 },
        { "lfoSpeed", "LFO SPEED", 0, 127, 64 },
        { "lfoDepth", "LFO DEPTH", 0, 127, 64 },
        { "lfoDest", "LFO TO", 0, numLfoDests - 1, 0 },
        { "lfoShape", "LFO SHAPE", 0, numLfoShapes - 1, 0 },
    };

    // what the four macros do, machine by machine (COLOR, SHAPE, SWEEP, CONTOUR)
    const char* const meanings[numMachines][4] {
        { "BITE", "CLICK", "PITCH DROP", "BOOM" },               // kick
        { "NOISE", "RATTLE", "PITCH DROP", "SNAP" },             // snare
        { "BRIGHTNESS", "SPREAD", "TONE", "SHIMMER" },          // metal
        { "RATIO", "DEPTH", "PITCH DROP", "RING" },              // perc
        { "RATIO", "DEPTH", "BRIGHT DECAY", "ATTACK" },          // tone
        { "CHORD", "VOICING", "BRIGHTNESS", "STRUM" },           // chord
    };

    // PERC's and TONE's modulator ratios (COLOR): the ones that sound like something (Chowning: integer
    // ratios give harmonic spectra, the others the inharmonic ones of drums and bells)
    constexpr double percRatios[] { 0.5, 1.0, 1.41, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 5.33 };
    constexpr double toneRatios[] { 0.5, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0 };
    // METAL: the TR-808 cymbal's six oscillators (Werner, Abel & Smith, ICMC 2014: 205.3, 304.4, 369.6 and
    // 522.7 Hz, and two trimmed: 540 and 800 Hz here, within their 254-627 and 359-1150 Hz), as ratios to the
    // lowest, in pairs (carrier, modulator)
    constexpr double metalRatios[] { 205.3 / 205.3, 304.4 / 205.3, 369.6 / 205.3, 522.7 / 205.3, 540.0 / 205.3, 800.0 / 205.3 };

    struct ChordType { const char* name; int notes[4]; };
    const ChordType chords[] {
        { "MAJ", { 0, 4, 7, 12 } }, { "MIN", { 0, 3, 7, 12 } }, { "7", { 0, 4, 7, 10 } }, { "MAJ7", { 0, 4, 7, 11 } },
        { "MIN7", { 0, 3, 7, 10 } }, { "SUS2", { 0, 2, 7, 12 } }, { "SUS4", { 0, 5, 7, 12 } }, { "6", { 0, 4, 7, 9 } },
        { "MIN6", { 0, 3, 7, 9 } }, { "ADD9", { 0, 4, 7, 14 } }, { "DIM", { 0, 3, 6, 9 } }, { "AUG", { 0, 4, 8, 12 } },
    };
    constexpr int numChords = (int) (sizeof (chords) / sizeof (chords[0]));

    template <typename T, size_t N> int pick (const T (&) [N], double v) { return std::clamp ((int) (v * (double) N), 0, (int) N - 1); }
    double norm (int v) { return std::clamp (v, 0, 127) / 127.0; }
    double expMap (double v, double lo, double hi) { return lo * std::pow (hi / lo, v); }
    int toneNote (double p) { return 24 + juce::roundToInt (p * 72.0); }   // C1..C7
    int chordRoot (double p) { return 36 + juce::roundToInt (p * 48.0); }  // C2..C6
    double noteHz (double note) { return 440.0 * std::pow (2.0, (note - 69.0) / 12.0); }
    juce::String noteName (int n) { return juce::MidiMessage::getMidiNoteName (n, true, true, 3); }
}

const char* machineName (int m) { static const char* n[] { "KICK", "SNARE", "METAL", "PERC", "TONE", "CHORD" }; return n[std::clamp (m, 0, numMachines - 1)]; }
const ParamInfo& paramInfo (int p) { return params[std::clamp (p, 0, numParams - 1)]; }
const char* macroMeaning (int m, int p) { return p >= pColor && p <= pContour ? meanings[std::clamp (m, 0, numMachines - 1)][p - pColor] : ""; }
const char* lfoDestName (int d) { static const char* n[] { "PITCH", "DECAY", "COLOR", "SHAPE", "SWEEP", "CONTOUR", "VOLUME", "PAN" }; return n[std::clamp (d, 0, numLfoDests - 1)]; }
const char* lfoShapeName (int s) { static const char* n[] { "SINE", "TRIANGLE", "RAMP", "SQUARE", "RANDOM" }; return n[std::clamp (s, 0, numLfoShapes - 1)]; }
const char* divisionName (int d) { static const char* n[] { "1/16", "1/8", "3/16", "1/4", "3/8", "1/2" }; return n[std::clamp (d, 0, numDivisions - 1)]; }

/** The machine's sound at these settings: what the window shows and the engine plays (all set by ear). */
namespace
{
    double decaySeconds (int machine, double d)
    {
        switch (machine)
        {
            case kick:  return expMap (d, 0.06, 2.0);
            case snare: return expMap (d, 0.05, 1.0);
            case metal: return expMap (d, 0.02, 2.5);
            case perc:  return expMap (d, 0.03, 1.5);
            case tone:  return expMap (d, 0.05, 4.0);
            case chord: default: return expMap (d, 0.08, 5.0);
        }
    }
    double baseHz (int machine, double p)
    {
        switch (machine)
        {
            case kick:  return expMap (p, 30.0, 120.0);
            case snare: return expMap (p, 120.0, 340.0);
            case metal: return expMap (p, 200.0, 800.0);
            case perc:  return expMap (p, 60.0, 960.0);
            case tone:  return noteHz (toneNote (p));
            case chord: default: return noteHz (chordRoot (p));
        }
    }
}

juce::String valueText (int machine, int param, int v)
{
    const double x = norm (v);
    switch (param)
    {
        case pMachine: return machineName (v);
        case pPitch:
            if (machine == tone) return noteName (toneNote (x));
            if (machine == chord) return noteName (chordRoot (x));
            return juce::String (juce::roundToInt (baseHz (machine, x))) + " HZ";
        case pDecay: { const double s = decaySeconds (machine, x); return s < 1.0 ? juce::String (juce::roundToInt (s * 1000.0)) + " MS" : juce::String (s, 2) + " S"; }
        case pColor:
            if (machine == perc) return juce::String (percRatios[pick (percRatios, x)], 2) + " : 1";
            if (machine == tone) return juce::String (toneRatios[pick (toneRatios, x)], 1) + " : 1";
            if (machine == chord) return chords[std::clamp ((int) (x * numChords), 0, numChords - 1)].name;
            return juce::String (v);
        case pShape:
            if (machine == chord) { static const char* n[] { "CLOSE", "1ST INV", "2ND INV", "OPEN", "WIDE", "DROP 2", "SPREAD", "HIGH" }; return n[std::clamp ((int) (x * 8.0), 0, 7)]; }
            return juce::String (v);
        case pPan: return v == 64 ? juce::String ("CENTRE") : (v < 64 ? "L" + juce::String (64 - v) : "R" + juce::String (v - 64));
        case pLfoSpeed: return juce::String (expMap (x, 0.05, 20.0), 2) + " HZ";
        case pLfoDepth: return (v > 64 ? "+" : "") + juce::String (v - 64);
        case pLfoDest: return lfoDestName (v);
        case pLfoShape: return lfoShapeName (v);
        default: return juce::String (v);
    }
}

bool Pattern::empty() const
{
    for (auto& t : steps) for (auto& s : t) if (s.on) return false;
    return true;
}

void State::init()
{
    const int machines[numTracks] { kick, snare, metal, perc, tone, chord };
    for (int t = 0; t < numTracks; ++t)
    {
        for (int p = 0; p < numParams; ++p) sound[t][p] = params[p].def;
        sound[t][pMachine] = machines[t];
        mute[t] = false;
    }
    // a kit to begin with
    sound[0][pPitch] = 40; sound[0][pDecay] = 70; sound[0][pSweep] = 80; sound[0][pColor] = 30; sound[0][pShape] = 50; sound[0][pContour] = 30;
    sound[1][pColor] = 70; sound[1][pDecay] = 50; sound[1][pReverb] = 30;
    sound[2][pDecay] = 20; sound[2][pColor] = 80; sound[2][pSweep] = 90; sound[2][pVolume] = 80;
    sound[3][pPitch] = 60; sound[3][pColor] = 40; sound[3][pDecay] = 40;
    sound[4][pPitch] = 40; sound[4][pColor] = 30; sound[4][pDecay] = 60; sound[4][pDelay] = 40;
    sound[5][pPitch] = 64; sound[5][pColor] = 20; sound[5][pDecay] = 90; sound[5][pReverb] = 60; sound[5][pVolume] = 80;
    for (auto& p : patterns) p = Pattern();
    auto& p = patterns[0];
    for (int s : { 0, 4, 8, 12 }) p.steps[0][s].on = 1;
    for (int s : { 4, 12 }) p.steps[1][s].on = 1;
    for (int s = 2; s < 16; s += 4) p.steps[2][s].on = 1;
    pattern = 0; tempo = 1200; swing = 50;
    delayTime = d8dot; delayFeedback = 50; delayTone = 80;
    reverbSize = 70; reverbDecay = 60; reverbTone = 70; master = 100;
}

//==============================================================================
struct Processor::Engine
{
    double sr = 48000.0;
    State s;
    juce::Random rng { 1 };

    // ---- a track's voice ----
    struct Voice
    {
        bool on = false;
        int machine = kick;
        double t = 0.0, velocity = 1.0, fade = 1.0;
        bool stopping = false;
        double x[numParams] {};             // the sound as struck (0..1), locks included
        double phase[8] {}, mphase[8] {};
        double noiseLp = 0.0, noiseHp = 0.0, hpIn = 0.0, hpOut[2] {};
        double fb = 0.0;
    };
    struct Track
    {
        Voice v;
        double lfoPhase = 0.0, lfoHeld = 0.0, lfoLast = 0.0;
        double level = 0.0;
    };
    std::array<Track, numTracks> tracks;

    // ---- the sequencer ----
    bool running = false;
    double stepPos = 0.0;                   // in steps since PLAY
    int lastStep = -1;
    int queuedPattern = -1;
    struct Pending { double at; int track; int step; int velocity; bool fromPattern; };
    std::array<Pending, 64> pending {};
    int numPending = 0;

    // ---- the effects ----
    std::vector<float> delayL, delayR;
    int delayWrite = 0;
    double delayLp[2] {};
    static constexpr int fdnN = 8;
    std::vector<float> fdn[fdnN];
    int fdnPos[fdnN] {}, fdnLen[fdnN] {};
    double fdnLp[fdnN] {};
    double fdnSizeUsed = -1.0;

    void prepare (double rate)
    {
        sr = rate;
        const int dlen = (int) (sr * 3.0) + 2;
        delayL.assign ((size_t) dlen, 0.0f); delayR.assign ((size_t) dlen, 0.0f); delayWrite = 0;
        for (auto& b : fdn) b.assign ((size_t) (sr * 0.25) + 8, 0.0f);
        fdnSizeUsed = -1.0;
        for (auto& t : tracks) t = Track();
        numPending = 0;
    }

    // the sound a step plays: the track's, with the step's locks
    void soundFor (int track, const Step* step, double out[numParams]) const
    {
        for (int p = 0; p < numParams; ++p)
        {
            int v = s.sound[track][p];
            if (step != nullptr && step->lock[p] >= 0) v = step->lock[p];
            out[p] = p == pMachine || p == pLfoDest || p == pLfoShape ? (double) v : norm (v);
        }
    }

    void strike (int track, const Step* step, double velocity)
    {
        auto& tr = tracks[(size_t) track];
        auto& v = tr.v;
        Voice nv;
        soundFor (track, step, nv.x);
        nv.on = true;
        nv.machine = std::clamp ((int) nv.x[pMachine], 0, numMachines - 1);
        nv.velocity = velocity;
        // a hit on a sounding voice: phases carry on (no click), the sound starts again
        if (v.on) for (int k = 0; k < 8; ++k) { nv.phase[k] = v.phase[k]; nv.mphase[k] = v.mphase[k]; }
        v = nv;
        tr.lfoPhase = 0.0;   // the LFO starts with the hit
    }

    double lfoValue (Track& tr, int shape, double hz)
    {
        const double ph = tr.lfoPhase;
        tr.lfoPhase += hz / sr * 32.0;   // (worked out every 32 samples)
        if (tr.lfoPhase >= 1.0)
        {
            tr.lfoPhase -= std::floor (tr.lfoPhase);
            tr.lfoHeld = rng.nextDouble() * 2.0 - 1.0;
        }
        switch (shape)
        {
            case lsSine: return std::sin (twoPi * ph);
            case lsTriangle: return 1.0 - 4.0 * std::abs (ph - 0.5);
            case lsRamp: return 1.0 - 2.0 * ph;
            case lsSquare: return ph < 0.5 ? 1.0 : -1.0;
            case lsRandom: default: return tr.lfoHeld;
        }
    }

    double noise() { return rng.nextDouble() * 2.0 - 1.0; }

    /** One sample of a voice (x: its settings now, LFO included). */
    double voiceSample (Voice& v, const double x[numParams])
    {
        const double t = v.t;
        const double P = x[pPitch], D = x[pDecay], C = x[pColor], S = x[pShape], W = x[pSweep], K = x[pContour];
        const double dt = 1.0 / sr;
        const double tau = decaySeconds (v.machine, D);
        double out = 0.0, amp = 0.0, alive = -1.0;   // alive: how loud it still is, where amp isn't the whole story
        switch (v.machine)
        {
            case kick:
            {
                // a sine whose pitch falls from above (SWEEP: how far, 0..4 octaves), held a little before it
                // decays (CONTOUR: from tight to an 808's boom); COLOR: a 1:1 modulator, its index decaying
                // fast (the beater's knock); SHAPE: a click
                const double hold = K * 0.15;
                amp = t < hold ? 1.0 : std::exp (-(t - hold) / tau);
                const double f = baseHz (kick, P) * std::pow (2.0, W * 4.0 * std::exp (-t / (0.008 + 0.04 * W)));
                const double index = C * 3.0 * std::exp (-t / 0.03);
                v.phase[0] += f * dt; v.phase[0] -= std::floor (v.phase[0]);
                out = std::sin (twoPi * v.phase[0] + index * std::sin (twoPi * v.phase[0]));
                out += S * 0.6 * std::exp (-t / 0.0025) * (0.5 * noise() + std::sin (twoPi * 1800.0 * t));
                break;
            }
            case snare:
            {
                // the body: a carrier and a modulator at 1.6 (Chowning's drum: an inharmonic ratio, its index
                // decaying) dropping in pitch; the snares: noise, high-passed. COLOR: noise against body,
                // SHAPE: the body's index, CONTOUR: the snares' decay against the body's
                amp = std::exp (-t / tau);
                const double f = baseHz (snare, P) * (1.0 + W * 1.5 * std::exp (-t / 0.02));
                v.phase[0] += f * dt; v.phase[0] -= std::floor (v.phase[0]);
                v.mphase[0] += f * 1.6 * dt; v.mphase[0] -= std::floor (v.mphase[0]);
                const double body = std::sin (twoPi * v.phase[0] + S * 4.0 * std::exp (-t / (tau * 0.5)) * std::sin (twoPi * v.mphase[0]));
                const double n = noise();
                v.noiseHp = 0.85 * (v.noiseHp + n - v.hpIn); v.hpIn = n;   // a gentle high-pass
                const double snares = v.noiseHp * std::exp (-t / (tau * (0.4 + 1.6 * K)));
                out = body * (1.0 - C) * amp + snares * C * 1.4;
                alive = std::max (amp, std::exp (-t / (tau * (0.4 + 1.6 * K))));
                amp = 1.0;
                break;
            }
            case metal:
            {
                // three FM pairs at the 808 cymbal's ratios (SHAPE spreads them), their index COLOR, then a
                // high-pass (SWEEP: 2 to 10 kHz); CONTOUR: a second, slower tail mixed in
                const double base = baseHz (metal, P);
                const double spread = 0.5 + S * 1.5;
                double sum = 0.0;
                for (int k = 0; k < 3; ++k)
                {
                    const double fc = base * (1.0 + (metalRatios[k * 2] - 1.0) * spread);
                    const double fm = base * (1.0 + (metalRatios[k * 2 + 1] - 1.0) * spread);
                    v.phase[k] += fc * dt; v.phase[k] -= std::floor (v.phase[k]);
                    v.mphase[k] += fm * dt; v.mphase[k] -= std::floor (v.mphase[k]);
                    sum += std::sin (twoPi * v.phase[k] + (1.0 + C * 7.0) * std::sin (twoPi * v.mphase[k]));
                }
                sum = sum / 3.0 + 0.25 * C * noise();
                const double fc = expMap (W, 2000.0, 10000.0);
                const double a = std::exp (-twoPi * fc / sr);
                const double y1 = a * (v.hpOut[0] + sum - v.hpIn); v.hpIn = sum; v.hpOut[0] = y1;
                const double y2 = a * (v.hpOut[1] + y1 - v.noiseLp); v.noiseLp = y1; v.hpOut[1] = y2;
                amp = (1.0 - K * 0.5) * std::exp (-t / tau) + K * 0.5 * std::exp (-t / (tau * 3.0));
                // (the high-pass takes most of it: back to the others' level, more as it rises; a soft clip
                // keeps the brightest settings in bounds)
                out = std::tanh (y2 * 6.0 * std::pow (fc / 2000.0, 0.9));
                break;
            }
            case perc:
            {
                // two operators (Chowning's wood-drum): the ratio from a musical set (COLOR), the index
                // (SHAPE) decaying as fast as CONTOUR says, the pitch falling by SWEEP
                amp = std::exp (-t / tau);
                const double f = baseHz (perc, P) * std::pow (2.0, W * 2.0 * std::exp (-t / 0.03));
                const double ratio = percRatios[pick (percRatios, C)];
                v.phase[0] += f * dt; v.phase[0] -= std::floor (v.phase[0]);
                v.mphase[0] += f * ratio * dt; v.mphase[0] -= std::floor (v.mphase[0]);
                const double index = S * 5.0 * std::exp (-t / (tau * (0.1 + 0.9 * K)));
                out = std::sin (twoPi * v.phase[0] + index * std::sin (twoPi * v.mphase[0]));
                break;
            }
            case tone:
            {
                // a two-operator FM voice: ratio (COLOR), index (SHAPE), a decaying extra index (SWEEP), an
                // attack (CONTOUR, up to 200 ms)
                const double attack = K * 0.2;
                amp = (attack > 0.0 ? std::min (1.0, t / attack) : 1.0) * std::exp (-std::max (0.0, t - attack) / tau);
                const double f = baseHz (tone, P);
                const double ratio = toneRatios[pick (toneRatios, C)];
                v.phase[0] += f * dt; v.phase[0] -= std::floor (v.phase[0]);
                v.mphase[0] += f * ratio * dt; v.mphase[0] -= std::floor (v.mphase[0]);
                const double index = S * 4.0 + W * 6.0 * std::exp (-t / (0.05 + tau * 0.3));
                out = std::sin (twoPi * v.phase[0] + index * std::sin (twoPi * v.mphase[0]));
                break;
            }
            case chord: default:
            {
                // four voices, each a carrier and a 1:1 modulator (SWEEP: their index, the brightness); COLOR
                // the chord, SHAPE its voicing, CONTOUR a strum (up to 120 ms across the four)
                const auto& ch = chords[std::clamp ((int) (C * numChords), 0, numChords - 1)];
                const int voicing = std::clamp ((int) (S * 8.0), 0, 7);
                static const int shift[8][4] { { 0, 0, 0, 0 }, { 12, 0, 0, 0 }, { 12, 12, 0, 0 }, { 0, 12, 0, 12 },
                                               { -12, 0, 12, 12 }, { 0, 0, -12, 0 }, { -12, 0, 12, 24 }, { 12, 12, 12, 12 } };
                const double root = chordRoot (P);
                for (int k = 0; k < 4; ++k)
                {
                    const double start = K * 0.04 * k;
                    if (t < start) continue;
                    const double f = noteHz (root + ch.notes[k] + shift[voicing][k]);
                    v.phase[k] += f * dt; v.phase[k] -= std::floor (v.phase[k]);
                    const double index = W * 2.5 * (0.4 + 0.6 * std::exp (-(t - start) / (tau * 0.4)));
                    out += std::sin (twoPi * v.phase[k] + index * std::sin (twoPi * v.phase[k])) * std::exp (-(t - start) / tau);
                }
                out *= 0.45;
                alive = std::exp (-std::max (0.0, t - K * 0.12) / tau);
                amp = std::min (1.0, t / 0.003);
                break;
            }
        }
        v.t += dt;
        if ((alive >= 0.0 ? alive : amp) < 1.0e-4 && v.t > 0.05) v.on = false;
        return out * amp;
    }

    void schedule (double at, int track, int step, int velocity, bool fromPattern)
    {
        if (numPending >= (int) pending.size()) return;
        pending[(size_t) numPending++] = { at, track, step, velocity, fromPattern };
    }

    void process (juce::AudioBuffer<float>& buffer, std::atomic<int>& shown, std::array<std::atomic<float>, numTracks>& trackLevels,
                  const std::array<std::atomic<bool>, numTracks>& leaves)
    {
        const int n = buffer.getNumSamples();
        auto* outL = buffer.getWritePointer (0);
        auto* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
        // the separate outputs, one a track (channels 2..7 when the host has them)
        float* own[numTracks] {};
        bool away[numTracks] {};
        for (int t = 0; t < numTracks; ++t)
        {
            own[t] = buffer.getNumChannels() > 2 + t ? buffer.getWritePointer (2 + t) : nullptr;
            away[t] = leaves[(size_t) t].load();
        }
        const double samplesPerStep = sr * 60.0 / (s.tempo / 10.0) / 4.0;
        const double swingDelay = (s.swing - 50) / 50.0 * samplesPerStep;   // the off-beat sixteenths, late
        // the delay's time, in the tempo
        static const double divSteps[] { 1.0, 2.0, 3.0, 4.0, 6.0, 8.0 };
        const double delaySamples = std::min ((double) delayL.size() - 2.0, divSteps[std::clamp (s.delayTime, 0, numDivisions - 1)] * samplesPerStep);
        const double fbk = norm (s.delayFeedback) * 0.9;
        const double dLp = 1.0 - std::exp (-twoPi * expMap (norm (s.delayTone), 800.0, 16000.0) / sr);
        // the reverb: eight delays (fitted to its size), mixed by a Householder matrix, damped
        const double size = 0.3 + 0.7 * norm (s.reverbSize);
        if (std::abs (size - fdnSizeUsed) > 1.0e-6)
        {
            static const int primes[fdnN] { 1117, 1277, 1429, 1571, 1723, 1873, 2017, 2179 };
            for (int k = 0; k < fdnN; ++k) fdnLen[k] = std::clamp ((int) (primes[k] * size * sr / 44100.0), 64, (int) fdn[k].size() - 1);
            fdnSizeUsed = size;
        }
        const double rt60 = expMap (norm (s.reverbDecay), 0.3, 8.0);
        const double rLp = 1.0 - std::exp (-twoPi * expMap (norm (s.reverbTone), 1500.0, 16000.0) / sr);
        const double master = norm (s.master);

        for (int i = 0; i < n; ++i)
        {
            // ---- the sequencer: a step at each sixteenth ----
            if (running)
            {
                const int step = (int) std::floor (stepPos);
                if (step != lastStep)
                {
                    lastStep = step;
                    auto& pat = s.patterns[(size_t) std::clamp (s.pattern, 0, numPatterns - 1)];
                    const int len = std::clamp (pat.length, 1, maxSteps);
                    const int k = step % len;
                    if (k == 0 && queuedPattern >= 0 && step > 0) { s.pattern = queuedPattern; queuedPattern = -1; }
                    auto& cur = s.patterns[(size_t) std::clamp (s.pattern, 0, numPatterns - 1)];
                    const int kk = step % std::clamp (cur.length, 1, maxSteps);
                    shown = kk;
                    const double late = (kk % 2) == 1 ? swingDelay : 0.0;
                    for (int t = 0; t < numTracks; ++t)
                    {
                        const auto& st = cur.steps[t][kk];
                        if (! st.on || s.mute[t]) continue;
                        if (st.chance < 100 && rng.nextInt (100) >= st.chance) continue;
                        const int hitsInStep = 1 + std::clamp ((int) st.retrig, 0, 3);
                        for (int h = 0; h < hitsInStep; ++h)
                            schedule (late + h * samplesPerStep / hitsInStep, t, kk, std::max (1, st.velocity - h * 12), true);
                    }
                }
                stepPos += 1.0 / samplesPerStep;
            }
            // ---- hits due ----
            for (int p = 0; p < numPending;)
            {
                auto& h = pending[(size_t) p];
                if (h.at <= 0.0)
                {
                    const Step* st = h.fromPattern ? &s.patterns[(size_t) std::clamp (s.pattern, 0, numPatterns - 1)].steps[h.track][h.step] : nullptr;
                    strike (h.track, st, h.velocity / 127.0);
                    pending[(size_t) p] = pending[(size_t) --numPending];
                }
                else { h.at -= 1.0; ++p; }
            }

            // ---- the voices ----
            double l = 0.0, r = 0.0, sendD = 0.0, sendR = 0.0;
            for (int t = 0; t < numTracks; ++t)
            {
                auto& tr = tracks[(size_t) t];
                auto& v = tr.v;
                if (! v.on) { tr.level *= 0.9995; if (own[t] != nullptr) own[t][i] = 0.0f; continue; }
                double x[numParams];
                std::copy (std::begin (v.x), std::end (v.x), x);
                if ((i & 31) == 0) tr.lfoLast = lfoValue (tr, (int) x[pLfoShape], expMap (x[pLfoSpeed], 0.05, 20.0));
                const double depth = (x[pLfoDepth] - 64.0 / 127.0) * 2.0;
                const int dest = std::clamp ((int) x[pLfoDest], 0, numLfoDests - 1);
                static const int destParam[numLfoDests] { pPitch, pDecay, pColor, pShape, pSweep, pContour, pVolume, pPan };
                x[destParam[dest]] = std::clamp (x[destParam[dest]] + depth * tr.lfoLast * 0.5, 0.0, 1.0);
                double y = voiceSample (v, x) * v.velocity;
                // drive: a soft clip, louder in than out
                if (x[pDrive] > 0.0) { const double g = 1.0 + x[pDrive] * 9.0; y = std::tanh (y * g) / std::tanh (g) ; }
                y *= x[pVolume] * x[pVolume] * 0.8;
                tr.level = std::max (std::abs (y), tr.level * 0.9995);
                // its own output: the dry voice, at the level it has in the main mix when panned centre
                if (own[t] != nullptr) own[t][i] = (float) (y * 1.41421356 * master);
                if (! away[t])
                {
                    const double pan = x[pPan] * 0.5 * juce::MathConstants<double>::pi;
                    l += y * std::cos (pan) * 1.41421356; r += y * std::sin (pan) * 1.41421356;
                }
                sendD += y * x[pDelay]; sendR += y * x[pReverb];   // the sends stay on the main output
            }

            // ---- the delay: ping-pong, filtered in the loop ----
            const int dl = (int) delayL.size();
            double rp = delayWrite - delaySamples;
            while (rp < 0) rp += dl;
            const int i0 = (int) rp, i1 = (i0 + 1) % dl;
            const double fr = rp - i0;
            const double dL = delayL[(size_t) i0] + (delayL[(size_t) i1] - delayL[(size_t) i0]) * fr;
            const double dR = delayR[(size_t) i0] + (delayR[(size_t) i1] - delayR[(size_t) i0]) * fr;
            delayLp[0] += (dR * fbk + sendD - delayLp[0]) * dLp;
            delayLp[1] += (dL * fbk - delayLp[1]) * dLp;
            delayL[(size_t) delayWrite] = (float) delayLp[0];
            delayR[(size_t) delayWrite] = (float) delayLp[1];
            delayWrite = (delayWrite + 1) % dl;
            l += dL; r += dR;

            // ---- the reverb ----
            double o[fdnN], sum = 0.0;
            for (int k = 0; k < fdnN; ++k)
            {
                o[k] = fdn[k][(size_t) fdnPos[k]];
                sum += o[k];
            }
            const double hh = 2.0 / fdnN * sum;
            double wetL = 0.0, wetR = 0.0;
            for (int k = 0; k < fdnN; ++k)
            {
                const double g = std::pow (10.0, -3.0 * fdnLen[k] / (rt60 * sr));
                fdnLp[k] += ((o[k] - hh) * g - fdnLp[k]) * rLp;
                fdn[k][(size_t) fdnPos[k]] = (float) (fdnLp[k] + sendR * 0.35);
                fdnPos[k] = (fdnPos[k] + 1) % fdnLen[k];
                (k & 1 ? wetR : wetL) += o[k];
            }
            l += wetL * 0.5; r += wetR * 0.5;

            outL[i] = (float) (l * master);
            if (outR != nullptr) outR[i] = (float) (r * master);
        }
        for (int t = 0; t < numTracks; ++t) trackLevels[(size_t) t] = (float) tracks[(size_t) t].level;
        if (! running) shown = -1;
    }
};

//==============================================================================
Processor::Processor()
    : juce::AudioProcessor (BusesProperties().withOutput ("Out", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("1", juce::AudioChannelSet::mono(), true)
                                             .withOutput ("2", juce::AudioChannelSet::mono(), true)
                                             .withOutput ("3", juce::AudioChannelSet::mono(), true)
                                             .withOutput ("4", juce::AudioChannelSet::mono(), true)
                                             .withOutput ("5", juce::AudioChannelSet::mono(), true)
                                             .withOutput ("6", juce::AudioChannelSet::mono(), true)),
      engine (std::make_unique<Engine>())
{
    edit.init();
    engine->s = edit;
}

Processor::~Processor() = default;

void Processor::changed()
{
    auto copy = std::make_unique<State> (edit);
    const juce::SpinLock::ScopedLockType sl (pendingLock);
    pending = std::move (copy);
    ++generation;
}

void Processor::setPlaying (bool on) { playing = on; }

void Processor::setOutputLeavesMain (int aux, bool leaves) noexcept
{
    if (aux >= 0 && aux < numTracks) leavesMain[(size_t) aux] = leaves;
}

juce::String Processor::outputLabel (int aux) const
{
    if (aux < 0 || aux >= numTracks) return {};
    return juce::String (aux + 1) + " " + machineName (std::clamp (edit.sound[aux][pMachine], 0, numMachines - 1));
}

void Processor::hit (int track, int velocity)
{
    const auto scope = hitFifo.write (1);
    if (scope.blockSize1 > 0) hits[(size_t) scope.startIndex1] = { std::clamp (track, 0, numTracks - 1), std::clamp (velocity, 1, 127) };
}

void Processor::prepareToPlay (double sampleRate, int)
{
    engine->prepare (sampleRate);
}

void Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    auto& e = *engine;
    // the window's changes (a pattern switch waits for the end of the one playing)
    {
        const juce::SpinLock::ScopedTryLockType sl (pendingLock);
        if (sl.isLocked() && pending != nullptr)
        {
            const int playingPattern = e.s.pattern;
            const int wanted = pending->pattern;
            e.s = *pending;
            pending.reset();
            if (e.running && wanted != playingPattern) { e.s.pattern = playingPattern; e.queuedPattern = wanted; }
        }
    }
    const bool wantRun = playing.load();
    if (wantRun && ! e.running) { e.running = true; e.stepPos = 0.0; e.lastStep = -1; }
    if (! wantRun && e.running) { e.running = false; e.numPending = 0; if (e.queuedPattern >= 0) { e.s.pattern = e.queuedPattern; e.queuedPattern = -1; } }
    // hits from the window
    const auto scope = hitFifo.read (hitFifo.getNumReady());
    for (int i = 0; i < scope.blockSize1; ++i) e.strike (hits[(size_t) (scope.startIndex1 + i)].track, nullptr, hits[(size_t) (scope.startIndex1 + i)].velocity / 127.0);
    for (int i = 0; i < scope.blockSize2; ++i) e.strike (hits[(size_t) (scope.startIndex2 + i)].track, nullptr, hits[(size_t) (scope.startIndex2 + i)].velocity / 127.0);
    // MIDI: C1..F1 (notes 36..41) play the six tracks
    for (const auto m : midi)
    {
        const auto msg = m.getMessage();
        if (msg.isNoteOn() && msg.getNoteNumber() >= 36 && msg.getNoteNumber() < 36 + numTracks)
            e.schedule (m.samplePosition, msg.getNoteNumber() - 36, 0, msg.getVelocity(), false);
    }
    midi.clear();
    buffer.clear();
    e.process (buffer, shownStep, levels, leavesMain);
}

//==============================================================================
std::unique_ptr<juce::XmlElement> Processor::toXml (bool) const
{
    auto x = std::make_unique<juce::XmlElement> ("FMDRUMS");
    x->setAttribute ("version", 1);
    const auto& s = edit;
    for (auto [name, v] : { std::pair<const char*, int> { "pattern", s.pattern }, { "tempo", s.tempo }, { "swing", s.swing },
                            { "delayTime", s.delayTime }, { "delayFeedback", s.delayFeedback }, { "delayTone", s.delayTone },
                            { "reverbSize", s.reverbSize }, { "reverbDecay", s.reverbDecay }, { "reverbTone", s.reverbTone }, { "master", s.master } })
        x->setAttribute (name, v);
    auto* kit = x->createNewChildElement ("KIT");
    for (int t = 0; t < numTracks; ++t)
    {
        auto* tr = kit->createNewChildElement ("TRACK");
        for (int p = 0; p < numParams; ++p) tr->setAttribute (params[p].id, s.sound[t][p]);
        tr->setAttribute ("mute", s.mute[t]);
    }
    auto* pats = x->createNewChildElement ("PATTERNS");
    for (int pi = 0; pi < numPatterns; ++pi)
    {
        const auto& pat = s.patterns[(size_t) pi];
        if (pat.empty() && pat.length == 16) continue;
        auto* pe = pats->createNewChildElement ("PATTERN");
        pe->setAttribute ("index", pi);
        pe->setAttribute ("length", pat.length);
        for (int t = 0; t < numTracks; ++t)
            for (int k = 0; k < maxSteps; ++k)
            {
                const auto& st = pat.steps[t][k];
                if (! st.on && ! st.hasLocks()) continue;
                auto* se = pe->createNewChildElement ("STEP");
                se->setAttribute ("track", t); se->setAttribute ("step", k); se->setAttribute ("on", st.on);
                se->setAttribute ("velocity", st.velocity); se->setAttribute ("chance", st.chance); se->setAttribute ("retrig", st.retrig);
                for (int p = 0; p < numParams; ++p) if (st.lock[p] >= 0) se->setAttribute (juce::String ("lock_") + params[p].id, st.lock[p]);
            }
    }
    return x;
}

void Processor::fromXml (const juce::XmlElement& x)
{
    if (! x.hasTagName ("FMDRUMS")) return;
    State s;
    s.init();
    for (auto& p : s.patterns) p = Pattern();
    auto get = [&x] (const char* n, int def, int lo, int hi) { return std::clamp (x.getIntAttribute (n, def), lo, hi); };
    s.pattern = get ("pattern", 0, 0, numPatterns - 1); s.tempo = get ("tempo", 1200, 400, 2400); s.swing = get ("swing", 50, 50, 75);
    s.delayTime = get ("delayTime", d8dot, 0, numDivisions - 1); s.delayFeedback = get ("delayFeedback", 50, 0, 127); s.delayTone = get ("delayTone", 80, 0, 127);
    s.reverbSize = get ("reverbSize", 70, 0, 127); s.reverbDecay = get ("reverbDecay", 60, 0, 127); s.reverbTone = get ("reverbTone", 70, 0, 127);
    s.master = get ("master", 100, 0, 127);
    if (auto* kit = x.getChildByName ("KIT"))
    {
        int t = 0;
        for (auto* tr : kit->getChildWithTagNameIterator ("TRACK"))
        {
            if (t >= numTracks) break;
            for (int p = 0; p < numParams; ++p) s.sound[t][p] = std::clamp (tr->getIntAttribute (params[p].id, params[p].def), params[p].lo, params[p].hi);
            s.mute[t] = tr->getBoolAttribute ("mute");
            ++t;
        }
    }
    if (auto* pats = x.getChildByName ("PATTERNS"))
        for (auto* pe : pats->getChildWithTagNameIterator ("PATTERN"))
        {
            auto& pat = s.patterns[(size_t) std::clamp (pe->getIntAttribute ("index"), 0, numPatterns - 1)];
            pat.length = std::clamp (pe->getIntAttribute ("length", 16), 1, maxSteps);
            for (auto* se : pe->getChildWithTagNameIterator ("STEP"))
            {
                auto& st = pat.steps[std::clamp (se->getIntAttribute ("track"), 0, numTracks - 1)][std::clamp (se->getIntAttribute ("step"), 0, maxSteps - 1)];
                st.on = (int8_t) (se->getIntAttribute ("on") != 0 ? 1 : 0);
                st.velocity = (int8_t) std::clamp (se->getIntAttribute ("velocity", 100), 1, 127);
                st.chance = (int8_t) std::clamp (se->getIntAttribute ("chance", 100), 1, 100);
                st.retrig = (int8_t) std::clamp (se->getIntAttribute ("retrig"), 0, 3);
                for (int p = 0; p < numParams; ++p)
                {
                    const auto key = juce::String ("lock_") + params[p].id;
                    if (se->hasAttribute (key)) st.lock[p] = (int16_t) std::clamp (se->getIntAttribute (key), params[p].lo, params[p].hi);
                }
            }
        }
    edit = s;
    changed();
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
    static const presets::Kind kind { "FM Drums", "tffd", "TFFD", "KIT", [] (const juce::XmlElement& s)
    {
        juce::StringArray machines;
        if (auto* kit = s.getChildByName ("KIT"))
            for (auto* tr : kit->getChildWithTagNameIterator ("TRACK")) machines.add (machineName (tr->getIntAttribute ("machine")));
        return juce::String (s.getIntAttribute ("tempo", 1200) / 10.0, 1) + " BPM" + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + machines.joinIntoString (" ");
    } };
    return kind;
}

const presets::Kind& Processor::presetKind() const { return fmd::presetKind(); }
} // namespace tf::fmd
