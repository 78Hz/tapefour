// Headless checks of the tape engine: record real signals, play them back, measure.
#include "../Source/TapeEngine.h"
#include <cstdio>

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
}

struct Rig
{
    Tape tape { 1 };
    TapeEngine eng;
    double phase = 0;

    Rig()
    {
        eng.prepare (sr, bs);
        auto& p = eng.params;
        p.hiss = 0; p.wow = 0; p.flutter = 0; p.sat = 0; p.hf = 0; p.xtalk = 0; p.drop = 0; p.drift = 0;
        p.nrOn = false;
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
            eng.beginBlock (bs, armed, 0);
            eng.readAll (outPtrs, bs);
            if (eng.isRecordingNow()) eng.writeTrack (0, in.data(), bs);
            eng.endBlock();
            captured.insert (captured.end(), outs[(size_t) listen].begin(), outs[(size_t) listen].end());
        }
        return captured;
    }

    void rewindTo (double cells)
    {
        eng.command (TapeEngine::Command::stop);
        bool armed[numTracks] {};
        eng.beginBlock (1, armed, 0); eng.endBlock();
        eng.setTape (nullptr);
        tape.position = cells;
        eng.setTape (&tape);
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
} // namespace

int main()
{
    const double sineRms = 0.5 / std::sqrt (2.0);

    {   // 1. clean record and playback at 1 7/8
        Rig r;
        r.run (2.0, TapeEngine::Command::rec, true, 1000.0, 0.5f);
        r.rewindTo (0);
        auto out = r.run (2.0, TapeEngine::Command::play, false, 0, 0);
        check (std::abs (freq (out, 0.5, 1.5) - 1000.0) < 3.0, "clean: pitch of 1 kHz recorded and played at 1 7/8", freq (out, 0.5, 1.5), 1000.0);
        check (std::abs (db (rms (out, 0.5, 1.5) / sineRms)) < 0.5, "clean: level (dB re input)", db (rms (out, 0.5, 1.5) / sineRms), 0.0);
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
            check (std::abs (lvl) < 1.5, name.toRawUTF8(), lvl, 0.0);

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
        r.eng.params.hiss = 0.55f;
        r.eng.params.nrOn = false;
        auto plain = r.run (1.0, TapeEngine::Command::play, false, 0, 0, 1);
        r.rewindTo (0);
        r.eng.params.nrOn = true;
        auto withNr = r.run (1.0, TapeEngine::Command::play, false, 0, 0, 1);
        auto a = db (rms (plain, 0.3, 0.9)), b = db (rms (withNr, 0.3, 0.9));
        check (b < a - 10.0, "hiss with NR decode is >10 dB lower than without (dBFS)", b, a - 10.0);
        check (a > -60.0 && a < -56.0, "hiss level at default setting (dBFS)", a, -58.0);
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

    {   // 5. pitch control +2 steps = +4.8 %
        Rig r;
        r.run (2.0, TapeEngine::Command::rec, true, 1000.0, 0.5f);
        r.rewindTo (0);
        r.eng.params.pitchStep = 2;
        auto out = r.run (1.5, TapeEngine::Command::play, false, 0, 0);
        check (std::abs (freq (out, 0.3, 1.3) - 1048.0) < 4.0, "pitch: +4.8% playback", freq (out, 0.3, 1.3), 1048.0);
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

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "SOME FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
