// TF ELECTRIC PIANO (Source/ElectricPiano.h) and the tuning systems (Source/Tuning.h).
//
//   Tuning: the historical systems against their published tables, just ratios, EDO, Scala
//   files and keyboard maps, per-note cents, A4 and stretch.
//   Piano: pitch, ring time by register, the bark growing with velocity, dampers, let-off,
//   MPE off by default, ageing, and the state round trip.
//
// With TF_EP_RENDER=<folder> it also writes WAVs to listen to.

#include "../Source/ElectricPiano.h"
#include "../Source/Tuning.h"

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
        if (annotate != nullptr) std::printf ("::error::ElectricPianoTest: %s (got %.3f, want %.3f)\n", what, got, want);
    }
}

double centsBetween (double a, double b) { return 1200.0 * std::log2 (a / b); }

constexpr double sr = 48000.0;
constexpr int bs = 256;

struct Rig
{
    ep::Processor p;
    Rig()
    {
        p.setRateAndBufferSizeDetails (sr, bs);
        p.prepareToPlay (sr, bs);
        auto s = p.settings();
        s.tuningSpreadCents = 0.0f; s.shortRingers = 0; s.ages = false;
        s.vibrato = 0.0f; s.humDb = -120.0f; s.actionNoiseDb = -120.0f; s.damperNoiseDb = -120.0f; s.pedalNoiseDb = -120.0f;
        s.rearJack = true; s.drive = 0.0f;
        p.setSettings (s);
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

    // ---------------------------------------------------------------- tuning
    {
        using namespace tuning;
        auto o = octaveOf (System::werckmeister);
        check (std::abs (o[4] - 390.225) < 0.01 && std::abs (o[6] - 588.270) < 0.01 && std::abs (o[11] - 1092.180) < 0.01,
               "tuning: Werckmeister III (E 390.225, F# 588.270, B 1092.180)", o[4], 390.225);
        o = octaveOf (System::kirnberger);
        check (std::abs (o[4] - 386.314) < 0.01 && std::abs (o[6] - 590.224) < 0.01 && std::abs (o[9] - 889.735) < 0.01,
               "tuning: Kirnberger III (E 386.314, F# 590.224, A 889.735)", o[4], 386.314);
        o = octaveOf (System::vallotti);
        check (std::abs (o[4] - 392.180) < 0.01 && std::abs (o[1] - 94.135) < 0.01 && std::abs (o[10] - 1000.0) < 0.01,
               "tuning: Vallotti (E 392.180, C# 94.135, Bb 1000.000)", o[4], 392.180);
        o = octaveOf (System::meantone);
        check (std::abs (o[4] - 386.314) < 0.01 && std::abs (o[1] - 76.049) < 0.01 && std::abs (o[8] - 772.627) < 0.01,
               "tuning: quarter-comma meantone (E 386.314, C# 76.049, G# 772.627)", o[4], 386.314);
        o = octaveOf (System::pythagorean);
        check (std::abs (o[7] - 701.955) < 0.01 && std::abs (o[4] - 407.820) < 0.01, "tuning: Pythagorean (G 701.955, E 407.820)", o[4], 407.820);

        Settings s;
        s.system = System::just; s.key = 2;   // just in D: F# is a pure major third above D
        auto f = frequencies (s);
        check (std::abs (f[66] / f[62] - 1.25) < 1.0e-9 && std::abs (f[69] / f[62] - 1.5) < 1.0e-9 && std::abs (f[69] - 440.0) < 1.0e-9,
               "tuning: just in D (F#/D 5/4, A/D 3/2, A4 at 440)", f[66] / f[62], 1.25);

        s = {};
        s.system = System::edo; s.edoSteps = 19;
        f = frequencies (s);
        check (std::abs (centsBetween (f[70], f[69]) - 1200.0 / 19.0) < 1.0e-6 && std::abs (f[88] / f[69] - 2.0) < 1.0e-9,
               "tuning: 19-EDO (a step 63.158 cents, 19 keys an octave)", centsBetween (f[70], f[69]), 1200.0 / 19.0);

        s = {};
        s.a4Hz = 415.0;
        f = frequencies (s);
        check (std::abs (f[69] - 415.0) < 1.0e-9 && std::abs (f[57] - 207.5) < 1.0e-9, "tuning: A4 at 415 Hz", f[69], 415.0);

        s = {};
        s.stretchCentsPerOctave = 3.0;
        f = frequencies (s);
        check (std::abs (centsBetween (f[81], f[69]) - 1203.0) < 1.0e-6, "tuning: 3 cents of stretch per octave", centsBetween (f[81], f[69]), 1203.0);

        s = {};
        s.system = System::custom; s.customCents[4] = -14.0;   // E 14 cents flat
        f = frequencies (s);
        check (std::abs (centsBetween (f[64], 440.0 * std::pow (2.0, -5.0 / 12.0)) + 14.0) < 1.0e-6, "tuning: per-note cents (E -14)", centsBetween (f[64], 440.0 * std::pow (2.0, -5.0 / 12.0)), -14.0);

        Scale sc;
        std::string err;
        const bool ok = parseScala ("! test.scl\n!\nA pentatonic\n 5\n!\n 9/8\n 5/4\n 3/2\n 1700.0 ! nine-ish\n 2/1\n", sc, err);
        s = {};
        s.system = System::scala; s.scale = sc;
        f = frequencies (s);
        // default map: key 60 is degree 0, key 69 (degree 9: one period plus 4 degrees) sounds at 440
        const double c69 = 1200.0 + 1700.0 - 1200.0 + 0.0;   // degree 9 = period (1200) + degree 4 (1700)?  see below
        (void) c69;
        check (ok && sc.cents.size() == 5 && std::abs (sc.cents[0] - 203.910) < 0.01 && std::abs (f[69] - 440.0) < 1.0e-9
                  && std::abs (centsBetween (f[61], f[60]) - 203.910) < 0.01 && std::abs (f[65] / f[60] - 2.0) < 1.0e-9,
               "tuning: a Scala scale (ratios and cents; 5 keys a period)", centsBetween (f[61], f[60]), 203.910);

        KeyboardMap km;
        const bool okm = parseKeyboardMap ("! map\n12\n0\n127\n60\n69\n432.0\n5\n0\n1\nx\n2\n3\n4\nx\nx\nx\nx\nx\nx\n", km, err);
        s.keyboardMap = km;
        f = frequencies (s);
        check (okm && f[62] <= 0.0 && std::abs (centsBetween (f[61], f[60]) - 203.910) < 0.01 && std::abs (f[72] / f[60] - 2.0) < 1.0e-9,
               "tuning: a keyboard map (unmapped keys silent, a period every 12 keys)", f[72] / f[60], 2.0);
        Scale bad;
        check (! parseScala ("x\n3\n9/8\n", bad, err), "tuning: a short Scala file is refused", 1, 1);
    }

    // ---------------------------------------------------------------- the piano
    {
        Rig r;
        auto a4 = r.run (2.0, { { 0.0, on (69, 0.5f) } });
        const double pitch = peakNear (a4, 0.3, 1.5, 440.0);
        check (std::abs (centsBetween (pitch, 440.0)) < 1.0, "piano: A4 plays at 440 Hz (cents off)", centsBetween (pitch, 440.0), 0.0);
        check (rms (a4, 0.05, 0.3) > 1.0e-3, "piano: it makes a sound", rms (a4, 0.05, 0.3), 0.01);
    }
    {
        Rig r;
        auto low = r.run (6.0, { { 0.0, on (45, 0.6f) } });
        Rig r2;
        auto high = r2.run (6.0, { { 0.0, on (93, 0.6f) } });
        const double dropLow = 20.0 * std::log10 (rms (low, 3.0, 3.5) / rms (low, 0.2, 0.7));
        const double dropHigh = 20.0 * std::log10 (std::max (1.0e-9, rms (high, 3.0, 3.5)) / rms (high, 0.2, 0.7));
        check (dropLow > dropHigh + 10.0, "piano: low reeds ring longer than high ones (dB lost in 3 s, A2 vs A6)", dropLow, dropHigh);
    }
    {
        // the bark: harmonics grow faster than the fundamental with velocity
        auto harm = [] (float vel)
        {
            Rig r;
            auto v = r.run (1.0, { { 0.0, on (57, vel) } });
            const double f1 = bin (v, 0.05, 0.45, 220.0), f2 = bin (v, 0.05, 0.45, 440.0), f3 = bin (v, 0.05, 0.45, 660.0);
            return 20.0 * std::log10 ((f2 + f3) / f1);
        };
        const double soft = harm (0.15f), hard = harm (1.0f);
        check (hard > soft + 8.0, "piano: played hard, it barks (2nd+3rd re fundamental, dB, soft vs hard)", hard, soft);
    }
    {
        Rig r;
        auto held = r.run (2.0, { { 0.0, on (60, 0.6f) } });
        Rig r2;
        auto let = r2.run (2.0, { { 0.0, on (60, 0.6f) }, { 0.5, off (60) } });
        check (rms (let, 1.0, 1.5) < 0.05 * rms (held, 1.0, 1.5), "piano: the damper stops a released note", rms (let, 1.0, 1.5) / rms (held, 1.0, 1.5), 0.0);
        Rig r3;
        auto pedal = r3.run (2.0, { { 0.0, juce::MidiMessage::controllerEvent (1, 64, 127) }, { 0.01, on (60, 0.6f) }, { 0.5, off (60) } });
        check (rms (pedal, 1.0, 1.5) > 0.5 * rms (held, 1.0, 1.5), "piano: ...unless the sustain pedal holds the dampers off", rms (pedal, 1.0, 1.5) / rms (held, 1.0, 1.5), 1.0);
    }
    {
        Rig r;
        auto s = r.p.settings(); s.letOffInches = 0.25f; r.p.setSettings (s);
        auto ghost = r.run (1.0, { { 0.0, on (60, 0.09f) } });
        Rig r2;
        auto heard = r2.run (1.0, { { 0.0, on (60, 0.09f) } });
        Rig r3;
        auto nothing = r3.run (1.0, {});
        const double db = 20.0 * std::log10 (rms (ghost, 0.0, 1.0) / rms (nothing, 0.0, 1.0));
        const double heardDb = 20.0 * std::log10 (rms (heard, 0.0, 1.0) / rms (nothing, 0.0, 1.0));
        check (std::abs (db) < 1.0 && heardDb > 6.0, "piano: a big let-off: the softest notes don't reach the reed (dB over the hiss)", db, heardDb);
    }
    {
        // the hammer stays on for 3/4 to 1 cycle of the reed's pitch (Miessner)
        const double a1 = ep::contactSeconds (55.0, 1.0, 0.5), c4 = ep::contactSeconds (261.63, 0.0, 0.5);
        check (std::abs (a1 - 0.75 / 55.0) < 1.0e-6 && std::abs (c4 - 1.0 / 261.63) < 1.0e-6,
               "piano: hammer contact, A1 played hard and C4 played softly (ms)", a1 * 1000.0, 13.6);
        // the pickup: nothing at rest; it rises as the reed swings into the fingers, flattens when it fills
        // them, and falls back once it's through
        const auto s = ep::Processor().settings();
        double peak = 0.0, peakAt = 0.0;
        for (int i = 0; i <= 250; ++i)
            if (const double v = ep::pickup (s, -0.01 * i, 60); v > peak) { peak = v; peakAt = 0.01 * i; }
        const double atRest = ep::pickup (s, 0.0, 60), into = ep::pickup (s, -0.3, 60), through = ep::pickup (s, -2.2, 60), away = ep::pickup (s, 0.6, 60);
        check (std::abs (atRest) < 1.0e-12 && into > 0.3 * peak && peakAt > 0.5 && peakAt < 1.0 && through < 0.1 * peak && away < 0.1 * into,
               "piano: the pickup rises into the fingers, peaks as the reed fills them (mm), falls back through", peakAt, 0.75);
    }
    {
        // the network evens the keyboard out: the bass's big swing comes out about as loud as the middle
        auto level = [] (int key)
        {
            Rig r;
            auto v = r.run (0.6, { { 0.0, on (key, 0.8f) } });
            return 20.0 * std::log10 (rms (v, 0.1, 0.5));
        };
        const double a1 = level (33), c4 = level (60), c6 = level (84);
        check (std::abs (a1 - c4) < 6.0 && std::abs (c6 - c4) < 9.0, "piano: A1, C4 and C6 come out within a few dB (A1 re C4)", a1 - c4, 0.0);
        // and middle C stays clean until it's played hard: then it folds through its fingers
        auto third = [] (float vel)
        {
            Rig r;
            auto v = r.run (0.6, { { 0.0, on (60, vel) } });
            return 20.0 * std::log10 (bin (v, 0.1, 0.5, 784.88) / bin (v, 0.1, 0.5, 261.63));
        };
        const double mf = third (0.62f), ff = third (1.0f);
        check (mf < -20.0 && ff > -10.0, "piano: C4's third harmonic: quiet at velocity 80, strong at 127 (dB re fundamental)", mf, ff);
    }
    {
        // MPE is off as it comes: pressure changes nothing
        Rig r;
        auto s = r.p.settings(); s.pressureTarget = 1; r.p.setSettings (s);
        auto plain = r.run (1.0, { { 0.0, on (60, 0.6f) } });
        Rig r2;
        auto s2 = r2.p.settings(); s2.pressureTarget = 1; r2.p.setSettings (s2);
        auto pressed = r2.run (1.0, { { 0.0, on (60, 0.6f) }, { 0.2, juce::MidiMessage::channelPressureChange (1, 127) } });
        check (std::abs (rms (pressed, 0.5, 0.9) / rms (plain, 0.5, 0.9) - 1.0) < 0.01, "piano: MPE off (as it comes): pressure does nothing",
               rms (pressed, 0.5, 0.9) / rms (plain, 0.5, 0.9), 1.0);
        check (! ep::Processor().settings().mpe, "piano: MPE is off as it comes", 0, 0);
    }
    {
        Rig r;
        auto s = r.p.settings(); s.ages = true; r.p.setSettings (s);
        const double before = r.p.hoursPlayed();
        r.run (2.0, { { 0.0, on (60, 0.6f) } });
        check (r.p.hoursPlayed() > before + 1.5 / 3600.0, "piano: it ages while it plays (hours)", (r.p.hoursPlayed() - before) * 3600.0, 2.0);
        const auto young = ep::ageing (0.0), old = ep::ageing (2000.0);
        check (old.spreadCents > young.spreadCents + 5.0 && old.felt > young.felt && old.shortRingers > 0,
               "piano: 2000 hours drift the reeds, harden the felt, make short ringers", old.spreadCents, 8.9);
    }
    {
        // the state: parameters, age, seed and the Scala file come back
        ep::Processor a;
        auto s = a.settings(); s.felt = 0.8f; s.model = 0; a.setSettings (s);
        tuning::Scale sc; std::string err;
        const std::string text = "! x\nThree\n3\n400.0\n800.0\n1200.0\n";
        tuning::parseScala (text, sc, err);
        a.setScale (sc, text);
        a.param ("system")->setValueNotifyingHost (a.param ("system")->convertTo0to1 ((float) tuning::System::scala));
        juce::MemoryBlock mb;
        a.getStateInformation (mb);
        ep::Processor b;
        b.setStateInformation (mb.getData(), (int) mb.getSize());
        const auto t = b.tuningSettings();
        check (std::abs (b.settings().felt - 0.8f) < 1.0e-4f && b.settings().model == 0 && b.seed() == a.seed() && t.system == tuning::System::scala
                   && t.scale.cents.size() == 3, "piano: its state comes back (controls, seed, Scala file)", b.settings().felt, 0.8);
    }

    {
        // presets: saved and loaded whole, the scale with them; the * ignores playing time
        const juce::String name = "TF test " + juce::String::toHexString (juce::Random::getSystemRandom().nextInt());
        ep::Processor a;
        auto s = a.settings(); s.felt = 0.9f; s.model = 0; a.setSettings (s);
        tuning::Scale sc; std::string err;
        const std::string text = "! x\nThree\n3\n400.0\n800.0\n1200.0\n";
        tuning::parseScala (text, sc, err);
        a.setScale (sc, text);
        a.param ("system")->setValueNotifyingHost (a.param ("system")->convertTo0to1 ((float) tuning::System::scala));
        juce::String error;
        const bool saved = ep::presets::save (a, name, error);
        check (saved && ep::presets::fileFor (name).existsAsFile() && a.currentPreset() == name && ! a.isModified(),
               "presets: saving writes the file and names the piano", saved, 1);

        bool listed = false;
        for (auto& e : ep::presets::list()) listed = listed || (e.name == name && e.summary.startsWith ("200"));
        check (listed, "presets: the list has it, with its summary", listed, 1);

        // a piano with no scale and different controls loads it whole
        ep::Processor b;
        auto x = b.toXml (true);
        x->setAttribute ("hours", 250.0);
        b.fromXml (*x);
        const double hoursBefore = b.hoursPlayed();
        const bool loaded = ep::presets::load (b, ep::presets::fileFor (name), error);
        check (loaded && std::abs (b.settings().felt - 0.9f) < 1.0e-4f && b.settings().model == 0 && b.seed() == a.seed()
                   && b.tuningSettings().scale.cents.size() == 3 && b.currentPreset() == name && ! b.isModified(),
               "presets: loading brings back every control, the seed and the Scala file", b.settings().felt, 0.9);
        check (std::abs (b.hoursPlayed() - a.hoursPlayed()) < 1.0e-9 && std::abs (hoursBefore - 250.0) < 1.0e-9,
               "presets: loading brings back its age", b.hoursPlayed(), a.hoursPlayed());

        // playing time alone doesn't make it modified; a control does
        auto y = b.toXml (true);
        y->setAttribute ("hours", b.hoursPlayed() + 40.0);
        const auto keep = b.currentPreset();
        b.fromXml (*y);
        check (! b.isModified(), "presets: playing time doesn't mark it changed", b.isModified(), 0);
        b.param ("felt")->setValueNotifyingHost (b.param ("felt")->convertTo0to1 (0.2f));
        check (b.isModified(), "presets: a control does", b.isModified(), 1);

        // and the name and the * survive the deck's saving
        juce::MemoryBlock mb;
        b.getStateInformation (mb);
        ep::Processor c;
        c.setStateInformation (mb.getData(), (int) mb.getSize());
        check (c.currentPreset() == keep && c.isModified(), "presets: the name and the * are kept with the deck", c.isModified(), 1);

        // a preset without a scale clears the one the piano had
        ep::Processor plain;
        const juce::String plainName = name + " plain";
        ep::presets::save (plain, plainName, error);
        ep::presets::load (c, ep::presets::fileFor (plainName), error);
        check (c.tuningSettings().scale.cents.empty() && c.scaleDescription() == plain.scaleDescription(),
               "presets: one with no Scala file clears the piano's", (double) c.tuningSettings().scale.cents.size(), 0);

        // rename keeps what's saved; delete removes the file
        const juce::String renamed = name + " renamed";
        const bool r = ep::presets::rename (c, renamed, error);
        check (r && ! ep::presets::fileFor (plainName).existsAsFile() && ep::presets::fileFor (renamed).existsAsFile() && c.currentPreset() == renamed,
               "presets: renaming moves the file", r, 1);
        const bool d = ep::presets::remove (c, error);
        check (d && ! ep::presets::fileFor (renamed).existsAsFile() && c.currentPreset().isEmpty(), "presets: deleting removes it", d, 1);
        ep::presets::fileFor (name).deleteFile();
    }

    // ---------------------------------------------------------------- listening
    if (auto* dir = std::getenv ("TF_EP_RENDER"))
    {
        juce::File folder { juce::String (dir) };
        folder.createDirectory();
        auto phrase = [] (Rig& r, float vel)
        {
            std::vector<std::pair<double, juce::MidiMessage>> e;
            const int chords[4][4] { { 53, 57, 60, 64 }, { 50, 57, 60, 65 }, { 55, 59, 62, 65 }, { 48, 55, 60, 64 } };
            for (int c = 0; c < 4; ++c)
                for (int n : chords[c]) { e.push_back ({ c * 1.2, on (n, vel) }); e.push_back ({ c * 1.2 + 1.1, off (n) }); }
            for (int i = 0; i < 8; ++i) { e.push_back ({ 5.0 + i * 0.25, on (72 + (i * 5) % 12, vel) }); e.push_back ({ 5.2 + i * 0.25, off (72 + (i * 5) % 12) }); }
            std::sort (e.begin(), e.end(), [] (auto& x, auto& y) { return x.first < y.first; });
            return r.run (8.0, e);
        };
        for (int model = 0; model < 2; ++model)
            for (float vel : { 0.35f, 0.95f })
                for (bool jack : { true, false })
                {
                    Rig r;
                    auto s = r.p.settings();
                    s.model = model; s.rearJack = jack; s.vibrato = 0.4f; s.humDb = -78.0f; s.actionNoiseDb = -50.0f; s.damperNoiseDb = -56.0f;
                    r.p.setSettings (s);
                    writeWav (folder.getChildFile (juce::String (model == 0 ? "200" : "200A") + (vel < 0.5f ? "-soft" : "-hard") + (jack ? "-jack" : "-speakers") + ".wav"),
                              phrase (r, vel));
                }
        // a velocity ladder: C2, C4, C6 at MIDI velocity 40, 64, 90, 110, 127, a second each
        for (int model = 0; model < 2; ++model)
        {
            Rig r;
            auto s = r.p.settings();
            s.model = model; s.rearJack = true; s.vibrato = 0.0f; s.humDb = -78.0f;
            r.p.setSettings (s);
            std::vector<std::pair<double, juce::MidiMessage>> e;
            double t = 0.0;
            for (int key : { 36, 60, 84 })
                for (int vel : { 40, 64, 90, 110, 127 })
                {
                    e.push_back ({ t, on (key, (float) vel / 127.0f) });
                    e.push_back ({ t + 0.9, off (key) });
                    t += 1.1;
                }
            writeWav (folder.getChildFile (juce::String ("ladder-") + (model == 0 ? "200" : "200A") + ".wav"), r.run (t + 0.5, e));
        }
        std::printf ("renders written to %s\n", dir);
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
