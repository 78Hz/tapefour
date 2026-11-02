// TF PHRASE SAMPLER (Source/PhraseSampler.h): after the Yamaha SU10.
//
//   The four grades' clocks and recording times, the ADPCM, the polyphony table, recording from the
//   input, the tape and a file, playing (pitch, loop modes, NOTE / TOGGLE / TRIGGER, alternate groups,
//   HOLD, REV), the voices cut, the ribbon's effects and the grades they're allowed at, songs, the edits
//   (copy, move, split, compact, delete) and the whole memory's round trip; the other tracks' outputs and the
//   mix as sources; the loop tempo as the unit shows it and nudged; the front panel's modes (SHIFT's
//   functions, EDIT, recording, UTILITY).
//
// With TF_SAMPLER_RENDER=<folder> it also writes WAVs to listen to.

#include "../Source/PhraseSampler.h"
#include "../Source/PhraseSamplerFront.h"
#include "../Source/EditorHosting.h"

#include <cstdio>
#include <cstdlib>

using namespace tf;
using namespace tf::sampler;

namespace
{
int failures = 0;
const char* annotate = std::getenv ("GITHUB_ACTIONS");

void check (bool ok, const char* what, double got, double want)
{
    std::printf ("%s  %-70s got %10.3f  want %10.3f\n", ok ? "PASS" : "FAIL", what, got, want);
    if (! ok)
    {
        ++failures;
        if (annotate != nullptr) std::printf ("::error::PhraseSamplerTest: %s (got %.3f, want %.3f)\n", what, got, want);
    }
}

constexpr double sr = 48000.0, twoPi = 6.283185307179586;
constexpr int bs = 256;

struct Rig
{
    Processor p;
    double t = 0.0;   // the input's sine phase (s)
    std::vector<float> tape;
    Rig()
    {
        p.setRateAndBufferSizeDetails (sr, bs);
        p.prepareToPlay (sr, bs);
    }
    void set (const char* id, float v) { auto* r = p.param (id); r->setValueNotifyingHost (r->convertTo0to1 (v)); }
    /** Runs, with a sine of inHz at inLevel on the input (0: silence); returns the left channel. */
    std::vector<float> run (double seconds, double inHz = 0.0, double inLevel = 0.0, bool onTape = false,
                            std::vector<std::pair<double, juce::MidiMessage>> events = {})
    {
        std::vector<float> out;
        juce::AudioBuffer<float> b (2, bs);
        std::vector<float> tapeBlock ((size_t) bs), silent ((size_t) bs, 0.0f);
        const int blocks = (int) std::lround (seconds * sr / bs);
        size_t e = 0;
        for (int k = 0; k < blocks; ++k)
        {
            for (int i = 0; i < bs; ++i)
            {
                const float x = inHz > 0.0 ? (float) (inLevel * std::sin (twoPi * inHz * (t + i / sr))) : 0.0f;
                b.setSample (0, i, onTape ? 0.0f : x);
                b.setSample (1, i, onTape ? 0.0f : x);
                tapeBlock[(size_t) i] = onTape ? x : 0.0f;
            }
            t += bs / sr;
            TrackFeed feed;
            feed.numTracks = 4;
            feed.ownTrack = 0;
            for (int q = 0; q < 4; ++q)
            {
                feed.tape[q] = feed.preFader[q] = silent.data();
                feed.postFader[q] = q == 2 ? tapeBlock.data() : silent.data();
                feed.heard[q] = true;
            }
            p.tapeFeed (feed);
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
    /** Records seconds of a sine into a pad, from the input (or track 3 of the tape). */
    bool record (int pad, double seconds, double hz, double level = 0.5, bool stereo = false, bool onTape = false)
    {
        auto r = p.recordSettings();
        r.toPad = pad; r.stereo = stereo; r.trigger = 0; r.gain = 8;
        r.source = onTape ? Source::track : Source::input;
        r.track = 2;
        p.setRecordSettings (r);
        juce::String error;
        if (! p.armRecord (error)) return false;
        run (seconds, hz, level, onTape);
        p.stopRecord();
        return p.pad (pad).used;
    }
    std::vector<float> play (int padInBank, double seconds, double releaseAt = -1.0)
    {
        p.pressPad (padInBank, 1.0f);
        if (releaseAt < 0.0) return run (seconds);
        auto a = run (releaseAt);
        p.releasePad (padInBank);
        auto b = run (seconds - releaseAt);
        a.insert (a.end(), b.begin(), b.end());
        return a;
    }
};

double rms (const std::vector<float>& v, double from, double to)
{
    const auto a = std::min (v.size(), (size_t) (from * sr)), b = std::min (v.size(), (size_t) (to * sr));
    double s = 0;
    for (auto i = a; i < b; ++i) s += (double) v[i] * v[i];
    return std::sqrt (s / (double) std::max<size_t> (1, b - a));
}

/** Frequency by counting upward zero crossings (with a little hysteresis). */
double frequency (const std::vector<float>& v, double from, double to)
{
    const auto a = (size_t) (from * sr), b = std::min (v.size(), (size_t) (to * sr));
    double first = -1, last = -1;
    int crossings = 0;
    bool low = false;
    for (auto i = a + 1; i < b; ++i)
    {
        if (v[i] < -0.01f) low = true;
        if (low && v[i - 1] <= 0.0f && v[i] > 0.0f)
        {
            const double at = (double) (i - 1) + v[i - 1] / (v[i - 1] - v[i]);
            if (first < 0) first = at; else ++crossings;
            last = at;
            low = false;
        }
    }
    return crossings > 0 ? crossings * sr / (last - first) : 0.0;
}

void write (const juce::String& name, const std::vector<float>& v)
{
    if (const char* dir = std::getenv ("TF_SAMPLER_RENDER"))
    {
        juce::File f = juce::File (dir).getChildFile (name + ".wav");
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
        if (auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (1).withBitsPerSample (24)))
        {
            const float* ch[] { v.data() };
            writer->writeFromFloatArrays (ch, 1, (int) v.size());
        }
    }
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    // ---------------------------------------------------------------- the grades
    check (std::abs (clockHz (Grade::hi, 10.0) - 44100.0) < 1.0e-6, "grades: HI at +10 % is 44.1 kHz (Hz)", clockHz (Grade::hi, 10.0), 44100.0);
    {
        // the owner's manual's times for the whole memory, mono, at pitch 0
        const double want[4] { 19.0, 27.0, 38.0, 54.0 };
        const char* what[4] { "grades: HI holds about 19 s (s)", "grades: STD about 27 s (s)", "grades: LONG about 38 s (s)", "grades: EXLG about 54 s (s)" };
        for (int g = 0; g < 4; ++g)
        {
            const double s = memorySeconds ((Grade) g);
            check (std::abs (s - want[g]) / want[g] < 0.04, what[g], s, want[g]);
        }
    }
    check (std::abs (clockHz (Grade::lng, -20.0) / clockHz (Grade::lng, 0.0) - 0.8) < 1.0e-9, "grades: pitch -20 % slows the clock to 0.8", clockHz (Grade::lng, -20.0) / clockHz (Grade::lng, 0.0), 0.8);

    // ---------------------------------------------------------------- the polyphony table
    {
        struct Row { Grade g; Effect e; bool scr; int want; const char* what; };
        const Row rows[] {
            { Grade::hi, Effect::none, false, 2, "voices: HI, normal" },             { Grade::std, Effect::none, false, 4, "voices: STD, normal" },
            { Grade::lng, Effect::none, false, 4, "voices: LONG, normal" },          { Grade::hi, Effect::pitch, false, 0, "voices: HI has no ribbon effects (pitch)" },
            { Grade::std, Effect::filter, false, 0, "voices: STD has no FILTER" },    { Grade::std, Effect::scale, false, 0, "voices: STD has no SCALE" },
            { Grade::std, Effect::crossfade, false, 2, "voices: STD, crossfade" },    { Grade::lng, Effect::filter, false, 2, "voices: LONG, filter" },
            { Grade::exlg, Effect::pitch, false, 4, "voices: EXLG, pitch" },          { Grade::std, Effect::none, true, 2, "voices: STD, scratch" },
            { Grade::hi, Effect::none, true, 0, "voices: HI has no SCRATCH" } };
        for (auto& r : rows) check (polyphony (r.g, r.e, r.scr) == r.want, r.what, polyphony (r.g, r.e, r.scr), r.want);
    }

    // ---------------------------------------------------------------- the ADPCM
    {
        Adpcm enc, dec;
        double sig = 0, err = 0;
        for (int i = 0; i < 20000; ++i)
        {
            const int x = (int) (12000.0 * std::sin (twoPi * 440.0 * i / 29157.0));
            const int y = dec.decode (enc.encode (x));
            if (i > 200) { sig += (double) x * x; err += (double) (x - y) * (x - y); }
        }
        const double snr = 10.0 * std::log10 (sig / std::max (1.0, err));
        check (snr > 18.0 && snr < 60.0, "ADPCM: 4 bits a sample, a sine comes back, not perfectly (SNR dB)", snr, 40.0);
        Adpcm a;
        int s = 0;
        for (int i = 0; i < 100; ++i) s = a.decode (7);
        check (a.step == 24576 && s == 32767, "ADPCM: the step and signal clamp at their limits", a.step, 24576);
    }

    // ---------------------------------------------------------------- recording from the input and playing
    {
        Rig r;
        r.set ("grade", 2.0f);   // LONG
        check (r.record (0, 1.0, 440.0, 0.5), "record: a second of the input into A1", 1, 1);
        const auto pd = r.p.pad (0);
        check (std::abs (pd.frames - clockHz (Grade::lng, 0.0)) < 400.0, "record: it's as many frames as the clock gave (frames)", pd.frames, clockHz (Grade::lng, 0.0));
        check (r.p.memoryUsedNibbles() == pd.frames, "record: a mono frame is a nibble of memory", r.p.memoryUsedNibbles(), pd.frames);
        r.set ("volume", 127.0f);
        auto out = r.play (0, 0.9, 0.8);
        write ("a1-440", out);
        const double f = frequency (out, 0.1, 0.7);
        check (std::abs (f - 440.0) < 2.0, "play: it plays back at the pitch it went in (Hz)", f, 440.0);
        const double lvl = rms (out, 0.1, 0.7) * std::sqrt (2.0);
        check (std::abs (20.0 * std::log10 (lvl / 0.5)) < 1.5, "play: at volume 127 it comes back at the level it went in (dB)", 20.0 * std::log10 (lvl / 0.5), 0.0);
        check (rms (out, 0.82, 0.9) < 1.0e-3, "play: NOTE stops when the pad's let go", rms (out, 0.82, 0.9), 0.0);

        // the pitch moves the clock: -20 % plays it at 0.8
        r.set ("pitch", -20.0f);
        out = r.play (0, 0.6, 0.5);
        check (std::abs (frequency (out, 0.1, 0.45) - 352.0) < 2.0, "pitch: -20 % plays it a fifth-ish down (Hz)", frequency (out, 0.1, 0.45), 352.0);
        r.set ("pitch", 0.0f);
        // a sample made at LONG played at STD: the faster clock plays it higher
        r.set ("grade", 1.0f);
        out = r.play (0, 0.4, 0.3);
        check (std::abs (frequency (out, 0.05, 0.25) - 440.0 * 8.0 / 5.5) < 4.0, "grades: a LONG sample at STD's clock plays higher (Hz)", frequency (out, 0.05, 0.25), 440.0 * 8.0 / 5.5);
        r.set ("grade", 2.0f);

        // ONE SHOT plays to its end, held or not; WHOLE LOOP goes round
        out = r.play (0, 1.5);
        check (rms (out, 1.2, 1.5) < 1.0e-3, "loop: ONE SHOT stops at the end", rms (out, 1.2, 1.5), 0.0);
        r.p.releasePad (0); r.run (0.05);
        auto pd2 = r.p.pad (0); pd2.loop = Loop::wholeLoop; r.p.setPad (0, pd2);
        out = r.play (0, 2.5, 2.4);
        check (rms (out, 1.5, 2.3) > 0.2, "loop: WHOLE LOOP goes round", rms (out, 1.5, 2.3), 0.35);
        // TOGGLE: a second press stops it
        pd2.sounding = Sounding::toggle; r.p.setPad (0, pd2);
        r.p.pressPad (0); r.run (0.1); r.p.releasePad (0);
        out = r.run (0.5);
        check (rms (out, 0.1, 0.5) > 0.2, "sounding: TOGGLE carries on after it's let go", rms (out, 0.1, 0.5), 0.35);
        r.p.pressPad (0); r.run (0.05); r.p.releasePad (0);
        out = r.run (0.2);
        check (rms (out, 0.05, 0.2) < 1.0e-3, "sounding: TOGGLE's second press stops it", rms (out, 0.05, 0.2), 0.0);
        // HOLD latches a NOTE pad
        pd2.sounding = Sounding::note; r.p.setPad (0, pd2);
        r.p.pressKey ("HOLD"); r.run (0.01);
        out = r.play (0, 0.6, 0.1);
        check (rms (out, 0.3, 0.6) > 0.2, "HOLD: a NOTE pad goes on when it's let go", rms (out, 0.3, 0.6), 0.35);
        r.p.pressKey ("HOLD");
        out = r.run (0.1);
        check (rms (out, 0.05, 0.1) < 1.0e-3, "HOLD: off, the latched pad stops", rms (out, 0.05, 0.1), 0.0);

        // ---- the voices: LONG has four; a fifth cuts the oldest
        r.p.pressKey ("HOLD"); r.run (0.01);
        for (int k = 1; k <= 4; ++k) { juce::String e; r.p.copyPad (0, k, e); }
        for (int k = 0; k < 5; ++k) { r.p.pressPad (k); r.run (0.02); }
        check (r.p.panel().voices == 4, "voices: five pads at LONG, four sound", r.p.panel().voices, 4);
        r.p.pressKey ("HOLD"); r.p.pressKey ("EXIT");
        for (int k = 0; k < 5; ++k) r.p.releasePad (k);
        r.run (0.1);
        // ---- alternate groups
        for (int k = 0; k < 2; ++k) { auto q = r.p.pad (k); q.group = 1; q.sounding = Sounding::toggle; r.p.setPad (k, q); }
        r.p.pressPad (0); r.run (0.05); r.p.pressPad (1); r.run (0.05);
        check (r.p.panel().voices == 1, "groups: two pads in one group, one sounds", r.p.panel().voices, 1);
        r.p.pressPad (1); r.run (0.05);
        for (int k = 0; k < 2; ++k) { auto q = r.p.pad (k); q.group = 0; q.sounding = Sounding::note; r.p.setPad (k, q); }

        // ---- the effects at the grades
        r.set ("grade", 0.0f);   // HI
        r.p.pressKey ("FILTER"); r.run (0.01);
        check (r.p.panel().effect == Effect::none, "effects: FILTER can't be had at HI", (int) r.p.panel().effect, 0);
        r.set ("grade", 2.0f);
        r.p.pressKey ("FILTER"); r.run (0.01);
        check (r.p.panel().effect == Effect::filter, "effects: FILTER at LONG", (int) r.p.panel().effect, (int) Effect::filter);
        // the low-pass under the ribbon, near its left end, takes the 440 Hz down
        r.set ("filterType", 0.0f);
        r.p.setRibbon (0.05f, true);
        out = r.play (0, 0.5, 0.45);
        const double cut = rms (out, 0.1, 0.4);
        r.p.setRibbon (1.0f, true);
        out = r.play (0, 0.5, 0.45);
        const double open = rms (out, 0.1, 0.4);
        check (20.0 * std::log10 (cut / open) < -20.0, "FILTER: the ribbon's left end cuts 440 Hz (dB)", 20.0 * std::log10 (cut / open), -30.0);
        r.p.setRibbon (0.5f, false);
        r.p.pressKey ("EXIT");
        // PITCH from the ribbon: the right end is +10 %
        r.p.pressKey ("PITCH"); r.run (0.01);
        r.p.setRibbon (1.0f, true);
        out = r.play (0, 0.5, 0.45);
        check (std::abs (frequency (out, 0.1, 0.4) - 484.0) < 3.0, "PITCH: the ribbon's right end is +10 % (Hz)", frequency (out, 0.1, 0.4), 484.0);
        r.p.setRibbon (0.5f, false);
        r.p.pressKey ("EXIT");
        // SCALE: pad 1 is eleven semitones under pad 12
        r.p.pressKey ("SCALE"); r.run (0.01);
        r.p.pressPad (0); r.run (0.01); r.p.releasePad (0);   // the sample
        out = r.play (0, 0.5, 0.45);
        const double f1 = frequency (out, 0.1, 0.4);
        out = r.play (11, 0.5, 0.45);
        const double f12 = frequency (out, 0.1, 0.4);
        check (std::abs (1200.0 * std::log2 (f12 / f1) - 1100.0) < 10.0, "SCALE: pad 1 to pad 12 is eleven semitones (cents)", 1200.0 * std::log2 (f12 / f1), 1100.0);
        check (std::abs (f12 - 440.0) < 2.0, "SCALE: pad 12 is the sample's own pitch (Hz)", f12, 440.0);
        r.p.pressKey ("EXIT");
        // CROSSFADER: right end is all input
        r.p.pressKey ("CROSSFADER"); r.run (0.01);
        r.p.setRibbon (1.0f, true);
        out = r.run (0.3, 1000.0, 0.5);
        check (std::abs (frequency (out, 0.05, 0.3) - 1000.0) < 5.0, "CROSSFADER: the right end is the input (Hz)", frequency (out, 0.05, 0.3), 1000.0);
        r.p.setRibbon (0.0f, true);
        out = r.run (0.3, 1000.0, 0.5);
        check (rms (out, 0.05, 0.3) < 1.0e-3, "CROSSFADER: the left end is the samples, the input gone", rms (out, 0.05, 0.3), 0.0);
        r.p.setRibbon (0.5f, false);
        r.p.pressKey ("EXIT"); r.run (0.01);
        out = r.run (0.2, 1000.0, 0.5);
        check (rms (out, 0.05, 0.2) < 1.0e-3, "input: with no effect the input isn't heard (the samples are)", rms (out, 0.05, 0.2), 0.0);

        // ---- songs: pads in time, and played back
        r.p.pressKey ("SONG 1"); r.p.pressKey ("REC"); r.run (0.01);
        r.p.pressPad (0); r.run (0.2); r.p.releasePad (0); r.run (0.3);
        r.p.pressPad (0); r.run (0.2); r.p.releasePad (0); r.run (0.05);
        r.p.pressKey ("STOP"); r.run (0.01);
        check (r.p.songEvents (0) == 4, "songs: two presses make four events", r.p.songEvents (0), 4);
        check (std::abs (r.p.songLength (0) - 0.7) < 0.03, "songs: in time (s)", r.p.songLength (0), 0.7);
        r.p.pressKey ("PLAY");
        out = r.run (0.8);
        check (rms (out, 0.05, 0.18) > 0.2 && rms (out, 0.3, 0.45) < 1.0e-3 && rms (out, 0.55, 0.68) > 0.2,
               "songs: it plays back as it was played", rms (out, 0.55, 0.68), 0.35);
        r.p.pressKey ("SONG 1"); r.run (0.01);

        // ---- the edits
        juce::String err;
        check (! r.p.copyPad (0, 1, err), "edits: copy onto a pad in use is refused", 0, 0);
        auto q = r.p.pad (0);
        q.start = 4000; q.end = 12000; r.p.setPad (0, q);
        const int before = r.p.memoryUsedNibbles();
        r.p.compactPad (0);
        check (r.p.pad (0).frames == 8000 && r.p.memoryUsedNibbles() == before - (q.frames - 8000),
               "edits: compact keeps start to end and frees the rest (frames)", r.p.pad (0).frames, 8000);
        r.p.deletePad (4);
        check (! r.p.pad (4).used, "edits: delete empties the pad", 0, 0);
        q = r.p.pad (1); q.end = q.frames / 2; r.p.setPad (1, q);
        const int was = q.frames;
        check (r.p.splitPad (1, 4, err) && r.p.pad (1).frames + r.p.pad (4).frames == was, "edits: split makes two pads of it (frames)",
               r.p.pad (1).frames + r.p.pad (4).frames, was);
        check (r.p.movePad (4, 20, err) && r.p.pad (20).used && ! r.p.pad (4).used && r.p.pad (20).note == 56, "edits: move, and the pad keeps its note", r.p.pad (20).note, 56);

        // ---- the whole memory round trip
        auto x = r.p.toXml (false);
        Rig r2;
        r2.p.fromXml (*x);
        bool same = r2.p.memoryUsedNibbles() == r.p.memoryUsedNibbles() && r2.p.songEvents (0) == 4;
        for (int k = 0; k < numPads; ++k) same = same && r2.p.padAudio (k) == r.p.padAudio (k) && r2.p.pad (k).end == r.p.pad (k).end;
        check (same, "state: the memory, pads and songs come back the same", same ? 1 : 0, 1);
    }

    // ---------------------------------------------------------------- the tape, stereo and a file
    {
        Rig r;
        r.set ("grade", 1.0f);   // STD
        check (r.record (0, 0.5, 300.0, 0.5, false, true), "tape: a track's playback recorded into A1", 1, 1);
        r.set ("volume", 127.0f);
        auto out = r.play (0, 0.4, 0.35);
        check (std::abs (frequency (out, 0.05, 0.3) - 300.0) < 2.0, "tape: it's the track's sound (Hz)", frequency (out, 0.05, 0.3), 300.0);
        r.record (1, 0.5, 300.0, 0.5, true);
        check (r.p.pad (1).stereo && r.p.memoryUsedNibbles() == r.p.pad (0).frames + 2 * r.p.pad (1).frames, "stereo: a frame takes two nibbles",
               r.p.memoryUsedNibbles(), r.p.pad (0).frames + 2 * r.p.pad (1).frames);
        // a file: 60 s of sine is more than EXLG holds, so it's cut at the memory's end
        r.set ("grade", 3.0f);
        juce::File f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tf_sampler_test.wav");
        f.deleteFile();
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16));
            std::vector<float> s (44100 * 60);
            for (size_t i = 0; i < s.size(); ++i) s[i] = 0.5f * (float) std::sin (twoPi * 220.0 * (double) i / 44100.0);
            const float* ch[] { s.data() };
            w->writeFromFloatArrays (ch, 1, (int) s.size());
        }
        juce::String err;
        r.p.importFile (f, 2, err);
        f.deleteFile();
        check (r.p.pad (2).used && r.p.memoryUsedNibbles() >= memoryNibbles - 1, "file: imported (stereo) till the memory's full (nibbles)", r.p.memoryUsedNibbles(), memoryNibbles);
        check (err.startsWith ("MEMORY FULL"), "file: and it says so", err.startsWith ("MEMORY FULL") ? 1 : 0, 1);
        out = r.play (2, 0.4, 0.35);
        check (std::abs (frequency (out, 0.05, 0.3) - 220.0) < 2.0, "file: it plays at its own pitch (Hz)", frequency (out, 0.05, 0.3), 220.0);
        write ("file-220-exlg", out);
        r.set ("grade", 0.0f);
        r.p.pressKey ("SCRATCH"); r.run (0.01);
        check (r.p.panel().scratchPad < 0, "effects: SCRATCH can't be had at HI", r.p.panel().scratchPad, -1);
    }


    // ---------------------------------------------------------------- the loop tempo, as the unit shows it
    {
        check (! tempoOf (0.374, false).shown && tempoOf (0.376, false).shown, "tempo: none for a loop under 0.375 s (manual p. 46)", tempoOf (0.374, false).shown ? 1 : 0, 0);
        check (tempoOf (0.5, false).text() == "120.00", "tempo: half a second is 120.00 BPM", tempoOf (0.5, false).bpm, 120.0);
        check (tempoOf (60.0 / 125.34 * 4.0, false).text() == "125.34", "tempo: four beats at 125.34 read 125.34, to the hundredth", tempoOf (60.0 / 125.34 * 4.0, false).bpm, 125.34);
        check (std::abs (tempoOf (3.0, false).bpm - 80.0) < 1.0e-9 && std::abs (tempoOf (2.0 * 60.0 / 170.0, false).bpm - 85.0) < 0.01, "tempo: always 80.00 to 159.99 (170 BPM over two beats reads 85)", tempoOf (2.0 * 60.0 / 170.0, false).bpm, 85.0);
        check (tempoOf (2.0, true).text() == "120", "tempo: a reverse loop to the whole beat", tempoOf (2.0, true).bpm, 120.0);
        check (tempoOf (8.0, false).wholeBeats && ! tempoOf (5.0, false).wholeBeats, "tempo: a very long forward loop (over 6 s) to the whole beat", tempoOf (8.0, false).wholeBeats ? 1 : 0, 1);
        check (tempoOf (60.0 / 159.999, false).text() == "159.99" || tempoOf (60.0 / 159.999, false).text() == "80.00", "tempo: never shows 160", tempoOf (60.0 / 159.999, false).bpm, 159.99);

        Rig r;
        r.record (0, 2.2, 200.0);
        auto p = r.p.pad (0);
        p.loop = Loop::wholeLoop;
        const double clock = clockHz (r.p.grade(), r.p.pitchPercent());
        p.end = (int) std::lround (60.0 / 125.30 * 4.0 * clock);
        r.p.setPad (0, p);
        const double before = r.p.tempo (0).bpm;
        r.p.nudgeTempo (0, 1);
        const double up = r.p.tempo (0).bpm;
        check (std::abs (up - before - 0.01) < 0.006 && r.p.pad (0).end < p.end, "tempo: + makes the loop 0.01 BPM faster (shorter), to the nearest sample", up - before, 0.01);
        for (int k = 0; k < 100; ++k) r.p.nudgeTempo (0, -1);
        check (std::abs (r.p.tempo (0).bpm - (up - 1.0)) < 0.02, "tempo: a hundred - make it 1 BPM slower", r.p.tempo (0).bpm, up - 1.0);
        check (r.p.pad (0).start == p.start, "tempo: only the end moves", r.p.pad (0).start, p.start);
    }

    // ---------------------------------------------------------------- the other tracks and the mix as sources
    {
        // tracks 1-4 at 200, 300, 400, 500 Hz before the fader and half as loud after it; the sampler on track 1
        Processor q;
        q.setRateAndBufferSizeDetails (sr, bs);
        q.prepareToPlay (sr, bs);
        double ph = 0.0;
        auto runFeed = [&] (double seconds, bool track4Heard)
        {
            std::vector<float> pre[4], post[4];
            for (int k = 0; k < 4; ++k) { pre[k].resize (bs); post[k].resize (bs); }
            juce::AudioBuffer<float> b (2, bs);
            for (int blk = 0; blk < (int) (seconds * sr / bs); ++blk)
            {
                for (int i = 0; i < bs; ++i)
                    for (int k = 0; k < 4; ++k)
                    {
                        pre[k][(size_t) i] = 0.4f * (float) std::sin (twoPi * (200.0 + 100.0 * k) * (ph + i / sr));
                        post[k][(size_t) i] = 0.5f * pre[k][(size_t) i];
                    }
                ph += bs / sr;
                TrackFeed f;
                f.numTracks = 4;
                f.ownTrack = 0;
                for (int k = 0; k < 4; ++k) { f.tape[k] = nullptr; f.preFader[k] = pre[k].data(); f.postFader[k] = post[k].data(); f.heard[k] = k != 3 || track4Heard; }
                q.tapeFeed (f);
                b.clear();
                juce::MidiBuffer m;
                q.processBlock (b, m);
            }
        };
        auto take = [&] (int pad, Source src, int track, bool post, bool track4Heard)
        {
            auto rs = q.recordSettings();
            rs.toPad = pad; rs.source = src; rs.track = track; rs.postFader = post; rs.trigger = 0; rs.gain = 8; rs.stereo = false;
            rs.defaultLoop = Loop::wholeLoop;
            q.setRecordSettings (rs);
            juce::String e;
            q.armRecord (e);
            runFeed (0.3, track4Heard);
            q.stopRecord();
            double sum = 0.0;
            const auto a = q.padAudio (pad);
            for (auto v : a) sum += (double) v * v;
            return std::sqrt (sum / (double) std::max<size_t> (1, a.size())) / 32768.0;
        };
        const double postLevel = take (0, Source::track, 2, true, true), preLevel = take (1, Source::track, 2, false, true);
        check (std::abs (preLevel / postLevel - 2.0) < 0.2, "sources: a track before its fader, and after it (here half: 6 dB down)", preLevel / postLevel, 2.0);
        check (std::abs (preLevel - 0.4 / std::sqrt (2.0)) < 0.05, "sources: TRACK 3 OUT is track 3's sound (RMS)", preLevel, 0.4 / std::sqrt (2.0));
        check (q.pad (0).loop == Loop::wholeLoop && q.pad (0).name == "track 3", "sources: the new sample gets the chosen loop mode, and the track's name", q.pad (0).loop == Loop::wholeLoop ? 1 : 0, 1);
        // the mix: the other three (not its own track), as heard
        const double mixAll = take (2, Source::mix, 0, false, true), mixMuted = take (3, Source::mix, 0, false, false);
        check (std::abs (mixAll - 0.4 * std::sqrt (3.0 / 2.0)) < 0.06, "sources: MIX is the other three tracks, not its own (RMS)", mixAll, 0.4 * std::sqrt (3.0 / 2.0));
        check (std::abs (mixMuted - 0.4) < 0.05, "sources: a muted track leaves the MIX", mixMuted, 0.4);
        check (take (4, Source::track, 0, true, true) < 1.0e-3, "sources: its own track's output isn't taken (it would feed back)", take (5, Source::track, 0, true, true), 0.0);
    }

    // ---------------------------------------------------------------- the front panel's modes
    {
        Rig r;
        r.record (0, 0.6, 300.0);
        r.record (1, 0.6, 400.0);
        auto& f = r.p.front();
        // SHIFT + FILTER (pad 8) chooses the filter, as on the unit
        f.key ("SHIFT", true); f.pad (7, true); f.pad (7, false); f.key ("SHIFT", false);
        r.run (0.05);
        check (r.p.panel().effect == Effect::filter, "panel: SHIFT + pad 8 is FILTER", (int) r.p.panel().effect, (int) Effect::filter);
        f.key ("EXIT", true); r.run (0.05);
        check (r.p.panel().effect == Effect::none, "panel: EXIT leaves it", (int) r.p.panel().effect, 0);
        // BANK: one key, round the four
        for (int k = 0; k < 5; ++k) { f.key ("BANK", true); r.run (0.02); }
        check (r.p.panel().bank == 1, "panel: BANK five times is bank B", r.p.panel().bank, 1);
        for (int k = 0; k < 3; ++k) { f.key ("BANK", true); r.run (0.02); }
        // EDIT: SHIFT + pad 5, the pad, ENTER; +1 on the volume; < to Delete (it wraps), ENTER twice
        f.key ("SHIFT", true); f.pad (4, true); f.key ("SHIFT", false);
        check (f.mode() == Front::Mode::edit && f.lcd().top == "Which pad?", "edit: SHIFT + pad 5 asks which pad", f.mode() == Front::Mode::edit ? 1 : 0, 1);
        f.pad (1, true); f.pad (1, false); f.key ("ENTER", true);
        check (f.editPad() == 1 && f.lcd().top == "LpMode", "edit: the pad chosen, the first function", f.editPad(), 1);
        f.key (">", true); f.key (">", true);
        const int vol = r.p.pad (1).volume;
        f.key ("-1", true);
        check (r.p.pad (1).volume == vol - 1 && f.lcd().top == "PadVol", "edit: PadVol -1", r.p.pad (1).volume, vol - 1);
        f.key ("FWD", true);
        const int end = r.p.pad (1).end;
        for (int k = 0; k < 40; ++k) f.key ("-1", true, k);   // held: faster
        check (end - r.p.pad (1).end > 1000, "edit: -1 held moves the end point faster and faster (frames)", end - r.p.pad (1).end, 11110);
        f.key ("<", true); f.key ("<", true); f.key ("<", true); f.key ("<", true);   // EdPnt (LpPnt skipped), StPnt.. back past the start
        while (f.function() != Front::del) f.key ("<", true);
        f.key ("ENTER", true);
        check (r.p.pad (1).used && f.lcd().bottom == "Sure?", "edit: Delete asks Sure?", r.p.pad (1).used ? 1 : 0, 1);
        f.key ("ENTER", true);
        check (! r.p.pad (1).used && f.lcd().top == "Which pad?", "edit: ENTER again deletes it, and asks for a pad", r.p.pad (1).used ? 1 : 0, 0);
        f.key ("EXIT", true);
        check (f.mode() == Front::Mode::play, "edit: EXIT back to PLAY", (int) f.mode(), 0);
        // recording: REC, a pad (full ones refused), ENTER, PLAY, STOP
        f.key ("REC", true);
        f.pad (0, true);
        check (r.p.recordSettings().toPad != 0, "rec: a full pad is refused", r.p.recordSettings().toPad == 0 ? 0 : 1, 1);
        f.pad (2, true); f.key ("ENTER", true);
        check (f.mode() == Front::Mode::recSettings && f.lcd().top == "Gain", "rec: the settings, from Gain", (int) f.mode(), (int) Front::Mode::recSettings);
        f.key ("PLAY", true);
        r.run (0.4, 500.0, 0.5);
        f.key ("STOP", true);
        check (r.p.pad (2).used && f.mode() == Front::Mode::play, "rec: PLAY records into pad 3, STOP ends it", r.p.pad (2).used ? 1 : 0, 1);
        // UTILITY: SHIFT + pad 6; InitData asks twice
        f.key ("SHIFT", true); f.pad (5, true); f.key ("SHIFT", false);
        while (f.lcd().top != "InitData") f.key (">", true);
        f.key ("ENTER", true);
        check (r.p.pad (0).used, "utility: InitData asks first", r.p.pad (0).used ? 1 : 0, 1);
        f.key ("ENTER", true);
        check (! r.p.pad (0).used && r.p.memoryUsedNibbles() == 0, "utility: then every sample goes", r.p.memoryUsedNibbles(), 0);
        f.key ("EXIT", true);
    }

    // ---------------------------------------------------------------- its window, opened again
    {
        // An editor window closed and opened again gets the editor again: the processor is told the first went
        // (the way PluginWindow takes and lets go of an editor).
        struct Plain : juce::AudioProcessorEditor { explicit Plain (juce::AudioProcessor& p) : juce::AudioProcessorEditor (p) { setSize (400, 300); } };
        Processor::editorFactory = [] (Processor& p) -> juce::AudioProcessorEditor* { return new Plain (p); };
        Processor p;
        int opened = 0;
        for (int k = 0; k < 3; ++k)
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (takeEditor (p));
            if (ed != nullptr && p.getActiveEditor() == ed.get()) ++opened;
            editorGoing (p, ed.get());
            ed.reset();
        }
        check (opened == 3 && p.getActiveEditor() == nullptr, "window: opened, closed and opened again, it has its editor each time", opened, 3);
        // and one the processor still points at (closed without telling it) doesn't stop the next
        std::unique_ptr<juce::AudioProcessorEditor> lost (p.createEditorIfNeeded());
        std::unique_ptr<juce::AudioProcessorEditor> next (takeEditor (p));
        check (next != nullptr, "window: an editor left behind doesn't make the next window empty", next != nullptr ? 1 : 0, 1);
        editorGoing (p, next.get());
        next.reset();
        lost.reset();
        Processor::editorFactory = nullptr;
    }

    std::printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
