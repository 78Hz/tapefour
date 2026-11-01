// TF N5 (Source/N5Synth.h): the Korg N5's programs, combinations, effects and arpeggiator.
//
//   The parameter tables against the manual's ranges and initial values; pitch, octaves and
//   fine tune; the voices (64, 32 in DOUBLE); the VDA EG's attack; the VDF; velocity windows;
//   mono legato; a split combination; the arpeggiator; drawn waves; all 48 effect types (finite,
//   and the delays' times); the effects' placements; the state round trip.
//
// With TF_N5_RENDER=<folder> it also writes WAVs to listen to.

#include "../Source/N5Synth.h"

#include <cstdio>
#include <cstdlib>

using namespace tf;
using namespace tf::n5;

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
        if (annotate != nullptr) std::printf ("::error::N5Test: %s (got %.3f, want %.3f)\n", what, got, want);
    }
}

constexpr double sr = 48000.0;
constexpr int bs = 256;

struct Rig
{
    Processor p;
    Rig() { p.setRateAndBufferSizeDetails (sr, bs); p.prepareToPlay (sr, bs); }
    ProgramData& prog() { return p.bank().programs[(size_t) p.bank().program]; }
    void dryFx()
    {
        // (no effects, and no sends: in PARA.1 a send with no effect on it passes its sound on, as the dry part of the balance)
        for (auto& pr : p.bank().programs) { pr.fx.setType (0, 0); pr.fx.setType (1, 0); pr.fx.placement = 1; for (auto& l : pr.layer) l[xSendC] = l[xSendD] = 0; }
        for (auto& c : p.bank().combis) { c.fx.setType (0, 0); c.fx.setType (1, 0); }
        p.changed();
    }
    std::vector<float> run (double seconds, std::vector<std::pair<double, juce::MidiMessage>> events = {}, std::vector<float>* right = nullptr)
    {
        std::vector<float> out;
        juce::AudioBuffer<float> b (2, bs);
        const int blocks = (int) std::ceil (seconds * sr / bs);
        size_t e = 0;
        for (int k = 0; k < blocks; ++k)
        {
            b.clear();
            juce::MidiBuffer m;
            while (e < events.size() && events[e].first * sr < (k + 1) * bs)
            {
                m.addEvent (events[e].second, std::clamp ((int) (events[e].first * sr) - k * bs, 0, bs - 1));
                ++e;
            }
            p.processBlock (b, m);
            out.insert (out.end(), b.getReadPointer (0), b.getReadPointer (0) + bs);
            if (right != nullptr) right->insert (right->end(), b.getReadPointer (1), b.getReadPointer (1) + bs);
        }
        return out;
    }
};

juce::MidiMessage on (int n, int v = 100) { return juce::MidiMessage::noteOn (1, n, (juce::uint8) v); }
juce::MidiMessage off (int n) { return juce::MidiMessage::noteOff (1, n); }

double rms (const std::vector<float>& v, double from, double to)
{
    const auto a = std::min (v.size(), (size_t) (from * sr)), b = std::min (v.size(), (size_t) (to * sr));
    double s = 0;
    for (auto i = a; i < b; ++i) s += (double) v[i] * v[i];
    return std::sqrt (s / (double) std::max<size_t> (1, b - a));
}

double frequency (const std::vector<float>& v, double from, double to)
{
    const auto a = (size_t) (from * sr), b = std::min (v.size(), (size_t) (to * sr));
    double first = -1, last = -1;
    int crossings = 0;
    for (auto i = a + 1; i < b; ++i)
        if (v[i - 1] <= 0.0f && v[i] > 0.0f)
        {
            const double at = (double) (i - 1) + v[i - 1] / (v[i - 1] - v[i]);
            if (first < 0) first = at; else ++crossings;
            last = at;
        }
    return crossings > 0 ? crossings * sr / (last - first) : 0.0;
}

bool finite (const std::vector<float>& v) { for (auto x : v) if (! std::isfinite (x) || std::abs (x) > 50.0f) return false; return true; }

void write (const juce::String& name, const std::vector<float>& v)
{
    if (const char* dir = std::getenv ("TF_N5_RENDER"))
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
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    // ---------------------------------------------------------------- the tables
    {
        bool ok = true;
        for (int i = 0; i < numLayer; ++i) { const auto& f = layerField (i); ok = ok && f.def >= f.lo && f.def <= f.hi; }
        for (int i = 0; i < numGlobal; ++i) { const auto& f = globalField (i); ok = ok && f.def >= f.lo && f.def <= f.hi; }
        check (ok, "tables: every program parameter's initial value is in its range", ok ? 1 : 0, 1);
        int types = 0;
        bool fxOk = true;
        for (int t = 1; t < numEffectTypes; ++t)
        {
            const auto& et = effectType (t);
            if (et.count > 0) ++types;
            for (int k = 0; k < et.count; ++k) fxOk = fxOk && et.params[k].def >= et.params[k].lo && et.params[k].def <= et.params[k].hi;
        }
        check (types == 48, "tables: 48 effect types (the manual's)", types, 48);
        check (fxOk, "tables: every effect parameter's initial value is in its range", fxOk ? 1 : 0, 1);
        check (juce::String (effectType (9).name) == "SPRING REVERB" && juce::String (effectType (48).name) == "RESONANCE FILTER", "tables: types 9 and 48 are the manual's", 1, 1);
        check (std::abs (speedHz (0) - 0.03) < 1e-9 && std::abs (speedHz (99) - 30.0) < 1e-6, "tables: mod speeds 0.03 to 30 Hz", speedHz (99), 30.0);
    }

    // ---------------------------------------------------------------- pitch
    {
        Rig r;
        r.dryFx();
        auto out = r.run (1.0, { { 0.0, on (69) }, { 0.9, off (69) } });
        write ("a440_saw", out);
        check (std::abs (frequency (out, 0.2, 0.8) - 440.0) < 1.0, "pitch: A4 is 440 Hz at 8'", frequency (out, 0.2, 0.8), 440.0);
        r.prog().layer[0][oOctave] = 1;   // 16'
        r.prog().layer[0][oWave] = sine;
        r.p.changed();
        out = r.run (1.0, { { 0.0, on (69) }, { 0.9, off (69) } });
        check (std::abs (frequency (out, 0.2, 0.8) - 220.0) < 0.5, "pitch: 16' an octave down", frequency (out, 0.2, 0.8), 220.0);
        r.prog().layer[0][oOctave] = 2;
        r.prog().layer[0][oFine] = 50;
        r.p.changed();
        out = r.run (1.0, { { 0.0, on (69) }, { 0.9, off (69) } });
        const double cents = 1200.0 * std::log2 (frequency (out, 0.2, 0.8) / 440.0);
        check (std::abs (cents - 50.0) < 2.0, "pitch: fine tune +50 is 50 cents", cents, 50.0);
        r.prog().layer[0][oFine] = 0;
        r.prog().layer[0][oSlope] = 0;   // pitch slope 0.0: every key the same pitch
        r.p.changed();
        out = r.run (1.0, { { 0.0, on (81) }, { 0.9, off (81) } });
        check (std::abs (frequency (out, 0.2, 0.8) - 261.63) < 1.0, "pitch: slope 0.0 plays every key at C4", frequency (out, 0.2, 0.8), 261.63);
    }

    // ---------------------------------------------------------------- voices
    {
        Rig r;
        r.dryFx();
        r.prog().layer[0][aeSustain] = 127;
        r.p.changed();
        std::vector<std::pair<double, juce::MidiMessage>> ev;
        for (int k = 0; k < 70; ++k) ev.push_back ({ 0.001 * k, on (30 + k) });
        r.run (0.2, ev);
        check (r.p.voicesSounding() == 64, "voices: 70 notes, single: 64 sound", r.p.voicesSounding(), 64);
        Rig d;
        d.dryFx();
        d.prog().global[gOscMode] = 1;
        d.p.changed();
        d.run (0.2, ev);
        check (d.p.voicesSounding() == 64, "voices: double: still 64 oscillators (32 notes)", d.p.voicesSounding(), 64);
    }

    // ---------------------------------------------------------------- envelopes, filter, velocity
    {
        Rig r;
        r.dryFx();
        r.prog().layer[0][aeAttackTime] = 80;   // egSeconds(80) = 0.66 s
        r.prog().layer[0][aeDecayTime] = 127;
        r.p.changed();
        auto out = r.run (1.5, { { 0.0, on (60) } });
        const double att = egSeconds (80);
        check (rms (out, 0.0, att * 0.3) < rms (out, att * 0.9, att * 1.1) * 0.5, "VDA EG: the attack rises over its time", rms (out, att * 0.9, att * 1.1), 0.1);
        Rig f;
        f.dryFx();
        f.prog().layer[0][fCutoff] = 127;
        f.p.changed();
        const auto bright = f.run (0.5, { { 0.0, on (48) } });
        Rig g;
        g.dryFx();
        g.prog().layer[0][fCutoff] = 40;
        g.p.changed();
        const auto dark = g.run (0.5, { { 0.0, on (48) } });
        check (rms (dark, 0.1, 0.4) < rms (bright, 0.1, 0.4) * 0.6, "VDF: a low cutoff takes the saw's harmonics off", rms (dark, 0.1, 0.4) / rms (bright, 0.1, 0.4), 0.3);
        write ("bright", bright);
        write ("dark", dark);
        Rig w;
        w.dryFx();
        w.prog().layer[0][oVelBottom] = 64;
        w.p.changed();
        const auto soft = w.run (0.3, { { 0.0, on (60, 40) } });
        const auto hard = w.run (0.3, { { 0.0, on (62, 100) } });
        check (rms (soft, 0.05, 0.25) < 1e-6 && rms (hard, 0.05, 0.25) > 0.01, "velocity window: a note below its bottom doesn't sound", rms (soft, 0.05, 0.25), 0.0);
    }

    // ---------------------------------------------------------------- mono
    {
        Rig r;
        r.dryFx();
        r.prog().global[gMonoPoly] = 0;
        r.prog().layer[0][oWave] = sine;
        r.p.changed();
        auto out = r.run (1.2, { { 0.0, on (57) }, { 0.4, on (69) }, { 0.8, off (69) } });
        check (r.p.voicesSounding() == 1, "mono: one voice for overlapping notes", r.p.voicesSounding(), 1);
        check (std::abs (frequency (out, 0.5, 0.75) - 440.0) < 1.0 && std::abs (frequency (out, 0.9, 1.15) - 220.0) < 1.0,
               "mono: the last note plays, and back to the held one when it's let go (Hz)", frequency (out, 0.9, 1.15), 220.0);
    }

    // ---------------------------------------------------------------- a combination
    {
        Rig r;
        r.dryFx();
        auto& b = r.p.bank();
        b.programs[1].layer[0][oWave] = sine;
        b.programs[1].layer[0][oOctave] = 1;
        b.combiMode = 1;
        auto& c = b.combis[0];
        c.timbre[0][tProgram] = 0; c.timbre[0][tNoteLo] = 60;
        c.timbre[1][tProgram] = 1; c.timbre[1][tNoteHi] = 59;
        r.p.changed();
        r.run (0.1, { { 0.0, on (48) } });
        check (r.p.voicesSounding() == 1, "combination: below the split, one timbre", r.p.voicesSounding(), 1);
        auto out = r.run (0.6, { { 0.0, off (48) }, { 0.05, on (45) } });
        check (std::abs (frequency (out, 0.3, 0.55) - 55.0) < 0.5, "combination: the lower timbre's program (16' sine): A2 at 55 Hz", frequency (out, 0.3, 0.55), 55.0);
    }

    // ---------------------------------------------------------------- the arpeggiator
    {
        Rig r;
        r.dryFx();
        auto& b = r.p.bank();
        b.arp[aOn] = 1; b.arp[aType] = 1; b.arp[aSpeed] = 120; b.arp[aStep] = 4; b.arp[aGate] = 50; b.arp[aOctave] = 1;
        r.prog().layer[0][oWave] = sine;
        r.prog().layer[0][aeReleaseTime] = 0;
        r.p.changed();
        auto out = r.run (2.2, { { 0.0, on (60) }, { 0.0, on (64) }, { 0.0, on (67) } });
        write ("arp_up", out);
        // 1/16 at 120 BPM: a note every 125 ms, C E G C E G...
        const double f1 = frequency (out, 0.01, 0.06), f2 = frequency (out, 0.135, 0.185), f3 = frequency (out, 0.26, 0.31), f4 = frequency (out, 0.385, 0.435);
        check (std::abs (f1 - 261.6) < 3 && std::abs (f2 - 329.6) < 3 && std::abs (f3 - 392.0) < 3 && std::abs (f4 - 261.6) < 3,
               "arpeggiator: UP plays C E G C, a 16th apart at 120 BPM (4th note, Hz)", f4, 261.6);
        check (rms (out, 0.08, 0.12) < rms (out, 0.01, 0.05) * 0.1, "arpeggiator: gate 50 %: silent in the second half of each step", rms (out, 0.08, 0.12), 0.0);
    }

    // ---------------------------------------------------------------- drawn waves
    {
        Rig r;
        std::array<float, drawnPoints> sq {};
        for (int i = 0; i < drawnPoints; ++i) sq[(size_t) i] = i < drawnPoints / 2 ? 0.8f : -0.8f;
        r.p.setDrawnWave (2, sq);
        const auto h = r.p.harmonicsOf (2);
        check (h[1] < 0.02f && h[3] < 0.02f && std::abs (h[2] - 1.0f / 3.0f) < 0.02f, "drawn waves: a drawn square has odd harmonics (3rd at 1/3)", h[2], 1.0 / 3.0);
        r.dryFx();
        r.prog().layer[0][oWave] = drawn;
        r.prog().layer[0][oDrawn] = 3;
        r.p.changed();
        auto out = r.run (0.5, { { 0.0, on (69) } });
        check (std::abs (frequency (out, 0.1, 0.45) - 440.0) < 1.0, "drawn waves: it plays at the note's pitch", frequency (out, 0.1, 0.45), 440.0);
    }

    // ---------------------------------------------------------------- every effect type
    {
        int okCount = 0;
        for (int t = 1; t < numEffectTypes; ++t)
        {
            Rig r;
            auto& fx = r.prog().fx;
            fx.placement = 1;
            fx.setType (0, t);
            fx.setType (1, 0);
            r.p.changed();
            auto out = r.run (1.0, { { 0.0, on (60) }, { 0.0, on (64) }, { 0.5, off (60) }, { 0.5, off (64) } });
            const bool ok = finite (out) && rms (out, 0.1, 0.5) > 1e-4;
            if (ok) ++okCount;
            else std::printf ("      effect %d %s: not finite or silent\n", t, effectType (t).name);
        }
        check (okCount == 48, "effects: all 48 types play, finite", okCount, 48);
        // the stereo delay: its left repeats at Delay Time L
        Rig r;
        auto& fx = r.prog().fx;
        fx.setType (0, 13);
        fx.unit[0].p[2] = 0;     // no feedback
        fx.unit[0].p[6] = 100;   // effect only
        fx.setType (1, 0);
        r.prog().layer[0][aeAttackTime] = 0; r.prog().layer[0][aeDecayTime] = 10; r.prog().layer[0][aeBreak] = 0; r.prog().layer[0][aeSustain] = 0;
        r.prog().layer[0][aeSlopeTime] = 0;
        r.p.changed();
        auto out = r.run (0.6, { { 0.0, on (60) }, { 0.005, off (60) } });
        double peakAt = 0, peak = 0;
        for (size_t i = 0; i < out.size(); ++i) if (std::abs (out[i]) > peak) { peak = std::abs (out[i]); peakAt = (double) i / sr; }
        check (std::abs (peakAt - 0.185) < 0.01, "effects: STEREO DELAY's left repeat at 185 ms (s)", peakAt, 0.185);
        // PARA.3: A/B passes dry with no sends
        Rig q;
        auto& f3 = q.prog().fx;
        f3.placement = 3;
        f3.setType (0, 1); f3.setType (1, 19);
        q.prog().layer[0][xSendC] = 0; q.prog().layer[0][xSendD] = 0;
        q.p.changed();
        Rig dry;
        dry.dryFx();
        const auto a = q.run (0.3, { { 0.0, on (60) } }), b = dry.run (0.3, { { 0.0, on (60) } });
        check (std::abs (rms (a, 0.05, 0.25) / rms (b, 0.05, 0.25) - 1.0) < 0.05, "placement: PARA.3 with no sends is the dry sound", rms (a, 0.05, 0.25) / rms (b, 0.05, 0.25), 1.0);
    }

    // ---------------------------------------------------------------- the state
    {
        Rig r;
        r.prog().layer[0][fCutoff] = 33;
        r.p.bank().combis[3].timbre[2][tTranspose] = -7;
        r.p.bank().arp[aType] = 14;
        r.p.setProgramName (0, "GLASSPAD");
        std::array<float, drawnPoints> w {};
        for (int i = 0; i < drawnPoints; ++i) w[(size_t) i] = (float) std::sin (6.283185307 * i / drawnPoints * 2) * 0.5f;
        r.p.setDrawnWave (5, w);
        r.p.changed();
        juce::MemoryBlock mb;
        r.p.getStateInformation (mb);
        Processor q;
        q.setStateInformation (mb.getData(), (int) mb.getSize());
        const bool same = q.bank().programs[0].layer[0][fCutoff] == 33 && q.bank().combis[3].timbre[2][tTranspose] == -7 && q.bank().arp[aType] == 14
                       && q.programName (0) == "GLASSPAD" && std::abs (q.drawnWave (5)[64] - w[64]) < 1e-3f;
        check (same, "state: programs, combinations, arpeggiator, names and drawn waves come back", same ? 1 : 0, 1);
    }

    std::printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
