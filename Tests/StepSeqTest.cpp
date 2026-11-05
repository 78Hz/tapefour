// TF STEP (Source/StepSeq.h): a 16-step MIDI sequencer with a time of its own for each step.
//
//   Each step starts when the one before has had its time, to the sample, whatever the block size;
//   a note sounds for its gate; a rest plays nothing; a tie holds the note into the next one (the new
//   note on before the old one off, so a mono synth glides; the same note tied isn't struck again);
//   LENGTH wraps; velocity is the step's; a key transposes what follows and stays; other MIDI passes
//   on; STOP leaves no note sounding; the state comes back as it went.

#include "../Source/StepSeq.h"

#include <cstdio>
#include <cstdlib>

using namespace tf;
using namespace tf::step;

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
        if (annotate != nullptr) std::printf ("::error::StepSeqTest: %s (got %.3f, want %.3f)\n", what, got, want);
    }
}

constexpr double sr = 48000.0;

struct Event { juce::int64 at; bool on; int note, velocity; };

struct Rig
{
    Processor p;
    std::vector<Event> events;
    std::vector<juce::MidiMessage> other;
    juce::int64 now = 0;
    int block;

    explicit Rig (int blockSize = 256) : block (blockSize)
    {
        p.setRateAndBufferSizeDetails (sr, block);
        p.prepareToPlay (sr, block);
    }
    /** Runs for a time; 'in' is MIDI from the keyboard at the first sample. */
    void run (double seconds, juce::MidiBuffer in = {})
    {
        const auto until = now + (juce::int64) (seconds * sr);
        juce::AudioBuffer<float> b (2, block);
        while (now < until)
        {
            const int n = (int) std::min<juce::int64> (block, until - now);
            juce::AudioBuffer<float> view (b.getArrayOfWritePointers(), 2, n);
            juce::MidiBuffer m;
            m.swapWith (in);
            p.processBlock (view, m);
            for (const auto meta : m)
            {
                const auto msg = meta.getMessage();
                if (msg.isNoteOn()) events.push_back ({ now + meta.samplePosition, true, msg.getNoteNumber(), msg.getVelocity() });
                else if (msg.isNoteOff()) events.push_back ({ now + meta.samplePosition, false, msg.getNoteNumber(), 0 });
                else other.push_back (msg);
            }
            now += n;
        }
    }
    std::vector<Event> ons() const { std::vector<Event> v; for (auto& e : events) if (e.on) v.push_back (e); return v; }
    int sounding() const
    {
        int c = 0;
        for (auto& e : events) c += e.on ? 1 : -1;
        return c;
    }
};

juce::int64 samples (double ms) { return (juce::int64) std::llround (ms * sr / 1000.0); }
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    // ---------------------------------------------------------------- timing, gate, velocity
    for (int blockSize : { 64, 256, 1000 })
    {
        Rig r (blockSize);
        auto& s = r.p.state();
        const double times[] { 100, 50, 250, 10 };
        s.length = 4;
        for (int i = 0; i < 4; ++i) { s.steps[(size_t) i].ms = times[i]; s.steps[(size_t) i].note = 40 + i; s.steps[(size_t) i].gate = 50; s.steps[(size_t) i].velocity = 30 + 20 * i; }
        r.p.changed();
        r.p.setPlaying (true);
        r.run (1.0);
        const auto ons = r.ons();
        bool onTime = ons.size() >= 8, notes = onTime, vel = onTime;
        juce::int64 t = 0, worst = 0;
        for (size_t k = 0; k < 8 && k < ons.size(); ++k)
        {
            worst = std::max (worst, std::abs (ons[k].at - t));
            notes = notes && ons[k].note == 40 + (int) (k % 4);
            vel = vel && ons[k].velocity == 30 + 20 * (int) (k % 4);
            t += samples (times[k % 4]);
        }
        onTime = onTime && worst <= 1;
        const juce::String what = "timing: each step starts after the last one's own time (block " + juce::String (blockSize) + ")";
        check (onTime, what.toRawUTF8(), (double) worst, 0.0);
        if (blockSize == 256)
        {
            check (notes, "timing: the steps play in order and LENGTH wraps to step 1", notes, 1);
            check (vel, "velocity: each step's own", vel, 1);
            juce::int64 off = -1;
            for (auto& e : r.events) if (! e.on && e.note == 40) { off = e.at; break; }
            check (std::abs (off - samples (50)) <= 1, "gate: a 50% gate on a 100 ms step lets go at 50 ms", (double) off, (double) samples (50));
            r.p.setPlaying (false);
            r.run (0.05);
            check (r.sounding() == 0, "STOP: no note is left sounding", r.sounding(), 0);
        }
    }

    // ---------------------------------------------------------------- rests and ties
    {
        Rig r;
        auto& s = r.p.state();
        s.length = 4;
        for (auto& st : s.steps) { st.ms = 100; st.gate = 50; }
        s.steps[0].note = 36;
        s.steps[1].rest = true;
        s.steps[2].note = 40; s.steps[2].tie = true;
        s.steps[3].note = 43;
        r.p.changed();
        r.p.setPlaying (true);
        r.run (0.39);
        auto ons = r.ons();
        bool restSilent = true;
        for (auto& e : ons) if (e.at >= samples (100) && e.at < samples (200)) restSilent = false;
        check (restSilent && ons.size() == 3, "rest: the step plays nothing", (double) ons.size(), 3);

        // the tie: 43 on at 300 ms, and 40 off at the same moment, after it
        size_t onIdx = 0, offIdx = 0;
        for (size_t i = 0; i < r.events.size(); ++i)
        {
            if (r.events[i].on && r.events[i].note == 43) onIdx = i;
            if (! r.events[i].on && r.events[i].note == 40) offIdx = i;
        }
        const bool legato = r.events[onIdx].at == samples (300) && r.events[offIdx].at == samples (300) && offIdx > onIdx;
        check (legato, "tie: the next note on before the tied one lets go (a mono synth glides)", legato, 1);

        Rig same;
        auto& s2 = same.p.state();
        s2.length = 2;
        for (auto& st : s2.steps) { st.ms = 100; st.note = 48; st.gate = 50; }
        s2.steps[0].tie = true;
        same.p.changed();
        same.p.setPlaying (true);
        same.run (0.19);
        check (same.ons().size() == 1, "tie: the same note tied on isn't struck again", (double) same.ons().size(), 1);
    }

    // ---------------------------------------------------------------- the keyboard
    {
        Rig r;
        auto& s = r.p.state();
        s.length = 1;
        s.steps[0].ms = 100; s.steps[0].note = 36;
        s.homeNote = 36;
        r.p.changed();
        r.p.setPlaying (true);
        r.run (0.15);
        juce::MidiBuffer key;
        key.addEvent (juce::MidiMessage::noteOn (1, 39, (juce::uint8) 100), 0);
        key.addEvent (juce::MidiMessage::controllerEvent (1, 74, 64), 0);
        r.run (0.01, key);
        juce::MidiBuffer up;
        up.addEvent (juce::MidiMessage::noteOff (1, 39), 0);
        r.run (0.2, up);
        auto ons = r.ons();
        const bool moved = ons.size() >= 4 && ons[0].note == 36 && ons[1].note == 36 && ons[2].note == 39 && ons.back().note == 39;
        check (moved, "keys: a key transposes the next steps, and it stays after the key is let go", ons.empty() ? 0 : ons.back().note, 39);
        check (r.other.size() == 1 && r.other[0].isController(), "keys: other MIDI (a controller) passes on", (double) r.other.size(), 1);
        check (r.p.transposition() == 3, "keys: the window sees the transposition", r.p.transposition(), 3);

        Rig off;
        off.p.state().transpose = false;
        off.p.state().length = 1;
        off.p.changed();
        off.p.setPlaying (true);
        juce::MidiBuffer k2;
        k2.addEvent (juce::MidiMessage::noteOn (1, 50, (juce::uint8) 100), 0);
        off.run (0.3, k2);
        bool plain = ! off.ons().empty();
        for (auto& e : off.ons()) plain = plain && e.note == 36;
        check (plain, "keys OFF: the keyboard doesn't reach the synth or move the sequence", plain, 1);
    }

    // ---------------------------------------------------------------- state
    {
        Processor a;
        auto& s = a.state();
        s.length = 7; s.transpose = false; s.homeNote = 48;
        s.steps[3] = { 55, false, true, 1234.5, 80, 90 };
        s.steps[5].rest = true;
        a.changed();
        auto x = a.toXml (false);
        Processor b;
        b.fromXml (*x);
        const auto& t = b.state();
        const bool same = t.length == 7 && ! t.transpose && t.homeNote == 48 && t.steps[3].note == 55 && t.steps[3].tie
                          && std::abs (t.steps[3].ms - 1234.5) < 1e-6 && t.steps[3].gate == 80 && t.steps[3].velocity == 90 && t.steps[5].rest;
        check (same, "state: the steps and settings come back as they went", same, 1);
        check (std::abs (t.cycleMs() - (6 * defaultMs + 1234.5)) < 1e-6, "CYCLE: the steps played, end to end", t.cycleMs(), 6 * defaultMs + 1234.5);
    }

    std::printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
