// TF NYMPHES (Source/Nymphes.h): an editor for a Dreadbox Nymphes over MIDI CC.
//
//   A control sends its CC (Dreadbox's chart) once when it changes, on channel 1; the matrix sends CC 30
//   (the source) before a depth, and not again while the source stays; a cell unset goes back to no
//   modulation; SEND ALL sends every control, the set cells and the MPE set-up; notes pass; with KEEP IN
//   STEP a CC coming back moves its knob and goes on only if it's new to the synth (its echo stops); with
//   MPE, a note's slide (CC 74 on its channel) isn't the cutoff knob; a learned parameter sends too; the
//   state comes back as it went, and sends the whole sound.

#include "../Source/Nymphes.h"

#include <cstdio>
#include <cstdlib>

using namespace tf;
using namespace tf::nym;

namespace
{
int failures = 0;
const char* annotate = std::getenv ("GITHUB_ACTIONS");

void check (bool ok, const char* what, double got, double want)
{
    std::printf ("%s  %-74s got %8.2f  want %8.2f\n", ok ? "PASS" : "FAIL", what, got, want);
    if (! ok)
    {
        ++failures;
        if (annotate != nullptr) std::printf ("::error::NymphesTest: %s (got %.2f, want %.2f)\n", what, got, want);
    }
}

struct Sent { int ch, cc, v; };

std::vector<Sent> run (Processor& p, juce::MidiBuffer in = {}, std::vector<juce::MidiMessage>* passed = nullptr)
{
    juce::AudioBuffer<float> b (2, 256);
    p.processBlock (b, in);
    std::vector<Sent> out;
    for (const auto meta : in)
    {
        const auto m = meta.getMessage();
        if (m.isController()) out.push_back ({ m.getChannel(), m.getControllerNumber(), m.getControllerValue() });
        else if (passed != nullptr) passed->push_back (m);
    }
    return out;
}
int count (const std::vector<Sent>& s, int cc) { int n = 0; for (auto& x : s) n += x.cc == cc ? 1 : 0; return n; }
int valueOf (const std::vector<Sent>& s, int cc) { for (auto& x : s) if (x.cc == cc) return x.v; return -1; }
int destIndex (int cc) { for (int d = 0; d < numDests; ++d) if (destCc (d) == cc) return d; return -1; }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    {
        Processor p;
        check (run (p).empty(), "a new editor sends nothing (the synth keeps its sound until you change one)", 0, 0);

        p.setValue (lpfCutoff, 88);
        auto s = run (p);
        check (s.size() == 1 && s[0].cc == 74 && s[0].v == 88 && s[0].ch == 1, "a knob sends its CC once, on channel 1 (cutoff: CC 74)", s.empty() ? -1 : s[0].v, 88);
        check (run (p).empty(), "and not again while it stays", 0, 0);

        int wrong = 0;
        for (int c = 0; c < numControls; ++c)
        {
            p.setValue (c, (p.value (c) + 1) % (control (c).hi + 1));   // a value it hasn't got
            auto one = run (p);
            if (one.size() != 1 || one[0].cc != control (c).cc) ++wrong;
        }
        check (wrong == 0, "every control on its own CC from Dreadbox's chart", wrong, 0);

        // the matrix: the source, then the depth
        const int cut = destIndex (42), res = destIndex (43);
        p.setDepth (1, cut, 80);
        s = run (p);
        check (s.size() == 2 && s[0].cc == 30 && s[0].v == 1 && s[1].cc == 42 && s[1].v == 80,
               "matrix: CC 30 picks the source (MOD WHEEL), then the depth's CC", (double) s.size(), 2);
        p.setDepth (1, res, 20);
        s = run (p);
        check (s.size() == 1 && s[0].cc == 43, "matrix: the same source isn't picked again", (double) s.size(), 1);
        p.setDepth (1, res, -1);
        s = run (p);
        check (s.size() == 1 && s[0].cc == 43 && s[0].v == 0, "matrix: a cell unset goes back to none (0)", valueOf (s, 43), 0);
        p.setDepthsCentred (true);
        p.setDepth (2, cut, 90);
        p.setDepth (2, cut, -1);
        s = run (p);
        check (count (s, 42) == 0, "matrix: a cell set and unset before a block sends nothing", count (s, 42), 0);
        p.setDepth (0, destIndex (56), 50);
        check (p.depth (0, destIndex (56)) == -1, "matrix: LFO 2 can't reach LFO 1 (not on the synth)", p.depth (0, destIndex (56)), -1);

        // SEND ALL, with MPE
        p.setMpe (true);
        p.setBendRange (48);
        run (p);
        p.sendAll();
        s = run (p);
        bool mpeSetup = false;
        for (size_t i = 0; i + 2 < s.size(); ++i) if (s[i].ch == 1 && s[i].cc == 101 && s[i + 1].cc == 100 && s[i + 1].v == 6 && s[i + 2].v == 15) mpeSetup = true;
        int bendSet = 0;
        for (size_t i = 0; i + 2 < s.size(); ++i) if (s[i].ch == 2 && s[i].cc == 101 && s[i + 1].cc == 100 && s[i + 1].v == 0 && s[i + 2].v == 48) ++bendSet;
        int controlsSent = 0;
        for (int c = 0; c < numControls; ++c) controlsSent += count (s, control (c).cc) >= 1 ? 1 : 0;
        check (controlsSent == numControls && count (s, 42) == 1 && mpeSetup && bendSet == 1,
               "SEND ALL: every control, the set cells, the MPE set-up (15 members, bend 48)", controlsSent, numControls);
    }

    // ---- notes pass; KEEP IN STEP; MPE slide ----
    {
        Processor p;
        juce::MidiBuffer in;
        in.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 3);
        in.addEvent (juce::MidiMessage::noteOff (1, 60), 100);
        std::vector<juce::MidiMessage> passed;
        run (p, in, &passed);
        check (passed.size() == 2 && passed[0].isNoteOn() && passed[0].getVelocity() == 110, "notes pass through, as played", (double) passed.size(), 2);

        juce::MidiBuffer back;
        back.addEvent (juce::MidiMessage::controllerEvent (1, 71, 90), 0);   // the synth's RES slider moved
        auto s = run (p, back);
        check (p.value (lpfRes) == 90 && s.size() == 1 && s[0].cc == 71, "keep in step: a CC coming back moves the knob (and goes on once)", p.value (lpfRes), 90);
        juce::MidiBuffer echo;
        echo.addEvent (juce::MidiMessage::controllerEvent (1, 71, 90), 0);
        check (run (p, echo).empty(), "keep in step: its echo stops here (no loop)", 0, 0);
        juce::MidiBuffer mod;
        mod.addEvent (juce::MidiMessage::controllerEvent (1, 30, 3), 0);
        mod.addEvent (juce::MidiMessage::controllerEvent (1, 42, 70), 0);
        run (p, mod);
        check (p.depth (3, destIndex (42)) == 70, "keep in step: matrix depths coming back land on their source", p.depth (3, destIndex (42)), 70);

        p.setMpe (true);
        run (p);
        juce::MidiBuffer slide;
        slide.addEvent (juce::MidiMessage::noteOn (3, 64, (juce::uint8) 100), 0);
        slide.addEvent (juce::MidiMessage::controllerEvent (3, 74, 20), 1);
        slide.addEvent (juce::MidiMessage::pitchWheel (3, 12288), 2);
        passed.clear();
        auto out = run (p, slide, &passed);
        bool slidePassed = false;
        for (auto& x : out) slidePassed = slidePassed || (x.ch == 3 && x.cc == 74 && x.v == 20);
        check (slidePassed && p.value (lpfCutoff) == control (lpfCutoff).def, "MPE: a note's slide (CC 74 on its channel) passes, not the cutoff knob", p.value (lpfCutoff), control (lpfCutoff).def);
        bool shown = false;
        for (int v = 0; v < numVoices; ++v)
            if (p.voice (v).note.load() == 64) shown = p.voice (v).channel.load() == 3 && std::abs (p.voice (v).slide.load() - 20.0f / 127.0f) < 1e-4f && p.voice (v).bend.load() > 0.4f;
        check (shown, "PLAYING NOW: the note, its channel, slide and bend", shown, 1);

        p.setKeepInStep (false);
        juce::MidiBuffer thru;
        thru.addEvent (juce::MidiMessage::controllerEvent (1, 71, 10), 0);
        s = run (p, thru);
        check (p.value (lpfRes) == 90 && s.size() == 1 && s[0].v == 10, "keep in step OFF: CCs pass to the synth, the knobs stay", p.value (lpfRes), 90);
    }

    // ---- learned parameters; state ----
    {
        Processor p;
        auto* res = p.getParameters()[lpfRes];
        check (p.getParameters().size() == numControls && res->getName (64) == "RESONANCE", "every control is a parameter (MIDI learn)", p.getParameters().size(), numControls);
        res->setValue (1.0f);
        auto s = run (p);
        check (s.size() == 1 && s[0].cc == 71 && s[0].v == 127, "a learned parameter sends its CC", s.empty() ? -1 : s[0].v, 127);

        p.setValue (playMode, 4);
        p.setDepth (2, destIndex (44), 77);
        p.setMpe (true);
        p.setBendRange (24);
        auto x = p.toXml (false);
        Processor q;
        q.fromXml (*x);
        const bool same = q.value (playMode) == 4 && q.value (lpfRes) == 127 && q.depth (2, destIndex (44)) == 77 && q.isMpe() && q.bendRange() == 24;
        check (same, "state: the sound, the matrix and MPE come back as they went", same, 1);
        s = run (q);
        int controlsSent = 0;
        for (int c = 0; c < numControls; ++c) controlsSent += count (s, control (c).cc) >= 1 ? 1 : 0;
        check (controlsSent == numControls && valueOf (s, 44) == 77, "state: loading it sends the whole sound to the synth", controlsSent, numControls);
    }

    std::printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
