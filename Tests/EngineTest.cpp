// Headless checks of the tape engine: record real signals, play them back, measure.
#include "../Source/TapeEngine.h"
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
}

struct Rig
{
    Tape tape;
    TapeEngine eng;
    double phase = 0;

    explicit Rig (int model = defaultTapeModel) : tape (1, model)
    {
        eng.prepare (sr, bs);
        auto& p = eng.params;
        p.hiss = 0; p.wow = 0; p.flutter = 0; p.sat = 0.5f; p.hf = 0; p.xtalk = 0; p.drop = 0; p.drift = 0;
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

int main()
{
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
        r.eng.params.hiss = 0.55f;
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
                r.eng.params.hiss = 0.5f;
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

        // An old tape (before models) still plays at the level it was recorded.
        Tape old (1);
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tf_model_test");
        dir.deleteRecursively();
        old.save (dir);
        auto xml = juce::XmlDocument::parse (dir.getChildFile ("tape.xml"));
        xml->removeAttribute ("model");
        xml->removeAttribute ("fluxScale");
        xml->writeTo (dir.getChildFile ("tape.xml"));
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
            r.tape.writeCell (0, c, 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 200.0 * (double) c / cellRate), false, 1.0f, 0);
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
