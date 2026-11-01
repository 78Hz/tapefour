#include "N5Synth.h"

namespace tf::n5
{
namespace
{
    constexpr double pi = 3.14159265358979323846, twoPi = 2.0 * pi;
    constexpr int maxVoices = 64, controlRate = 32;
    constexpr float voiceGain = 0.18f;   // a voice at full level, before the output (set by ear: a chord stays clear of clipping)

    /** A single cycle of a standard wave, 2048 points (the tables are band-limited from it). */
    std::vector<float> standardCycle (int wave)
    {
        std::vector<float> c (2048);
        for (int i = 0; i < 2048; ++i)
        {
            const double t = i / 2048.0;
            double v = 0.0;
            switch (wave)
            {
                case sine: v = std::sin (twoPi * t); break;
                case triangle: v = t < 0.25 ? 4 * t : t < 0.75 ? 2 - 4 * t : 4 * t - 4; break;
                case saw: v = 1.0 - 2.0 * t; break;
                case square: v = t < 0.5 ? 1.0 : -1.0; break;
                case pulse25: v = t < 0.25 ? 1.0 : -1.0; break;
                case pulse12: v = t < 0.125 ? 1.0 : -1.0; break;
                default: break;
            }
            c[(size_t) i] = (float) v;
        }
        return c;
    }

    struct Eg
    {
        // stages: 0 attack, 1 decay, 2 slope, 3 hold (sustain), 4 release, 5 done
        float value = 0.0f, from = 0.0f;
        int stage = 0;
        double t = 0.0, len = 0.0;
        float targets[5] {};
        double times[5] {};
        void start (float startLevel)
        {
            value = from = startLevel; stage = 0; t = 0.0; len = times[0];
        }
        void release()
        {
            if (stage >= 4) return;
            from = value; stage = 4; t = 0.0; len = times[4];
        }
        void advance (double dt) noexcept
        {
            if (stage == 3 || stage >= 5) return;
            t += dt;
            for (;;)
            {
                if (t >= len)
                {
                    value = from = targets[stage];
                    t -= len;
                    stage = stage == 2 ? 3 : stage == 4 ? 5 : stage + 1;
                    if (stage == 3 || stage == 5) { t = 0.0; return; }
                    len = times[stage];
                    continue;
                }
                value = from + (targets[stage] - from) * (float) (t / len);
                return;
            }
        }
    };

    struct Lfo
    {
        double phase = 0.0, age = 0.0;
        float hold = 0.0f;
        uint32_t seed = 1;
        float next (int wave, double hz, double dt, double delay, double fade) noexcept
        {
            age += dt;
            const double before = phase;
            phase += hz * dt;
            if (phase >= 1.0)
            {
                phase -= std::floor (phase);
                seed = seed * 1664525u + 1013904223u;
                hold = (float) ((seed >> 8) / 8388608.0 - 1.0);
            }
            juce::ignoreUnused (before);
            float v = 0.0f;
            switch (wave)
            {
                case 0: v = (float) (phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase); break;   // TRI
                case 1: v = (float) (2.0 * phase - 1.0); break;                                      // SAW UP
                case 2: v = (float) (1.0 - 2.0 * phase); break;                                      // SAW DOWN
                case 3: v = phase < 0.5 ? 1.0f : -1.0f; break;                                       // SQUARE 1
                case 4: v = phase < 0.5 ? 1.0f : 0.0f; break;                                        // SQUARE 2 (one-sided)
                case 5: default: v = hold; break;                                                    // RANDOM
            }
            if (age < delay) return 0.0f;
            const double amount = fade <= 0.0 ? 1.0 : std::min (1.0, (age - delay) / fade);
            return v * (float) amount;
        }
    };

    struct Filter   // the VDF: a two-pole low-pass; Color lifts it at the cutoff (Q 0.6 to 2: set by ear)
    {
        float g = 0.1f, k = 1.4f, s1 = 0, s2 = 0;
        void set (double hz, double q, double sr)
        {
            g = (float) std::tan (pi * std::clamp (hz, 16.0, sr * 0.45) / sr);
            k = (float) (1.0 / q);
        }
        float run (float x) noexcept
        {
            const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
            const float v3 = x - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3;
            s1 = 2 * v1 - s1; s2 = 2 * v2 - s2;
            return v2;
        }
    };

    // ---- the arpeggiator's patterns. Their names are the manual's; what each plays isn't published: set by ear ----
    struct Step { int index; int octave; float vel; float gate; };   // index: 0.. a held note (by order), -1 rest, 99 all
    struct Pattern { int length; Step steps[16]; };
    const Pattern patterns[] {
        { 4, { { 0, 0, 1, 1 }, { 2, 0, .8f, 1 }, { 1, 0, .9f, 1 }, { 3, 0, .8f, 1 } } },                                          // ARP 1
        { 6, { { 0, 0, 1, 1 }, { 1, 0, .8f, 1 }, { 0, 0, .9f, 1 }, { 2, 0, .8f, 1 }, { 0, 0, .9f, 1 }, { 3, 0, .8f, 1 } } },       // ARP 2
        { 8, { { 0, 0, 1, 1 }, { 0, 1, .8f, .5f }, { 1, 0, .9f, 1 }, { 1, 1, .8f, .5f }, { 2, 0, .9f, 1 }, { 2, 1, .8f, .5f }, { 3, 0, .9f, 1 }, { 3, 1, .8f, .5f } } },   // ARP 3
        { 4, { { 99, 0, 1, .6f }, { -1, 0, 0, 0 }, { 99, 0, .7f, .3f }, { -1, 0, 0, 0 } } },                                      // ARP 4
        { 8, { { 0, 0, 1, .5f }, { 1, 0, .7f, .5f }, { -1, 0, 0, 0 }, { 2, 0, .8f, .5f }, { 3, 0, .7f, .5f }, { -1, 0, 0, 0 }, { 2, 0, .8f, .5f }, { 1, 0, .7f, .5f } } },   // ARP 5
        { 8, { { 3, 0, 1, 1 }, { 1, 0, .8f, 1 }, { 2, 0, .9f, 1 }, { 0, 0, .8f, 1 }, { 3, 1, 1, 1 }, { 1, 1, .8f, 1 }, { 2, 1, .9f, 1 }, { 0, 1, .8f, 1 } } },   // ARP 6
        { 8, { { 0, 0, 1, .5f }, { 0, 0, .6f, .3f }, { 0, 1, .9f, .4f }, { 0, 0, .6f, .3f }, { 0, 0, 1, .5f }, { 0, 0, .6f, .3f }, { 0, 1, .9f, .4f }, { 1, 0, .7f, .3f } } },   // B-TECHNO
        { 8, { { -1, 0, 0, 0 }, { 0, 0, 1, .5f }, { -1, 0, 0, 0 }, { 0, 1, .9f, .5f }, { -1, 0, 0, 0 }, { 0, 0, 1, .5f }, { -1, 0, 0, 0 }, { 0, 1, .9f, .5f } } },   // B-DANCE
        { 8, { { 0, 0, 1, .4f }, { -1, 0, 0, 0 }, { 0, 1, .8f, .2f }, { 0, 0, .9f, .4f }, { -1, 0, 0, 0 }, { 1, 0, .8f, .4f }, { 0, 1, .7f, .2f }, { -1, 0, 0, 0 } } },   // B-FUNK
        { 8, { { 0, 0, 1, .8f }, { -1, 0, 0, 0 }, { -1, 0, 0, 0 }, { 1, 0, .8f, .4f }, { 2, 0, .9f, .8f }, { -1, 0, 0, 0 }, { 1, 0, .7f, .4f }, { 0, 0, .8f, .4f } } },   // B-SOUL
        { 4, { { 0, 0, 1, .9f }, { 1, 0, .8f, .9f }, { 2, 0, .9f, .9f }, { 3, 0, .8f, .9f } } },                                  // B-JAZZ (walking)
        { 8, { { 0, 0, 1, .3f }, { 2, 0, .6f, .2f }, { 1, 0, .9f, .3f }, { 2, 0, .6f, .2f }, { 0, 0, 1, .3f }, { 2, 0, .6f, .2f }, { 1, 0, .9f, .3f }, { 2, 0, .8f, .2f } } },   // D-TECHNO
        { 16, { { 0, 0, 1, .3f }, { 2, 0, .5f, .2f }, { 1, 0, .9f, .3f }, { -1, 0, 0, 0 }, { 2, 0, .5f, .2f }, { 0, 0, .8f, .3f }, { 1, 0, .9f, .3f }, { 2, 0, .5f, .2f },
                { -1, 0, 0, 0 }, { 0, 0, .9f, .3f }, { 1, 0, 1, .3f }, { 2, 0, .5f, .2f }, { 1, 0, .7f, .3f }, { 2, 0, .5f, .2f }, { 1, 0, .9f, .3f }, { 2, 0, .6f, .2f } } },   // D-JUNGLE
        { 8, { { 0, 0, 1, .3f }, { -1, 0, 0, 0 }, { 2, 0, .6f, .2f }, { 0, 0, .8f, .3f }, { 1, 0, 1, .3f }, { 2, 0, .6f, .2f }, { 0, 0, .7f, .3f }, { 2, 0, .6f, .2f } } },   // D-FUNK
        { 8, { { 0, 0, 1, .3f }, { 2, 0, .6f, .2f }, { -1, 0, 0, 0 }, { 0, 0, .8f, .3f }, { 1, 0, 1, .3f }, { 2, 0, .6f, .2f }, { 2, 0, .5f, .2f }, { -1, 0, 0, 0 } } },   // D-R&B
    };
}

//==============================================================================
void WaveTable::build (const std::vector<std::complex<double>>& harmonics)
{
    juce::dsp::FFT fft (11);
    std::vector<float> data (2 * size);
    for (int level = 0; level < levels; ++level)
    {
        const int maxHarm = std::max (1, (size / 2) >> level);
        std::fill (data.begin(), data.end(), 0.0f);
        for (int h = 1; h <= maxHarm && h < (int) harmonics.size(); ++h)
        {
            data[(size_t) (2 * h)] = (float) harmonics[(size_t) h].real();
            data[(size_t) (2 * h + 1)] = (float) harmonics[(size_t) h].imag();
        }
        fft.performRealOnlyInverseTransform (data.data());
        auto& t = table[(size_t) level];
        for (int i = 0; i < size; ++i) t[(size_t) i] = data[(size_t) i];
        t[size] = t[0];
    }
    // every wave to the same peak: the N5's multisamples are levelled alike
    float peak = 1.0e-9f;
    for (int k = 0; k < size; ++k) peak = std::max (peak, std::abs (table[0][(size_t) k]));
    const float norm = 1.0f / peak;
    for (auto& t : table) for (auto& v : t) v *= norm;
}

float WaveTable::read (double phase, double increment) const noexcept
{
    // the table whose top harmonic stays below Nyquist
    int level = 0;
    double top = (size / 2) * increment;   // the top harmonic's frequency, in cycles a sample
    while (top > 0.45 && level < levels - 1) { top *= 0.5; ++level; }
    const auto& t = table[(size_t) level];
    const double x = (phase - std::floor (phase)) * size;
    const int i = (int) x;
    const float f = (float) (x - i);
    return t[(size_t) i] + (t[(size_t) i + 1] - t[(size_t) i]) * f;
}

static std::vector<std::complex<double>> harmonicsOfCycle (const std::vector<float>& cycle)
{
    const int n = (int) cycle.size();
    int order = 0;
    while ((1 << order) < n) ++order;
    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) (2 * n), 0.0f);
    std::copy (cycle.begin(), cycle.end(), data.begin());
    fft.performRealOnlyForwardTransform (data.data());
    std::vector<std::complex<double>> h ((size_t) (n / 2 + 1));
    for (int k = 1; k <= n / 2; ++k) h[(size_t) k] = { data[(size_t) (2 * k)] / (double) n, data[(size_t) (2 * k + 1)] / (double) n };
    return h;
}

//==============================================================================
struct Processor::Engine
{
    double sr = 48000.0;
    // the memory, as the engine plays it
    juce::SpinLock bankLock;
    BankData pending, bank;
    std::atomic<int> generation { 0 };
    int seen = -1;
    // the waves: six standard, sixteen drawn (each two tables: the editor writes the one not in use)
    std::array<WaveTable, 6> standard;
    std::array<std::array<WaveTable, 2>, numDrawn> drawnTables;
    juce::SpinLock waveLock;
    std::array<std::atomic<int>, numDrawn> drawnActive {};
    std::array<int, numDrawn> drawnInUse {};

    struct Voice
    {
        bool active = false, released = false, held = false, sustained = false;
        int note = 60, layer = 0, part = -1;
        float vel = 1.0f;
        int prog = 0;
        double phase = 0.0, inc = 0.0, delayLeft = 0.0;
        double glide = 0.0;   // semitones still to glide (portamento)
        Eg pEg, fEg, aEg;
        Lfo pLfo, fLfo, aLfo;
        Filter filt;
        float amp = 0.0f, ampTarget = 0.0f, ampStep = 0.0f;
        float panL = 0.7f, panR = 0.7f, sendC = 0.0f, sendD = 0.0f;
        bool panOff = false;
        int ctrl = 0;
        int64_t age = 0;
        uint32_t noise = 22222;
    };
    std::array<Voice, maxVoices> voices;
    int64_t ages = 0;
    juce::Random rnd;
    // controllers
    float bend = 0.0f, mod1 = 0.0f, mod2 = 0.0f, mod3 = 0.0f, after = 0.0f;
    bool damper = false;
    int cc7 = 127, cc11 = 127, ccCutoff = 64, ccAttack = 64, ccRelease = 64, ccDecay = 64;
    double lastNotePitch = -1.0;
    std::vector<int> monoStack;
    int heldKeys = 0;
    // arpeggiator
    struct Held { int note, vel; };
    std::vector<Held> arpHeld, arpLatched;
    bool arpLatchedActive = false;
    int arpStep = 0, arpVelocity = 100;
    double arpNext = 0.0, arpClock = 0.0;
    std::vector<std::pair<double, int>> arpOffs;   // when (seconds on the clock) each note goes off
    // the panel's keyboard
    juce::SpinLock panelLock;
    juce::MidiBuffer panelNotes;
    // effects
    EffectRack rack;
    juce::AudioBuffer<float> buses;   // AB L, AB R, C L, C R, D L, D R
    FxContext ctx;
    int noteOnsThisBlock = 0;
    bool firstNoteOnThisBlock = false;
    float lastVdaEg = 0.0f;

    void prepare (double rate, int block)
    {
        sr = rate;
        buses.setSize (6, std::max (block, 64));
        rack.prepare (rate, std::max (block, 64));
        for (auto& v : voices) v.active = false;
        arpOffs.reserve (512); monoStack.reserve (256); arpHeld.reserve (256); arpLatched.reserve (256);
        panelNotes.ensureSize (1024);
    }

    // ---- notes ----
    const ProgramData& programOf (int part) const
    {
        if (bank.combiMode == 0 || part < 0) return bank.programs[(size_t) std::clamp (bank.program, 0, numPrograms - 1)];
        const auto& c = bank.combis[(size_t) std::clamp (bank.combi, 0, numCombis - 1)];
        return bank.programs[(size_t) std::clamp (c.timbre[part][tProgram], 0, numPrograms - 1)];
    }

    Voice& allocate()
    {
        for (auto& v : voices) if (! v.active) return v;
        Voice* best = nullptr;
        for (auto& v : voices)   // the oldest released first, then the oldest
            if (best == nullptr || (v.released && ! best->released) || (v.released == best->released && v.age < best->age)) best = &v;
        return *best;
    }

    static double timeScale (int sw, int amount, double x)   // x: -1..1; + switch: higher x shorter
    {
        if (sw == 0 || amount == 0) return 1.0;
        const double s = sw == 1 ? 1.0 : -1.0;
        return std::pow (2.0, -s * amount / 127.0 * x * 2.0);
    }

    static bool inRegion (int mode, int note, int key) { return mode == 3 || (mode == 1 && note < key) || (mode == 2 && note > key); }

    void startLayer (int note, float vel, int layer, int part, const ProgramData& p)
    {
        const int* L = p.layer[layer];
        const int v127 = juce::roundToInt (vel * 127.0f);
        if (v127 < L[oVelBottom] || v127 > L[oVelTop]) return;
        auto& v = allocate();
        v = Voice();
        v.active = true; v.note = note; v.layer = layer; v.part = part; v.vel = vel; v.held = true;
        v.age = ++ages;
        v.noise = (uint32_t) rnd.nextInt();
        v.delayLeft = egSeconds (L[oDelay]);
        v.pLfo.seed = (uint32_t) rnd.nextInt();
        const double vx = vel * 2.0 - 1.0;   // velocity, -1..1
        // pitch EG
        {
            const double ts = std::pow (2.0, -L[peTimeVel] / 127.0 * vx * 2.0);
            v.pEg.times[0] = egSeconds (L[peAttackTime]) * ts; v.pEg.targets[0] = (float) L[peAttackLevel] / 127.0f;
            v.pEg.times[1] = egSeconds (L[peDecayTime]) * ts;  v.pEg.targets[1] = 0.0f;
            v.pEg.times[2] = 0.0;                                v.pEg.targets[2] = 0.0f;
            v.pEg.times[4] = egSeconds (L[peReleaseTime]) * ts; v.pEg.targets[4] = (float) L[peReleaseLevel] / 127.0f;
            v.pEg.start ((float) L[peStart] / 127.0f);
        }
        // VDF EG: times by velocity and keyboard, each stage as its switch says; the part's attack/decay/release knobs
        {
            const double kx = inRegion (L[fKtMode], note, L[fKtKey]) ? (note - L[fKtKey]) / 12.0 : 0.0;
            const int tv[4] { L[feTvSwA], L[feTvSwD], L[feTvSwS], L[feTvSwR] }, kt[4] { L[fKtSwA], L[fKtSwD], L[fKtSwS], L[fKtSwR] };
            const int t[4] { L[feAttackTime], L[feDecayTime], L[feSlopeTime], L[feReleaseTime] };
            const double cc[4] { std::pow (2.0, (ccAttack - 64) / 32.0), std::pow (2.0, (ccDecay - 64) / 32.0), 1.0, std::pow (2.0, (ccRelease - 64) / 32.0) };
            const int slot[4] { 0, 1, 2, 4 };
            for (int k = 0; k < 4; ++k)
                v.fEg.times[slot[k]] = egSeconds (t[k]) * timeScale (tv[k], L[feTimeVel], vx) * timeScale (kt[k], L[fKtEgTime], kx) * cc[k];
            v.fEg.targets[0] = (float) L[feAttackLevel] / 127.0f; v.fEg.targets[1] = (float) L[feBreak] / 127.0f;
            v.fEg.targets[2] = (float) L[feSustain] / 127.0f;     v.fEg.targets[4] = (float) L[feReleaseLevel] / 127.0f;
            v.fEg.start (0.0f);
        }
        // VDA EG
        {
            const double kx = inRegion (L[aKtMode], note, L[aKtKey]) ? (note - L[aKtKey]) / 12.0 : 0.0;
            const int tv[4] { L[aeTvSwA], L[aeTvSwD], L[aeTvSwS], L[aeTvSwR] }, kt[4] { L[aKtSwA], L[aKtSwD], L[aKtSwS], L[aKtSwR] };
            const int t[4] { L[aeAttackTime], L[aeDecayTime], L[aeSlopeTime], L[aeReleaseTime] };
            const double cc[4] { std::pow (2.0, (ccAttack - 64) / 32.0), std::pow (2.0, (ccDecay - 64) / 32.0), 1.0, std::pow (2.0, (ccRelease - 64) / 32.0) };
            const int slot[4] { 0, 1, 2, 4 };
            for (int k = 0; k < 4; ++k)
                v.aEg.times[slot[k]] = egSeconds (t[k]) * timeScale (tv[k], L[aeTimeVel], vx) * timeScale (kt[k], L[aKtEgTime], kx) * cc[k];
            v.aEg.targets[0] = (float) L[aeAttackLevel] / 127.0f; v.aEg.targets[1] = (float) L[aeBreak] / 127.0f;
            v.aEg.targets[2] = (float) L[aeSustain] / 127.0f;     v.aEg.targets[4] = 0.0f;
            v.aEg.start (0.0f);
        }
        // pan and sends: a combination's timbre's pan adds to the program's; its sends replace the program's
        int pan = L[xPan];
        float sc = (float) L[xSendC] / 127.0f, sd = (float) L[xSendD] / 127.0f;
        if (part >= 0 && bank.combiMode != 0)
        {
            const auto& tm = bank.combis[(size_t) bank.combi].timbre[part];
            if (tm[tPan] <= 0 || pan <= 0) pan = 0;
            else if (tm[tPan] >= 128 || pan >= 128) pan = 128;
            else pan = std::clamp (pan + tm[tPan] - 64, 1, 127);
            sc = (float) tm[tSendC] / 127.0f; sd = (float) tm[tSendD] / 127.0f;
        }
        if (pan <= 0) pan = 1 + rnd.nextInt (127);
        v.panOff = pan >= 128;
        const double a = (std::clamp (pan, 1, 127) - 1) / 126.0 * pi * 0.5;
        v.panL = (float) std::cos (a); v.panR = (float) std::sin (a);
        v.sendC = sc; v.sendD = sd;
        v.prog = part;
        // portamento: from the last note's pitch
        if (p.global[gPortaSw] != 0 && lastNotePitch >= 0.0) v.glide = lastNotePitch - note;
        v.ctrl = 0;
        ++noteOnsThisBlock;
    }

    void noteOn (int note, int velocity)
    {
        const float vel = (float) std::clamp (velocity, 1, 127) / 127.0f;
        if (heldKeys == 0) firstNoteOnThisBlock = true;
        ++heldKeys;
        auto play = [&] (int part, const ProgramData& p, int transposed)
        {
            if (p.global[gMonoPoly] == 0)
            {
                // mono: one note at a time, the last played; held notes glide (or jump) to it without restarting
                bool legato = false;
                for (auto& v : voices)
                    if (v.active && ! v.released && v.prog == part)
                    {
                        legato = true;
                        if (p.global[gPortaSw] != 0) v.glide += v.note - transposed; else v.glide = 0.0;
                        v.note = transposed;
                    }
                if (legato) return;
            }
            startLayer (transposed, vel, 0, part, p);
            if (p.global[gOscMode] == 1) startLayer (transposed, vel, 1, part, p);
        };
        if (bank.combiMode == 0)
            play (-1, programOf (-1), note);
        else
        {
            const auto& c = bank.combis[(size_t) bank.combi];
            for (int t = 0; t < numTimbres; ++t)
            {
                const auto* tm = c.timbre[t];
                if (tm[tProgram] < 0 || tm[tRxNoteOn] == 0) continue;
                if (note < tm[tNoteLo] || note > tm[tNoteHi] || velocity < tm[tVelLo] || velocity > tm[tVelHi]) continue;
                play (t, bank.programs[(size_t) tm[tProgram]], note);
            }
        }
        monoStack.push_back (note);
        lastNotePitch = note;
    }

    void noteOff (int note)
    {
        heldKeys = std::max (0, heldKeys - 1);
        monoStack.erase (std::remove (monoStack.begin(), monoStack.end(), note), monoStack.end());
        for (auto& v : voices)
            if (v.active && v.held && v.note == note)
            {
                const auto& p = programOf (v.prog);
                // mono: back to the previous held note
                if (p.global[gMonoPoly] == 0 && ! monoStack.empty())
                {
                    const int back = monoStack.back();
                    if (p.global[gPortaSw] != 0) v.glide += v.note - back;
                    v.note = back;
                    continue;
                }
                v.held = false;
                if (damper) { v.sustained = true; continue; }
                v.released = true;
                v.pEg.release(); v.fEg.release(); v.aEg.release();
            }
    }

    void setDamper (bool down)
    {
        damper = down;
        if (down) return;
        for (auto& v : voices)
            if (v.active && v.sustained && ! v.held)
            {
                v.sustained = false; v.released = true;
                v.pEg.release(); v.fEg.release(); v.aEg.release();
            }
    }

    void allOff() { for (auto& v : voices) v.active = false; monoStack.clear(); heldKeys = 0; arpHeld.clear(); arpLatched.clear(); }

    // ---- the arpeggiator ----
    bool arpOn() const { return bank.arp[aOn] != 0; }

    double stepSeconds() const
    {
        static const double fraction[6] { 1.0, 2.0 / 3.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0 };
        return 60.0 / std::clamp (bank.arp[aSpeed], 40, 240) * fraction[std::clamp (bank.arp[aStep], 0, 5)];
    }

    void arpKey (int note, int velocity, bool on)
    {
        const int latch = bank.arp[aLatch];
        const bool latching = latch == 1 || latch == 3, keySync = latch == 2 || latch == 3;
        if (on)
        {
            if (arpHeld.empty())
            {
                if (latching) arpLatched.clear();
                if (keySync) { arpStep = 0; arpNext = arpClock; }
            }
            arpHeld.push_back ({ note, velocity });
            if (latching) arpLatched.push_back ({ note, velocity });
            arpVelocity = velocity;
        }
        else
            arpHeld.erase (std::remove_if (arpHeld.begin(), arpHeld.end(), [note] (const Held& h) { return h.note == note; }), arpHeld.end());
    }

    /** The notes the pattern plays from: held (or latched), sorted or as played, over the octaves. */
    std::vector<Held> arpNotes() const
    {
        const int latch = bank.arp[aLatch];
        auto notes = (latch == 1 || latch == 3) ? arpLatched : arpHeld;
        if (bank.arp[aSort] != 0) std::sort (notes.begin(), notes.end(), [] (const Held& a, const Held& b) { return a.note < b.note; });
        return notes;
    }

    void arpTick (double now)
    {
        // notes going off
        for (auto it = arpOffs.begin(); it != arpOffs.end();)
            if (it->first <= now) { noteOff (it->second); it = arpOffs.erase (it); }
            else ++it;
        if (! arpOn() || now < arpNext) return;
        const auto notes = arpNotes();
        const double step = stepSeconds();
        // swing: the even steps later or earlier
        const double swing = (arpStep % 2 == 1) ? 0.0 : bank.arp[aSwing] / 100.0 * step * 0.5;
        arpNext += step;
        const int stepIndex = arpStep++;
        if (notes.empty()) return;
        const int octaves = std::clamp (bank.arp[aOctave], 1, 4), type = std::clamp (bank.arp[aType], 1, 20);
        const int count = (int) notes.size(), span = count * octaves;
        std::vector<std::pair<int, float>> play;   // note, velocity scale
        float gateScale = 1.0f;
        auto at = [&] (int k) { k = ((k % span) + span) % span; return notes[(size_t) (k % count)].note + 12 * (k / count); };
        switch (type)
        {
            case 1: play.push_back ({ at (stepIndex), 1.0f }); break;
            case 2: play.push_back ({ at (span - 1 - stepIndex % span), 1.0f }); break;
            case 3: { const int cyc = std::max (1, 2 * span); const int k = stepIndex % cyc; play.push_back ({ at (k < span ? k : cyc - 1 - k), 1.0f }); break; }
            case 4: { const int cyc = std::max (1, 2 * span - 2); const int k = span <= 1 ? 0 : stepIndex % cyc; play.push_back ({ at (k < span ? k : cyc - k), 1.0f }); break; }
            case 5: play.push_back ({ at (juce::Random::getSystemRandom().nextInt (span)), 1.0f }); break;
            default:
            {
                const auto& pat = patterns[type - 6];
                const auto& st = pat.steps[stepIndex % pat.length];
                gateScale = st.gate;
                if (st.index == 99) for (int k = 0; k < count; ++k) play.push_back ({ notes[(size_t) k].note, st.vel });
                else if (st.index >= 0)
                {
                    const int pass = (stepIndex / pat.length) % octaves;
                    play.push_back ({ notes[(size_t) (st.index % count)].note + 12 * (st.octave + pass), st.vel });
                }
                break;
            }
        }
        const int velSetting = bank.arp[aVelocity], gateSetting = bank.arp[aGate];
        for (auto& [n, vs] : play)
        {
            const int note = std::clamp (n, 0, 127);
            const int vel = velSetting == 0 ? arpVelocity : velSetting >= 128 ? juce::roundToInt (100 * vs) : velSetting;
            const double gate = (gateSetting >= 101 ? gateScale : gateSetting / 100.0) * step;
            noteOn (note, std::clamp (vel, 1, 127));
            arpOffs.push_back ({ now + swing + std::max (0.005, gate), note });
        }
    }

    // ---- the sound ----
    void control (Voice& v, const ProgramData& p, double dt)
    {
        const int* L = p.layer[v.layer];
        const int* G = p.global;
        const int* tm = (v.part >= 0 && bank.combiMode != 0) ? bank.combis[(size_t) bank.combi].timbre[v.part] : nullptr;
        const bool rxBend = tm == nullptr || tm[tRxBend] != 0, rxAft = tm == nullptr || tm[tRxAft] != 0, rxCc = tm == nullptr || tm[tRxCc] != 0;
        const float b = rxBend ? bend : 0.0f, at = rxAft ? after : 0.0f, mw = rxCc ? mod1 : 0.0f;
        if (v.delayLeft > 0.0) { v.delayLeft -= dt; v.ampTarget = 0.0f; return; }
        v.pEg.advance (dt); v.fEg.advance (dt); v.aEg.advance (dt);
        const double vx = v.vel * 2.0 - 1.0;
        // pitch
        const double plDepth = ((float) L[plInt] + mw * (float) L[plModInt] + at * (float) L[plAftInt]) / 127.0;          // 127: a semitone (set by ear)
        const int plSpeed = std::clamp (L[plFreq] + juce::roundToInt (mw * (float) L[plModSpeed] + at * (float) L[plAftSpeed]), 0, 127);
        const float plv = v.pLfo.next (L[plWave], lfoHz (plSpeed), dt, egSeconds (L[plDelay]), egSeconds (L[plFade]));
        const double peAmount = L[peInt] / 127.0 * std::max (0.0, 1.0 + L[peIntVel] / 127.0 * vx) * 12.0;   // 127: an octave (set by ear)
        if (G[gPortaSw] != 0 && std::abs (v.glide) > 1.0e-4)
            v.glide *= std::exp (-dt / std::max (1.0e-4, egSeconds (G[gPortaTime]) * 0.3));
        else if (G[gPortaSw] == 0) v.glide = 0.0;
        const double slope = L[oSlope] / 10.0;
        double semis = 60.0 + slope * (v.note - 60) + L[oTranspose] + 12.0 * (L[oOctave] - 2) + L[oFine] / 100.0 + v.glide
                     + b * (float) G[gBendRange] + at * (float) G[gAftBend] + plv * plDepth + v.pEg.value * peAmount;
        if (tm != nullptr) semis += tm[tTranspose] + tm[tFine] / 100.0;
        v.inc = 440.0 * std::pow (2.0, (semis - 69.0) / 12.0) / sr;
        // VDF
        double cutoff = L[fCutoff] + (ccCutoff - 64);
        if (inRegion (L[fKtMode], v.note, L[fKtKey])) cutoff += L[fKtInt] / 127.0 * (v.note - L[fKtKey]) * 127.0 / 123.6;   // 127: tracks the keyboard
        cutoff += v.fEg.value * (float) L[feInt] * std::max (0.0, 1.0 + L[feIntVel] / 127.0 * vx);
        cutoff += v.fLfo.next (L[flWave], lfoHz (L[flFreq]), dt, egSeconds (L[flDelay]), egSeconds (L[flFade])) * ((float) L[flInt] + at * (float) G[gAftVdfLfo]) / 127.0 * 48.0;
        cutoff += b * (float) G[gBendVdf] / 127.0 * 64.0 + mw * (float) G[gModVdf] / 127.0 * 64.0 + at * (float) G[gAftVdf] / 127.0 * 64.0;
        const double color = std::clamp (L[fColor] + L[fColorVel] * vx, 0.0, 127.0);
        v.filt.set (cutoffHz (cutoff), 0.6 + 1.4 * color / 127.0, sr);
        // VDA
        double gain = L[oLevel] / 127.0;
        const double eg = v.aEg.value;
        gain *= eg * eg;
        const double ampVel = L[aeAmpVel] / 127.0;
        gain *= ampVel >= 0.0 ? 1.0 - ampVel * (1.0 - v.vel) : 1.0 + ampVel * v.vel;
        if (inRegion (L[aKtMode], v.note, L[aKtKey])) gain *= juce::Decibels::decibelsToGain (L[aKtInt] / 127.0 * (v.note - L[aKtKey]) / 12.0 * 6.0);
        const float alv = v.aLfo.next (L[alWave], lfoHz (L[alFreq]), dt, egSeconds (L[alDelay]), egSeconds (L[alFade]));
        const double depth = std::abs (L[alInt]) / 127.0;
        gain *= 1.0 - depth * 0.5 * (1.0 - (L[alInt] >= 0 ? alv : -alv));
        gain *= std::pow (2.0, at * (float) G[gAftVda] / 127.0);
        gain *= cc7 / 127.0 * cc11 / 127.0;
        if (tm != nullptr) gain *= tm[tVolume] / 127.0;
        v.ampTarget = (float) gain;
        lastVdaEg = std::max (lastVdaEg, (float) eg);
        if (v.aEg.stage >= 5) v.active = false;
    }

    void render (int start, int n)
    {
        float* ab[2] { buses.getWritePointer (0, start), buses.getWritePointer (1, start) };
        float* cb[2] { buses.getWritePointer (2, start), buses.getWritePointer (3, start) };
        float* db[2] { buses.getWritePointer (4, start), buses.getWritePointer (5, start) };
        const double dt = controlRate / sr;
        for (auto& v : voices)
        {
            if (! v.active) continue;
            const auto& p = programOf (v.prog);
            const int wave = p.layer[v.layer][oWave];
            const int drawnSlot = std::clamp (p.layer[v.layer][oDrawn] - 1, 0, numDrawn - 1);
            const WaveTable* table = wave == drawn ? &drawnTables[(size_t) drawnSlot][(size_t) drawnInUse[(size_t) drawnSlot]]
                                                  : wave == noise ? nullptr : &standard[(size_t) std::clamp (wave, 0, 5)];
            for (int i = 0; i < n; ++i)
            {
                if (v.ctrl-- <= 0)
                {
                    v.ctrl = controlRate - 1;
                    control (v, p, dt);
                    v.ampStep = (v.ampTarget - v.amp) / controlRate;
                    if (! v.active) break;
                }
                float x;
                if (table != nullptr) x = table->read (v.phase, v.inc);
                else { v.noise = v.noise * 1664525u + 1013904223u; x = (float) ((v.noise >> 8) / 8388608.0 - 1.0); }
                v.phase += v.inc;
                if (v.phase >= 1.0) v.phase -= std::floor (v.phase);
                const float y = v.filt.run (x) * v.amp * voiceGain;
                v.amp += v.ampStep;
                const float l = y * v.panL, r = y * v.panR;
                if (! v.panOff) { ab[0][i] += l; ab[1][i] += r; }
                cb[0][i] += l * v.sendC; cb[1][i] += r * v.sendC;
                db[0][i] += l * v.sendD; db[1][i] += r * v.sendD;
            }
        }
    }

    void handle (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())
        {
            if (arpOn()) arpKey (m.getNoteNumber(), m.getVelocity(), true);
            else noteOn (m.getNoteNumber(), m.getVelocity());
        }
        else if (m.isNoteOff())
        {
            if (arpOn()) arpKey (m.getNoteNumber(), 0, false);
            else noteOff (m.getNoteNumber());
        }
        else if (m.isPitchWheel()) bend = (float) (m.getPitchWheelValue() - 8192) / 8192.0f;
        else if (m.isChannelPressure()) after = (float) m.getChannelPressureValue() / 127.0f;
        else if (m.isAftertouch()) after = (float) m.getAfterTouchValue() / 127.0f;
        else if (m.isAllNotesOff() || m.isAllSoundOff()) allOff();
        else if (m.isController())
        {
            const int v = m.getControllerValue();
            switch (m.getControllerNumber())
            {
                case 1: mod1 = (float) v / 127.0f; break;
                case 16: mod2 = (float) v / 127.0f; break;
                case 17: mod3 = (float) v / 127.0f; break;
                case 64: setDamper (v >= 64); break;
                case 7: cc7 = v; break;
                case 11: cc11 = v; break;
                case 74: ccCutoff = v; break;   // the N5's realtime knobs (Performance Edit mode)
                case 73: ccAttack = v; break;
                case 72: ccRelease = v; break;
                case 75: ccDecay = v; break;
                default: break;
            }
        }
    }

    void process (juce::AudioBuffer<float>& out, juce::MidiBuffer& midi)
    {
        const int n = out.getNumSamples();
        {
            const juce::SpinLock::ScopedTryLockType sl (bankLock);
            if (sl.isLocked() && generation.load() != seen)
            {
                const bool modeChanged = bank.combiMode != pending.combiMode || bank.program != pending.program || bank.combi != pending.combi;
                bank = pending;
                seen = generation.load();
                if (modeChanged) for (auto& v : voices) if (v.active) { v.released = true; v.aEg.release(); }
            }
        }
        {
            const juce::SpinLock::ScopedTryLockType wl (waveLock);
            if (wl.isLocked()) for (int k = 0; k < numDrawn; ++k) drawnInUse[(size_t) k] = drawnActive[(size_t) k].load();
        }
        {
            const juce::SpinLock::ScopedTryLockType pl (panelLock);
            if (pl.isLocked() && ! panelNotes.isEmpty()) { midi.addEvents (panelNotes, 0, -1, 0); panelNotes.clear(); }
        }
        if (buses.getNumSamples() < n) buses.setSize (6, n, false, false, true);
        buses.clear();
        noteOnsThisBlock = 0;
        firstNoteOnThisBlock = false;
        lastVdaEg = 0.0f;
        // events in time: the MIDI, and the arpeggiator's steps
        int done = 0;
        auto runTo = [&] (int until)
        {
            while (done < until)
            {
                // the arpeggiator, a step at a time
                int stop = until;
                if (arpOn() || ! arpOffs.empty())
                {
                    double next = arpOn() ? arpNext : 1.0e30;
                    for (auto& o : arpOffs) next = std::min (next, o.first);
                    const int at = juce::jlimit (done, until, (int) std::ceil ((next - arpClock) * sr) + done);
                    if (at <= done) { arpTick (arpClock); stop = done + 1; }
                    else stop = std::min (until, at);
                }
                render (done, stop - done);
                arpClock += (stop - done) / sr;
                done = stop;
            }
        };
        for (const auto meta : midi)
        {
            runTo (juce::jlimit (0, n, meta.samplePosition));
            handle (meta.getMessage());
        }
        runTo (n);
        // the effects
        const auto& fx = bank.combiMode != 0 ? bank.combis[(size_t) bank.combi].fx : bank.programs[(size_t) bank.program].fx;
        ctx.mod[1] = mod1; ctx.mod[2] = mod2; ctx.mod[3] = mod3; ctx.mod[4] = after; ctx.mod[5] = lastVdaEg;
        ctx.noteOns = noteOnsThisBlock;
        ctx.firstNoteOn = firstNoteOnThisBlock;
        ctx.held = heldKeys + (int) arpHeld.size();
        ctx.lfoHz = lfoHz (programOf (-1).layer[0][flFreq]);
        const float* abp[2] { buses.getReadPointer (0), buses.getReadPointer (1) };
        const float* cp[2] { buses.getReadPointer (2), buses.getReadPointer (3) };
        const float* dp[2] { buses.getReadPointer (4), buses.getReadPointer (5) };
        rack.process (abp, cp, dp, out.getWritePointer (0), out.getNumChannels() > 1 ? out.getWritePointer (1) : buses.getWritePointer (2), n, fx, ctx);
    }
};

//==============================================================================
Processor::Processor()
    : juce::AudioProcessor (BusesProperties().withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
      engine (std::make_unique<Engine>())
{
    edit.init();
    for (int i = 0; i < numPrograms; ++i) programNames[(size_t) i] = i == 0 ? "INIT SAW" : "INIT " + juce::String (i).paddedLeft ('0', 2);
    for (int i = 0; i < numCombis; ++i) combiNames[(size_t) i] = "COMBI " + juce::String (i).paddedLeft ('0', 2);
    for (int w = 0; w < 6; ++w) engine->standard[(size_t) w].build (harmonicsOfCycle (standardCycle (w)));
    // drawn waves start as a sine with some of its odd harmonics: something to draw from
    for (int s = 0; s < numDrawn; ++s)
    {
        auto& shape = drawnShapes[(size_t) s];
        for (int i = 0; i < drawnPoints; ++i)
        {
            const double t = i / (double) drawnPoints;
            shape[(size_t) i] = (float) (0.75 * std::sin (twoPi * t) + 0.2 * std::sin (3 * twoPi * t) * (s % 2) + 0.1 * std::sin (5 * twoPi * t) * (s % 3 == 0 ? 1 : 0));
        }
        drawnNames[(size_t) s] = "WAVE " + juce::String (s + 1).paddedLeft ('0', 2);
        rebuildWave (s);
    }
    changed();
}

Processor::~Processor() = default;

const juce::String Processor::getName() const { return "TF N5"; }

void Processor::changed()
{
    const juce::SpinLock::ScopedLockType sl (engine->bankLock);
    engine->pending = edit;
    ++engine->generation;
}

void Processor::rebuildWave (int slot)
{
    std::vector<float> cycle (drawnShapes[(size_t) slot].begin(), drawnShapes[(size_t) slot].end());
    const auto harm = harmonicsOfCycle (cycle);
    const juce::SpinLock::ScopedLockType wl (engine->waveLock);
    const int target = 1 - engine->drawnInUse[(size_t) slot];
    engine->drawnTables[(size_t) slot][(size_t) target].build (harm);
    engine->drawnActive[(size_t) slot] = target;
}

void Processor::setDrawnWave (int slot, const std::array<float, drawnPoints>& points)
{
    slot = std::clamp (slot, 0, numDrawn - 1);
    drawnShapes[(size_t) slot] = points;
    rebuildWave (slot);
}

std::array<float, 16> Processor::harmonicsOf (int slot) const
{
    std::vector<float> cycle (drawnShapes[(size_t) std::clamp (slot, 0, numDrawn - 1)].begin(), drawnShapes[(size_t) std::clamp (slot, 0, numDrawn - 1)].end());
    const auto h = harmonicsOfCycle (cycle);
    std::array<float, 16> out {};
    float peak = 1.0e-9f;
    for (int k = 1; k <= 16 && k < (int) h.size(); ++k) { out[(size_t) k - 1] = (float) std::abs (h[(size_t) k]); peak = std::max (peak, out[(size_t) k - 1]); }
    for (auto& v : out) v /= peak;
    return out;
}

void Processor::playNote (int note, int velocity, bool on)
{
    // (the panel's keyboard: through the same MIDI handling, at the next block)
    const juce::SpinLock::ScopedLockType sl (engine->panelLock);
    engine->panelNotes.addEvent (on ? juce::MidiMessage::noteOn (1, note, (juce::uint8) std::clamp (velocity, 1, 127)) : juce::MidiMessage::noteOff (1, note), 0);
}

void Processor::prepareToPlay (double sampleRate, int maximumBlock)
{
    engine->prepare (sampleRate, maximumBlock);
    changed();
}

void Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    engine->process (buffer, midi);
    int count = 0;
    for (auto& v : engine->voices) if (v.active) ++count;
    sounding = count;
    midi.clear();
}

//==============================================================================
namespace
{
    void writeInts (juce::XmlElement& x, const char* tag, const int* values, int count)
    {
        juce::StringArray s;
        for (int i = 0; i < count; ++i) s.add (juce::String (values[i]));
        x.setAttribute (tag, s.joinIntoString (","));
    }
    void readInts (const juce::XmlElement& x, const char* tag, int* values, int count)
    {
        const auto s = juce::StringArray::fromTokens (x.getStringAttribute (tag), ",", "");
        for (int i = 0; i < count && i < s.size(); ++i) values[i] = s[i].getIntValue();
    }
    void writeFx (juce::XmlElement& parent, const FxSettings& fx)
    {
        auto* x = parent.createNewChildElement ("FX");
        x->setAttribute ("placement", fx.placement);
        x->setAttribute ("pan3", fx.pan3);
        x->setAttribute ("pan4", fx.pan4);
        writeInts (*x, "levels", fx.level, 4);
        for (int u = 0; u < 2; ++u)
        {
            auto* e = x->createNewChildElement ("UNIT");
            e->setAttribute ("type", fx.unit[u].type);
            e->setAttribute ("on", fx.unit[u].on);
            e->setAttribute ("modSource", fx.unit[u].modSource);
            e->setAttribute ("modInt", fx.unit[u].modInt);
            writeInts (*e, "p", fx.unit[u].p, maxEffectParams);
        }
    }
    void readFx (const juce::XmlElement* parent, FxSettings& fx)
    {
        if (parent == nullptr) return;
        auto* x = parent->getChildByName ("FX");
        if (x == nullptr) return;
        fx.placement = std::clamp (x->getIntAttribute ("placement", 1), 0, 3);
        fx.pan3 = std::clamp (x->getIntAttribute ("pan3", 0), -1, 100);
        fx.pan4 = std::clamp (x->getIntAttribute ("pan4", 100), -1, 100);
        readInts (*x, "levels", fx.level, 4);
        int u = 0;
        for (auto* e : x->getChildWithTagNameIterator ("UNIT"))
        {
            if (u > 1) break;
            fx.unit[u].type = std::clamp (e->getIntAttribute ("type"), 0, numEffectTypes - 1);
            fx.unit[u].on = e->getIntAttribute ("on", 1);
            fx.unit[u].modSource = std::clamp (e->getIntAttribute ("modSource"), 0, 5);
            fx.unit[u].modInt = std::clamp (e->getIntAttribute ("modInt"), -15, 15);
            readInts (*e, "p", fx.unit[u].p, maxEffectParams);
            ++u;
        }
    }
}

std::unique_ptr<juce::XmlElement> Processor::toXml (bool) const
{
    auto x = std::make_unique<juce::XmlElement> ("N5");
    x->setAttribute ("version", 1);
    x->setAttribute ("combiMode", edit.combiMode);
    x->setAttribute ("program", edit.program);
    x->setAttribute ("combi", edit.combi);
    writeInts (*x, "arp", edit.arp, numArp);
    for (int i = 0; i < numPrograms; ++i)
    {
        auto* p = x->createNewChildElement ("PROGRAM");
        p->setAttribute ("name", programNames[(size_t) i]);
        writeInts (*p, "layer1", edit.programs[(size_t) i].layer[0], numLayer);
        writeInts (*p, "layer2", edit.programs[(size_t) i].layer[1], numLayer);
        writeInts (*p, "global", edit.programs[(size_t) i].global, numGlobal);
        writeFx (*p, edit.programs[(size_t) i].fx);
    }
    for (int i = 0; i < numCombis; ++i)
    {
        auto* c = x->createNewChildElement ("COMBI");
        c->setAttribute ("name", combiNames[(size_t) i]);
        for (int t = 0; t < numTimbres; ++t) writeInts (*c, ("t" + juce::String (t)).toRawUTF8(), edit.combis[(size_t) i].timbre[t], numTimbre);
        writeFx (*c, edit.combis[(size_t) i].fx);
    }
    for (int s = 0; s < numDrawn; ++s)
    {
        auto* w = x->createNewChildElement ("WAVE");
        w->setAttribute ("name", drawnNames[(size_t) s]);
        juce::StringArray v;
        for (auto f : drawnShapes[(size_t) s]) v.add (juce::String (f, 4));
        w->setAttribute ("points", v.joinIntoString (","));
    }
    return x;
}

void Processor::fromXml (const juce::XmlElement& x)
{
    if (! x.hasTagName ("N5")) return;
    BankData b;
    b.init();
    b.combiMode = std::clamp (x.getIntAttribute ("combiMode"), 0, 1);
    b.program = std::clamp (x.getIntAttribute ("program"), 0, numPrograms - 1);
    b.combi = std::clamp (x.getIntAttribute ("combi"), 0, numCombis - 1);
    readInts (x, "arp", b.arp, numArp);
    int i = 0;
    for (auto* p : x.getChildWithTagNameIterator ("PROGRAM"))
    {
        if (i >= numPrograms) break;
        programNames[(size_t) i] = p->getStringAttribute ("name", programNames[(size_t) i]);
        readInts (*p, "layer1", b.programs[(size_t) i].layer[0], numLayer);
        readInts (*p, "layer2", b.programs[(size_t) i].layer[1], numLayer);
        readInts (*p, "global", b.programs[(size_t) i].global, numGlobal);
        readFx (p, b.programs[(size_t) i].fx);
        ++i;
    }
    i = 0;
    for (auto* c : x.getChildWithTagNameIterator ("COMBI"))
    {
        if (i >= numCombis) break;
        combiNames[(size_t) i] = c->getStringAttribute ("name", combiNames[(size_t) i]);
        for (int t = 0; t < numTimbres; ++t) readInts (*c, ("t" + juce::String (t)).toRawUTF8(), b.combis[(size_t) i].timbre[t], numTimbre);
        readFx (c, b.combis[(size_t) i].fx);
        ++i;
    }
    // every value back within its field's range
    for (auto& p : b.programs)
    {
        for (auto& l : p.layer) for (int k = 0; k < numLayer; ++k) l[k] = std::clamp (l[k], layerField (k).lo, layerField (k).hi);
        for (int k = 0; k < numGlobal; ++k) p.global[k] = std::clamp (p.global[k], globalField (k).lo, globalField (k).hi);
    }
    for (auto& c : b.combis) for (auto& t : c.timbre) for (int k = 0; k < numTimbre; ++k) t[k] = std::clamp (t[k], timbreField (k).lo, timbreField (k).hi);
    for (int k = 0; k < numArp; ++k) b.arp[k] = std::clamp (b.arp[k], arpField (k).lo, arpField (k).hi);
    edit = b;
    int s = 0;
    for (auto* w : x.getChildWithTagNameIterator ("WAVE"))
    {
        if (s >= numDrawn) break;
        drawnNames[(size_t) s] = w->getStringAttribute ("name", drawnNames[(size_t) s]);
        const auto v = juce::StringArray::fromTokens (w->getStringAttribute ("points"), ",", "");
        for (int k = 0; k < drawnPoints && k < v.size(); ++k) drawnShapes[(size_t) s][(size_t) k] = juce::jlimit (-1.0f, 1.0f, v[k].getFloatValue());
        rebuildWave (s);
        ++s;
    }
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
    auto x = getXmlFromBinary (data, size);
    if (x == nullptr || ! x->hasTagName ("N5")) return;
    fromXml (*x);
    readPresetAttributes (*x);
}

const presets::Kind& presetKind()
{
    static const presets::Kind kind { "N5", "tfn5", "TFN5", "MEMORY", [] (const juce::XmlElement& s)
    {
        const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
        int drawnUsed = 0;
        juce::ignoreUnused (drawnUsed);
        return juce::String (numPrograms) + " PROGRAMS" + dot + juce::String (numCombis) + " COMBIS" + dot + (s.getIntAttribute ("combiMode") != 0 ? "COMBI MODE" : "PROGRAM MODE");
    } };
    return kind;
}

const presets::Kind& Processor::presetKind() const { return n5::presetKind(); }
} // namespace tf::n5
