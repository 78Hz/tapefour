#include "N5Effects.h"

namespace tf::n5
{
namespace
{
    constexpr double pi = 3.14159265358979323846, twoPi = 2.0 * pi;

    struct Line
    {
        std::vector<float> buf;
        int w = 0;
        void size (int n) { buf.assign ((size_t) std::max (4, n), 0.0f); w = 0; }
        void clear() { std::fill (buf.begin(), buf.end(), 0.0f); }
        void push (float x) noexcept { buf[(size_t) w] = x; w = (w + 1) % (int) buf.size(); }
        /** d samples ago (fractional; 1 = the last pushed). */
        float at (double d) const noexcept
        {
            const int n = (int) buf.size();
            d = std::clamp (d, 1.0, (double) n - 2.0);
            const int i = (int) d;
            const float f = (float) (d - i);
            int a = w - i; if (a < 0) a += n;
            int b = a - 1; if (b < 0) b += n;
            return buf[(size_t) a] + (buf[(size_t) b] - buf[(size_t) a]) * f;
        }
    };

    struct OnePole
    {
        float a = 0.0f, y = 0.0f;
        void set (double hz, double sr) { a = (float) std::exp (-twoPi * std::min (hz, sr * 0.45) / sr); }
        float lp (float x) noexcept { y = x + a * (y - x); return y; }
    };

    struct Svf   // a state-variable filter (TPT)
    {
        float g = 0.1f, k = 1.4f, s1 = 0, s2 = 0, lpOut = 0, bpOut = 0, hpOut = 0;
        void set (double hz, double q, double sr)
        {
            g = (float) std::tan (pi * std::clamp (hz, 10.0, sr * 0.45) / sr);
            k = (float) (1.0 / std::max (0.3, q));
        }
        void run (float x) noexcept
        {
            const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
            const float v3 = x - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3;
            s1 = 2 * v1 - s1; s2 = 2 * v2 - s2;
            lpOut = v2; bpOut = v1; hpOut = x - k * v1 - v2;
        }
        void reset() { s1 = s2 = 0; }
    };

    /** The units' EQ Low and EQ High (shelves; their corners aren't published: 200 Hz and 5 kHz, set by ear). */
    struct Eq
    {
        Biquad lo[2], hi[2];
        int lastLo = 99, lastHi = 99;
        void set (int lowDb, int highDb, double sr)
        {
            if (lowDb != lastLo) { for (auto& b : lo) b.setLowShelf (sr, 200.0, lowDb); lastLo = lowDb; }
            if (highDb != lastHi) { for (auto& b : hi) b.setHighShelf (sr, 5000.0, highDb); lastHi = highDb; }
        }
        void run (float& l, float& r) noexcept
        {
            if (lastLo != 0) { l = lo[0].process (l); r = lo[1].process (r); }
            if (lastHi != 0) { l = hi[0].process (l); r = hi[1].process (r); }
        }
        void reset() { for (auto& b : lo) b.reset(); for (auto& b : hi) b.reset(); }
    };

    float lfoShape (double phase, int wave) noexcept   // 0 SIN, 1 TRI: -1..1
    {
        phase -= std::floor (phase);
        return wave == 0 ? (float) std::sin (twoPi * phase) : (float) (phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase);
    }

    /** The quadrature choruses' shape: T+10..T-10 (triangles, skewed), S-10..S+10 (sines, skewed). */
    float skewed (double phase, int shape) noexcept
    {
        phase -= std::floor (phase);
        const bool tri = shape <= 20;
        const double sym = tri ? (10 - shape) / 10.0 : (shape - 31) / 10.0;   // -1..1
        const double knee = std::clamp (0.5 + 0.45 * sym, 0.05, 0.95);
        const double p = phase < knee ? 0.5 * phase / knee : 0.5 + 0.5 * (phase - knee) / (1.0 - knee);
        return tri ? (float) (p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p) : (float) std::sin (twoPi * p - pi * 0.5);
    }
}

//==============================================================================
struct EffectUnit::State
{
    double sr = 48000.0;
    Line line[4];
    OnePole damp[4];
    Eq eq;
    double ph[4] {};
    float fb[4] {};
    // reverb: an 8-line feedback delay network, early reflections and pre-delay
    Line rev[8], pre[2], er[2];
    OnePole revDamp[8];
    // spring reverb (type 9): TF Spring Reverb's simulated springs
    std::vector<spring::ModalPath> springs;
    float springGain = 1.0f;
    int springTime = -1;
    // filters
    Svf svf[6];
    Biquad bq[8];
    float env = 0.0f, envTarget = 0.0f;
    int egStage = 0;          // resonance filter: 0 idle, 1 attack, 2 decay
    float rotSpeed = 0.0f;    // rotary: 0 slow .. 1 fast
    float apState[2][8] {};
    std::array<double, 6> symPhase {};
    juce::Random rnd;

    void prepare (double rate)
    {
        sr = rate;
        for (auto& l : line) l.size ((int) (rate * 1.05));
        static const double lens[8] { 0.0297, 0.0371, 0.0411, 0.0437, 0.0533, 0.0601, 0.0677, 0.0743 };
        for (int i = 0; i < 8; ++i) rev[i].size ((int) (lens[i] * rate * 2.2) + 8);
        for (auto& p : pre) p.size ((int) (0.25 * rate));
        for (auto& e : er) e.size ((int) (0.85 * rate));
        reset();
    }

    void reset()
    {
        for (auto& l : line) l.clear();
        for (auto& l : rev) l.clear();
        for (auto& p : pre) p.clear();
        for (auto& e : er) e.clear();
        for (auto& d : damp) d.y = 0.0f;
        for (auto& d : revDamp) d.y = 0.0f;
        for (auto& s : svf) s.reset();
        for (auto& b : bq) b.reset();
        for (auto& s : springs) s.reset();
        eq.reset();
        std::fill (std::begin (fb), std::end (fb), 0.0f);
        env = 0.0f; egStage = 0;
        for (auto& a : apState) std::fill (std::begin (a), std::end (a), 0.0f);
    }
};

EffectUnit::EffectUnit() : st (std::make_unique<State>()) {}
EffectUnit::~EffectUnit() = default;
void EffectUnit::prepare (double sampleRate) { st->prepare (sampleRate); lastType = -1; }
void EffectUnit::reset() { st->reset(); }

void EffectUnit::process (float* l, float* r, int n, const FxSettings::Unit& u, const FxContext& ctx) noexcept
{
    auto& s = *st;
    const int type = u.type;
    if (type <= 0) return;
    if (type != lastType) { s.reset(); lastType = type; }
    const auto& et = effectType (type);
    // the parameters, with dynamic modulation applied to the type's marked one
    int p[maxEffectParams];
    const float mod = ctx.mod[std::clamp (u.modSource, 0, 5)] * (u.modSource == 0 ? 0.0f : 1.0f);
    for (int i = 0; i < maxEffectParams; ++i)
    {
        p[i] = u.p[i];
        if (i < et.count && et.params[i].dynamic && u.modSource != 0)
        {
            const auto& d = et.params[i];
            p[i] = juce::jlimit (d.lo, d.hi, p[i] + juce::roundToInt ((float) u.modInt / 15.0f * mod * (float) (d.hi - d.lo)));
        }
    }
    const double sr = s.sr;
    auto bal = [] (int v) { return (float) std::clamp (v, 0, 100) / 100.0f; };
    auto ms = [sr] (double v) { return std::max (1.0, v * 0.001 * sr); };
    const bool on = u.on != 0;
    // types whose EQ stays when switched off (the manual, p.46)
    const bool eqAlways = type == 13 || type == 14 || type == 19 || type == 20 || type == 28 || type == 35 || type == 36;
    if (! on)
    {
        if (eqAlways)
        {
            const int eqAt = type == 13 || type == 14 ? 4 : type == 28 ? 2 : 4;
            s.eq.set (p[eqAt], p[eqAt + 1], sr);
            for (int i = 0; i < n; ++i) s.eq.run (l[i], r[i]);
        }
        return;
    }

    auto reverb = [&] (float inL, float inR, double t60, int preMs, float erLevel, int highDamp, double size, float& outL, float& outR)
    {
        // pre-delay, early reflections, then the network
        s.pre[0].push (inL); s.pre[1].push (inR);
        const float pl = s.pre[0].at (ms (preMs)), pr = s.pre[1].at (ms (preMs));
        s.er[0].push (pl); s.er[1].push (pr);
        static const double taps[6] { 0.0071, 0.0113, 0.0179, 0.0233, 0.0311, 0.0397 };
        float el = 0, erR = 0;
        for (int k = 0; k < 6; ++k)
        {
            el += s.er[k & 1].at (ms (taps[k] * 1000.0 * size)) * (1.0f - 0.12f * (float) k);
            erR += s.er[(k + 1) & 1].at (ms (taps[k] * 1100.0 * size)) * (1.0f - 0.12f * (float) k);
        }
        static const double lens[8] { 0.0297, 0.0371, 0.0411, 0.0437, 0.0533, 0.0601, 0.0677, 0.0743 };
        float y[8];
        for (int k = 0; k < 8; ++k) y[k] = s.rev[k].at (lens[k] * size * sr);
        // Householder mix
        float sum = 0; for (float v : y) sum += v;
        sum *= 0.25f;
        const double dampHz = 18000.0 * std::pow (0.04, highDamp / 99.0);
        for (int k = 0; k < 8; ++k)
        {
            const double g = std::pow (10.0, -3.0 * lens[k] * size / std::max (0.1, t60));
            s.revDamp[k].set (dampHz, sr);
            const float in = (k < 4 ? pl : pr) * 0.35f;
            s.rev[k].push (s.revDamp[k].lp ((y[k] - sum) * (float) g) + in);
        }
        outL = (y[0] - y[2] + y[4] - y[6]) * 0.5f + erLevel * el * 0.3f;
        outR = (y[1] - y[3] + y[5] - y[7]) * 0.5f + erLevel * erR * 0.3f;
    };

    auto delayDamped = [&] (int ch, float in, double dMs, int feedback, int highDamp)
    {
        const float y = s.line[ch].at (ms (dMs));
        s.damp[ch].set (20000.0 * std::pow (0.05, highDamp / 99.0), sr);
        s.line[ch].push (in + s.damp[ch].lp (y) * (float) feedback / 100.0f);
        return y;
    };

    auto distort = [&] (int ch, float x, int drive, int hotSpot, int reso, int level, bool hard)
    {
        const float g = juce::Decibels::decibelsToGain ((float) drive * 0.4f);
        float y = x * g;
        y = hard ? std::tanh (y * 1.5f) * 0.9f + 0.1f * std::clamp (y, -1.0f, 1.0f) : y / (1.0f + std::abs (y));
        s.svf[ch].set (200.0 * std::pow (25.0, hotSpot / 99.0), 0.7 + reso / 10.0, sr);
        s.svf[ch].run (y);
        y = 0.5f * y + s.svf[ch].bpOut * (0.5f + (float) reso / 40.0f);
        return y * (float) (level / 99.0) * 2.0f;
    };

    auto chorusRead = [&] (int ch, double baseMs, double depthMs, float lfo)
    {
        return s.line[ch].at (ms (baseMs + depthMs * (0.5 + 0.5 * lfo)) + 1.0);
    };

    auto rotarySpeedHz = [] (int v) { return 0.8 * std::exp (0.0483 * (v - 25)); };   // slow 25 = 0.8 Hz, fast 69 = 6.7 Hz (set by ear)

    auto rotary = [&] (float x, int vibDepth, int accel, int slow, int fast, float& outL, float& outR)
    {
        const float target = mod > 0.5f ? 1.0f : 0.0f;
        const float rate = 1.0f / (float) (0.2 * accel * sr);
        s.rotSpeed += std::clamp (target - s.rotSpeed, -rate, rate);
        const double hornHz = rotarySpeedHz (slow) + (rotarySpeedHz (fast) - rotarySpeedHz (slow)) * s.rotSpeed;
        const double drumHz = hornHz * 0.85;
        s.ph[0] += hornHz / sr; s.ph[1] += drumHz / sr;
        s.svf[0].set (800.0, 0.7, sr);
        s.svf[0].run (x);
        const float lowBand = s.svf[0].lpOut, highBand = x - lowBand;
        s.line[0].push (highBand);
        const float hs = (float) std::sin (twoPi * s.ph[0]), hc = (float) std::cos (twoPi * s.ph[0]);
        const float horn = s.line[0].at (ms (1.0 + vibDepth * 0.1 * (1.0 + hs)));
        const float ds = (float) std::sin (twoPi * s.ph[1]);
        outL = horn * (0.75f + 0.25f * hc) + lowBand * (0.8f + 0.2f * ds);
        outR = horn * (0.75f - 0.25f * hc) + lowBand * (0.8f - 0.2f * ds);
    };

    auto phaser = [&] (int ch, float x, int manual, double speed, int depth, int feedback, int wave, double phase0)
    {
        const float lfo = lfoShape (s.ph[0] + phase0, wave);
        const double centre = 100.0 * std::pow (40.0, manual / 99.0);
        const double hz = centre * std::pow (4.0, lfo * (float) depth / 99.0);
        const float a = (float) ((std::tan (pi * std::min (hz, sr * 0.45) / sr) - 1.0) / (std::tan (pi * std::min (hz, sr * 0.45) / sr) + 1.0));
        float v = x + s.fb[ch] * (float) feedback / 105.0f;
        for (int k = 0; k < 6; ++k)
        {
            const float y = a * v + s.apState[ch][k];
            s.apState[ch][k] = v - a * y;
            v = y;
        }
        s.fb[ch] = v;
        juce::ignoreUnused (speed);
        return v;
    };

    switch (type)
    {
        case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
        {
            static const double sizes[9] { 1, 1.0, 1.15, 1.45, 0.45, 0.7, 0.85, 0.55, 0.4 };
            const bool plate = type >= 7;
            const double t60 = plate ? 0.3 + 5.7 * std::pow (p[0] / 99.0, 1.5) : p[0] / 10.0;
            const float erLevel = plate ? (float) p[2] / 10.0f : (float) p[2] / 99.0f;
            s.eq.set (p[4], p[5], sr);
            const float b = bal (p[6]);
            for (int i = 0; i < n; ++i)
            {
                float wl, wr;
                reverb (l[i], r[i], t60, p[1], erLevel, p[3], sizes[type], wl, wr);
                s.eq.run (wl, wr);
                l[i] = l[i] * (1.0f - b) + wl * b;
                r[i] = r[i] * (1.0f - b) + wr * b;
            }
            break;
        }
        case 9:
        {
            // the N5's spring type, made with TF Spring Reverb's simulated springs: time 0..99 their decay (set by ear)
            if (s.springs.empty() || s.springTime != p[0])
            {
                s.springTime = p[0];
                s.springs.assign (2, {});
                double e = 0.0;
                for (int k = 0; k < 2; ++k)
                {
                    auto ph = spring::tankPhysics (spring::Tank::fender, k);
                    ph.sigma = 6.9 / (0.5 + 4.0 * p[0] / 99.0);
                    s.springs[(size_t) k].build (ph, sr, 8000.0);
                    e += s.springs[(size_t) k].clickEnergy();
                }
                s.springGain = (float) (1.0 / std::sqrt (std::max (1.0e-30, e)));
            }
            s.eq.set (p[4], p[5], sr);
            const float b = bal (p[6]);
            for (int i = 0; i < n; ++i)
            {
                const float in = 0.5f * (l[i] + r[i]);
                s.pre[0].push (in);
                const float pd = s.pre[0].at (ms (p[1]));
                float wl = s.springs[0].process (pd) * s.springGain * 0.25f, wr = s.springs[1].process (pd) * s.springGain * 0.25f;
                s.eq.run (wl, wr);
                l[i] = l[i] * (1.0f - b) + wl * b;
                r[i] = r[i] * (1.0f - b) + wr * b;
            }
            break;
        }
        case 10: case 11: case 12:
        {
            // early reflections over ER Time: 1 decaying, 2 a different envelope, 3 reversed (set by ear)
            s.eq.set (p[2], p[3], sr);
            const float b = bal (p[4]);
            for (int i = 0; i < n; ++i)
            {
                s.pre[0].push (l[i]); s.pre[1].push (r[i]);
                const float pl = s.pre[0].at (ms (p[1])), pr = s.pre[1].at (ms (p[1]));
                s.er[0].push (pl); s.er[1].push (pr);
                float wl = 0, wr = 0;
                constexpr int taps = 12;
                for (int k = 0; k < taps; ++k)
                {
                    const double frac = (k + 0.5) / taps;
                    double g = type == 10 ? 1.0 - frac : type == 11 ? std::sin (pi * frac) * (1.0 - 0.5 * frac) : frac;
                    g *= 0.35;
                    wl += (float) g * s.er[k & 1].at (ms (p[0] * frac));
                    wr += (float) g * s.er[(k + 1) & 1].at (ms (p[0] * (frac * 0.93 + 0.03)));
                }
                s.eq.run (wl, wr);
                l[i] = l[i] * (1.0f - b) + wl * b;
                r[i] = r[i] * (1.0f - b) + wr * b;
            }
            break;
        }
        case 13: case 14:
        {
            s.eq.set (p[4], p[5], sr);
            const float b = bal (p[6]);
            for (int i = 0; i < n; ++i)
            {
                float il = l[i], ir = r[i];
                s.eq.run (il, ir);
                const float yl = s.line[0].at (ms (p[0])), yr = s.line[1].at (ms (p[1]));
                s.damp[0].set (20000.0 * std::pow (0.05, p[3] / 99.0), sr);
                s.damp[1].set (20000.0 * std::pow (0.05, p[3] / 99.0), sr);
                const float fl = s.damp[0].lp (type == 14 ? yr : yl) * (float) p[2] / 100.0f, fr = s.damp[1].lp (type == 14 ? yl : yr) * (float) p[2] / 100.0f;
                s.line[0].push (il + fl);
                s.line[1].push (ir + fr);
                l[i] = il * (1.0f - b) + yl * b;
                r[i] = ir * (1.0f - b) + yr * b;
            }
            break;
        }
        case 15:
            for (int i = 0; i < n; ++i)
            {
                const float yl = delayDamped (0, l[i], p[0], p[1], p[2]), yr = delayDamped (1, r[i], p[4], p[5], p[6]);
                l[i] = l[i] * (1.0f - bal (p[3])) + yl * bal (p[3]);
                r[i] = r[i] * (1.0f - bal (p[7])) + yr * bal (p[7]);
            }
            break;
        case 16: case 17: case 18:
        {
            // two delays from each input; one fed back (1), cross-panned (2), feedback alternating (3)
            s.eq.set (p[3], p[4], sr);
            const float b = bal (p[5]);
            for (int i = 0; i < n; ++i)
            {
                float il = l[i], ir = r[i];
                s.eq.run (il, ir);
                const float in = 0.5f * (il + ir);
                const float d1 = s.line[0].at (ms (p[0])), d2 = s.line[1].at (ms (p[1]));
                const float f = (float) p[2] / 100.0f;
                s.line[0].push (in + (type == 18 ? d2 * f : 0.0f));
                s.line[1].push (in + (type == 18 ? d1 * f * 0.5f : d2 * f));
                float wl = d1, wr = d2;
                if (type == 17) { s.ph[0] += 0.25 / sr; const float pan = (float) (0.5 + 0.5 * std::sin (twoPi * s.ph[0])); wl = d1 * pan + d2 * (1 - pan); wr = d1 * (1 - pan) + d2 * pan; }
                l[i] = il * (1.0f - b) + wl * b;
                r[i] = ir * (1.0f - b) + wr * b;
            }
            break;
        }
        case 19: case 20:
        {
            s.eq.set (p[4], p[5], sr);
            const float b = bal (p[6]);
            const double hz = speedHz (p[1]), depthMs = 5.0 * p[2] / 99.0;
            for (int i = 0; i < n; ++i)
            {
                float il = l[i], ir = r[i];
                s.eq.run (il, ir);
                s.ph[0] += hz / sr;
                const float lfoL = lfoShape (s.ph[0], p[3]), lfoR = type == 19 ? -lfoL : lfoL;   // 1: opposite phase, 2: in phase
                s.line[0].push (il); s.line[1].push (ir);
                const float wl = chorusRead (0, p[0], depthMs, lfoL), wr = chorusRead (1, p[0], depthMs, lfoR);
                l[i] = il * (1.0f - b) + wl * b;
                r[i] = ir * (1.0f - b) + wr * b;
            }
            break;
        }
        case 21: case 22:
        {
            s.eq.set (p[5], p[6], sr);
            const float b = bal (p[7]);
            const double hz = speedHz (p[2]), depthMs = 6.0 * p[3] / 99.0;
            for (int i = 0; i < n; ++i)
            {
                float il = l[i], ir = r[i];
                s.eq.run (il, ir);
                s.ph[0] += hz / sr;
                s.line[0].push (il); s.line[1].push (ir);
                const float cl = chorusRead (0, p[0], depthMs, skewed (s.ph[0], p[4])), cr = chorusRead (1, p[1], depthMs, skewed (s.ph[0] + 0.25, p[4]));
                const float wl = type == 22 ? cr : cl, wr = type == 22 ? cl : cr;   // crossover: each channel's chorus into the other
                l[i] = il * (1.0f - b) + wl * b;
                r[i] = ir * (1.0f - b) + wr * b;
            }
            break;
        }
        case 23:
        {
            // quadrature chorus above the split, the lows straight through
            const double split = 100.0 * std::pow (2.0, p[4] / 3.0);
            s.svf[0].set (split, 0.707, sr); s.svf[1].set (split, 0.707, sr);
            const double hz = speedHz (p[2]), depthMs = 6.0 * p[3] / 99.0;
            const float b = bal (p[5]);
            for (int i = 0; i < n; ++i)
            {
                s.svf[0].run (l[i]); s.svf[1].run (r[i]);
                const float lowL = s.svf[0].lpOut, lowR = s.svf[1].lpOut;
                const float highL = l[i] - lowL, highR = r[i] - lowR;
                s.ph[0] += hz / sr;
                s.line[0].push (highL); s.line[1].push (highR);
                const float cl = chorusRead (0, p[0], depthMs, lfoShape (s.ph[0], 1)), cr = chorusRead (1, p[1], depthMs, lfoShape (s.ph[0] + 0.25, 1));
                l[i] = lowL + highL * (1.0f - b) + cl * b;
                r[i] = lowR + highR * (1.0f - b) + cr * b;
            }
            break;
        }
        case 24:
        {
            // a multi-stage chorus: three slow and fast modulations a side (rates set by ear)
            s.eq.set (p[1], p[2], sr);
            const float b = bal (p[3]);
            static const double rates[6] { 0.53, 0.71, 0.97, 5.9, 6.7, 7.3 };
            const double depthMs = 4.0 * p[0] / 99.0;
            for (int i = 0; i < n; ++i)
            {
                float il = l[i], ir = r[i];
                s.eq.run (il, ir);
                s.line[0].push (il); s.line[1].push (ir);
                float wl = 0, wr = 0;
                for (int k = 0; k < 3; ++k)
                {
                    s.symPhase[(size_t) k] += rates[k] / sr; s.symPhase[(size_t) k + 3] += rates[k + 3] / sr;
                    const float slow = (float) std::sin (twoPi * s.symPhase[(size_t) k]), fast = 0.15f * (float) std::sin (twoPi * s.symPhase[(size_t) k + 3]);
                    wl += chorusRead (0, 8.0 + 3.0 * k, depthMs, slow + fast) / 3.0f;
                    wr += chorusRead (1, 9.0 + 3.0 * k, depthMs, -slow - fast) / 3.0f;
                }
                l[i] = il * (1.0f - b) + wl * b;
                r[i] = ir * (1.0f - b) + wr * b;
            }
            break;
        }
        case 25: case 26: case 27:
        {
            s.eq.set (p[4], p[5], sr);
            const float b = bal (p[6]);
            const double hz = speedHz (p[2]);
            const double baseMs = std::max (0.1, (double) p[0]), depthMs = baseMs * p[1] / 99.0 + 0.5 * p[1] / 99.0;
            for (int i = 0; i < n; ++i)
            {
                float il = l[i], ir = r[i];
                s.eq.run (il, ir);
                s.ph[0] += hz / sr;
                const float lfoL = lfoShape (s.ph[0], 1), lfoR = type == 25 ? lfoL : -lfoL;
                const float yl = s.line[0].at (ms (0.1 + depthMs * (0.5 + 0.5 * lfoL)) + 1.0);
                const float yr = s.line[1].at (ms (0.1 + depthMs * (0.5 + 0.5 * lfoR)) + 1.0);
                const float res = (float) p[3] / 105.0f;
                s.line[0].push (il + (type == 27 ? yr : yl) * res);
                s.line[1].push (ir + (type == 27 ? yl : yr) * res);
                l[i] = il * (1.0f - b) + yl * b;
                r[i] = ir * (1.0f - b) + yr * b;
            }
            break;
        }
        case 28:
        {
            // the highs above the emphatic point, made harmonically richer, blended in (or out, negative)
            const double hz = 1000.0 * std::pow (1.25, p[1] - 1);
            s.svf[0].set (hz, 0.7, sr); s.svf[1].set (hz, 0.7, sr);
            s.eq.set (p[2], p[3], sr);
            const float b = bal (p[4]), blend = (float) p[0] / 99.0f;
            for (int i = 0; i < n; ++i)
            {
                float il = l[i], ir = r[i];
                s.eq.run (il, ir);
                s.svf[0].run (il); s.svf[1].run (ir);
                const float hl = s.svf[0].hpOut, hr = s.svf[1].hpOut;
                const float wl = il + blend * (hl + 0.6f * std::abs (hl)), wr = ir + blend * (hr + 0.6f * std::abs (hr));
                l[i] = il * (1.0f - b) + wl * b;
                r[i] = ir * (1.0f - b) + wr * b;
            }
            break;
        }
        case 29:
        {
            const double hz = 500.0 * std::pow (1.2, p[1] - 1);
            s.svf[0].set (hz, 0.7, sr); s.svf[1].set (hz, 0.7, sr);
            s.eq.set (p[4], p[5], sr);
            const float b = bal (p[6]), density = (float) p[0] / 99.0f, width = (float) p[2] / 99.0f;
            for (int i = 0; i < n; ++i)
            {
                float il = l[i], ir = r[i];
                s.eq.run (il, ir);
                s.svf[0].run (il); s.svf[1].run (ir);
                const float el = il + density * std::tanh (3.0f * s.svf[0].hpOut), er = ir + density * std::tanh (3.0f * s.svf[1].hpOut);
                s.line[0].push (er);
                const float delayed = s.line[0].at (ms (p[3]));
                const float wl = el + width * 0.5f * delayed, wr = er - width * 0.5f * delayed;
                l[i] = il * (1.0f - b) + wl * b;
                r[i] = ir * (1.0f - b) + wr * b;
            }
            break;
        }
        case 30: case 31:
        {
            s.eq.set (p[3], p[4], sr);
            const float b = bal (p[6]);
            for (int i = 0; i < n; ++i)
            {
                const float in = 0.5f * (l[i] + r[i]);
                float wl = distort (0, in, p[0], p[1], p[2], p[5], type == 30), wr = wl;
                s.eq.run (wl, wr);
                l[i] = l[i] * (1.0f - b) + wl * b;
                r[i] = r[i] * (1.0f - b) + wr * b;
            }
            break;
        }
        case 32: case 33:
        {
            const double hz = speedHz (p[1]);
            const float b = bal (p[5]);
            for (int i = 0; i < n; ++i)
            {
                s.ph[0] += hz / sr;
                const float wl = phaser (0, l[i], p[0], hz, p[2], p[3], p[4], 0.0), wr = phaser (1, r[i], p[0], hz, p[2], p[3], p[4], type == 32 ? 0.5 : 0.0);
                l[i] = l[i] * (1.0f - b) + wl * b;
                r[i] = r[i] * (1.0f - b) + wr * b;
            }
            break;
        }
        case 34:
        {
            const float b = bal (p[4]);
            for (int i = 0; i < n; ++i)
            {
                float wl, wr;
                rotary (0.5f * (l[i] + r[i]), p[0], p[1], p[2], p[3], wl, wr);
                l[i] = l[i] * (1.0f - b) + wl * b;
                r[i] = r[i] * (1.0f - b) + wr * b;
            }
            break;
        }
        case 35: case 36:
        {
            s.eq.set (p[4], p[5], sr);
            const float b = bal (p[6]);
            const double hz = speedHz (p[2]);
            const float depth = (float) p[3] / 99.0f, shape = (float) p[1] / 99.0f;
            auto gainOf = [&] (double phase)
            {
                float v = 0.5f + 0.5f * lfoShape (phase, p[0]);              // 0..1
                v = shape >= 0 ? std::pow (v, 1.0f / (1.0f + 3.0f * shape)) : std::pow (v, 1.0f - 3.0f * shape);   // Mod Shape bends it
                return 1.0f - depth * (1.0f - v);
            };
            for (int i = 0; i < n; ++i)
            {
                float il = l[i], ir = r[i];
                s.eq.run (il, ir);
                s.ph[0] += hz / sr;
                const float wl = il * gainOf (s.ph[0]), wr = ir * gainOf (s.ph[0] + (type == 35 ? 0.5 : 0.0));
                l[i] = il * (1.0f - b) + wl * b;
                r[i] = ir * (1.0f - b) + wr * b;
            }
            break;
        }
        case 37:
        {
            // the band frequencies' tables aren't published: low 20 Hz..1 kHz, mid 100 Hz..10 kHz, high 1..18 kHz (set by ear)
            const double lowHz = 20.0 * std::pow (50.0, p[0] / 29.0), midHz = 100.0 * std::pow (100.0, p[2] / 99.0), highHz = 1000.0 * std::pow (18.0, p[5] / 29.0);
            for (int c = 0; c < 2; ++c)
            {
                s.bq[c].setLowShelf (sr, lowHz, p[1]);
                s.bq[c + 4].setHighShelf (sr, highHz, p[6]);
                const double A = std::pow (10.0, p[3] / 40.0), w = twoPi * std::min (midHz, sr * 0.45) / sr, q = 4.0 * std::pow (0.08, p[4] / 99.0);
                const double al = std::sin (w) / (2.0 * q);
                s.bq[c + 2].set (1 + al * A, -2 * std::cos (w), 1 - al * A, 1 + al / A, -2 * std::cos (w), 1 - al / A);
            }
            const float b = bal (p[7]);
            for (int i = 0; i < n; ++i)
            {
                const float wl = s.bq[4].process (s.bq[2].process (s.bq[0].process (l[i])));
                const float wr = s.bq[5].process (s.bq[3].process (s.bq[1].process (r[i])));
                l[i] = l[i] * (1.0f - b) + wl * b;
                r[i] = r[i] * (1.0f - b) + wr * b;
            }
            break;
        }
        case 38: case 39:
        {
            // mono in: chorus or flanger with LFOs 90 degrees apart, then a stereo delay
            const float b = bal (p[6]);
            const double hz = speedHz (p[2]), baseMs = std::max (0.2, (double) p[0]), depthMs = (type == 38 ? 5.0 : baseMs + 1.0) * p[3] / 99.0;
            for (int i = 0; i < n; ++i)
            {
                const float in = 0.5f * (l[i] + r[i]);
                s.ph[0] += hz / sr;
                const float c1 = s.line[0].at (ms (baseMs + depthMs * (0.5 + 0.5 * lfoShape (s.ph[0], 1)))), c2 = s.line[0].at (ms (baseMs + depthMs * (0.5 + 0.5 * lfoShape (s.ph[0] + 0.25, 1))));
                s.line[0].push (in + 0.5f * (c1 + c2) * (float) p[1] / 105.0f);
                const float dl = s.line[1].at (ms (p[4])), dr = s.line[2].at (ms (p[4] * 0.75 + 1.0));
                s.line[1].push (c1 + dl * (float) p[5] / 100.0f);
                s.line[2].push (c2 + dr * (float) p[5] / 100.0f);
                l[i] = l[i] * (1.0f - b) + (c1 + dl) * 0.7f * b;
                r[i] = r[i] * (1.0f - b) + (c2 + dr) * 0.7f * b;
            }
            break;
        }
        case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
        {
            // two effects side by side: a delay on the left channel, the other on the right (the manual, p.55)
            const bool damped = type != 44 && type != 45 && type != 47;
            const int dBal = damped ? 3 : 2;
            for (int i = 0; i < n; ++i)
            {
                const float y = delayDamped (0, l[i], p[0], p[1], damped ? p[2] : 0);
                l[i] = l[i] * (1.0f - bal (p[dBal])) + y * bal (p[dBal]);
                const float in = r[i];
                float w = in, b2 = 1.0f;
                switch (type)
                {
                    case 40: case 41:
                    {
                        float wl, wr;
                        reverb (in, in, p[4] / 10.0, p[5], 0.5f, p[6], type == 40 ? 1.0 : 0.5, wl, wr);
                        w = 0.5f * (wl + wr); b2 = bal (p[7]);
                        break;
                    }
                    case 42:
                    {
                        s.ph[1] += speedHz (p[4]) / sr;
                        s.line[1].push (in);
                        w = chorusRead (1, 4.0, 5.0 * p[5] / 99.0, lfoShape (s.ph[1], p[6])); b2 = bal (p[7]);
                        break;
                    }
                    case 43:
                    {
                        s.ph[1] += speedHz (p[4]) / sr;
                        const float yv = s.line[1].at (ms (0.2 + 4.0 * p[5] / 99.0 * (0.5 + 0.5 * lfoShape (s.ph[1], 1))) + 1.0);
                        s.line[1].push (in + yv * (float) p[6] / 105.0f);
                        w = yv; b2 = bal (p[7]);
                        break;
                    }
                    case 44: case 45: w = distort (1, in, p[3], p[4], p[5], p[6], type == 44); b2 = 1.0f; break;
                    case 46: s.ph[0] += speedHz (p[4]) / sr; w = phaser (1, in, 60, speedHz (p[4]), p[5], p[6], 1, 0.0); b2 = bal (p[7]); break;
                    case 47: { float wl, wr; rotary (in, 2, p[3], p[4], p[5], wl, wr); w = 0.5f * (wl + wr); b2 = bal (p[6]); break; }
                    default: break;
                }
                r[i] = in * (1.0f - b2) + w * b2;
            }
            break;
        }
        case 48:
        {
            // resonance filter: its own attack/decay EG sweeps the cutoff from FC, triggered as Trigger says; the LFO at
            // the first oscillator's VDF LFO speed
            if (ctx.noteOns > 0)
            {
                const bool trigger = p[6] == 0 ? ctx.firstNoteOn : true;
                if (trigger) { if (p[6] == 2) s.env = 0.0f; s.egStage = 1; }
            }
            if (ctx.held == 0 && s.egStage == 1) s.egStage = 2;
            const float attack = 1.0f / (float) std::max (1.0, egSeconds (p[4]) * sr), decay = 1.0f / (float) std::max (1.0, egSeconds (p[5]) * sr);
            const float trim = (float) p[0] / 99.0f;
            for (int i = 0; i < n; ++i)
            {
                if (s.egStage == 1) { s.env += attack; if (s.env >= 1.0f) { s.env = 1.0f; s.egStage = 2; } }
                else if (s.egStage == 2) { s.env = std::max (0.0f, s.env - decay); if (s.env <= 0.0f) s.egStage = 0; }
                s.ph[0] += ctx.lfoHz / sr;
                const double fc = (float) p[2] + (float) p[3] * s.env + p[7] / 127.0 * 30.0 * std::sin (twoPi * s.ph[0]);
                const double hz = 50.0 * std::pow (240.0, std::clamp (fc, 0.0, 99.0) / 99.0);
                const double q = 0.7 + 14.0 * p[1] / 99.0;
                if ((i & 15) == 0) { s.svf[0].set (hz, q, sr); s.svf[1].set (hz, q, sr); }
                s.svf[0].run (l[i] * trim); s.svf[1].run (r[i] * trim);
                l[i] = s.svf[0].lpOut;
                r[i] = s.svf[1].lpOut;
            }
            break;
        }
        default: break;
    }
}

//==============================================================================
void EffectRack::prepare (double sampleRate, int maxBlock)
{
    for (auto& u : units) u.prepare (sampleRate);
    work1.setSize (2, maxBlock);
    work2.setSize (2, maxBlock);
}

void EffectRack::reset() { for (auto& u : units) u.reset(); }

void EffectRack::process (const float* const* ab, const float* const* c, const float* const* d, float* outL, float* outR, int n,
                          const FxSettings& fx, const FxContext& ctx) noexcept
{
    if (n > work1.getNumSamples()) { work1.setSize (2, n, false, false, true); work2.setSize (2, n, false, false, true); }
    float* w1[2] { work1.getWritePointer (0), work1.getWritePointer (1) };
    float* w2[2] { work2.getWritePointer (0), work2.getWritePointer (1) };
    // PAN 3 and 4 (SERIAL, PARA.1, PARA.2): where each effect output channel goes, L .. R
    auto panOut = [&] (const float* l, const float* r, int k)
    {
        const float p3 = fx.pan3 < 0 ? -1.0f : (float) fx.pan3 / 100.0f, p4 = fx.pan4 < 0 ? -1.0f : (float) fx.pan4 / 100.0f;
        if (p3 >= 0.0f) { outL[k] += l[k] * std::sqrt (1.0f - p3); outR[k] += l[k] * std::sqrt (p3); }
        if (p4 >= 0.0f) { outL[k] += r[k] * std::sqrt (1.0f - p4); outR[k] += r[k] * std::sqrt (p4); }
    };
    std::fill_n (outL, n, 0.0f);
    std::fill_n (outR, n, 0.0f);
    switch (fx.placement)
    {
        case 0:   // SERIAL: A/B through 1; then C and D join; all through 2
            for (int k = 0; k < n; ++k) { w1[0][k] = ab[0][k]; w1[1][k] = ab[1][k]; }
            units[0].process (w1[0], w1[1], n, fx.unit[0], ctx);
            for (int k = 0; k < n; ++k) { w1[0][k] += c[0][k] + d[0][k]; w1[1][k] += c[1][k] + d[1][k]; }
            units[1].process (w1[0], w1[1], n, fx.unit[1], ctx);
            for (int k = 0; k < n; ++k) panOut (w1[0], w1[1], k);
            break;
        case 2:   // PARA.2: C and D through 2, into 1 with A/B
            for (int k = 0; k < n; ++k) { w2[0][k] = c[0][k] + d[0][k]; w2[1][k] = c[1][k] + d[1][k]; }
            units[1].process (w2[0], w2[1], n, fx.unit[1], ctx);
            for (int k = 0; k < n; ++k) { w1[0][k] = ab[0][k] + w2[0][k]; w1[1][k] = ab[1][k] + w2[1][k]; }
            units[0].process (w1[0], w1[1], n, fx.unit[0], ctx);
            for (int k = 0; k < n; ++k) panOut (w1[0], w1[1], k);
            break;
        case 3:   // PARA.3: A/B straight out; C through 1, D through 2, at their levels (GM's)
        {
            for (int k = 0; k < n; ++k) { w1[0][k] = c[0][k]; w1[1][k] = c[1][k]; w2[0][k] = d[0][k]; w2[1][k] = d[1][k]; }
            units[0].process (w1[0], w1[1], n, fx.unit[0], ctx);
            units[1].process (w2[0], w2[1], n, fx.unit[1], ctx);
            const float g[4] { fx.level[0] / 9.0f, fx.level[1] / 9.0f, fx.level[2] / 9.0f, fx.level[3] / 9.0f };
            for (int k = 0; k < n; ++k)
            {
                outL[k] = ab[0][k] + w1[0][k] * g[0] + w2[0][k] * g[2];
                outR[k] = ab[1][k] + w1[1][k] * g[1] + w2[1][k] * g[3];
            }
            break;
        }
        case 1: default:   // PARA.1: A/B through 1; C and D through 2; mixed
            for (int k = 0; k < n; ++k) { w1[0][k] = ab[0][k]; w1[1][k] = ab[1][k]; w2[0][k] = c[0][k] + d[0][k]; w2[1][k] = c[1][k] + d[1][k]; }
            units[0].process (w1[0], w1[1], n, fx.unit[0], ctx);
            units[1].process (w2[0], w2[1], n, fx.unit[1], ctx);
            for (int k = 0; k < n; ++k) { w1[0][k] += w2[0][k]; w1[1][k] += w2[1][k]; panOut (w1[0], w1[1], k); }
            break;
    }
}
} // namespace tf::n5
