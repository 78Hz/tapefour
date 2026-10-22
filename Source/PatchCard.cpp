#include "PatchCard.h"

namespace tf
{
//==============================================================================
Overlay::Overlay (std::unique_ptr<juce::Component> content, juce::Rectangle<int> cardBounds)
    : card (std::move (content))
{
    addAndMakeVisible (*card);
    card->setBounds (cardBounds);
    setWantsKeyboardFocus (false);
}

void Overlay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0a0908).withAlpha (0.62f));
    if (! cardShadow) return;
    auto r = card->getBounds().toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRoundedRectangle (r.translated (0, 14).expanded (6), 12.0f);
}

void Overlay::mouseDown (const juce::MouseEvent& e)
{
    if (e.eventComponent == this && onDismiss)
        onDismiss();
}

//==============================================================================
namespace
{
    constexpr int headerH = 72, pad = 22, labelW = 160, chipH = 40, chipGap = 6;
    const juce::Colour ink { 0xff1a1917 };
}

PatchCard::PatchCard (AudioEngine& e, int track)
    : engine (e), t (track)
{
    const char* names[] { "AUDIO IN", "INSTRUMENT", "HARDWARE SYNTH", "BOUNCE" };
    const char* hints[] { "Mic, guitar, line", "VST3 or built-in", "MIDI out, audio back in", "Mix other tracks" };
    for (int i = 0; i < 4; ++i)
    {
        auto* c = sourceCards.add (new ChipButton (names[i], hints[i]));
        c->onClick = [this, i]
        {
            auto current = (Source) engine.track[t].source.load();
            switch (i)
            {
                case 0: if (current != Source::audio) { if (onClosePluginWindow) onClosePluginWindow (t); engine.setAudioSource (t, engine.track[t].inputChannel.load(), engine.track[t].inputPair.load()); } break;
                case 1: if (current != Source::instrument) engine.setInstrument (t, BuiltIn::create (BuiltIn::poly)); break;
                case 2: if (current != Source::hardware) { if (onClosePluginWindow) onClosePluginWindow (t); engine.setHardwareSource (t); } break;
                case 3: if (current != Source::bounce) { if (onClosePluginWindow) onClosePluginWindow (t); engine.unplug (t); engine.track[t].bounceMask = 0; engine.track[t].source = (int) Source::bounce; } break;
                default: break;
            }
            changed();
        };
        addAndMakeVisible (c);
    }

    channelLabel.setText ("CH", juce::dontSendNotification);
    channelLabel.setFont (DeckLookAndFeel::get().cond (16.0f, true));
    channelLabel.setColour (juce::Label::textColourId, juce::Colour (0xff5c564a));
    channelBox.setFont (DeckLookAndFeel::get().mono (19.0f));
    channelBox.setJustification (juce::Justification::centred);
    channelBox.setInputRestrictions (3, "0123456789ALal");
    channelBox.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xfffffdf6));
    channelBox.setColour (juce::TextEditor::textColourId, ink);
    channelBox.setColour (juce::TextEditor::outlineColourId, ink);
    channelBox.setColour (juce::TextEditor::focusedOutlineColourId, Pal::orange);
    channelBox.setTooltip ("MIDI channel 1-16 for the synth, or ALL to pass channels through as played");
    auto commitChannel = [this]
    {
        auto text = channelBox.getText().trim();
        auto ch = text.getIntValue();
        engine.track[t].midiOutChannel = (text.isEmpty() || text.equalsIgnoreCase ("all") || ch <= 0) ? 0 : juce::jlimit (1, 16, ch);
        changed();
    };
    channelBox.onReturnKey = commitChannel;
    channelBox.onFocusLost = commitChannel;
    addChildComponent (channelBox);
    addChildComponent (channelLabel);

    done.onClick = [this] { if (onDone) onDone(); };
    closeX.textSize = 20.0f;
    closeX.onClick = [this] { if (onDone) onDone(); };
    addAndMakeVisible (done);
    addAndMakeVisible (closeX);

    rebuild();
}

void PatchCard::changed()
{
    engine.saveSettings();
    juce::Component::SafePointer<PatchCard> safe (this);
    juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->rebuild(); });
}

ChipButton* PatchCard::addChip (const juce::String& text, bool chosen, std::function<void()> action)
{
    auto* c = chips.add (new ChipButton (text));
    c->chosen = chosen;
    c->onClick = [this, action] { action(); changed(); };
    addAndMakeVisible (c);
    rows.back().count++;
    return c;
}

void PatchCard::addRow (const juce::String& label, const juce::String& sub)
{
    Row r;
    r.label = label;
    r.sub = sub;
    r.first = chips.size();
    rows.push_back (r);
}

void PatchCard::rebuild()
{
    chips.clear();
    rows.clear();
    channelBox.setVisible (false);
    channelLabel.setVisible (false);

    auto& p = engine.track[t];
    auto source = (Source) p.source.load();
    for (int i = 0; i < 4; ++i)
    {
        static const Source order[] { Source::audio, Source::instrument, Source::hardware, Source::bounce };
        sourceCards[i]->chosen = source == order[i];
        sourceCards[i]->repaint();
    }

    auto midiInRow = [&]
    {
        addRow ("PLAY WITH", "MIDI in");
        auto current = p.midiPort.load();
        addChip ("ANY (WHEN ARMED)", current == AudioEngine::midiAnyWhenArmed, [this] { engine.setTrackMidiPort (t, AudioEngine::midiAnyWhenArmed); });
        for (auto& in : engine.getMidiInputs())
        {
            if (! in.active) continue;
            auto label = (in.slot == AudioEngine::computerKeyboardPort ? juce::String ("COMPUTER KEYS") : in.name.toUpperCase())
                         + (in.mpe ? " (MPE)" : "");
            auto slot = in.slot;
            addChip (label, current == slot, [this, slot] { engine.setTrackMidiPort (t, slot); });
        }
        addChip ("NONE", current == AudioEngine::midiNone, [this] { engine.setTrackMidiPort (t, AudioEngine::midiNone); });
    };

    auto audioInRow = [&] (const juce::String& label, const juce::String& sub)
    {
        addRow (label, sub);
        auto names = engine.getActiveInputNames();
        auto ch = p.inputChannel.load();
        auto pair = p.inputPair.load();
        for (int i = 0; i < names.size(); ++i)
            addChip ("IN " + juce::String (i + 1), ch == i && ! pair, [this, i] { engine.track[t].inputChannel = i; engine.track[t].inputPair = false; });
        for (int i = 0; i + 1 < names.size(); i += 2)
            addChip ("IN " + juce::String (i + 1) + "+" + juce::String (i + 2) + " MONO", ch == i && pair,
                     [this, i] { engine.track[t].inputChannel = i; engine.track[t].inputPair = true; });
        if (names.isEmpty())
            addChip ("NO INPUTS ACTIVE", false, [] {});
    };

    switch (source)
    {
        case Source::audio:
            audioInRow ("AUDIO IN", "mono track");
            break;

        case Source::instrument:
        {
            midiInRow();
            addRow ("INSTRUMENT", {});
            auto* inst = engine.getInstrument (t);
            for (int k = 0; k < BuiltIn::numKinds; ++k)
            {
                auto name = BuiltIn::nameOf (k);
                bool chosen = inst != nullptr && inst->getPlugin() == nullptr && inst->getName() == name;
                addChip (name, chosen, [this, k] { if (onClosePluginWindow) onClosePluginWindow (t); engine.setInstrument (t, BuiltIn::create (k)); });
            }
            for (auto& desc : engine.knownPlugins.getTypes())
            {
                if (! desc.isInstrument) continue;
                bool chosen = inst != nullptr && inst->getPlugin() != nullptr && inst->getPlugin()->getPluginDescription().isDuplicateOf (desc);
                addChip (desc.name.toUpperCase(), chosen, [this, desc]
                {
                    if (onClosePluginWindow) onClosePluginWindow (t);
                    engine.loadPlugin (t, desc, {}, [safe = juce::Component::SafePointer<PatchCard> (this)] (const juce::String&)
                    {
                        if (safe != nullptr) safe->rebuild();
                    });
                });
            }
            addChip (utf ("FIND VST3\xe2\x80\xa6"), false, [this] { if (onFindInstruments) onFindInstruments(); });
            if (inst != nullptr && inst->getPlugin() != nullptr)
                addChip ("SHOW WINDOW", false, [this] { if (onShowPluginWindow) onShowPluginWindow (t); });
            break;
        }

        case Source::hardware:
        {
            midiInRow();
            addRow ("SEND TO", "MIDI out");
            auto current = p.midiOutPort.load();
            int shown = 0;
            for (auto& out : engine.getMidiOutputs())
            {
                if (! out.active) continue;
                ++shown;
                auto slot = out.slot;
                addChip (engine.midiOutputLabel (slot), current == slot, [this, slot] { engine.track[t].midiOutPort = slot; });
            }
            if (shown == 0)
                addChip ("TICK A MIDI OUTPUT IN SETUP", false, [] {});
            addChip ("NONE", current < 0, [this] { engine.track[t].midiOutPort = -1; });
            rows.back().extra = &channelBox;
            channelBox.setVisible (true);
            channelLabel.setVisible (true);
            auto ch = p.midiOutChannel.load();
            channelBox.setText (ch == 0 ? juce::String ("ALL") : juce::String (ch), false);
            audioInRow ("LISTEN TO", "audio in");
            break;
        }

        case Source::bounce:
        {
            addRow ("MIX FROM TAPE", {});
            auto mask = p.bounceMask.load();
            for (int s = 0; s < numTracks; ++s)
                if (s != t)
                    addChip ("TRACK " + juce::String (s + 1), (mask & (1 << s)) != 0, [this, s] { engine.toggleBounceSource (t, s); });
            break;
        }

        case Source::none: default:
            break;
    }

    preview = describeTrack (engine, t);
    resized();
    repaint();
}

void PatchCard::resized()
{
    auto w = getWidth();
    closeX.setBounds (w - pad - 44, 14, 44, 44);

    auto y = headerH + pad + 22;
    auto cardW = (w - 2 * pad - 3 * chipGap) / 4;
    for (int i = 0; i < 4; ++i)
        sourceCards[i]->setBounds (pad + i * (cardW + chipGap), y, cardW, 70);
    y += 70 + 26;

    for (auto& row : rows)
    {
        row.y = y;
        auto x = pad + labelW;
        auto right = w - pad;
        auto rowY = y;
        for (int i = row.first; i < row.first + row.count; ++i)
        {
            auto* c = chips[i];
            auto cw = std::min (c->idealWidth(), right - (pad + labelW));
            if (x + cw > right) { x = pad + labelW; rowY += chipH + chipGap; }
            c->setBounds (x, rowY, cw, chipH);
            x += cw + chipGap;
        }
        if (row.extra == &channelBox)
        {
            if (x + 120 > right) { x = pad + labelW; rowY += chipH + chipGap; }
            channelLabel.setBounds (x + 6, rowY, 34, chipH);
            channelBox.setBounds (x + 40, rowY, 60, chipH);
        }
        row.height = rowY + chipH - y;
        y += row.height + 18;
    }

    done.setBounds (w - pad - 120, getHeight() - pad - 46, 120, 46);
}

void PatchCard::paint (juce::Graphics& g)
{
    auto& lf = DeckLookAndFeel::get();
    auto r = getLocalBounds().toFloat();
    g.setColour (Pal::cream);
    g.fillRoundedRectangle (r, 8.0f);

    // header band
    juce::Path head;
    head.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), (float) headerH, 8.0f, 8.0f, true, true, false, false);
    g.setColour (juce::Colour (0xff26232c));
    g.fillPath (head);
    g.setColour (Pal::cream);
    g.setFont (lf.brand (34.0f));
    g.drawText ("TRACK " + juce::String (t + 1), pad, 0, 200, headerH, juce::Justification::centredLeft);
    g.setColour (juce::Colour (0xffcdb57c));
    g.setFont (lf.cond (16.0f, true).withExtraKerningFactor (0.24f));
    g.drawText ("PATCH", pad + 150, 0, 120, headerH, juce::Justification::centredLeft);

    g.setColour (Pal::menuHead);
    g.setFont (lf.cond (15.0f, true).withExtraKerningFactor (0.22f));
    g.drawText ("WHAT'S PLUGGED IN", pad, headerH + pad, 300, 16, juce::Justification::centredLeft);

    for (auto& row : rows)
    {
        g.setColour (juce::Colour (0xff5c564a));
        g.setFont (lf.cond (17.0f, true).withExtraKerningFactor (0.1f));
        g.drawText (row.label, pad, row.y + 2, labelW - 10, 20, juce::Justification::topLeft);
        if (row.sub.isNotEmpty())
        {
            g.setFont (lf.cond (16.0f));
            g.drawText (row.sub, pad, row.y + 21, labelW - 10, 18, juce::Justification::topLeft);
        }
    }

    // footer: exactly what the strip will show
    auto footY = getHeight() - pad - 46 - 14;
    g.setColour (juce::Colour (0xffc9c1ad));
    g.fillRect (pad, footY, getWidth() - 2 * pad, 1);
    g.setColour (Pal::menuHead);
    g.setFont (lf.cond (14.0f, true).withExtraKerningFactor (0.22f));
    g.drawText ("ON THE TRACK DISPLAY", pad, footY + 14, 400, 16, juce::Justification::centredLeft);
    g.setColour (ink);
    g.setFont (lf.mono (17.0f));
    auto line = preview.top + utf ("  \xc2\xb7  ") + preview.mid + (preview.low.isNotEmpty() ? utf ("  \xc2\xb7  ") + preview.low : juce::String());
    g.drawFittedText (line, pad, footY + 34, getWidth() - 2 * pad - 140, 24, juce::Justification::centredLeft, 1, 0.8f);
}
} // namespace tf
