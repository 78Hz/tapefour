// The built-in instruments' windows (Source/PhraseSamplerPanel.cpp, Source/N5Panel.cpp, FM Drums, TF Step), clicked.
//
//   Every button and chip on every tab is clicked, one after another, letting the window rebuild in
//   between as it does in the app. Many clicks rebuild the tab, deleting the controls, the clicked one
//   among them: that must happen after the click, not inside it. Run under the address sanitizer (the CI's
//   sanitizer build does) this catches a control used after it's gone.

#include "../Source/PhraseSamplerPanel.h"
#include "../Source/N5Panel.h"
#include "../Source/FmDrumsPanel.h"
#include "../Source/StepPanel.h"
#include "../Source/NymphesPanel.h"
#include "../Source/TapeEchoPanel.h"
#include "../Source/PlatePanel.h"
#include "../Source/DeckLookAndFeel.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace tf;

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
        if (annotate != nullptr) std::printf ("::error::PanelTest: %s (got %.3f, want %.3f)\n", what, got, want);
    }
}

void collect (juce::Component& c, std::vector<LcdChip*>& out)
{
    for (auto* k : c.getChildren())
    {
        if (auto* b = dynamic_cast<LcdChip*> (k)) out.push_back (b);
        collect (*k, out);
    }
}

/** The labels not to click: they open a file chooser or a menu (a window of their own). */
bool opensSomething (const juce::String& label) { return label.contains (juce::CharPointer_UTF8 ("\xe2\x80\xa6")) || label.contains ("...") || label.endsWith ("  v"); }

/** Clicks the n-th clickable chip; false when there's no n-th. The window rebuilds afterwards, as it does. */
bool clickNth (juce::Component& panel, int n, int tabs)
{
    std::vector<LcdChip*> chips;
    collect (panel, chips);
    // (not the tabs: the panel's first children; clicking one moves to another tab)
    chips.erase (std::remove_if (chips.begin(), chips.end(), [&] (LcdChip* c) { return c->getParentComponent() == &panel && panel.getIndexOfChildComponent (c) < tabs; }), chips.end());
    int k = 0;
    for (auto* c : chips)
    {
        if (opensSomething (c->getButtonText()) || ! c->isEnabled()) continue;
        if (k++ != n) continue;
        juce::Component::SafePointer<LcdChip> safe (c);
        if (c->onClick) c->onClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
        return true;
    }
    return false;
}

/** Clicks the first chip with this label (not a tab). */
bool clickLabel (juce::Component& panel, const juce::String& label, int tabs)
{
    std::vector<LcdChip*> chips;
    collect (panel, chips);
    for (auto* c : chips)
        if (c->getButtonText() == label && ! (c->getParentComponent() == &panel && panel.getIndexOfChildComponent (c) < tabs))
        {
            if (c->onClick) c->onClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
            return true;
        }
    return false;
}

int clickEverything (juce::Component& panel, int tabs, int limit = 400)
{
    int clicks = 0;
    while (clicks < limit && clickNth (panel, clicks, tabs)) ++clicks;
    return clicks;
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    DeckLookAndFeel lnf;

    // ---------------------------------------------------------------- the phrase sampler
    {
        sampler::Processor p;
        p.setRateAndBufferSizeDetails (48000.0, 512);
        p.prepareToPlay (48000.0, 512);
        // two samples to edit
        for (int pad = 0; pad < 2; ++pad)
        {
            auto rs = p.recordSettings(); rs.toPad = pad; rs.trigger = 0; p.setRecordSettings (rs);
            juce::String e;
            p.armRecord (e);
            juce::AudioBuffer<float> b (2, 512);
            juce::MidiBuffer m;
            for (int k = 0; k < 60; ++k)
            {
                for (int i = 0; i < 512; ++i) b.setSample (0, i, 0.3f * std::sin ((float) i * 0.05f));
                b.copyFrom (1, 0, b, 0, 0, 512);
                p.processBlock (b, m);
            }
            p.stopRecord();
        }
        check (p.pad (0).used && p.pad (1).used, "sampler: two pads to edit", p.pad (1).used ? 1 : 0, 1);
        {
            // the ones that crashed: LOOP, PAD PLAYS, ALTERNATE GROUP, BANK and PAD on EDIT, SOURCE on SAMPLE
            int tab = 2;
            p.editorPad = 0;
            PhraseSamplerPanel panel (p, tab);
            panel.setSize (832, 600);
            int done = 0;
            for (auto* label : { "WHOLE LOOP", "PARTIAL LOOP", "REVERSE LOOP", "ONE SHOT", "TOGGLE", "TRIGGER", "NOTE (WHILE HELD)", "OFF", "B", "A", "2", "1" })
                done += clickLabel (panel, label, 5) ? 1 : 0;
            check (done == 12, "sampler: EDIT's LOOP, PAD PLAYS, BANK and PAD clicked, the tab rebuilt each time", done, 12);
            tab = 1;
            PhraseSamplerPanel sample (p, tab);
            sample.setSize (832, 600);
            done = 0;
            for (auto* label : { "TRACK 2 OUT", "MIX", "THIS TRACK IN", "ALWAYS", "WHILE RECORDING" })
                done += clickLabel (sample, label, 5) ? 1 : 0;
            check (done == 5, "sampler: SAMPLE's SOURCE and MONITOR clicked", done, 5);
        }
        const char* tabs[] { "sampler: PLAY's chips, all clicked", "sampler: SAMPLE's chips, all clicked", "sampler: EDIT's chips, all clicked",
                             "sampler: SONGS' chips, all clicked", "sampler: SETUP's chips, all clicked" };
        for (int t = 0; t < 5; ++t)
        {
            int tab = t;
            p.editorPad = 0;
            PhraseSamplerPanel panel (p, tab);
            panel.setSize (832, 600);
            const int clicks = clickEverything (panel, 5);
            check (clicks >= (t == 0 ? 0 : 10), tabs[t], clicks, clicks);   // (PLAY is the front panel: no chips)
        }
    }

    // ---------------------------------------------------------------- the N5
    {
        n5::Processor p;
        p.setRateAndBufferSizeDetails (48000.0, 512);
        p.prepareToPlay (48000.0, 512);
        for (int t = 0; t < 9; ++t)
        {
            p.editorTab = t;
            N5Panel panel (p);
            panel.setSize (832, 600);
            const int clicks = clickEverything (panel, 9);
            check (clicks >= 3, ("N5: tab " + juce::String (t + 1) + "'s buttons, all clicked").toRawUTF8(), clicks, clicks);
        }
    }

    // ---------------------------------------------------------------- FM drums
    {
        fmd::Processor p;
        p.setRateAndBufferSizeDetails (48000.0, 512);
        p.prepareToPlay (48000.0, 512);
        for (int t = 0; t < 2; ++t)
            for (int step : { -1, 4 })
            {
                p.editorTab = t; p.editorTrack = 1; p.editorStep = t == 0 ? step : -1;
                FmDrumsPanel panel (p);
                panel.setSize (832, 600);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
                const int clicks = clickEverything (panel, 2);
                check (clicks >= 4, ("FM drums: " + juce::String (t == 0 ? "PATTERN" : "FX") + (step >= 0 && t == 0 ? " (a step locked)" : "") + "'s buttons, all clicked").toRawUTF8(), clicks, clicks);
            }
    }

    // ---------------------------------------------------------------- TF STEP
    {
        step::Processor p;
        p.setRateAndBufferSizeDetails (48000.0, 512);
        p.prepareToPlay (48000.0, 512);
        {
            StepPanel panel (p);
            panel.setSize (832, 510);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
            const bool len = clickLabel (panel, "4", 0) && p.state().length == 4;
            const bool doubled = clickLabel (panel, juce::String (juce::CharPointer_UTF8 ("\xc3\x97" "2")), 0) && std::abs (p.state().steps[0].ms - 2 * step::defaultMs) < 1e-6;
            const bool off = clickLabel (panel, "OFF", 0) && ! p.state().transpose;
            check (len && doubled && off, "TF STEP: LENGTH, STEP TIMES x2 and KEYS OFF do what they say", len + doubled + off, 3);
        }
        StepPanel panel (p);
        panel.setSize (832, 510);
        const int clicks = clickEverything (panel, 0);
        check (clicks >= 20, "TF STEP: every button clicked", clicks, clicks);
    }

    // ---------------------------------------------------------------- TF PLATE
    {
        plate::Processor p;
        {
            PlatePanel panel (p);
            panel.setSize (PlatePanel::panelWidth, PlatePanel::panelHeight);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
            const bool gold = clickLabel (panel, "EMT 240 GOLD FOIL", 0) && p.plateIndex() == 1;
            const bool mono = clickLabel (panel, "MONO", 0) && p.get ("stereo") < 0.5f;
            const bool light = clickLabel (panel, "LIGHT", 0) && ! p.simulated();
            check (gold && mono && light, "TF PLATE: the plate, the pickups and the engine do what they say", gold + mono + light, 3);
            int outside = 0;
            for (auto* c : panel.getChildren())
                if (c->isVisible() && ! panel.getLocalBounds().contains (c->getBounds())) ++outside;
            check (outside == 0, "TF PLATE: everything inside the window", outside, 0);
        }
        PlatePanel panel (p);
        panel.setSize (PlatePanel::panelWidth, PlatePanel::panelHeight);
        const int clicks = clickEverything (panel, 0);
        check (clicks >= 7, "TF PLATE: every button clicked", clicks, clicks);
    }

    // ---------------------------------------------------------------- TF TAPE ECHO
    {
        echo::Processor p;
        {
            TapeEchoPanel panel (p);
            panel.setSize (TapeEchoPanel::panelWidth, TapeEchoPanel::panelHeight);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
            const bool erase = clickLabel (panel, "ERASE", 0) && p.get ("erase") < 0.5f;
            const bool hold = clickLabel (panel, "HOLD", 0) && p.get ("hold") > 0.5f;
            const bool note = clickLabel (panel, "1/4", 0) && p.noteValue() == 5;
            const bool loop = clickLabel (panel, "60 S", 0) && juce::exactlyEqual (p.loopNominal(), 60.0);
            p.setHeadPos (0, 0.2);
            const bool placed = clickLabel (panel, "STRAIGHT 1:2:3", 0) && std::abs (p.headPos (2) - 0.6) < 1e-3;
            check (erase && hold && note && loop && placed, "TF TAPE ECHO: ERASE, HOLD, the note value, the loop's length and PLACE do what they say",
                   erase + hold + note + loop + placed, 5);
            int outside = 0;
            for (auto* c : panel.getChildren())
                if (c->isVisible() && ! panel.getLocalBounds().contains (c->getBounds())) ++outside;
            check (outside == 0, "TF TAPE ECHO: everything inside the window", outside, 0);
        }
        TapeEchoPanel panel (p);
        panel.setSize (TapeEchoPanel::panelWidth, TapeEchoPanel::panelHeight);
        const int clicks = clickEverything (panel, 0);
        check (clicks >= 15, "TF TAPE ECHO: every button clicked", clicks, clicks);
    }

    // ---------------------------------------------------------------- TF NYMPHES
    {
        nym::Processor p;
        {
            NymphesPanel panel (p);
            panel.setSize (NymphesPanel::panelWidth, NymphesPanel::panelHeight);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
            const bool mono = clickLabel (panel, "MONO", 0) && p.value (nym::playMode) == 5;
            const bool mpe = clickLabel (panel, "ON: LOWER ZONE", 0) && p.isMpe();
            const bool centred = clickLabel (panel, "64 = NONE", 0) && p.depthsCentred();
            const bool keep = clickLabel (panel, "KEEP IN STEP", 0) && ! p.keepsInStep();
            check (mono && mpe && centred && keep, "TF NYMPHES: PLAY MODE, MPE, the depths' zero and KEEP IN STEP do what they say", mono + mpe + centred + keep, 4);
            int outside = 0;
            for (auto* c : panel.getChildren())
                if (c->isVisible() && ! panel.getLocalBounds().contains (c->getBounds())) ++outside;
            check (outside == 0, "TF NYMPHES: everything inside the window", outside, 0);
        }
        NymphesPanel panel (p);
        panel.setSize (NymphesPanel::panelWidth, NymphesPanel::panelHeight);
        const int clicks = clickEverything (panel, 0);
        check (clicks >= 20, "TF NYMPHES: every button clicked", clicks, clicks);
    }

    std::printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
