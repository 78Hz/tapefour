// Headless checks of the tape engine: record real signals, play them back, measure.
#include <cstdlib>
#include "../Source/TapeEngine.h"
#include "../Source/Wear.h"
#include <cstdio>
#include <complex>

using namespace tf;

namespace
{
constexpr double sr = 48000.0;
constexpr int bs = 256;
int failures = 0;

void check (bool ok, const char* what, double got, double want)
{
    std::printf ("%s  %-58s got %10.4f  want %10.4f\n", ok ? "PASS" : "FAIL", what, got, want);
    if (! ok) ++failures;
    // On GitHub, a failure also shows on the run's page.
    if (! ok && std::getenv ("GITHUB_ACTIONS") != nullptr)
        std::printf ("::error::EngineTest: %s (got %.4f, want %.4f)\n", what, got, want);
}

struct Rig
{
    Tape tape;
    TapeEngine eng;
    double phase = 0;

    // A perfect machine and a perfect cassette, so each test hears only what it measures.
    explicit Rig (int model = defaultTapeModel) : tape (1, model, tfmt::Condition())
    {
        eng.prepare (sr, bs);
        eng.params.sat = 0.5f;
        eng.params.nrOn = false;
        auto& m = eng.machine;
        m.wowPct = 0; m.flutterPct = 0; m.driftPct = 0; m.cutoffHz = 20000.0f; m.crosstalkDb = -120.0f;
        eng.setHissForTests (false);
        eng.setTape (&tape);
    }

    // Runs the transport for `seconds`; returns what track `listen` played back.
    std::vector<float> run (double seconds, TapeEngine::Command cmd, bool armed0, double freq, float amp, int listen = 0)
    {
        eng.command (cmd);
        bool armed[numTracks] { armed0, false, false, false };
        std::vector<float> in ((size_t) bs), captured;
        std::vector<std::vector<float>> outs (numTracks, std::vector<float> ((size_t) bs));
        float* outPtrs[numTracks] { outs[0].data(), outs[1].data(), outs[2].data(), outs[3].data() };
        auto blocks = (int) (seconds * sr / bs);
        for (int b = 0; b < blocks; ++b)
        {
            for (auto& v : in) { v = amp * (float) std::sin (phase); phase += juce::MathConstants<double>::twoPi * freq / sr; }
            eng.beginBlock (bs, armed);
            eng.readAll (outPtrs, bs);
            if (eng.isRecordingNow()) eng.writeTrack (0, in.data(), bs, 0.0);
            eng.endBlock();
            captured.insert (captured.end(), outs[(size_t) listen].begin(), outs[(size_t) listen].end());
        }
        return captured;
    }

    void rewindTo (double cells)
    {
        eng.command (TapeEngine::Command::stop);
        bool armed[numTracks] {};
        eng.beginBlock (1, armed); eng.endBlock();
        eng.setTape (nullptr);
        tape.position = cells;
        eng.setTape (&tape, false);
    }
};

double rms (const std::vector<float>& v, double from, double to)
{
    auto a = (size_t) (from * sr), b = std::min (v.size(), (size_t) (to * sr));
    double s = 0; for (auto i = a; i < b; ++i) s += (double) v[i] * v[i];
    return std::sqrt (s / std::max<double> (1, (double) (b - a)));
}

double freq (const std::vector<float>& v, double from, double to)
{
    auto a = (size_t) (from * sr), b = std::min (v.size(), (size_t) (to * sr));
    int crossings = 0;
    for (auto i = a + 1; i < b; ++i) if (v[i - 1] < 0 && v[i] >= 0) ++crossings;
    return crossings / ((double) (b - a) / sr);
}

double db (double x) { return 20.0 * std::log10 (std::max (1e-9, x)); }

// amplitude of one frequency in [from, to) seconds (Hann window)
double tone (const std::vector<float>& v, double hz, double from, double to)
{
    auto a = (size_t) (from * sr), b = std::min (v.size(), (size_t) (to * sr));
    double re = 0, im = 0, wsum = 0;
    for (auto i = a; i < b; ++i)
    {
        auto w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) (i - a) / (double) (b - a));
        auto ph = juce::MathConstants<double>::twoPi * hz * (double) i / sr;
        re += w * v[i] * std::cos (ph); im += w * v[i] * std::sin (ph);
        wsum += w;
    }
    return 2.0 * std::sqrt (re * re + im * im) / wsum;
}

// A-weighted RMS, written out from IEC 61672 pole frequencies independently of the engine's own.
double aWeighted (const std::vector<float>& v, double from, double to)
{
    auto f = [] (double hz) { return std::exp (-juce::MathConstants<double>::twoPi * hz / sr); };
    // four poles at 20.6 Hz (2), 107.7, 737.9 as one-pole high-passes; two at 12194 Hz as one-pole low-passes
    double hpA[4] { f (20.6), f (20.6), f (107.7), f (737.9) }, lpA = f (12194.0);
    double xs[4] {}, ys[4] {}, l1 = 0, l2 = 0;
    auto run = [&] (double x)
    {
        for (int k = 0; k < 4; ++k) { auto y = hpA[k] * (ys[k] + x - xs[k]); xs[k] = x; ys[k] = y; x = y; }
        l1 = x * (1 - lpA) + l1 * lpA; l2 = l1 * (1 - lpA) + l2 * lpA;
        return l2;
    };
    // gain at 1 kHz, to normalise
    double g1 = 0; { double s2 = 0; for (int i = 0; i < (int) sr * 2; ++i) { auto y = run (std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / sr)); if (i > sr) s2 += y * y; } g1 = std::sqrt (s2 / sr) * std::sqrt (2.0); }
    for (auto& x : xs) x = 0;
    for (auto& y : ys) y = 0;
    l1 = l2 = 0;
    auto a = (size_t) (from * sr), b = std::min (v.size(), (size_t) (to * sr));
    double s2 = 0; size_t n = 0;
    for (size_t i = 0; i < b; ++i) { auto y = run (v[i]); if (i >= a) { s2 += y * y; ++n; } }
    return std::sqrt (s2 / (double) std::max<size_t> (1, n)) / g1;
}
} // namespace

// The tape's whole character, recorded and played: twenty numbers that change if any of it does.
std::vector<double> characterSignature (Rig& r)
{
    r.run (1.0, TapeEngine::Command::rec, true, 1000.0, tapeRefPeak);
    r.rewindTo (0);
    std::vector<double> sig;
    auto own = r.run (1.0, TapeEngine::Command::play, false, 0, 0, 0);
    r.rewindTo (0);
    auto next = r.run (1.0, TapeEngine::Command::play, false, 0, 0, 1);
    for (auto* v : { &own, &next })
        for (int k = 0; k < 10; ++k)
        {
            double e = 0;
            for (size_t i = (size_t) (k * 4800); i < (size_t) ((k + 1) * 4800) && i < v->size(); ++i)
                e += (double) (*v)[i] * (*v)[i] * (double) (1 + (int) (i % 7));
            sig.push_back (e);
        }
    return sig;
}

// TAPEFOUR_WEAR_RENDER=<folder>: one take rendered through wear and age, for tuning Wear.h by ear.
static void renderWear (const juce::File& folder)
{
    folder.createDirectory();
    auto write = [&] (const juce::String& name, const std::vector<float>& v)
    {
        juce::WavAudioFormat wav;
        auto out = folder.getChildFile (name + ".wav");
        out.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (out.createOutputStream());
        if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (1).withBitsPerSample (24)))
        {
            const float* ch[] { v.data() };
            writer->writeFromFloatArrays (ch, 1, (int) v.size());
        }
        std::printf ("wrote %s\n", out.getFullPathName().toRawUTF8());
    };
    // A take: plucked notes with silence between (where print-through shows), then a chord.
    auto take = [] (Rig& r)
    {
        const double notes[] { 220.0, 277.2, 329.6, 440.0 };
        for (int k = 0; k < 4; ++k)
            for (int64_t c = 0; c < cellRate; ++c)
            {
                auto t = (double) c / cellRate;
                auto v = 0.5f * (float) (std::exp (-4.0 * t) * std::sin (juce::MathConstants<double>::twoPi * notes[k] * t));
                r.tape.writeCell (0, (int64_t) (cellRate * (1.0 + 1.5 * k)) + c, v, true, 0, 1.0f, 0);
            }
        for (int64_t c = 0; c < cellRate * 3; ++c)
        {
            auto t = (double) c / cellRate;
            float v = 0;
            for (auto f : notes) v += 0.12f * (float) std::sin (juce::MathConstants<double>::twoPi * f * t);
            r.tape.writeCell (0, (int64_t) (cellRate * 8.0) + c, v, true, 0, 1.0f, 0);
        }
    };
    auto play = [] (Rig& r)
    {
        r.eng.setHissForTests (true);
        r.eng.params.nrOn = true;
        r.rewindTo (0);
        return r.run (12.0, TapeEngine::Command::play, false, 0, 0);
    };
    for (int passes : { 0, 10, 100, 1000 })
    {
        Rig r;
        take (r);
        r.tape.setPassesForTests ((uint16_t) passes);
        write ("wear-" + juce::String (passes) + "-passes", play (r));
    }
    for (int years : { 1, 5, 20 })
    {
        Rig r;
        take (r);
        r.tape.setToday ((uint16_t) (r.tape.getToday() + years * 365));
        write ("age-" + juce::String (years) + "-years", play (r));
    }
}

int main()
{
    if (auto* dir = std::getenv ("TAPEFOUR_WEAR_RENDER"))
    {
        renderWear (juce::File (juce::String (dir)));
        return 0;
    }
    {   // 0. The split into cassette and machine changed nothing it shouldn't have. These numbers were
        //    recorded and played by the engine before the split, with its old defaults (hiss 0.55, wow 0.30,
        //    flutter 0.25, drive 0.5, HF loss 0.20, crosstalk 0.30, drift 0.20, NR on, no dropouts). The same
        //    settings, as they convert, must give the same sound.
        static const double before[] { 457.5322862, 492.060268, 492.0634275, 492.4476368, 493.1673327, 491.6123144,
                                       491.6096707, 490.965613, 491.3987192, 463.57769, 7.865097508e-09, 7.598422481e-09,
                                       8.935458353e-09, 1.080143634e-08, 6.399276531e-09, 8.220858832e-09, 8.95409076e-09,
                                       6.984399025e-09, 1.011320478e-08, 8.652631648e-09 };
        Rig r;
        tfmt::Condition c;
        c.hissDb = (0.55 - 0.5) * 24.0;   // the old HISS control
        r.tape.setCondition (c);
        r.eng.setHissForTests (true);
        auto& m = r.eng.machine;
        m.wowPct = 0.30f * 0.5f; m.flutterPct = 0.25f * 0.3f; m.driftPct = 0.20f;
        m.cutoffHz = (float) (20000.0 * std::pow (0.25, 0.20)); m.crosstalkDb = -80.0f + 0.30f * 50.0f;
        r.eng.params.nrOn = true;
        auto now = characterSignature (r);
        double worst = 0;
        for (size_t k = 0; k < now.size(); ++k)
            worst = std::max (worst, std::abs (now[k] - before[k]) / before[k]);
        check (worst < 1.0e-4, "converted settings sound as before the split (worst relative change)", worst, 0.0);
    }

    {   // 0b. A tape saved before the cassette format is converted on loading, and sounds the same.
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tf_legacy_test");
        dir.deleteRecursively();
        dir.createDirectory();
        // Its track 1: a second of tone, as the old deck stored it (16-bit, full-scale flux).
        juce::MemoryBlock pcm;
        for (int c = 0; c < cellRate; ++c)
        {
            auto v = (int16_t) juce::roundToInt (8000.0 * std::sin (juce::MathConstants<double>::twoPi * 500.0 * c / cellRate));
            pcm.append (&v, sizeof (v));
        }
        dir.getChildFile ("track1.pcm").replaceWithData (pcm.getData(), pcm.getSize());
        juce::MemoryBlock flags, speeds;
        for (int b = 0; b < 10; ++b) { uint8_t f = 1 | 2 | (1 << 2); flags.append (&f, 1); uint16_t sp = 1000; speeds.append (&sp, 2); }
        juce::XmlElement x ("TAPEFOUR_TAPE");
        x.setAttribute ("version", 2); x.setAttribute ("id", "legacy-tape"); x.setAttribute ("name", "four track demos");
        x.setAttribute ("created", "24 OCT 2026"); x.setAttribute ("type", 2); x.setAttribute ("model", 2);
        x.setAttribute ("fluxScale", 1.0); x.setAttribute ("counterZero", 64000.0); x.setAttribute ("position", 1000.0);
        auto* sheet = x.createNewChildElement ("TRACK_SHEET");
        sheet->setAttribute ("layout", "inlay1");
        sheet->createNewChildElement ("LINE")->setAttribute ("text", "DEMOS");
        auto* tr = x.createNewChildElement ("TRACK");
        tr->setAttribute ("index", 0); tr->setAttribute ("flags", flags.toBase64Encoding()); tr->setAttribute ("speeds", speeds.toBase64Encoding());
        x.writeTo (dir.getChildFile ("tape.xml"));
        juce::XmlElement deck ("DECK");
        deck.setAttribute ("version", 4);
        auto* td = deck.createNewChildElement ("TAPE_DECK");
        td->setAttribute ("hiss", 0.75); td->setAttribute ("drop", 0.35); td->setAttribute ("speed", 1);
        deck.writeTo (dir.getChildFile ("deck.xml"));

        auto t = Tape::load (dir);
        bool audioSame = t != nullptr;
        for (int c = 0; audioSame && c < cellRate; c += 13)
            audioSame = std::abs (t->readCell (0, c) - 8000.0f * (float) std::sin (juce::MathConstants<double>::twoPi * 500.0 * c / cellRate) / 32767.0f) < 2.0e-4f;
        check (audioSame, "legacy: the recording loads unchanged", audioSame ? 1 : 0, 1);
        check (t != nullptr && t->getModel() == 2 && t->getTypeInfo().minutes == 45 && t->name == "four track demos" && t->trackSheet[0] == "DEMOS"
                   && t->createdIso == "2026-10-24T00:00:00Z" && t->isNrEncoded (0, 3) && ! t->isRecorded (0, 11),
               "legacy: formulation, length, label, inlay, date and maps carry over", 1, 1);
        check (t != nullptr && std::abs (t->getCondition().hissDb - 6.0) < 1e-6 && std::abs (t->getCondition().dropoutsPerMinute - 14.7) < 1e-6
                   && t->getCondition().grade == "chewed",
               "legacy: its old hiss and dropouts become the cassette's condition", t != nullptr ? t->getCondition().dropoutsPerMinute : -1, 14.7);
        check (t != nullptr && std::abs (t->counterZero.load() - 64000.0) < 1e-6 && t->session != nullptr
                   && std::abs (t->session->getChildByName ("TAPE_DECK")->getDoubleAttribute ("counterZero") - 64000.0) < 1e-6,
               "legacy: the old deck file becomes the session, with the counter's zero", 1, 1);
        check (dir.getChildFile (tfmt::CassFiles::manifest).existsAsFile() && dir.getChildFile ("session.xml").existsAsFile()
                   && dir.getChildFile ("tape-v2.xml.bak").existsAsFile() && ! dir.getChildFile ("tape.xml").existsAsFile(),
               "legacy: converted in place, the old files kept aside", 1, 1);
        auto again = Tape::load (dir);
        bool same = again != nullptr && again->id == "legacy-tape" && again->getCondition() == t->getCondition();
        for (int c = 0; same && c < cellRate; c += 13) same = std::abs (again->readCell (0, c) - t->readCell (0, c)) < 1e-9f;
        check (same && again->isNrEncoded (0, 3) && again->recordedPosition (0, 3) == 0,
               "legacy: the converted cassette loads again as itself", same ? 1 : 0, 1);
        dir.deleteRecursively();
    }

    {   // 0c. Dropouts are places on the cassette: the same on every play, and gone when the deck plays it as new.
        Rig r;
        tfmt::Condition c;
        c.dropoutsPerMinute = 300.0;   // dense, so two seconds always hold several
        r.tape.setCondition (c);
        for (int64_t cell = 0; cell < (int64_t) cellRate * 3; ++cell)
            r.tape.writeCell (0, cell, 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 400.0 * (double) cell / cellRate), false, 0, 1.0f, 0);
        auto first = r.run (2.5, TapeEngine::Command::play, false, 0, 0);
        r.rewindTo (0);
        auto second = r.run (2.5, TapeEngine::Command::play, false, 0, 0);
        // The tape's modulation noise is different on every play, as on a real one, so compare where the
        // dips are: a window is a dip, or clearly isn't one (a window catching the edge of a dropout sits
        // near any threshold, and noise can tip it either way). The first play also wore the tape by one
        // pass, which can add the odd new dropout (Wear::dropoutFactor) but never moves or removes one.
        int dips = 0, gone = 0, added = 0;
        const double fullA = rms (first, 0.1, 2.4), fullB = rms (second, 0.1, 2.4);
        for (double t0 = 0.2; t0 < 2.4; t0 += 0.01)
        {
            const double a = rms (first, t0, t0 + 0.01) / fullA, b = rms (second, t0, t0 + 0.01) / fullB;
            dips += a < 0.7 ? 1 : 0;
            gone += a < 0.6 && b > 0.85 ? 1 : 0;
            added += b < 0.6 && a > 0.85 ? 1 : 0;
        }
        check (dips > 2 && gone == 0, "dropouts: at the same places on every play (windows that lost one)", gone, 0.0);
        check (added * 5 <= dips, "dropouts: one more pass adds few new ones (windows that gained one)", added, 0.2 * dips);
        r.rewindTo (0);
        r.eng.machine.honoursTapeCondition = false;
        auto asNew = r.run (2.5, TapeEngine::Command::play, false, 0, 0);
        double lowest = 1.0;
        for (double t0 = 0.2; t0 < 2.4; t0 += 0.02) lowest = std::min (lowest, rms (asNew, t0, t0 + 0.02) / rms (asNew, 0.1, 2.4));
        check (lowest > 0.95, "dropouts: none when the deck plays every cassette as new", lowest, 1.0);
    }

    const double sineRms = 0.5 / std::sqrt (2.0);

    {   // 1. clean record and playback at 1 7/8
        Rig r;
        r.run (2.0, TapeEngine::Command::rec, true, 1000.0, tapeRefPeak);
        r.rewindTo (0);
        auto out = r.run (2.0, TapeEngine::Command::play, false, 0, 0);
        auto sens = tapeModels[defaultTapeModel].sensitivity;
        check (std::abs (freq (out, 0.5, 1.5) - 1000.0) < 3.0, "clean: pitch of 1 kHz recorded and played at 1 7/8", freq (out, 0.5, 1.5), 1000.0);
        check (std::abs (db (rms (out, 0.5, 1.5) / (tapeRefPeak / std::sqrt (2.0))) - sens) < 0.5, "clean: 0 VU plays back at about the tape's sensitivity (dB)",
               db (rms (out, 0.5, 1.5) / (tapeRefPeak / std::sqrt (2.0))), sens);
        check (r.tape.isRecorded (0, 5) && ! r.tape.isRecorded (1, 5), "clean: only track 1 marked recorded", r.tape.tracksUsed(), 1);
    }

    {   // 2. noise reduction encode + decode should be transparent-ish
        for (int type = 0; type < 3; ++type)
        {
            Rig r;
            r.eng.params.nrOn = true;
            r.eng.params.nrType = type;
            // quiet and bright, so every type of compander has work to do
            r.run (2.0, TapeEngine::Command::rec, true, 5000.0, 0.01f);
            r.rewindTo (0);
            auto out = r.run (2.0, TapeEngine::Command::play, false, 0, 0);
            auto lvl = db (rms (out, 0.5, 1.5) / (0.01 / std::sqrt (2.0)));
            juce::String name = "NR type " + juce::String (type) + ": encode+decode level (dB)";
            // 2:1 decoding doubles any level error, including the tape's sensitivity
            auto want = 2.0 * tapeModels[defaultTapeModel].sensitivity;
            check (std::abs (lvl - want) < 1.5, name.toRawUTF8(), lvl, want);

            r.rewindTo (0);
            r.eng.params.nrOn = false;
            auto raw = r.run (2.0, TapeEngine::Command::play, false, 0, 0);
            auto lvlRaw = db (rms (raw, 0.5, 1.5) / (0.01 / std::sqrt (2.0)));
            name = "NR type " + juce::String (type) + ": encoded tape played with NR off is louder";
            check (lvlRaw > lvl + 2.0, name.toRawUTF8(), lvlRaw, lvl + 2.0);
            check (r.tape.isNrEncoded (0, 5), "NR flag stored on tape", 1, 1);
        }
    }

    {   // 3. NR pushes hiss down on blank tape
        Rig r;
        r.eng.setHissForTests (true);
        r.eng.params.nrOn = false;
        auto plain = r.run (1.0, TapeEngine::Command::play, false, 0, 0, 1);
        r.rewindTo (0);
        r.eng.params.nrOn = true;
        auto withNr = r.run (1.0, TapeEngine::Command::play, false, 0, 0, 1);
        auto a = db (rms (plain, 0.3, 0.9)), b = db (rms (withNr, 0.3, 0.9));
        check (b < a - 10.0, "hiss with NR decode is >10 dB lower than without (dBFS)", b, a - 10.0);
        juce::ignoreUnused (sineRms);
    }

    {   // 4. recorded at 3 3/4, played at 1 7/8: an octave down
        Rig r;
        r.eng.params.speedSel = 2;
        r.run (2.0, TapeEngine::Command::rec, true, 1000.0, 0.5f);
        r.rewindTo (0);
        r.eng.params.speedSel = 1;
        auto out = r.run (3.0, TapeEngine::Command::play, false, 0, 0);
        check (std::abs (freq (out, 0.5, 2.5) - 500.0) < 3.0, "speed: 3 3/4 recording played at 1 7/8", freq (out, 0.5, 2.5), 500.0);
        check (std::abs (r.tape.recordedSpeed (0, 5) - 2.0f) < 0.01f, "speed: recorded speed stored", r.tape.recordedSpeed (0, 5), 2.0);
    }

    {   // 5. pitch is continuous: +3.7% plays a 1000 Hz tone at 1037 Hz
        Rig r;
        r.run (2.0, TapeEngine::Command::rec, true, 1000.0, 0.5f);
        r.rewindTo (0);
        r.eng.params.pitch = 3.7f;
        auto out = r.run (1.5, TapeEngine::Command::play, false, 0, 0);
        check (std::abs (freq (out, 0.3, 1.3) - 1037.0) < 4.0, "pitch: +3.7% playback (continuous, not stepped)", freq (out, 0.3, 1.3), 1037.0);
    }

    {   // 5b. turning pitch while playing glides like a motor rather than jumping
        Rig r;
        r.run (4.0, TapeEngine::Command::rec, true, 1000.0, 0.5f);
        r.rewindTo (0);
        r.eng.params.pitch = 0.0f;
        r.run (0.5, TapeEngine::Command::play, false, 0, 0);
        r.eng.params.pitch = 12.0f;
        auto out = r.run (1.0, TapeEngine::Command::none, false, 0, 0);
        auto early = freq (out, 0.0, 0.05), late = freq (out, 0.5, 0.9);
        check (early > 1005.0 && early < 1100.0 && std::abs (late - 1120.0) < 5.0,
               "pitch: a jump on the knob glides to the new speed", early, 1060.0);
    }

    {   // 6. punch-in replaces only the middle
        Rig r;
        r.run (3.0, TapeEngine::Command::rec, true, 500.0, 0.5f);
        r.rewindTo (0);
        r.run (1.0, TapeEngine::Command::play, true, 2000.0, 0.5f);          // armed but playing: nothing written
        r.run (1.0, TapeEngine::Command::rec, true, 2000.0, 0.5f);           // punch in for 1 s
        r.run (0.5, TapeEngine::Command::play, true, 2000.0, 0.5f);          // punch out
        r.rewindTo (0);
        auto out = r.run (3.0, TapeEngine::Command::play, false, 0, 0);
        check (std::abs (freq (out, 0.2, 0.9) - 500.0) < 3.0, "punch: before the punch-in untouched", freq (out, 0.2, 0.9), 500.0);
        check (std::abs (freq (out, 1.2, 1.8) - 2000.0) < 5.0, "punch: punched section replaced", freq (out, 1.2, 1.8), 2000.0);
        check (std::abs (freq (out, 2.2, 2.9) - 500.0) < 3.0, "punch: after the punch-out untouched", freq (out, 2.2, 2.9), 500.0);
    }

    {   // 6b. a broken-out tab: REC won't go down
        Rig r;
        r.tape.protectA = true;
        r.run (0.2, TapeEngine::Command::rec, true, 1000.0, tapeRefPeak);
        check (r.eng.getMode() != Mode::rec && ! r.tape.isRecorded (0, 0), "write-protect: a tape with its tab out can't be recorded on", (int) r.eng.getMode(), 0);
    }

    {   // 6c. Use: every second that passes the heads in play or record counts; winding doesn't.
        Rig r;
        r.run (2.5, TapeEngine::Command::play, false, 0, 0);
        r.rewindTo (0);
        r.run (1.5, TapeEngine::Command::rec, true, 1000.0, 0.1f);
        r.run (0.5, TapeEngine::Command::ff, false, 0, 0);
        check (r.tape.passesAt (0.5 * cellRate) == 2 && r.tape.passesAt (2.2 * cellRate) == 1 && r.tape.passesAt (40.0 * cellRate) == 0,
               "wear: passes counted per second of tape (play and record, not winding)", r.tape.passesAt (0.5 * cellRate), 2);

        // A read-only engine (exports) neither wears the tape nor moves it.
        TapeEngine render;
        render.prepare (sr, bs);
        render.setReadOnly (true);
        render.setTape (&r.tape, false);
        const auto before = r.tape.position.load();
        render.command (TapeEngine::Command::play);
        bool armed[numTracks] {};
        std::vector<float> o ((size_t) bs);
        float* outs[numTracks] { o.data(), o.data(), o.data(), o.data() };
        for (int b = 0; b < 200; ++b) { render.beginBlock (bs, armed); render.readAll (outs, bs); render.endBlock(); }
        check (std::abs (r.tape.position.load() - before) < 1e-9 && r.tape.passesAt (0.5 * cellRate) == 2,
               "wear: rendering for export leaves the tape as it was", r.tape.passesAt (0.5 * cellRate), 2);

        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tf_wear_test");
        dir.deleteRecursively();
        r.tape.save (dir);
        auto back = Tape::load (dir);
        check (back != nullptr && back->passesAt (0.5 * cellRate) == 2 && dir.getChildFile (tfmt::CassFiles::wear).existsAsFile(),
               "wear: kept with the cassette (wear.bin)", back != nullptr ? back->passesAt (0.5 * cellRate) : -1, 2);
        dir.deleteRecursively();
    }

    {   // 6d. Wear is subtle: treble at 10 kHz after 100 and 1000 passes, and none when the deck plays it as new.
        auto at10k = [] (uint16_t passes, bool asNew)
        {
            Rig r;
            r.run (1.5, TapeEngine::Command::rec, true, 10000.0, tapeRefPeak * 0.1f);
            r.tape.setPassesForTests (passes);
            r.eng.machine.honoursTapeCondition = ! asNew;
            r.rewindTo (0);
            auto out = r.run (1.5, TapeEngine::Command::play, false, 0, 0);
            return db (tone (out, 10000.0, 0.3, 1.2));
        };
        const auto fresh = at10k (0, false), hundred = at10k (100, false), thousand = at10k (1000, false), ignored = at10k (1000, true);
        check (fresh - hundred > 0.05 && fresh - hundred < 0.4, "wear: 100 passes cost a fifth of a dB at 10 kHz", fresh - hundred, 0.2);
        check (fresh - thousand > 0.4 && fresh - thousand < 1.0, "wear: 1000 passes cost under a dB at 10 kHz", fresh - thousand, 0.7);
        check (std::abs (fresh - ignored) < 0.02, "wear: none when the deck plays every cassette as new", fresh - ignored, 0.0);
    }

    {   // 6e. Age: print-through grows from the day a stretch was recorded, onto the turns either side.
        auto preEcho = [] (int daysLater)
        {
            Rig r;
            for (int64_t c = (int64_t) cellRate * 10; c < (int64_t) cellRate * 11; ++c)
                r.tape.writeCell (0, c, 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) c / cellRate), false, 0, 1.0f, 0);
            r.tape.setToday ((uint16_t) (r.tape.getToday() + daysLater));
            r.rewindTo (5.0 * cellRate);
            auto out = r.run (6.5, TapeEngine::Command::play, false, 0, 0);
            // The turn ahead of 10 s on a C-60: about 2.9 s of tape, so the copy sits at about 7.1 s (out at 2.1 s).
            const auto turn = Wear::turnCells (10.0 / (30.0 * 60.0), r.tape.getTypeInfo().fullPackMm, cellRate) / cellRate;
            auto copyAt = 10.0 - turn - 5.0;
            return db (tone (out, 1000.0, copyAt + 0.15, copyAt + 0.85) / tone (out, 1000.0, 5.15, 5.85));
        };
        const auto sameDay = preEcho (0), aYear = preEcho (365), tenYears = preEcho (3652);
        check (sameDay < -90.0, "age: a fresh recording has no print-through (dB re the recording)", sameDay, -100.0);
        check (std::abs (aYear - (Wear::printThroughDb (1.0f) - 0.6)) < 1.5, "age: after a year, a pre-echo about 54 dB down", aYear, -54.9);
        check (tenYears > aYear + 2.0 && tenYears < -48.0, "age: after ten years a little more, still faint", tenYears, -51.9);
    }

    {   // 7. end of tape stops the transport
        Rig r;
        r.rewindTo ((double) r.tape.getLengthCells() - cellRate * 0.5);
        r.run (1.0, TapeEngine::Command::play, false, 0, 0);
        check (r.eng.getMode() == Mode::stop && r.eng.takeEndOfTapeFlag(), "end of tape stops and flags", (int) r.eng.getMode(), 0);
    }

    {   // 8. whole tape saved and loaded back
        Rig r;
        r.eng.params.nrOn = true;
        r.run (1.5, TapeEngine::Command::rec, true, 440.0, 0.4f);
        r.tape.name = "round trip";
        r.tape.trackSheet.set (1, "1 - test tone");
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tf_engine_test");
        dir.deleteRecursively();
        bool saved = r.tape.save (dir);
        auto loaded = Tape::load (dir);
        bool same = loaded != nullptr;
        if (same)
            for (int64_t c = 0; c < cellRate * 2; c += 7)
                same = same && std::abs (loaded->readCell (0, c) - r.tape.readCell (0, c)) < 1e-6f;
        check (saved && same, "save/load: audio identical", same ? 1 : 0, 1);
        check (loaded != nullptr && loaded->name == "round trip" && loaded->trackSheet[1] == "1 - test tone"
               && loaded->isNrEncoded (0, 3), "save/load: label, track sheet, NR flags", 1, 1);
        dir.deleteRecursively();
    }

    {   // 9. tape models: each behaves as its real cassette measured
        double modSide[numTapeModels] {};
        for (int m = 0; m < numTapeModels; ++m)
        {
            const auto& tm = tapeModels[m];
            auto label = [&] (const char* what) { return (juce::String (tm.name) + ": " + what).toStdString(); };
            const double refRms = tapeRefPeak / std::sqrt (2.0);

            // MOL: 400 Hz at the tape's MOL gives 3% third harmonic
            {
                Rig r (m);
                auto molPeak = tapeRefPeak * std::pow (10.0, tm.mol400 / 20.0);
                r.run (2.0, TapeEngine::Command::rec, true, 400.0, (float) molPeak);
                r.rewindTo (0);
                auto out = r.run (2.0, TapeEngine::Command::play, false, 0, 0);
                auto h3 = 100.0 * tone (out, 1200.0, 0.4, 1.6) / tone (out, 400.0, 0.4, 1.6);
                check (h3 > 2.0 && h3 < 4.5, label ("third harmonic at MOL 400 Hz (%)").c_str(), h3, 3.0);
            }
            // SOL: 10 kHz driven far past saturation tops out near the tape's SOL
            // (measured against a quiet 10 kHz tone, so the deck's own treble response cancels out)
            {
                auto play10k = [&] (double amp)
                {
                    Rig r (m);
                    r.run (1.5, TapeEngine::Command::rec, true, 10000.0, (float) amp);
                    r.rewindTo (0);
                    auto out = r.run (1.5, TapeEngine::Command::play, false, 0, 0);
                    return db (tone (out, 10000.0, 0.3, 1.2) / amp);
                };
                auto quiet = play10k (tapeRefPeak * 0.05);
                auto hot = tapeRefPeak * std::pow (10.0, (tm.sol10k + 12.0) / 20.0);
                auto lvl = tm.sol10k + 12.0 + play10k (hot) - quiet;
                check (std::abs (lvl - tm.sol10k) < 2.0, label ("10 kHz saturates at SOL (dB re Dolby level)").c_str(), lvl, tm.sol10k);
            }
            // S/N: blank tape hiss, A-weighted, below Dolby level
            {
                Rig r (m);
                r.eng.setHissForTests (true);   // the cassette as measured: a new one adds nothing
                auto out = r.run (2.5, TapeEngine::Command::play, false, 0, 0, 1);
                auto snr = db (refRms / aWeighted (out, 0.8, 2.4));
                check (std::abs (snr - tm.snr) < 1.0, label ("hiss S/N, A-weighted (dB)").c_str(), snr, tm.snr);
            }
            // sensitivity and modulation noise on a 1 kHz tone at Dolby level
            {
                Rig r (m);
                r.run (3.0, TapeEngine::Command::rec, true, 1000.0, tapeRefPeak);
                r.rewindTo (0);
                auto out = r.run (3.0, TapeEngine::Command::play, false, 0, 0);

                Rig q (m);   // sensitivity is measured 20 dB down, clear of compression
                q.run (2.0, TapeEngine::Command::rec, true, 1000.0, tapeRefPeak * 0.1f);
                q.rewindTo (0);
                auto quiet = q.run (2.0, TapeEngine::Command::play, false, 0, 0);
                auto lvl = db (tone (quiet, 1000.0, 0.5, 1.5) / (tapeRefPeak * 0.1));
                check (std::abs (lvl - tm.sensitivity) < 0.2, label ("sensitivity, 1 kHz at -20 dB (dB)").c_str(), lvl, tm.sensitivity);
                double side = 0;
                for (double f = 760; f <= 960; f += 20)  side += std::pow (tone (out, f, 0.5, 2.5), 2) + std::pow (tone (out, 2000.0 - f, 0.5, 2.5), 2);
                modSide[m] = db (std::sqrt (side) / tone (out, 1000.0, 0.5, 2.5));
            }
        }
        double spread = modSide[0] - modSide[3], want = (double) (tapeModels[0].modNoise - tapeModels[3].modNoise);
        check (std::abs (spread - want) < 3.0, "modulation noise: BASIC vs MX sidebands (dB)", spread, want);
        check (modSide[0] > modSide[1] && modSide[1] > modSide[2] && modSide[2] > modSide[3], "modulation noise falls BASIC > SD > XR > MX", modSide[0], modSide[3]);

        // A tape from before models (and before the cassette format) loads as SD at full-scale flux.
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tf_model_test");
        dir.deleteRecursively();
        dir.createDirectory();
        dir.getChildFile ("tape.xml").replaceWithText ("<TAPEFOUR_TAPE version=\"1\" id=\"old-one\" name=\"old\" type=\"1\"/>");
        auto loaded = Tape::load (dir);
        check (loaded != nullptr && loaded->getModel() == defaultTapeModel, "old tapes load as SD", loaded != nullptr ? loaded->getModel() : -1, defaultTapeModel);
        dir.deleteRecursively();

        Tape mx (0, 3);
        mx.save (dir);
        auto mxBack = Tape::load (dir);
        check (mxBack != nullptr && mxBack->getModel() == 3 && mxBack->productName() == "MX46", "tape model saved with the tape", mxBack != nullptr ? mxBack->getModel() : -1, 3);
        dir.deleteRecursively();
    }

    {   // 10. the tape switch: bias, EQ, erase
        // record a quiet tone with the switch at recPos, play it back at playPos; level in dB re input
        auto through = [] (int model, int recPos, int playPos, double hz, double amp = tapeRefPeak * 0.1)
        {
            Rig r (model);
            r.eng.params.tapeSwitch = recPos;
            r.run (1.5, TapeEngine::Command::rec, true, hz, (float) amp);
            r.rewindTo (0);
            r.eng.params.tapeSwitch = playPos;
            auto out = r.run (1.5, TapeEngine::Command::play, false, 0, 0);
            return db (tone (out, hz, 0.3, 1.2) / amp);
        };

        {
            Rig r (2);
            check (r.eng.params.tapeSwitch.load() == 1, "switch: an XR tape going in sets CHROME", r.eng.params.tapeSwitch.load(), 1);
            r.eng.params.tapeSwitch = 0;
            r.run (0.5, TapeEngine::Command::rec, true, 1000.0, 0.1f);
            check (r.tape.recordedPosition (0, 2) == 0, "switch: position stored with the recording", r.tape.recordedPosition (0, 2), 0);
            check (r.tape.recordedPosition (1, 2) == 1, "switch: unrecorded tape reads as its own type", r.tape.recordedPosition (1, 2), 1);
        }

        // playback EQ: 70 us recording through 120 us is brighter by about 20 log(120/70) at the top
        auto want8k = 20.0 * std::log10 (std::abs (std::complex<double> (1.0, 2 * juce::MathConstants<double>::pi * 8000 * 120e-6))
                                          / std::abs (std::complex<double> (1.0, 2 * juce::MathConstants<double>::pi * 8000 * 70e-6)));
        auto bright = through (2, 1, 0, 8000.0) - through (2, 1, 1, 8000.0);
        check (std::abs (bright - want8k) < 0.6, "EQ: chrome recording played at NORMAL, 8 kHz (dB)", bright, want8k);
        auto low = through (2, 1, 0, 200.0) - through (2, 1, 1, 200.0);
        check (std::abs (low) < 0.4, "EQ: ...200 Hz unchanged (dB)", low, 0.0);
        auto dull = through (1, 0, 1, 8000.0) - through (1, 0, 0, 8000.0);
        check (std::abs (dull + want8k) < 0.6, "EQ: normal recording played at CHROME, 8 kHz (dB)", dull, -want8k);

        // bias: normal tape at METAL is heavily over-biased (dull); chrome tape at NORMAL under-biased (bright)
        auto over = through (1, 2, 2, 10000.0) - through (1, 0, 0, 10000.0);
        check (over < -12.0, "bias: SD at METAL loses treble (10 kHz, dB)", over, -16.0);
        // (30 dB down: under-biased tape saturates treble early, which would hide the lift)
        auto under = through (2, 0, 0, 10000.0, tapeRefPeak * 0.03) - through (2, 1, 1, 10000.0, tapeRefPeak * 0.03);
        check (under > 1.5 && under < 4.0, "bias: XR at NORMAL gains treble (10 kHz, dB)", under, 2.6);

        // under-bias distorts earlier: metal tape at CHROME at its own MOL
        {
            Rig r (3);
            r.eng.params.tapeSwitch = 1;
            auto molPeak = tapeRefPeak * std::pow (10.0, tapeModels[3].mol400 / 20.0);
            r.run (2.0, TapeEngine::Command::rec, true, 400.0, (float) molPeak);
            r.rewindTo (0);
            auto out = r.run (2.0, TapeEngine::Command::play, false, 0, 0);
            auto h3 = 100.0 * tone (out, 1200.0, 0.4, 1.6) / tone (out, 400.0, 0.4, 1.6);
            check (h3 > 8.0, "bias: MX at CHROME distorts at its MOL (H3 %)", h3, 8.0);
        }

        // erase: metal tape recorded over at NORMAL keeps a ghost of the old recording
        auto ghost = [] (int overPos)
        {
            Rig r (3);
            r.run (2.0, TapeEngine::Command::rec, true, 300.0, tapeRefPeak);
            r.rewindTo (0);
            r.eng.params.tapeSwitch = overPos;
            r.run (2.0, TapeEngine::Command::rec, true, 1000.0, tapeRefPeak * 0.5f);
            r.rewindTo (0);
            r.eng.params.tapeSwitch = overPos;
            auto out = r.run (2.0, TapeEngine::Command::play, false, 0, 0);
            return db (tone (out, 300.0, 0.3, 1.7) / tapeRefPeak);
        };
        auto bleed = ghost (0), clean = ghost (2);
        check (bleed > -40.0 && bleed < -28.0, "erase: MX overwritten at NORMAL, old 300 Hz (dB)", bleed, -33.0);
        check (clean < -70.0, "erase: MX overwritten at METAL, old 300 Hz gone (dB)", clean, -70.0);
    }

    {   // 11. punch-in and punch-out crossfade: no click either side
        Rig r;
        // a steady 200 Hz tone already on the tape
        for (int64_t c = 0; c < (int64_t) cellRate * 4; ++c)
            r.tape.writeCell (0, c, 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 200.0 * (double) c / cellRate), false, 0, 1.0f, 0);
        r.run (1.0, TapeEngine::Command::play, true, 0, 0);
        r.run (0.5, TapeEngine::Command::rec, true, 0, 0);     // punch in silence...
        r.run (1.0, TapeEngine::Command::play, true, 0, 0);    // ...and out again

        float maxStep = 0, tone = 0.3f * (float) (juce::MathConstants<double>::twoPi * 200.0 / cellRate);
        double gapRms = 0;
        for (int64_t c = 1; c < (int64_t) cellRate * 3; ++c)
            maxStep = std::max (maxStep, std::abs (r.tape.readCell (0, c) - r.tape.readCell (0, c - 1)));
        for (int64_t c = (int64_t) (cellRate * 1.15); c < (int64_t) (cellRate * 1.4); ++c)
            gapRms += std::pow (r.tape.readCell (0, c), 2.0);
        gapRms = std::sqrt (gapRms / (cellRate * 0.25));
        check (maxStep < tone * 1.6f, "punch in/out: no step bigger than the tone's own", maxStep, tone);
        check (gapRms < 0.005, "punch: the punched-in stretch holds the new (silent) take", gapRms, 0.0);
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "SOME FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
