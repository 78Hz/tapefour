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

/** Every instrument TapeFour knows, searchable and grouped by maker. */
class InstrumentBrowser : public juce::Component,
                          private juce::ListBoxModel
{
public:
    struct Item
    {
        bool header = false;
        juce::String text, sub;
        int builtIn = -1;
        juce::PluginDescription desc;
        bool chosen = false;
    };

    std::function<void (const Item&)> onPick;
    std::function<void()> onFind, onCancel;

    InstrumentBrowser (AudioEngine& e, int track) : engine (e), t (track)
    {
        auto& lf = DeckLookAndFeel::get();
        search.setFont (lf.cond (18.0f, true));
        search.setTextToShowWhenEmpty (utf ("SEARCH INSTRUMENTS \xe2\x80\xa6"), juce::Colour (0xff8c877b));
        search.setIndents (12, 8);
        search.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xfffffdf6));
        search.setColour (juce::TextEditor::textColourId, ink);
        search.setColour (juce::TextEditor::outlineColourId, ink);
        search.setColour (juce::TextEditor::focusedOutlineColourId, Pal::orange);
        search.setColour (juce::CaretComponent::caretColourId, ink);
        search.onTextChange = [this] { refilter(); };
        search.onEscapeKey = [this] { if (onCancel) onCancel(); };
        search.onReturnKey = [this]
        {
            for (auto& it : shown) if (! it.header) { if (onPick) onPick (it); return; }
        };
        addAndMakeVisible (search);

        list.setModel (this);
        list.setRowHeight (30);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xfffbf8ef));
        list.setColour (juce::ListBox::outlineColourId, ink);
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        find.textSize = 14.0f;
        find.onClick = [this] { if (onFind) onFind(); };
        addAndMakeVisible (find);

        gather();
        refilter();
    }

    void grabSearchFocus() { search.grabKeyboardFocus(); }

    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (40);
        find.setBounds (top.removeFromRight (150));
        top.removeFromRight (8);
        search.setBounds (top);
        r.removeFromTop (8);
        list.setBounds (r);
    }

private:
    void gather()
    {
        all.clear();
        auto* inst = engine.getInstrument (t);
        all.push_back ({ true, "BUILT IN", {}, -1, {}, false });
        for (int k = 0; k < BuiltIn::numKinds; ++k)
            all.push_back ({ false, BuiltIn::nameOf (k), utf ("TAPE\xc2\xb7" "FOUR"), k, {},
                             inst != nullptr && inst->getPlugin() == nullptr && inst->getName() == BuiltIn::nameOf (k) });

        auto types = engine.knownPlugins.getTypes();
        std::vector<juce::PluginDescription> instruments;
        for (auto& d : types) if (d.isInstrument) instruments.push_back (d);
        std::sort (instruments.begin(), instruments.end(), [] (const auto& a, const auto& b)
        {
            auto ma = a.manufacturerName.isEmpty() ? juce::String ("~") : a.manufacturerName.toLowerCase();
            auto mb = b.manufacturerName.isEmpty() ? juce::String ("~") : b.manufacturerName.toLowerCase();
            return ma != mb ? ma < mb : a.name.compareIgnoreCase (b.name) < 0;
        });

        // Plug-ins that share a name are all listed; where they came from tells them apart.
        juce::StringArray seen, repeated;
        for (auto& d : instruments)
        {
            auto key = d.name.toLowerCase();
            if (seen.contains (key)) repeated.addIfNotAlreadyThere (key); else seen.add (key);
        }

        juce::String maker = "\x01";
        for (auto& d : instruments)
        {
            auto m = d.manufacturerName.isNotEmpty() ? d.manufacturerName : juce::String ("Unknown maker");
            if (m != maker) { all.push_back ({ true, m.toUpperCase(), {}, -1, {}, false }); maker = m; }

            juce::String sub = d.pluginFormatName;
            if (d.version.isNotEmpty()) sub << " " << d.version;
            if (repeated.contains (d.name.toLowerCase()))
            {
                juce::File f (d.fileOrIdentifier);
                sub << utf ("  \xc2\xb7  ") << f.getParentDirectory().getFileName() << "/" << f.getFileName();
            }
            bool chosen = inst != nullptr && inst->getPlugin() != nullptr && inst->getPlugin()->getPluginDescription().isDuplicateOf (d);
            all.push_back ({ false, d.name, sub, -1, d, chosen });
        }
    }

    void refilter()
    {
        auto q = search.getText().trim().toLowerCase();
        shown.clear();
        const Item* pendingHeader = nullptr;
        for (auto& it : all)
        {
            if (it.header) { pendingHeader = &it; continue; }
            bool match = q.isEmpty() || it.text.toLowerCase().contains (q) || it.sub.toLowerCase().contains (q)
                         || (pendingHeader != nullptr && pendingHeader->text.toLowerCase().contains (q));
            if (! match) continue;
            if (pendingHeader != nullptr) { shown.push_back (*pendingHeader); pendingHeader = nullptr; }
            shown.push_back (it);
        }
        list.updateContent();
        list.repaint();
        for (int i = 0; i < (int) shown.size(); ++i)
            if (shown[(size_t) i].chosen) { list.scrollToEnsureRowIsOnscreen (i); break; }
    }

    int getNumRows() override { return (int) shown.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) shown.size())) return;
        auto& lf = DeckLookAndFeel::get();
        auto& it = shown[(size_t) row];
        if (it.header)
        {
            g.setColour (juce::Colour (0xffece4cf));
            g.fillRect (0, 0, w, h);
            g.setColour (Pal::menuHead);
            g.setFont (lf.cond (14.0f, true).withExtraKerningFactor (0.2f));
            g.drawText (it.text, 12, 0, w - 24, h, juce::Justification::bottomLeft);
            return;
        }
        if (it.chosen || selected)
        {
            g.setColour (it.chosen ? ink : juce::Colour (0xffe8dfc8));
            g.fillRect (0, 1, w, h - 2);
        }
        g.setColour (it.chosen ? Pal::orange : ink);
        g.setFont (lf.cond (18.0f, true));
        auto nameW = std::min (w / 2 + 40, (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), it.text) + 30);
        g.drawText (it.text, 24, 0, nameW, h, juce::Justification::centredLeft);
        g.setColour (it.chosen ? juce::Colour (0xffcdb57c) : juce::Colour (0xff6b665c));
        g.setFont (lf.cond (14.0f));
        g.drawFittedText (it.sub, 24 + nameW, 0, w - nameW - 36, h, juce::Justification::centredLeft, 1, 0.8f);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        if (juce::isPositiveAndBelow (row, (int) shown.size()) && ! shown[(size_t) row].header && onPick)
        {
            auto it = shown[(size_t) row];   // copy: picking rebuilds the card
            onPick (it);
        }
    }

    AudioEngine& engine;
    const int t;
    juce::TextEditor search;
    juce::ListBox list;
    FlatButton find { utf ("FIND VST3\xe2\x80\xa6") };
    std::vector<Item> all, shown;
};

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
                case 0: if (current != Source::audio) { engine.setAudioSource (t, engine.track[t].inputChannel.load(), engine.track[t].inputPair.load()); } break;
                case 1: if (current != Source::instrument) engine.setInstrument (t, BuiltIn::create (BuiltIn::poly)); break;
                case 2: if (current != Source::hardware) { engine.setHardwareSource (t); } break;
                case 3: if (current != Source::bounce) { engine.unplug (t); engine.track[t].bounceMask = 0; engine.track[t].source = (int) Source::bounce; } break;
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

PatchCard::~PatchCard() = default;

void PatchCard::openBrowser (bool open)
{
    browsing = open;
    if (open)
    {
        browser = std::make_unique<InstrumentBrowser> (engine, t);
        browser->onPick = [this] (const InstrumentBrowser::Item& it)
        {
            auto safeThis = juce::Component::SafePointer<PatchCard> (this);
            if (it.builtIn >= 0) pickBuiltIn (it.builtIn);
            else pickPlugin (it.desc);
            if (safeThis != nullptr) juce::MessageManager::callAsync ([safeThis] { if (safeThis != nullptr) safeThis->openBrowser (false); });
        };
        browser->onFind = [this] { if (onFindInstruments) onFindInstruments(); };
        browser->onCancel = [this]
        {
            juce::Component::SafePointer<PatchCard> safe (this);
            juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->openBrowser (false); });
        };
        addAndMakeVisible (*browser);
    }
    else
    {
        browser.reset();
    }
    rebuild();
    if (browser != nullptr) browser->grabSearchFocus();
}

void PatchCard::pickBuiltIn (int kind)
{
    engine.setInstrument (t, BuiltIn::create (kind));
    changed();
}

void PatchCard::pickPlugin (const juce::PluginDescription& desc)
{
    engine.loadPlugin (t, desc, {}, [safe = juce::Component::SafePointer<PatchCard> (this)] (const juce::String&)
    {
        if (safe != nullptr) safe->rebuild();
    });
    changed();
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
        addChip ("ANY", current == AudioEngine::midiAny, [this] { engine.setTrackMidiPort (t, AudioEngine::midiAny); });
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
            // One line: what's loaded, its editor, and a way to change it.
            addRow ("INSTRUMENT", {});
            auto* inst = engine.getInstrument (t);
            auto current = inst != nullptr ? inst->getName().toUpperCase() : juce::String ("NOTHING LOADED");
            addChip (current, true, [] {});
            if (inst != nullptr && inst->getPlugin() != nullptr)
                addChip ("EDIT SOUND", false, [this] { if (onShowPluginWindow) onShowPluginWindow (t); });
            addChip (browsing ? juce::String ("CLOSE LIST") : utf ("CHANGE\xe2\x80\xa6"), browsing, [this]
            {
                juce::Component::SafePointer<PatchCard> safe (this);
                auto open = ! browsing;
                juce::MessageManager::callAsync ([safe, open] { if (safe != nullptr) safe->openBrowser (open); });
            });
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

    if (browser != nullptr)
    {
        auto footY = getHeight() - pad - 46 - 14;
        browser->setBounds (pad, y - 4, w - 2 * pad, std::max (120, footY - 14 - (y - 4)));
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
