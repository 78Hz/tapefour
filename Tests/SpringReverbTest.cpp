// TF SPRING REVERB (Source/SpringReverb.h): springs simulated as their modes, or as a dispersive waveguide.
//
//   The spring's transition frequency and echo spacing from its physics; the tanks' decay against
//   Accutronics' decay classes; the chirps (higher frequencies arriving later); the waveguide matching the
//   simulation; the valve driver clipping as DWELL goes up; the level; the state; and how much processor
//   each takes (printed).
//
// With TF_SPRING_RENDER=<folder> it also writes each tank's click and a drum-ish loop through it.

#include "../Source/SpringReverb.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>

using namespace tf;
using namespace tf::spring;

namespace
{
int failures = 0;
const char* annotate = std::getenv ("GITHUB_ACTIONS");

void check (bool ok, const char* what, double got, double want)
{
    std::printf ("%s  %-72s got %10.3f  want %10.3f\n", ok ? "PASS" : "FAIL", what, got, want);
    if (! ok)
    {
        ++failures;
        if (annotate != nullptr) std::printf ("::error::SpringReverbTest: %s (got %.3f, want %.3f)\n", what, got, want);
    }
}

constexpr double sr = 48000.0, twoPi = 6.283185307179586;

/** A band of a signal (two band-pass biquads), squared and smoothed: its energy over time. */
std::vector<double> bandEnergy (const std::vector<float>& x, double lo, double hi, double smoothMs = 2.0)
{
    auto bp = [] (double f0, double q)
    {
        Biquad b;
        const double w = twoPi * f0 / sr, al = std::sin (w) / (2.0 * q), c = std::cos (w);
        b.set (al, 0.0, -al, 1.0 + al, -2.0 * c, 1.0 - al);
        return b;
    };
    const double f0 = std::sqrt (lo * hi), q = f0 / (hi - lo);
    auto b1 = bp (f0, q), b2 = bp (f0, q);
    std::vector<double> e (x.size());
    const double a = std::exp (-1.0 / (smoothMs * 0.001 * sr));
    double s = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double y = b2.process (b1.process (x[i]));
        s = a * s + (1.0 - a) * y * y;
        e[i] = s;
    }
    return e;
}

/** T60 (s) from a band's energy: Schroeder's backward integral, fitted between -5 and -25 dB. */
double t60Of (const std::vector<float>& x, double lo, double hi)
{
    const auto e = bandEnergy (x, lo, hi, 0.5);
    std::vector<double> back (e.size());
    double s = 0.0;
    for (size_t i = e.size(); i-- > 0;) { s += e[i]; back[i] = s; }
    const double total = back[0];
    double t5 = -1, t25 = -1;
    for (size_t i = 0; i < back.size(); ++i)
    {
        const double db = 10.0 * std::log10 (std::max (1.0e-30, back[i] / total));
        if (t5 < 0 && db <= -5.0) t5 = (double) i / sr;
        if (t25 < 0 && db <= -25.0) { t25 = (double) i / sr; break; }
    }
    return t5 >= 0 && t25 > t5 ? 3.0 * (t25 - t5) : 0.0;
}

/** The time of the first strong arrival in a band (s): where its energy first reaches a fifth of its peak in the first 0.1 s. */
double arrival (const std::vector<float>& x, double lo, double hi)
{
    const auto e = bandEnergy (x, lo, hi, 1.0);
    const size_t n = std::min (e.size(), (size_t) (0.1 * sr));
    double peak = 0.0;
    for (size_t i = 0; i < n; ++i) peak = std::max (peak, e[i]);
    for (size_t i = 0; i < n; ++i) if (e[i] >= 0.2 * peak) return (double) i / sr;
    return 0.0;
}

/** How strongly the low band's envelope repeats at a lag (s), against its strongest repeat between 20 and 120 ms. */
double repeatAt (const std::vector<float>& x, double lagS)
{
    const auto e = bandEnergy (x, 150.0, 900.0, 1.5);
    const size_t n = std::min (e.size(), (size_t) (1.0 * sr));
    auto corr = [&] (int lag) { double s = 0.0; for (size_t i = 0; i + (size_t) lag < n; ++i) s += e[i] * e[i + (size_t) lag]; return s; };
    double best = 0.0, near = 0.0;
    for (int lag = (int) (0.02 * sr); lag < (int) (0.12 * sr); lag += 4)
    {
        const double c = corr (lag);
        best = std::max (best, c);
        if (std::abs (lag / sr - lagS) < 0.003) near = std::max (near, c);
    }
    return near / std::max (1.0e-30, best);
}

/** The echo spacing (s): the strongest repeat of the low band's envelope between 20 and 120 ms. */
double echoSpacing (const std::vector<float>& x)
{
    const auto e = bandEnergy (x, 150.0, 900.0, 1.5);
    const size_t n = std::min (e.size(), (size_t) (1.0 * sr));
    double best = 0.0, bestLag = 0.0;
    for (int lag = (int) (0.02 * sr); lag < (int) (0.12 * sr); ++lag)
    {
        double s = 0.0;
        for (size_t i = 0; i + (size_t) lag < n; ++i) s += e[i] * e[i + (size_t) lag];
        if (s > best) { best = s; bestLag = lag / sr; }
    }
    return bestLag;
}

double rms (const std::vector<float>& v, size_t from, size_t to)
{
    double s = 0.0;
    for (size_t i = from; i < to; ++i) s += (double) v[i] * v[i];
    return std::sqrt (s / (double) std::max<size_t> (1, to - from));
}

void write (const juce::String& name, const std::vector<float>& v)
{
    if (const char* dir = std::getenv ("TF_SPRING_RENDER"))
    {
        auto f = juce::File (dir).getChildFile (name + ".wav");
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
        if (auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (1).withBitsPerSample (24)))
        {
            const float* ch[] { v.data() };
            w->writeFromFloatArrays (ch, 1, (int) v.size());
        }
    }
}

/** Runs the processor on a signal (mono in, wet out). */
std::vector<float> run (Processor& p, const std::vector<float>& in)
{
    std::vector<float> out;
    juce::AudioBuffer<float> b (2, 256);
    juce::MidiBuffer m;
    for (size_t at = 0; at < in.size(); at += 256)
    {
        const int n = (int) std::min<size_t> (256, in.size() - at);
        b.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i) { b.setSample (0, i, in[at + (size_t) i]); b.setSample (1, i, in[at + (size_t) i]); }
        p.processBlock (b, m);
        out.insert (out.end(), b.getReadPointer (0), b.getReadPointer (0) + n);
    }
    return out;
}

void set (Processor& p, const char* id, float v) { auto* r = p.param (id); r->setValueNotifyingHost (r->convertTo0to1 (v)); }
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const char* names[] { "fender", "three", "short" };

    // ---------------------------------------------------------------- the spring's physics
    {
        const Physics p;
        check (std::abs (p.transitionHz() - 4300.0) < 250.0, "physics: the fitted spring's transition frequency, near 4.3 kHz (Hz)", p.transitionHz(), 4300.0);
        check (std::abs (p.roundTripSeconds() - 0.0351) < 0.002, "physics: its low echoes about 35 ms apart (s)", p.roundTripSeconds(), 0.0351);
        const Physics s = p.lengthened (0.55);
        check (std::abs (s.transitionHz() - p.transitionHz()) < 60.0, "physics: a shorter spring of the same wire keeps its transition (Hz)", s.transitionHz(), p.transitionHz());
        check (std::abs (s.roundTripSeconds() / p.roundTripSeconds() - 0.55) < 0.01, "physics: and its echoes come 0.55 times as often apart", s.roundTripSeconds() / p.roundTripSeconds(), 0.55);
    }

    // ---------------------------------------------------------------- each tank's click
    for (int t = 0; t < numTanks; ++t)
    {
        const auto& info = tankInfo ((Tank) t);
        const auto c = Processor::click ((Tank) t, Model::simulated, sr, 5.0);
        write (juce::String ("click_") + names[t], c);
        juce::String what;
        const double lo = info.sigma < 2.5 ? 2.75 : 1.75, hi = info.sigma < 2.5 ? 4.0 : 3.0;
        const double t60 = t60Of (c, 350.0, 700.0);
        what = juce::String (info.code) + ": decays within its class at 500 Hz (T60, s)";
        check (t60 >= lo * 0.9 && t60 <= hi * 1.1, what.toRawUTF8(), t60, 0.5 * (lo + hi));
        const auto w = Processor::click ((Tank) t, Model::waveguide, sr, 5.0);
        write (juce::String ("click_") + names[t] + "_waveguide", w);
        const double wt60 = t60Of (w, 350.0, 700.0);
        what = juce::String (info.code) + ": the waveguide decays alike (T60, s)";
        check (wt60 >= lo * 0.8 && wt60 <= hi * 1.2, what.toRawUTF8(), wt60, 0.5 * (lo + hi));
    }

    // ---------------------------------------------------------------- one spring, both ways
    {
        const Physics p;
        auto impulse = [] (auto& path)
        {
            std::vector<float> out ((size_t) (1.5 * sr));
            for (size_t k = 0; k < out.size(); ++k) out[k] = path.process (k == 0 ? 1.0f : 0.0f);
            return out;
        };
        ModalPath m;
        m.build (p, sr, 10000.0);
        WaveguidePath g;
        g.build (p, sr);
        const auto mo = impulse (m), go = impulse (g);
        write ("spring_modal", mo);
        write ("spring_waveguide", go);
        const double rt = p.roundTripSeconds();
        // (the simulation also has the second family of echoes van Walstijn notes, about 35 ms apart: the round trip
        // must be one of the strong repeats, not necessarily the strongest)
        check (repeatAt (mo, rt) > 0.6, "spring, simulated: its echoes repeat a round trip apart (against the strongest)", repeatAt (mo, rt), 1.0);
        check (std::abs (echoSpacing (go) - rt) < 0.003, "spring, waveguide: the same (s)", echoSpacing (go), rt);
        const double a500 = arrival (mo, 400.0, 600.0), a3k = arrival (mo, 2700.0, 3300.0);
        check (a500 > 0.3 * rt && a500 < 0.5 * rt + 0.004, "spring, simulated: 500 Hz arrives within one length (s)", a500, 0.5 * rt);
        check (a3k > a500 + 0.004, "spring, simulated: the chirp: 3 kHz arrives later (ms)", (a3k - a500) * 1000.0, 10.0);
        const double g500 = arrival (go, 400.0, 600.0), g3k = arrival (go, 2700.0, 3300.0);
        check (std::abs (g500 - 0.5 * rt) < 0.006, "spring, waveguide: 500 Hz arrives after one length (s)", g500, 0.5 * rt);
        check (g3k > g500 + 0.004, "spring, waveguide: the chirp: 3 kHz arrives later (ms)", (g3k - g500) * 1000.0, 10.0);
    }

    // ---------------------------------------------------------------- the driver
    {
        auto distortion = [] (float dwell)
        {
            Circuit c;
            c.prepare (sr, tankInfo (Tank::fender));
            // a 250 Hz sine at a track's level (peaks at -12 dBFS): how much of the third harmonic comes out
            double h1r = 0, h1i = 0, h2r = 0, h2i = 0;
            const int n = (int) sr;
            for (int i = 0; i < n; ++i)
            {
                const float y = c.drive ((float) (0.25 * std::sin (twoPi * 250.0 * i / sr)), dwell);
                if (i < n / 2) continue;
                const double ph = twoPi * 250.0 * i / sr;
                h1r += y * std::cos (ph); h1i += y * std::sin (ph);
                h2r += y * std::cos (2 * ph); h2i += y * std::sin (2 * ph);
            }
            // the coil's lag takes the 2nd harmonic down 6 dB against the 1st: put it back
            return 2.0 * std::hypot (h2r, h2i) / std::max (1.0e-12, std::hypot (h1r, h1i));
        };
        const double soft = distortion (2.0f), hard = distortion (10.0f);
        check (soft < 0.02, "driver: at DWELL 2 the 12AT7 is clean (2nd harmonic / fundamental)", soft, 0.0);
        check (hard > 0.1, "driver: at DWELL 10 it clips (2nd harmonic / fundamental)", hard, 0.2);
    }

    // ---------------------------------------------------------------- the level, the state and the processor
    {
        Processor p;
        p.setRateAndBufferSizeDetails (sr, 256);
        p.prepareToPlay (sr, 256);
        juce::Random rnd (3);
        std::vector<float> noise ((size_t) (4.0 * sr));
        for (auto& v : noise) v = 0.1f * (rnd.nextFloat() * 2.0f - 1.0f);   // white noise at -20.8 dBFS RMS
        for (int t = 0; t < numTanks; ++t)
            for (int m = 0; m < 2; ++m)
            {
                set (p, "tank", (float) t);
                set (p, "model", (float) m);
                const auto start = std::chrono::steady_clock::now();
                const auto out = run (p, noise);
                const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
                const double ratio = 20.0 * std::log10 (rms (out, out.size() / 2, out.size()) / rms (noise, 0, noise.size()));
                const juce::String what = juce::String ("level: ") + tankInfo ((Tank) t).code + (m == 0 ? " simulated" : " waveguide") + ", wet against dry at DWELL 5 (dB)";
                check (std::abs (ratio) < 6.0, what.toRawUTF8(), ratio, 0.0);
                std::printf ("      %s %s: %d modes, %.1f%% of one core at 48 kHz\n", tankInfo ((Tank) t).code, m == 0 ? "simulated" : "waveguide",
                             m == 0 ? p.modesInUse() : 0, 100.0 * secs / 4.0);
            }
        set (p, "tank", 2.0f); set (p, "model", 1.0f); set (p, "dwell", 7.5f); set (p, "tone", 3.0f);
        juce::MemoryBlock mb;
        p.getStateInformation (mb);
        Processor q;
        q.setStateInformation (mb.getData(), (int) mb.getSize());
        const bool same = q.tank() == Tank::threeShort && q.model() == Model::waveguide
                       && std::abs (q.param ("dwell")->convertFrom0to1 (q.param ("dwell")->getValue()) - 7.5f) < 0.01f;
        check (same, "state: tank, model, dwell and tone come back", same ? 1 : 0, 1);
    }

    std::printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
