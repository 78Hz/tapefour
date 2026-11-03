// TF FM DRUMS (Source/FmDrums.h): the six machines, the sequencer and the effects.
//
//   Each machine sounds, at its pitch, for as long as DECAY says, and stays in bounds whatever its
//   knobs (every one swept, with drive); the macros do what they say (the kick's pitch drop, TONE's
//   ratio, CHORD's notes); the sequencer's steps fall on the sixteenths at the tempo, swing moves the
//   off-beats, locks change a step's sound, chance and retrigs, pattern lengths, a pattern change waits
//   for the bar; MIDI notes play the tracks; the delay repeats in the tempo; the reverb rings; the state
//   comes back as it went.
//
// With TF_FMDRUMS_RENDER=<folder> it also writes a WAV of each machine and a pattern.

#include "../Source/FmDrums.h"

#include <cstdio>
#include <cstdlib>

using namespace tf;
using namespace tf::fmd;

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
        if (annotate != nullptr) std::printf ("::error::FmDrumsTest: %s (got %.3f, want %.3f)\n", what, got, want);
    }
}

constexpr double sr = 48000.0;
constexpr int bs = 256;

struct Rig
{
    Processor p;
    Rig()
    {
        p.setRateAndBufferSizeDetails (sr, bs);
        p.prepareToPlay (sr, bs);
        // no effects, centred, unless a test asks
        auto& s = p.state();
        for (int t = 0; t < fmd::numTracks; ++t) { s.sound[t][pDelay] = 0; s.sound[t][pReverb] = 0; s.sound[t][pPan] = 64; s.sound[t][pLfoDepth] = 64; s.sound[t][pDrive] = 0; }
        for (auto& pat : s.patterns) pat = Pattern();
        p.changed();
    }
    std::vector<float> run (double seconds, std::vector<std::pair<double, juce::MidiMessage>> midi = {})
    {
        std::vector<float> out;
        juce::AudioBuffer<float> b (2, bs);
        size_t e = 0;
        const int blocks = (int) std::lround (seconds * sr / bs);
        for (int k = 0; k < blocks; ++k)
        {
            juce::MidiBuffer m;
            while (e < midi.size() && midi[e].first * sr < (k + 1) * bs)
            {
                m.addEvent (midi[e].second, std::clamp ((int) (midi[e].first * sr) - k * bs, 0, bs - 1));
                ++e;
            }
            p.processBlock (b, m);
            out.insert (out.end(), b.getReadPointer (0), b.getReadPointer (0) + bs);
        }
        return out;
    }
    /** One track's machine, set up, struck once. */
    std::vector<float> strike (int machine, std::function<void (int*)> set, double seconds = 1.0)
    {
        auto& s = p.state();
        s.sound[0][pMachine] = machine;
        if (set) set (s.sound[0]);
        p.changed();
        run (0.02);
        p.hit (0, 127);
        return run (seconds);
    }
};

double rms (const std::vector<float>& v, double from, double to)
{
    const auto a = std::min (v.size(), (size_t) (from * sr)), b = std::min (v.size(), (size_t) (to * sr));
    double s = 0;
    for (auto i = a; i < b; ++i) s += (double) v[i] * v[i];
    return std::sqrt (s / (double) std::max<size_t> (1, b - a));
}
double peak (const std::vector<float>& v) { double m = 0; for (auto x : v) m = std::max (m, (double) std::abs (x)); return m; }
/** The strongest frequency between lo and hi (Hz), by a sliding DFT. */
double strongest (const std::vector<float>& v, double from, double to, double lo, double hi)
{
    const auto a = (size_t) (from * sr), b = std::min (v.size(), (size_t) (to * sr));
    double best = 0, bestHz = 0;
    for (double hz = lo; hz <= hi; hz *= 1.005)
    {
        double re = 0, im = 0;
        for (auto i = a; i < b; ++i) { const double w = 6.283185307179586 * hz * (double) i / sr; re += v[i] * std::cos (w); im += v[i] * std::sin (w); }
        const double m = re * re + im * im;
        if (m > best) { best = m; bestHz = hz; }
    }
    return bestHz;
}
/** When the next hit starts (s), from a time on: the sound passing a level after it was quiet (its
    envelope, held over 3 ms, under a quarter of that level). */
double onset (const std::vector<float>& v, double from, double level = 0.02)
{
    double env = 0.0;
    const double fall = std::exp (-1.0 / (0.003 * sr));
    const auto start = (size_t) (from * sr);
    for (auto i = start > 480 ? start - 480 : 0; i < start && i < v.size(); ++i) env = std::max ((double) std::abs (v[i]), env * fall);
    bool quiet = env < level * 0.25;   // (from the start of the run: silence before it)
    for (auto i = start; i < v.size(); ++i)
    {
        const double a = std::abs (v[i]);
        if (quiet && a > level) return (double) i / sr;
        env = std::max (a, env * fall);
        if (env < level * 0.25) quiet = true;
    }
    return -1.0;
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    // ---------------------------------------------------------------- the machines
    {
        for (int m = 0; m < numMachines; ++m)
        {
            Rig r;
            auto out = r.strike (m, nullptr, 4.0);
            check (rms (out, 0.0, 0.2) > 0.01, (juce::String ("machines: ") + machineName (m) + " sounds").toRawUTF8(), rms (out, 0.0, 0.2), 0.1);
            check (rms (out, 3.8, 4.0) < 0.002, (juce::String ("machines: ") + machineName (m) + " dies away").toRawUTF8(), std::abs (out.back()), 0.0);
        }
        // every knob swept, with drive: never too loud, never NaN (the sweet spots hold)
        juce::Random rnd (3);
        double worst = 0.0;
        bool finite = true;
        for (int k = 0; k < 120; ++k)
        {
            Rig r;
            auto out = r.strike (k % numMachines, [&rnd] (int* s) { for (int p = pPitch; p <= pDrive; ++p) s[p] = rnd.nextInt (128); s[pVolume] = 127; }, 0.25);
            for (auto x : out) if (! std::isfinite (x)) finite = false;
            worst = std::max (worst, peak (out));
        }
        check (finite && worst < 2.0, "machines: 120 random settings, all six: in bounds (peak)", worst, 1.0);

        // KICK: at its pitch once the drop is over; SWEEP starts it higher
        Rig k;
        auto kickOut = k.strike (kick, [] (int* s) { s[pPitch] = 64; s[pSweep] = 0; s[pColor] = 0; s[pShape] = 0; s[pDecay] = 100; });
        const double kickHz = 30.0 * std::pow (4.0, 64.0 / 127.0);
        check (std::abs (strongest (kickOut, 0.1, 0.6, 20.0, 200.0) / kickHz - 1.0) < 0.03, "kick: PITCH 64 is 60 Hz (30-120 Hz across the knob)", strongest (kickOut, 0.1, 0.6, 20.0, 200.0), kickHz);
        Rig k2;
        auto swept = k2.strike (kick, [] (int* s) { s[pPitch] = 64; s[pSweep] = 127; s[pColor] = 0; s[pShape] = 0; s[pDecay] = 100; });
        check (strongest (swept, 0.0, 0.012, 100.0, 1200.0) > kickHz * 4.0, "kick: SWEEP starts it octaves higher", strongest (swept, 0.0, 0.012, 100.0, 1200.0), kickHz * 8.0);
        // DECAY: longer
        Rig d1, d2;
        auto shortK = d1.strike (kick, [] (int* s) { s[pDecay] = 20; s[pContour] = 0; });
        auto longK = d2.strike (kick, [] (int* s) { s[pDecay] = 110; s[pContour] = 0; });
        check (rms (longK, 0.4, 0.6) > 10.0 * rms (shortK, 0.4, 0.6), "kick: DECAY up, the sound lasts longer", rms (longK, 0.4, 0.6) / std::max (1.0e-9, rms (shortK, 0.4, 0.6)), 10.0);

        // TONE: the note, and COLOR's ratio puts the modulator's sidebands there
        Rig t;
        auto toneOut = t.strike (tone, [] (int* s) { s[pPitch] = 64; s[pShape] = 0; s[pSweep] = 0; s[pContour] = 0; s[pDecay] = 110; });
        const double noteF = 440.0 * std::pow (2.0, ((24 + juce::roundToInt (64.0 / 127.0 * 72.0)) - 69) / 12.0);
        check (std::abs (strongest (toneOut, 0.05, 0.4, 100.0, 1000.0) / noteF - 1.0) < 0.01, "tone: PITCH is a note (64: F#4)", strongest (toneOut, 0.05, 0.4, 100.0, 1000.0), noteF);
        check (valueText (tone, pColor, 0) == "0.5 : 1" && valueText (tone, pColor, 127) == "8.0 : 1", "tone: COLOR picks a ratio from 0.5:1 to 8:1", 1, 1);

        // CHORD: its notes
        Rig c;
        auto chordOut = c.strike (chord, [] (int* s) { s[pPitch] = 64; s[pColor] = 0; s[pShape] = 0; s[pSweep] = 0; s[pContour] = 0; s[pDecay] = 110; });
        const double root = 440.0 * std::pow (2.0, ((36 + 24) - 69) / 12.0);   // C4
        const double third = root * std::pow (2.0, 4.0 / 12.0), fifth = root * std::pow (2.0, 7.0 / 12.0);
        auto near = [&] (double hz) { return std::abs (strongest (chordOut, 0.05, 0.5, hz * 0.97, hz * 1.03) / hz - 1.0) < 0.01; };
        check (near (root) && near (third) && near (fifth) && valueText (chord, pColor, 0) == "MAJ", "chord: COLOR 0 is a major chord on PITCH (C4, E4, G4)", 1, 1);
    }

    // ---------------------------------------------------------------- the sequencer
    {
        Rig r;
        auto& s = r.p.state();
        s.tempo = 1200;
        for (int k = 0; k < 16; k += 4) s.patterns[0].steps[0][k].on = 1;   // the kick on the beats
        s.sound[0][pDecay] = 10; s.sound[0][pSweep] = 0; s.sound[0][pContour] = 0;
        r.p.changed();
        r.run (0.02);
        r.p.setPlaying (true);
        auto out = r.run (2.1);
        const double a = onset (out, 0.0), b = onset (out, a + 0.1), c = onset (out, b + 0.1);
        check (std::abs ((b - a) - 0.5) < 0.003 && std::abs ((c - b) - 0.5) < 0.003, "sequencer: 120 BPM, a hit every 4 steps: every 0.5 s", b - a, 0.5);

        // swing 75: the off-beat sixteenths half a step late
        r.p.setPlaying (false);
        r.run (0.1);
        for (auto& st : s.patterns[0].steps[0]) st.on = 0;
        s.patterns[0].steps[0][0].on = 1; s.patterns[0].steps[0][1].on = 1;
        s.sound[0][pMachine] = metal; s.sound[0][pDecay] = 0;   // short, so each hit stands apart
        s.swing = 75;
        r.p.changed();
        r.run (0.02);
        r.p.setPlaying (true);
        out = r.run (0.4);
        const double s0 = onset (out, 0.0), s1 = onset (out, s0 + 0.05);
        check (std::abs ((s1 - s0) - 0.125 * 1.5) < 0.003, "sequencer: swing 75 %: the second sixteenth half a step late", s1 - s0, 0.1875);
        r.p.setPlaying (false);
        r.run (0.1);
    }
    {
        // locks: a step's own pitch; chance 0 never plays; retrig: four hits in the step
        Rig r;
        auto& s = r.p.state();
        s.sound[0][pMachine] = tone; s.sound[0][pPitch] = 40; s.sound[0][pDecay] = 40; s.sound[0][pShape] = 0; s.sound[0][pSweep] = 0; s.sound[0][pContour] = 0;
        s.patterns[0].length = 8;
        s.patterns[0].steps[0][0].on = 1;
        s.patterns[0].steps[0][4].on = 1; s.patterns[0].steps[0][4].lock[pPitch] = 76;
        r.p.changed();
        r.run (0.02);
        r.p.setPlaying (true);
        auto out = r.run (1.0);
        const double f0 = strongest (out, 0.01, 0.12, 60.0, 2000.0), f4 = strongest (out, 0.51, 0.62, 60.0, 2000.0);
        const double want0 = 440.0 * std::pow (2.0, ((24 + juce::roundToInt (40.0 / 127.0 * 72.0)) - 69) / 12.0);
        const double want4 = 440.0 * std::pow (2.0, ((24 + juce::roundToInt (76.0 / 127.0 * 72.0)) - 69) / 12.0);
        check (std::abs (f0 / want0 - 1.0) < 0.01 && std::abs (f4 / want4 - 1.0) < 0.01, "locks: step 5's PITCH lock plays its own note; step 1 the track's", f4, want4);
        check (r.p.playingStep() >= 0, "sequencer: the window sees the step playing", r.p.playingStep(), 1);
        r.p.setPlaying (false);
        r.run (0.1);

        for (auto& st : s.patterns[0].steps[0]) st = Step();
        s.patterns[0].length = 16;
        s.patterns[0].steps[0][0].on = 1; s.patterns[0].steps[0][0].chance = 1;
        s.sound[0][pMachine] = perc; s.sound[0][pDecay] = 5;
        int heard = 0;
        r.p.changed();
        r.run (0.05);
        r.p.setPlaying (true);
        for (int bar = 0; bar < 20; ++bar) { auto o = r.run (0.5); if (rms (o, 0.0, 0.05) > 0.003) ++heard; }
        check (heard <= 3, "chance: 1 % plays (almost) never in 20 bars", heard, 0);
        r.p.setPlaying (false);
        r.run (0.1);

        s.patterns[0].steps[0][0].chance = 100; s.patterns[0].steps[0][0].retrig = 3;
        s.sound[0][pMachine] = metal; s.sound[0][pDecay] = 0;
        s.tempo = 400;   // 40 BPM: a step is 375 ms, the hits 94 ms apart
        r.p.changed();
        r.run (0.05);
        r.p.setPlaying (true);
        out = r.run (0.37);
        int hitsSeen = 0;
        double last = -1.0;
        for (double tt = onset (out, 0.0, 0.05); tt >= 0.0; tt = onset (out, last + 0.02, 0.05)) { ++hitsSeen; last = tt; if (hitsSeen > 8) break; }
        check (hitsSeen == 4, "retrig: 3 more hits in the step: four", hitsSeen, 4);
        r.p.setPlaying (false);
        r.run (0.1);
    }
    {
        // a pattern change waits for the end of the one playing
        Rig r;
        auto& s = r.p.state();
        s.sound[0][pMachine] = perc; s.sound[0][pDecay] = 5;
        s.sound[1][pMachine] = perc; s.sound[1][pDecay] = 5; s.sound[1][pPitch] = 120;
        s.patterns[0].steps[0][0].on = 1;
        s.patterns[1].steps[1][12].on = 1;  // 1.5 s into its bar
        r.p.changed();
        r.run (0.02);
        r.p.setPlaying (true);
        r.run (0.6);                       // into the bar (2 s at 120)
        s.pattern = 1;
        r.p.changed();
        auto out = r.run (3.2);            // the rest of the bar (1.4 s), then pattern 2's: its hit 1.5 s in
        const double hitAt = onset (out, 0.0, 0.003);
        check (std::abs (hitAt - 2.9) < 0.02, "patterns: a new pattern starts at the end of the bar playing", hitAt, 2.9);
        r.p.setPlaying (false);
        r.run (0.1);
    }
    {
        // MIDI: C1 to F1 play the six tracks
        Rig r;
        for (int t : { 0, 2 }) { r.p.state().sound[t][pMachine] = perc; r.p.state().sound[t][pDecay] = 0; }
        r.p.changed();
        auto out = r.run (0.5, { { 0.1, juce::MidiMessage::noteOn (1, 36, (juce::uint8) 120) }, { 0.3, juce::MidiMessage::noteOn (1, 38, (juce::uint8) 120) } });
        check (std::abs (onset (out, 0.0) - 0.1) < 0.006 && std::abs (onset (out, 0.25, 0.003) - 0.3) < 0.006, "MIDI: C1 plays track 1, D1 track 3, when they come", onset (out, 0.0), 0.1);
    }
    {
        // the delay repeats a dotted eighth later at 120 BPM (0.375 s); the reverb rings on
        Rig r;
        auto& s = r.p.state();
        s.sound[3][pMachine] = perc; s.sound[3][pDecay] = 3; s.sound[3][pDelay] = 127;
        s.delayTime = d8dot; s.delayFeedback = 40;
        r.p.changed();
        r.run (0.02);
        r.p.hit (3, 127);
        auto out = r.run (1.0);
        const double first = onset (out, 0.0, 0.01), echo = onset (out, first + 0.1, 0.01);
        check (std::abs ((echo - first) - 0.375) < 0.004, "delay: 3/16 at 120 BPM: the echo 0.375 s after", echo - first, 0.375);
        Rig q;
        auto& s2 = q.p.state();
        s2.sound[3][pMachine] = perc; s2.sound[3][pDecay] = 3; s2.sound[3][pReverb] = 127; s2.reverbDecay = 100;
        q.p.changed();
        q.run (0.02);
        q.p.hit (3, 127);
        out = q.run (1.5);
        check (rms (out, 0.6, 1.0) > 1.0e-3, "reverb: rings on after the hit has died", rms (out, 0.6, 1.0), 0.01);
    }
    {
        // the state, as it went
        Rig r;
        auto& s = r.p.state();
        s.tempo = 987; s.swing = 61; s.sound[4][pColor] = 33; s.mute[2] = true;
        s.patterns[5].length = 48; s.patterns[5].steps[3][47].on = 1; s.patterns[5].steps[3][47].lock[pDecay] = 12; s.patterns[5].steps[3][47].retrig = 2;
        auto x = r.p.toXml (false);
        Processor q;
        q.fromXml (*x);
        auto& t = q.state();
        check (t.tempo == 987 && t.swing == 61 && t.sound[4][pColor] == 33 && t.mute[2] && t.patterns[5].length == 48
               && t.patterns[5].steps[3][47].on && t.patterns[5].steps[3][47].lock[pDecay] == 12 && t.patterns[5].steps[3][47].retrig == 2,
               "state: the kit, the patterns and their locks come back", 1, 1);
    }

    if (auto* folder = std::getenv ("TF_FMDRUMS_RENDER"))
    {
        juce::File dir (folder);
        dir.createDirectory();
        auto write = [&] (const juce::String& name, const std::vector<float>& v)
        {
            auto f = dir.getChildFile (name + ".wav");
            f.deleteFile();
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
            juce::WavAudioFormat wav;
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (1).withBitsPerSample (24));
            const float* ch[] { v.data() };
            w->writeFromFloatArrays (ch, 1, (int) v.size());
        };
        for (int m = 0; m < numMachines; ++m) { Rig r; write (machineName (m), r.strike (m, nullptr, 1.5)); }
        Processor p;
        p.setRateAndBufferSizeDetails (sr, bs);
        p.prepareToPlay (sr, bs);
        p.setPlaying (true);
        std::vector<float> out;
        juce::AudioBuffer<float> b (2, bs);
        for (int k = 0; k < (int) (8.0 * sr / bs); ++k) { juce::MidiBuffer m; p.processBlock (b, m); out.insert (out.end(), b.getReadPointer (0), b.getReadPointer (0) + bs); }
        write ("pattern", out);
    }

    std::printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
