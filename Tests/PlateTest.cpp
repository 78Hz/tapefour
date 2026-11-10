// TF PLATE (Source/Plate.h): a plate reverb, as a plate's modes (SIMULATED) or a network (LIGHT).
//
//   The physics: a 2 x 1 m, 0.5 mm steel plate has about 1.3 resonances a hertz, the EMT 240's foil
//   more than twice as many (EMT: "above 3 per Hz"); DAMPING gives its decay at 500 Hz, and more damping
//   less; highs decay faster; a click arrives bright first (bending waves are dispersive); the two
//   pickups hear different things, one pickup the same on both sides; both engines sit at about the same
//   level; an aged plate is duller; another plate's modes come in off the audio thread; the state comes
//   back, and an old plate's too.

#include "../Source/Plate.h"

#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace tf;
using namespace tf::plate;

namespace
{
int failures = 0;
const char* annotate = std::getenv ("GITHUB_ACTIONS");
constexpr double sr = 48000.0;

void check (bool ok, const char* what, double got, double want)
{
    std::printf ("%s  %-78s got %10.4f  want %10.4f\n", ok ? "PASS" : "FAIL", what, got, want);
    if (! ok)
    {
        ++failures;
        if (annotate != nullptr) std::printf ("::error::PlateTest: %s (got %.4f, want %.4f)\n", what, got, want);
    }
}

std::vector<float> band (const std::vector<float>& x, double lo, double hi)
{
    Biquad a, b, c, d;
    a.setHighPass (sr, lo); b.setHighPass (sr, lo); c.setLowPass (sr, hi); d.setLowPass (sr, hi);
    std::vector<float> y (x.size());
    for (size_t i = 0; i < x.size(); ++i) y[i] = d.process (c.process (b.process (a.process (x[i]))));
    return y;
}

/** The decay (s to -60 dB), from the backward-integrated energy between -5 and -25 dB. */
double t60 (const std::vector<float>& x)
{
    std::vector<double> e (x.size() + 1, 0.0);
    for (size_t i = x.size(); i-- > 0;) e[i] = e[i + 1] + (double) x[i] * x[i];
    double t5 = -1, t25 = -1;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double db = 10.0 * std::log10 (e[i] / e[0] + 1e-30);
        if (t5 < 0 && db <= -5.0) t5 = (double) i / sr;
        if (t25 < 0 && db <= -25.0) { t25 = (double) i / sr; break; }
    }
    return t25 > t5 && t5 >= 0 ? 3.0 * (t25 - t5) : -1.0;
}

/** When a band first comes within 20 dB of its loudest. */
double arrival (const std::vector<float>& x)
{
    float top = 0.0f;
    for (auto v : x) top = std::max (top, std::abs (v));
    for (size_t i = 0; i < x.size(); ++i) if (std::abs (x[i]) > top * 0.1f) return (double) i / sr;
    return -1.0;
}

struct Run { std::vector<float> l, r; };
Run run (Processor& p, double seconds, bool clickIn = true, bool paced = false)
{
    Run out;
    juce::AudioBuffer<float> b (2, 512);
    juce::MidiBuffer m;
    const int total = (int) (seconds * sr);
    for (int at = 0; at < total; at += 512)
    {
        const int n = std::min (512, total - at);
        b.setSize (2, n, false, false, true);
        b.clear();
        if (clickIn && at == 0) { b.setSample (0, 0, 1.0f); b.setSample (1, 0, 1.0f); }
        p.processBlock (b, m);
        // a pause once the click is in, so anything the builder hands over lands while it rings
        // (it once built the plate a second time after prepareToPlay, and the crossfade to those
        // silent modes cut the ring off)
        if (paced && at == 0) std::this_thread::sleep_for (std::chrono::milliseconds (250));
        for (int i = 0; i < n; ++i) { out.l.push_back (b.getSample (0, i)); out.r.push_back (b.getSample (1, i)); }
    }
    return out;
}

double rms (const std::vector<float>& v) { double e = 0; for (auto x : v) e += (double) x * x; return std::sqrt (e / (double) std::max<size_t> (1, v.size())); }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    // ---- the physics ----
    {
        const double d140 = modesPerHz (plateInfo (0)), d240 = modesPerHz (plateInfo (1));
        check (std::abs (d140 - 1.31) < 0.05, "EMT 140 (2 x 1 m, 0.5 mm steel): about 1.3 resonances a hertz", d140, 1.31);
        check (d240 > 3.0 && d240 > 2.0 * d140, "EMT 240's foil: above 3 a hertz, more than twice the 140 (EMT's figures)", d240, 3.3);
        Modes m;
        m.build (0, 0.7, sr, Processor::modeBudget);
        const double counted = m.count / (m.topHz - 60.0);
        check (m.count > 3000 && std::abs (counted - d140) / d140 < 0.2, "the modes it builds are as dense as that", counted, d140);
    }

    // ---- decay: DAMPING sets it at 500 Hz, for both engines ----
    for (const bool sim : { true, false })
    {
        Processor p;
        p.set ("engine", sim ? 0.0f : 1.0f);
        p.set ("predelay", 0.0f);
        p.set ("condition", 1.0f);
        p.set ("damping", 0.5f);
        p.prepareToPlay (sr, 512);
        const double want = p.decay500();
        auto out = run (p, want * 1.2, true, true);
        const double got = t60 (band (out.l, 400.0, 630.0));
        if (sim)
            check (p.handovers() == 0, "prepared, it plays the modes it built: the builder doesn't build them again", p.handovers(), 0);
        check (std::abs (got - want) / want < 0.3, sim ? "SIMULATED: the decay at 500 Hz is what DAMPING says" : "LIGHT: the decay at 500 Hz is what DAMPING says", got, want);
        const double high = t60 (band (out.l, 5000.0, 8000.0));
        check (high > 0.0 && high < got * 0.8, sim ? "SIMULATED: highs die away sooner" : "LIGHT: highs die away sooner", high, got * 0.6);
        if (sim)
        {
            Processor q;
            q.set ("predelay", 0.0f);
            q.set ("condition", 1.0f);
            q.set ("damping", 0.95f);
            q.prepareToPlay (sr, 512);
            auto damped = run (q, 2.0, true, true);
            const double shorter = t60 (band (damped.l, 400.0, 630.0));
            check (shorter < got * 0.6, "more damping (the damper nearer): a shorter decay", shorter, q.decay500());
        }
    }

    // ---- a click arrives bright first ----
    {
        Processor p;
        p.set ("predelay", 0.0f);
        p.prepareToPlay (sr, 512);
        auto out = run (p, 0.4);
        const double hi = arrival (band (out.l, 2000.0, 3000.0)), lo = arrival (band (out.l, 120.0, 180.0));
        check (hi >= 0.0 && lo > hi + 0.002, "SIMULATED: highs arrive before lows (bending waves are dispersive)", lo - hi, 0.0077);
    }

    // ---- pickups ----
    {
        Processor p;
        p.set ("predelay", 0.0f);
        p.prepareToPlay (sr, 512);
        auto st = run (p, 1.0);
        double xy = 0, xx = 0, yy = 0;
        for (size_t i = 0; i < st.l.size(); ++i) { xy += (double) st.l[i] * st.r[i]; xx += (double) st.l[i] * st.l[i]; yy += (double) st.r[i] * st.r[i]; }
        const double corr = xy / std::sqrt (xx * yy + 1e-30);
        check (std::abs (corr) < 0.4, "two pickups at different places hear different things", corr, 0.0);
        p.set ("stereo", 0.0f);
        p.prepareToPlay (sr, 512);
        auto mono = run (p, 0.5);
        double diff = 0;
        for (size_t i = 0; i < mono.l.size(); ++i) diff = std::max (diff, (double) std::abs (mono.l[i] - mono.r[i]));
        check (diff < 1e-6, "MONO: one pickup, the same on both sides", diff, 0.0);
    }

    // ---- the engines' levels; noise in ----
    {
        double levels[2];
        for (int e = 0; e < 2; ++e)
        {
            Processor p;
            p.set ("engine", (float) e);
            p.prepareToPlay (sr, 512);
            juce::AudioBuffer<float> b (2, 512);
            juce::MidiBuffer m;
            juce::Random rnd (9);
            std::vector<float> out;
            for (int k = 0; k < (int) (3.0 * sr / 512); ++k)
            {
                for (int i = 0; i < 512; ++i) { const float v = 0.1f * (rnd.nextFloat() * 2.0f - 1.0f); b.setSample (0, i, v); b.setSample (1, i, v); }
                p.processBlock (b, m);
                if (k > 100) for (int i = 0; i < 512; ++i) out.push_back (b.getSample (0, i));
            }
            bool finite = true;
            for (auto v : out) finite = finite && std::isfinite (v);
            levels[e] = finite ? rms (out) : -1.0;
        }
        const double ratio = 20.0 * std::log10 (levels[0] / levels[1]);
        check (levels[0] > 0.005 && levels[1] > 0.005 && std::abs (ratio) < 6.0, "SIMULATED and LIGHT sit within 6 dB of each other", ratio, 0.0);
    }

    // ---- condition: an aged, slack plate is duller ----
    {
        auto treble = [] (float cond)
        {
            Processor p;
            p.set ("predelay", 0.0f);
            p.set ("condition", cond);
            p.prepareToPlay (sr, 512);
            auto out = run (p, 1.5);
            return t60 (band (out.l, 5000.0, 8000.0));
        };
        const double fresh = treble (1.0f), aged = treble (0.0f);
        check (aged < fresh * 0.85, "CONDITION: an aged plate's highs die away sooner", aged / fresh, 0.7);
    }

    // ---- another plate: its modes come from the builder, and take over ----
    {
        Processor p;
        p.prepareToPlay (sr, 512);
        run (p, 0.1);
        const int before = p.modesInUse();
        p.set ("plate", 2.0f);
        bool switched = false;
        for (int k = 0; k < 200 && ! switched; ++k)
        {
            run (p, 0.02, false);
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
            switched = p.modesInUse() != before;
        }
        check (switched && p.modesTopHz() > 5000.0, "another plate: its modes are built off the audio thread and take over", p.modesTopHz(), 8000.0);
    }

    // ---- state, and a plate from before ----
    {
        Processor p;
        p.set ("plate", 1.0f);
        p.set ("damping", 0.2f);
        p.set ("stereo", 0.0f);
        p.set ("lx", 0.6f);
        juce::MemoryBlock mb;
        p.getStateInformation (mb);
        Processor q;
        q.setStateInformation (mb.getData(), (int) mb.getSize());
        const bool same = q.plateIndex() == 1 && std::abs (q.get ("damping") - 0.2f) < 1e-3 && q.get ("stereo") < 0.5f && std::abs (q.get ("lx") - 0.6f) < 1e-3;
        check (same, "state: the plate, its damper and the pickups come back", same, 1);

        juce::XmlElement old ("FX");
        old.setAttribute ("size", 1.0);
        old.setAttribute ("damping", 0.45);
        old.setAttribute ("predelay", 30.0);
        juce::MemoryBlock ob;
        juce::AudioProcessor::copyXmlToBinary (old, ob);
        Processor r;
        r.setStateInformation (ob.getData(), (int) ob.getSize());
        check (r.plateIndex() == 0 && r.decay500() > 3.5 && std::abs (r.get ("predelay") - 30.0f) < 0.01f,
               "a plate saved before the model: the 140, its size as a long decay, its pre-delay", r.decay500(), 4.0);
    }

    // ---- how much of the processor it takes (printed, not checked) ----
    for (int e = 0; e < 2; ++e)
    {
        Processor p;
        p.set ("engine", (float) e);
        p.prepareToPlay (sr, 256);
        juce::AudioBuffer<float> b (2, 256);
        juce::MidiBuffer m;
        juce::Random rnd (2);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const int blocks = (int) (2.0 * sr / 256);
        for (int k = 0; k < blocks; ++k)
        {
            for (int i = 0; i < 256; ++i) { const float v = 0.1f * (rnd.nextFloat() * 2.0f - 1.0f); b.setSample (0, i, v); b.setSample (1, i, v); }
            p.processBlock (b, m);
        }
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        std::printf ("INFO  %s: %.1f%% of one core in real time (%d modes)\n", e == 0 ? "SIMULATED" : "LIGHT", ms / 20.0, p.modesInUse());
    }

    std::printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
