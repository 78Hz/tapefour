#include "DeckComponent.h"

// DeckComponent's MIDI learn mode: the overlay that outlines what can be learned, and the menus.
namespace tf
{
//==============================================================================
/** Over the whole deck in learn mode: every control that can be learned is outlined, with what it's mapped to. */
class LearnLayer : public juce::Component
{
public:
    explicit LearnLayer (DeckComponent& d) : deck (d) { setWantsKeyboardFocus (false); }

    void paint (juce::Graphics& g) override
    {
        auto& lf = DeckLookAndFeel::get();
        g.fillAll (juce::Colours::black.withAlpha (0.18f));
        const bool learning = deck.engine.mapper.isLearning();
        auto current = deck.engine.mapper.getLearnTarget();
        for (auto& lt : deck.learnTargets)
        {
            if (! lt.component->isShowing()) continue;
            auto r = getLocalArea (lt.component, lt.component->getLocalBounds()).toFloat().expanded (3.0f);
            const bool isCurrent = learning && ! current.plugin && current == lt.target;
            auto label = deck.engine.mapper.labelFor (lt.target);
            if (isCurrent)
            {
                g.setColour (Pal::accent.withAlpha (0.25f));
                g.fillRoundedRectangle (r, 4.0f);
                g.setColour (Pal::accent);
                g.drawRoundedRectangle (r, 4.0f, 2.0f);
            }
            else
            {
                juce::Path p;
                p.addRoundedRectangle (r, 4.0f);
                const float dash[] { 4.0f, 3.0f };
                juce::PathStrokeType (1.2f).createDashedStroke (p, p, dash, 2);
                g.setColour ((label.isNotEmpty() ? Pal::green : Pal::lamp).withAlpha (0.9f));
                g.fillPath (p);
            }
            auto badge = isCurrent ? juce::String ("MOVE A CONTROL") : label;
            if (badge.isNotEmpty())
            {
                g.setFont (lf.mono (10.0f));
                auto w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), badge) + 8.0f;
                auto b = juce::Rectangle<float> (w, 14.0f).withPosition (r.getRight() - w, r.getY() - 7.0f);
                g.setColour (isCurrent ? Pal::accent : Pal::green);
                g.fillRoundedRectangle (b, 2.0f);
                g.setColour (juce::Colour (0xff141311));
                g.drawText (badge, b, juce::Justification::centred);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto pos = e.getPosition();
        if (getLocalArea (&deck.learnBtn, deck.learnBtn.getLocalBounds()).contains (pos))
        {
            deck.setLearnMode (false);
            return;
        }
        // the smallest target under the mouse
        const DeckComponent::LearnTarget* hit = nullptr;
        for (auto& lt : deck.learnTargets)
        {
            if (! lt.component->isShowing()) continue;
            auto r = getLocalArea (lt.component, lt.component->getLocalBounds()).expanded (3);
            if (r.contains (pos) && (hit == nullptr || r.getWidth() * r.getHeight() < hit->component->getWidth() * hit->component->getHeight()))
                hit = &lt;
        }
        if (hit == nullptr) return;
        if (e.mods.isPopupMenu()) deck.learnTargetMenu (*hit);
        else                      deck.startLearning (hit->target);
        repaint();
    }

private:
    DeckComponent& deck;
};

//==============================================================================
void DeckComponent::gatherLearnTargets()
{
    learnTargets.clear();
    using C = MidiMapper::Control;
    auto add = [this] (juce::Component& c, C control, int t)
    {
        MidiMapper::Target target;
        target.control = control;
        target.track = t;
        learnTargets.push_back ({ &c, target });
    };
    for (int t = 0; t < numTracks; ++t)
        strips[t]->forEachLearnTarget ([&] (juce::Component& c, C control) { add (c, control, t); });
    master.forEachLearnTarget ([&] (juce::Component& c, C control) { add (c, control, -1); });
    add (pitch.slider, C::pitch, -1);
    add (speedLo, C::speed, -1);
    add (speedHi, C::speed, -1);
    add (nrButton, C::nr, -1);
    add (playKey, C::play, -1);
    add (stopKey, C::stop, -1);
    add (recKey, C::rec, -1);
    add (rewKey, C::rew, -1);
    add (ffKey, C::ff, -1);
    add (rtzBtn, C::rtz, -1);
}

const DeckComponent::LearnTarget* DeckComponent::learnTargetFor (juce::Component* c) const
{
    for (; c != nullptr && c != this; c = c->getParentComponent())
        for (auto& lt : learnTargets)
            if (lt.component == c) return &lt;
    return nullptr;
}

juce::String DeckComponent::targetName (const MidiMapper::Target& t) const
{
    auto name = MidiMapper::controlName (t.control);
    return MidiMapper::isPerTrack (t.control) ? "TRACK " + juce::String (t.track + 1) + " " + name : name;
}

void DeckComponent::setLearnMode (bool on)
{
    if (on == (learnLayer != nullptr)) return;
    engine.mapper.cancelLearn();
    if (on)
    {
        closeOverlay();
        learnLayer = std::make_unique<LearnLayer> (*this);
        learnLayer->setBounds (getLocalBounds());
        addAndMakeVisible (*learnLayer);
        learnLayer->toFront (false);
        grabKeyboardFocus();   // Esc leaves learn mode
    }
    else
    {
        learnLayer.reset();
        engine.saveSettings();
    }
    learnBtn.setLed (on);
}

void DeckComponent::startLearning (const MidiMapper::Target& target)
{
    engine.mapper.beginLearn (target);
    say ("LEARN " + targetName (target) + ": MOVE A CONTROL", 1.0);
}

void DeckComponent::learnTargetMenu (const LearnTarget& lt)
{
    auto target = lt.target;
    auto label = engine.mapper.labelFor (target);
    juce::PopupMenu m;
    m.addSectionHeader (targetName (target) + (label.isNotEmpty() ? juce::String (utf (" \xc2\xb7 ")) + label : juce::String()));
    m.addItem ("Learn MIDI control", [this, target]
    {
        setLearnMode (true);
        startLearning (target);
    });
    m.addItem ("Forget MIDI control", label.isNotEmpty(), false, [this, target]
    {
        engine.mapper.removeForTarget (target);
        engine.saveSettings();
        say ("FORGOTTEN: " + targetName (target));
        if (learnLayer != nullptr) learnLayer->repaint();
    });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (lt.component));
}

void DeckComponent::remoteControl (MidiMapper::Control c)
{
    using C = MidiMapper::Control;
    using Cmd = TapeEngine::Command;
    switch (c)
    {
        case C::play: transport (Cmd::play); break;
        case C::stop: transport (Cmd::stop); break;
        case C::rec:  transport (Cmd::rec); break;
        case C::rew:  transport (Cmd::rew); break;
        case C::ff:   transport (Cmd::ff); break;
        case C::rtz:  transport (Cmd::rtz); break;
        case C::markAdd:  addMark(); break;
        case C::markPrev: locateMark (-1); break;
        case C::markNext: locateMark (+1); break;
        case C::gain: case C::high: case C::mid: case C::freq: case C::low: case C::pan: case C::fader:
        case C::mute: case C::solo: case C::arm: case C::masterFader: case C::phones: case C::pitch:
        case C::speed: case C::nr: case C::numControls: break;   // not transport
    }
}

//==============================================================================
void DeckComponent::addMark()
{
    auto* t = engine.getTape();
    if (t == nullptr) { say ("NO TAPE - PRESS EJECT"); return; }
    const bool realtime = engine.tapeEngine.machine.counterRealtime.load();
    const double v = engine.tapeEngine.getCounter();
    // What the counter shows: the wheels roll round below 000 to 999.
    const int value = realtime ? (int) std::floor (std::abs (v)) : (((int) std::floor (v) % 1000) + 1000) % 1000;
    const auto text = Inlay::counterText (value, realtime);
    const int row = Inlay::addMark (t->trackSheet, value, realtime, "mark");
    if (row == -2) { say ("MARK " + text + " IS ON THE INLAY ALREADY"); return; }
    if (row < 0)   { say ("THE INLAY'S SONG LIST IS FULL", 3.0); return; }
    t->markMetaChanged();
    if (openCard != nullptr) openCard->reloadSongs();
    inlayThumb.repaint();
    say ("MARK " + text + " WRITTEN ON THE INLAY", 2.5);
}

void DeckComponent::locateMark (int dir)
{
    auto* t = engine.getTape();
    if (t == nullptr) { say ("NO TAPE - PRESS EJECT"); return; }
    int row = -1;
    const int target = Inlay::findMark (t->trackSheet, engine.tapeEngine.getCounter(), dir, &row);
    if (target < 0) { say (dir < 0 ? "NO MARK BEFORE HERE ON THE INLAY" : "NO MARK AFTER HERE ON THE INLAY"); return; }
    const bool realtime = engine.tapeEngine.machine.counterRealtime.load();
    locateName = Inlay::counterText (target, realtime);
    if (auto name = t->trackSheet[Inlay::songName (row)].trim(); name.isNotEmpty()) locateName << " " << name.toUpperCase();
    transport (dir < 0 ? TapeEngine::Command::rew : TapeEngine::Command::ff);
    locateTarget = target;
    locateDir = dir;
    locateFrames = 0;
    say ("WINDING TO " + locateName, 30.0);
}

void DeckComponent::followLocate (Mode mode)
{
    if (locateTarget < 0) return;
    ++locateFrames;
    const double v = engine.tapeEngine.getCounter();
    const bool winding = mode == (locateDir < 0 ? Mode::rew : Mode::ff);
    // Stop where the counter reads the mark (it shows whole numbers, rounded down).
    if (winding && (locateDir > 0 ? v >= locateTarget + 0.05 : v <= locateTarget + 0.7))
    {
        locateTarget = -1;
        transport (TapeEngine::Command::stop);
        say ("AT " + locateName, 2.5);
    }
    else if (! winding && locateFrames > 6)
        locateTarget = -1;   // stopped, played or the tape ran out: the locate is over
}
} // namespace tf
