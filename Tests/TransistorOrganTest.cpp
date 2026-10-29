// TF TRANSISTOR (Source/TransistorOrgan.h): the divide-down organ.
//
//   The twelve oscillators and their exact octaves, each footage's pitch, the drawbars, the reed and
//   flute tones, the key contacts, the vibrato, the drift and tuning up, the tuning systems a
//   divide-down organ can have, and the state round trip and presets.
//
// With TF_ORGAN_RENDER=<folder> it also writes WAVs to listen to.

#include "../Source/TransistorOrgan.h"

#include <cstdio>
#include <cstdlib>

using namespace tf;

namespace
{
int failures = 0;
const char* annotate = std::getenv ("GITHUB_ACTIONS");

void check (bool ok, const char* what, double got, double want)
{
    std::printf ("%s  %-66s got %10.3f  want %10.3f\n", ok ? "PASS" : "FAIL", what, got, want);
    if (! ok)
    {
        ++failures;
        if (annotate != nullptr) std::printf ("::error::TransistorOrganTest: %s (got %.3f, want %.3f)\n", what, got, want);
    }
}

double centsBetween (double a, double b) { return 1200.0 * std::log2 (a / b); }

constexpr double sr = 48000.0;
constexpr int bs = 256;

struct Rig
{
    organ::Processor p;
    Rig()
    {
        p.setRateAndBufferSizeDetails (sr, bs);
        p.prepareToPlay (sr, bs);
        auto s = p.settings();
        s.vibrato = false; s.driftCents = 0.0f; s.bounceMs = 0.0f; s.spreadMs = 0.0f;
        s.drawbar16 = 0.0f; s.drawbar8 = 8.0f; s.drawbar4 = 0.0f; s.drawbarIV = 0.0f; s.flute = 0.0f; s.reed = 8.0f;
        p.setSettings (s);
        p.tuneUp();
        run (0.02, {});   // the tune-up takes effect on the audio thread
    }
    // plays: events at (seconds, message); returns the left channel
    std::vector<float> run (double seconds, std::vector<std::pair<double, juce::MidiMessage>> events)
    {
        std::vector<float> out;
        juce::AudioBuffer<float> b (2, bs);
        const int blocks = (int) (seconds * sr / bs);
        size_t e = 0;
        for (int k = 0; k < blocks; ++k)
        {
            juce::MidiBuffer m;
            while (e < events.size() && events[e].first * sr < (k + 1) * bs)
            {
                m.addEvent (events[e].second, std::clamp ((int) (events[e].first * sr) - k * bs, 0, bs - 1));
                ++e;
            }
            p.processBlock (b, m);
            out.insert (out.end(), b.getReadPointer (0), b.getReadPointer (0) + bs);
        }
        return out;
    }
};

double rms (const std::vector<float>& v, double from, double to)
{
    const auto a = (size_t) (from * sr), b = std::min (v.size(), (size_t) (to * sr));
    double s = 0;
    for (auto i = a; i < b; ++i) s += (double) v[i] * v[i];
    return std::sqrt (s / (double) std::max<size_t> (1, b - a));
}

// The amplitude of a frequency in a stretch (a single DFT bin, Hann window).
double bin (const std::vector<float>& v, double from, double to, double f)
{
    const auto a = (size_t) (from * sr), b = std::min (v.size(), (size_t) (to * sr));
    const double n = (double) (b - a);
    double re = 0, im = 0;
    for (auto i = a; i < b; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979 * (double) (i - a) / n);
        const double ph = 2.0 * 3.14159265358979 * f * (double) i / sr;
        re += v[i] * w * std::cos (ph); im += v[i] * w * std::sin (ph);
    }
    return 2.0 * std::sqrt (re * re + im * im) / (n * 0.5);
}

// The strongest frequency near f (cents resolution), by scanning bins.
double peakNear (const std::vector<float>& v, double from, double to, double f)
{
    double best = f, bestA = 0;
    for (double c = -30.0; c <= 30.0; c += 0.25)
    {
        const double ff = f * std::pow (2.0, c / 1200.0);
        const double a = bin (v, from, to, ff);
        if (a > bestA) { bestA = a; best = ff; }
    }
    return best;
}

juce::MidiMessage on (int n, float v) { return juce::MidiMessage::noteOn (1, n, v); }
juce::MidiMessage off (int n) { return juce::MidiMessage::noteOff (1, n); }

// a peak, found coarsely then refined to a hundredth of a cent
double finePeak (const std::vector<float>& v, double from, double to, double f)
{
    double best = peakNear (v, from, to, f), bestA = 0;
    const double centre = best;
    for (double c = -0.4; c <= 0.4; c += 0.01)
    {
        const double ff = centre * std::pow (2.0, c / 1200.0);
        const double a = bin (v, from, to, ff);
        if (a > bestA) { bestA = a; best = ff; }
    }
    return best;
}

double db (double a, double b) { return 20.0 * std::log10 (std::max (1.0e-12, a) / std::max (1.0e-12, b)); }

void writeWav (const juce::File& f, const std::vector<float>& v)
{
    juce::AudioBuffer<float> b (1, (int) v.size());
    b.copyFrom (0, 0, v.data(), (int) v.size());
    f.deleteFile();
    std::unique_ptr<juce::OutputStream> out (f.createOutputStream());
    if (out == nullptr) return;
    juce::WavAudioFormat wav;
    auto options = juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (1).withBitsPerSample (24);
    if (auto w = wav.createWriterFor (out, options))
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    // ---------------------------------------------------------------- the generators
    {
        organ::Processor p;
        const auto osc = p.oscillatorHz();
        check (std::abs (osc[9] - 3520.0) < 1.0e-6 && std::abs (osc[0] - 2093.005) < 0.01, "generators: the oscillators are the top octave (A7, Hz)", osc[9], 3520.0);
        const auto k = organ::keyTones (60);
        check (k.foot16 == 48 && k.foot8 == 60 && k.foot4 == 72 && k.mixture == std::array<int, 4> { 79, 84, 88, 96 },
               "keys: C4's contacts: 16' C3, 8' C4, 4' C5, IV G5 C6 E6 C7", k.mixture[0], 79);
        const auto top = organ::keyTones (84);
        check (top.mixture[3] == 108 - 12 && top.foot16 == 72, "keys: the top key's 1' folds back an octave (no tone above B7)", top.mixture[3], 96);
    }
    {
        Rig r;
        auto a4 = r.run (2.0, { { 0.0, on (69, 0.8f) } });
        const double f = finePeak (a4, 0.2, 1.8, 440.0);
        check (std::abs (centsBetween (f, 440.0)) < 0.2, "pitch: A4 at 440 Hz (cents off)", centsBetween (f, 440.0), 0.0);
        check (bin (a4, 0.2, 1.8, 880.0) < 0.01 * bin (a4, 0.2, 1.8, 440.0), "pitch: a divider's square wave has no 2nd harmonic (re fundamental)",
               bin (a4, 0.2, 1.8, 880.0) / bin (a4, 0.2, 1.8, 440.0), 0.0);
    }
    {
        // octaves are exact even when the oscillators drift: C4 and C5 share one coil (unrelated oscillators drifting
        // 12 cents would be several cents apart; the measurement itself is good to about a tenth of a cent)
        Rig r;
        auto s = r.p.settings(); s.driftCents = 12.0f; r.p.setSettings (s);
        auto both = r.run (3.0, { { 0.0, on (60, 0.8f) }, { 0.0, on (72, 0.8f) } });
        const double lo = finePeak (both, 0.5, 2.9, 261.63), hi = finePeak (both, 0.5, 2.9, 523.25);
        check (std::abs (centsBetween (hi, 2.0 * lo)) < 0.2, "generators: drifting, C5 is still exactly C4's octave (cents off)", centsBetween (hi, 2.0 * lo), 0.0);
    }
    {
        // each footage at its pitch
        auto footage = [] (int which)
        {
            Rig r;
            auto s = r.p.settings();
            s.drawbar8 = 0.0f;
            if (which == 0) s.drawbar16 = 8.0f; else if (which == 1) s.drawbar4 = 8.0f; else s.drawbarIV = 8.0f;
            r.p.setSettings (s);
            return r.run (1.0, { { 0.0, on (60, 0.8f) } });
        };
        auto f16 = footage (0), f4 = footage (1), iv = footage (2);
        const double c3 = 130.81, c5 = 523.25;
        check (bin (f16, 0.2, 0.9, c3) > 10.0 * bin (f16, 0.2, 0.9, 261.63), "footages: 16' sounds C3 for C4", bin (f16, 0.2, 0.9, c3), 1.0);
        check (bin (f4, 0.2, 0.9, c5) > 10.0 * bin (f4, 0.2, 0.9, 261.63), "footages: 4' sounds C5 for C4", bin (f4, 0.2, 0.9, c5), 1.0);
        const double g5 = 783.99, c6 = 1046.5, e6 = 1318.51, c7 = 2093.0;
        const double floor = bin (iv, 0.2, 0.9, 261.63);
        check (bin (iv, 0.2, 0.9, g5) > 10.0 * floor && bin (iv, 0.2, 0.9, c6) > 10.0 * floor && bin (iv, 0.2, 0.9, e6) > 10.0 * floor
                   && bin (iv, 0.2, 0.9, c7) > 10.0 * floor, "footages: IV sounds G5, C6, E6 and C7 for C4", bin (iv, 0.2, 0.9, g5), 1.0);
    }
    {
        // the drawbars: none out is silence; both tones in is silence
        Rig r;
        auto s = r.p.settings(); s.flute = 0.0f; s.reed = 0.0f; r.p.setSettings (s);
        auto none = r.run (0.5, { { 0.0, on (60, 0.8f) } });
        check (rms (none, 0.1, 0.5) < 1.0e-6, "drawbars: both tones pushed in: silence", rms (none, 0.1, 0.5), 0.0);
        Rig r2;
        auto one = r2.run (0.5, { { 0.0, on (60, 0.8f) } });
        auto s2 = r2.p.settings(); s2.drawbar8 = 4.0f; r2.p.setSettings (s2);
        auto half = r2.run (0.5, {});
        check (std::abs (db (rms (half, 0.1, 0.5), rms (one, 0.1, 0.5)) + 12.0) < 0.5, "drawbars: 4 is 12 dB below 8 (3 dB a step)", db (rms (half, 0.1, 0.5), rms (one, 0.1, 0.5)), -12.0);
    }
    {
        // reed and flute: the flute tone is the reed tone filtered further, near a sine
        auto tone = [] (bool flute)
        {
            Rig r;
            auto s = r.p.settings(); s.flute = flute ? 8.0f : 0.0f; s.reed = flute ? 0.0f : 8.0f; r.p.setSettings (s);
            auto v = r.run (1.0, { { 0.0, on (60, 0.8f) } });
            return db (bin (v, 0.2, 0.9, 784.88), bin (v, 0.2, 0.9, 261.63));
        };
        const double reed = tone (false), flute = tone (true);
        check (reed > flute + 10.0 && flute < -28.0, "tones: the 3rd harmonic, reed vs flute (dB re fundamental)", reed, flute);
    }
    {
        // the key contacts
        organ::Settings s;
        s.spreadMs = 4.0f; s.bounceMs = 1.0f;
        double first = 1.0, last = 0.0;
        for (int c = 0; c < 4; ++c) { const double d = organ::contactDelay (s, 1234u, 60, c, false); first = std::min (first, d); last = std::max (last, d); }
        check (last - first > 0.001 && last <= 0.004, "contacts: a key's four contacts close apart, within the spread (ms)", (last - first) * 1000.0, 4.0);
        int bouncing = 0;
        bool allClosed = true;
        for (int key = 48; key < 72; ++key)
        {
            int changes = 0;
            bool was = false;
            for (double t = 0.0; t < 0.006; t += 0.00005)
            {
                const bool now = organ::contactClosed (s, 1234u, key, 1, t, -1.0);
                if (now != was) ++changes;
                was = now;
            }
            if (changes > 2) ++bouncing;
            allClosed = allClosed && was;
        }
        check (bouncing > 8 && allClosed, "contacts: they bounce as they close, then stay closed (keys of 24 seen bouncing)", bouncing, 12.0);
        s.spreadMs = 0.0f; s.bounceMs = 0.0f;
        bool all = true;
        for (int c = 0; c < 4; ++c) all = all && organ::contactClosed (s, 1234u, 60, c, 0.0, -1.0) && ! organ::contactClosed (s, 1234u, 60, c, 0.5, 0.0001);
        check (all, "contacts: with no spread or bounce, all four switch at once, on and off", all, 1);
    }
    {
        // keys switch tones straight on: the note is at full level within a millisecond, and gone as fast
        Rig r;
        auto v = r.run (0.5, { { 0.1, on (60, 0.8f) }, { 0.3, off (60) } });
        check (rms (v, 0.101, 0.106) > 0.5 * rms (v, 0.2, 0.25) && rms (v, 0.302, 0.4) < 1.0e-5,
               "keys: on at once (as built), off at once", rms (v, 0.101, 0.106) / rms (v, 0.2, 0.25), 1.0);
        Rig r2;
        auto chord = r2.run (0.5, { { 0.0, on (48, 0.8f) }, { 0.0, on (52, 0.8f) }, { 0.0, on (55, 0.8f) }, { 0.0, on (60, 0.8f) },
                                    { 0.0, on (64, 0.8f) }, { 0.0, on (67, 0.8f) }, { 0.0, on (72, 0.8f) }, { 0.0, on (76, 0.8f) } });
        check (bin (chord, 0.1, 0.45, 329.63) > 0.3 * bin (chord, 0.1, 0.45, 261.63) && bin (chord, 0.1, 0.45, 659.26) > 0.3 * bin (chord, 0.1, 0.45, 261.63),
               "keys: polyphonic: every note of an 8-note chord sounds", bin (chord, 0.1, 0.45, 659.26) / bin (chord, 0.1, 0.45, 261.63), 1.0);
        Rig r3;
        auto loud = r3.run (0.5, { { 0.0, on (60, 1.0f) } });
        Rig r4;
        auto soft = r4.run (0.5, { { 0.0, on (60, 0.1f) } });
        check (std::abs (db (rms (soft, 0.1, 0.4), rms (loud, 0.1, 0.4))) < 0.1, "keys: no velocity: soft and hard the same (dB)", db (rms (soft, 0.1, 0.4), rms (loud, 0.1, 0.4)), 0.0);
    }
    {
        // the level: a chord on the panel as it comes peaks at -15 dBFS out of the organ, which the track's
        // mono sum (+3 dB for a sound the same on both sides) puts at 0 on the meters (-12 dBFS)
        // (its dividers start in their own phases each time, so the peak varies a little: the middle of eight)
        std::vector<double> peaks;
        for (int run = 0; run < 8; ++run)
        {
            organ::Processor p;
            p.setRateAndBufferSizeDetails (sr, bs);
            p.prepareToPlay (sr, bs);
            std::vector<float> out;
            juce::AudioBuffer<float> b (2, bs);
            for (int k = 0; k < (int) (1.0 * sr / bs); ++k)
            {
                juce::MidiBuffer m;
                if (k == 0) for (int n : { 60, 64, 67, 72 }) m.addEvent (on (n, 0.8f), 0);
                p.processBlock (b, m);
                out.insert (out.end(), b.getReadPointer (0), b.getReadPointer (0) + bs);
            }
            float peak = 0.0f;
            for (size_t i = (size_t) (0.1 * sr); i < out.size(); ++i) peak = std::max (peak, std::abs (out[i]));
            peaks.push_back (20.0 * std::log10 (peak));
        }
        std::sort (peaks.begin(), peaks.end());
        std::printf ("      organ chord peaks: %.1f .. %.1f dBFS\n", peaks.front(), peaks.back());
        const double pk = 0.5 * (peaks[3] + peaks[4]);
        check (std::abs (pk + 15.0) < 1.5, "level: a four-note chord as it comes peaks at -15 dBFS (0 on the track meter)", pk, -15.0);
    }
    {
        // the vibrato: all twelve oscillators together; its pitch swing follows DEPTH
        auto swing = [] (bool on_)
        {
            Rig r;
            auto s = r.p.settings(); s.vibrato = on_; s.vibratoCents = 18.0f; s.vibratoHz = 6.4f; r.p.setSettings (s);
            auto v = r.run (1.0, { { 0.0, on (69, 0.8f) } });
            // the pitch from rising zero crossings, 20 ms at a time
            double lo = 1.0e9, hi = 0.0;
            for (double t = 0.2; t < 0.9; t += 0.02)
            {
                const auto a = (size_t) (t * sr), b = (size_t) ((t + 0.02) * sr);
                double first = -1, last = -1; int n = 0;
                for (auto i = a + 1; i < b; ++i)
                    if (v[i - 1] < 0.0f && v[i] >= 0.0f)
                    {
                        const double x = (double) (i - 1) + v[i - 1] / (v[i - 1] - v[i]);
                        if (first < 0) first = x; else ++n;
                        last = x;
                    }
                if (n > 0) { const double hz = n * sr / (last - first); lo = std::min (lo, hz); hi = std::max (hi, hz); }
            }
            return centsBetween (hi, lo);
        };
        const double with = swing (true), without = swing (false);
        check (with > 20.0 && with < 40.0 && without < 1.0, "vibrato: pitch swing with and without (cents, top to bottom)", with, without);
        check (std::abs (organ::vibratoShape (0.0, 6.4) + 1.0) < 1.0e-9 && organ::vibratoShape (0.07, 6.4) > 0.9,
               "vibrato: a square wave with rounded corners (-1 at the start, near +1 by the half cycle)", organ::vibratoShape (0.07, 6.4), 1.0);
    }
    {
        // drift wanders; tuning up puts it back
        Rig r;
        auto s = r.p.settings(); s.driftCents = 10.0f; r.p.setSettings (s);
        r.run (5.0, {});
        double moved = 0.0;
        for (auto d : r.p.driftNow()) moved = std::max (moved, std::abs (d));
        r.p.tuneUp();
        auto s0 = r.p.settings(); s0.driftCents = 0.0f; r.p.setSettings (s0);
        r.run (0.05, {});
        double after = 0.0;
        for (auto d : r.p.driftNow()) after = std::max (after, std::abs (d));
        check (moved > 0.5 && after < 1.0e-6, "drift: the oscillators wander (cents), and tuning up zeroes them", moved, after);
    }
    {
        // the tuning systems a divide-down organ can have
        organ::Processor p;
        auto* sys = p.param ("system");
        sys->setValueNotifyingHost (sys->convertTo0to1 (4.0f));   // Werckmeister III in C
        const auto osc = p.oscillatorHz();
        const double e = centsBetween (osc[4], osc[0]);
        check (std::abs (e - 390.225) < 0.01, "tuning: Werckmeister III, E above C (cents)", e, 390.225);
        tuning::Scale five; std::string err;
        tuning::parseScala ("! x\nFive\n5\n200.\n400.\n700.\n900.\n2/1\n", five, err);
        const bool refused = ! p.setScale (five, "", err);
        tuning::Scale twelve;
        std::string text = "! y\nTwelve\n12\n";
        for (int i = 1; i < 12; ++i) text += juce::String (i * 100.0 + (i == 4 ? -14.0 : 0.0), 1).toStdString() + "\n";
        text += "2/1\n";
        tuning::parseScala (text, twelve, err);
        const bool taken = p.setScale (twelve, text, err);
        sys->setValueNotifyingHost (sys->convertTo0to1 (8.0f));
        const auto sc = p.oscillatorHz();
        check (refused && taken && std::abs (centsBetween (sc[4], sc[0]) - 386.0) < 0.01, "tuning: Scala: 5 notes refused, 12 taken (E above C, cents)",
               centsBetween (sc[4], sc[0]), 386.0);
    }
    {
        // the state, and presets
        organ::Processor a;
        auto s = a.settings(); s.drawbar16 = 2.0f; s.reed = 5.0f; s.vibratoHz = 7.0f; a.setSettings (s);
        juce::MemoryBlock mb;
        a.getStateInformation (mb);
        organ::Processor b;
        b.setStateInformation (mb.getData(), (int) mb.getSize());
        check (std::abs (b.settings().drawbar16 - 2.0f) < 1.0e-4f && std::abs (b.settings().vibratoHz - 7.0f) < 1.0e-4f && b.seed() == a.seed(),
               "state: the drawbars, the case's settings and the seed come back", b.settings().drawbar16, 2.0);
        const juce::String name = "TF organ test " + juce::String::toHexString (juce::Random::getSystemRandom().nextInt());
        juce::String error;
        const bool saved = presets::save (a, name, error);
        organ::Processor c;
        const bool loaded = presets::load (c, presets::fileFor (organ::presetKind(), name), error);
        bool listed = false;
        for (auto& e : presets::list (organ::presetKind())) listed = listed || (e.name == name && e.summary.startsWith ("2853"));
        check (saved && loaded && listed && std::abs (c.settings().reed - 5.0f) < 1.0e-4f && c.currentPreset() == name && ! c.isModified(),
               "presets: saved, listed with its drawbars, loaded whole", c.settings().reed, 5.0);
        auto t = c.settings(); t.flute = 1.0f; c.setSettings (t);
        check (c.isModified(), "presets: a drawbar moved marks it changed", c.isModified(), 1);
        presets::remove (c, error);
        check (! presets::fileFor (organ::presetKind(), name).existsAsFile(), "presets: deleted", 0, 0);
    }

    if (auto* dir = std::getenv ("TF_ORGAN_RENDER"))
    {
        juce::File folder { juce::String (dir) };
        folder.createDirectory();
        auto play = [&] (const char* file, organ::Settings s)
        {
            Rig r;
            s.driftCents = 3.0f; s.bounceMs = 1.0f; s.spreadMs = 3.0f;
            r.p.setSettings (s);
            std::vector<std::pair<double, juce::MidiMessage>> e;
            const int chords[4][4] { { 57, 60, 64, 69 }, { 55, 59, 62, 67 }, { 53, 57, 60, 65 }, { 52, 56, 59, 64 } };
            for (int c = 0; c < 4; ++c)
                for (int n : chords[c]) { e.push_back ({ c * 1.0, on (n, 0.8f) }); e.push_back ({ c * 1.0 + 0.9, off (n) }); }
            for (int i = 0; i < 12; ++i) { e.push_back ({ 4.2 + i * 0.15, on (69 + (i * 7) % 12, 0.8f) }); e.push_back ({ 4.33 + i * 0.15, off (69 + (i * 7) % 12) }); }
            std::sort (e.begin(), e.end(), [] (auto& x, auto& y) { return x.first < y.first; });
            writeWav (folder.getChildFile (file), r.run (6.5, e));
        };
        organ::Settings full;   // the defaults: 16' 8' 4' IV at 6 8 5 3, flute 4, reed 8, vibrato on
        play ("organ-defaults.wav", full);
        organ::Settings reedOnly = full; reedOnly.flute = 0.0f; reedOnly.drawbar16 = 0.0f; reedOnly.drawbarIV = 0.0f;
        play ("organ-8-4-reed.wav", reedOnly);
        organ::Settings fluteOnly = full; fluteOnly.reed = 0.0f; fluteOnly.flute = 8.0f; fluteOnly.vibrato = false;
        play ("organ-flute.wav", fluteOnly);
        organ::Settings bright = full; bright.drawbarIV = 8.0f; bright.drawbar16 = 8.0f; bright.drawbar4 = 8.0f;
        play ("organ-everything.wav", bright);
        std::printf ("renders written to %s\n", dir);
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
