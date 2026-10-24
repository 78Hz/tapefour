#include "BlockPicker.h"
#include "ClapFormat.h"
#include <map>

namespace tf
{
namespace
{
    const juce::Colour rule { 0xff5a4020 };

    void styleSearch (juce::TextEditor& e, const juce::String& hint)
    {
        auto& lf = DeckLookAndFeel::get();
        e.setFont (lf.mono (14.0f));
        e.setTextToShowWhenEmpty (hint, Pal::lcdDim.withAlpha (0.7f));
        e.setIndents (8, 6);
        e.setColour (juce::TextEditor::backgroundColourId, Pal::lcd);
        e.setColour (juce::TextEditor::textColourId, Pal::lcdText);
        e.setColour (juce::TextEditor::outlineColourId, rule);
        e.setColour (juce::TextEditor::focusedOutlineColourId, Pal::lcdText);
        e.setColour (juce::TextEditor::highlightColourId, Pal::lcdDim.withAlpha (0.4f));
        e.setColour (juce::TextEditor::highlightedTextColourId, Pal::lcdText);
        e.setColour (juce::CaretComponent::caretColourId, Pal::lcdText);
    }

    void paintRow (juce::Graphics& g, int w, int h, bool chosen, const juce::String& name, const juce::String& sub)
    {
        auto& lf = DeckLookAndFeel::get();
        if (chosen)
        {
            g.setColour (Pal::lcdText);
            g.fillRoundedRectangle (juce::Rectangle<float> (0, 1, (float) w, (float) h - 2), 2.0f);
        }
        g.setColour (chosen ? Pal::lcd : Pal::lcdText);
        g.setFont (lf.mono (13.0f));
        g.drawFittedText (name, 8, 0, w - 130, h, juce::Justification::centredLeft, 1, 0.8f);
        g.setColour (chosen ? Pal::lcd : Pal::lcdDim);
        g.setFont (lf.mono (10.0f));
        g.drawFittedText (sub, w - 124, 0, 116, h, juce::Justification::centredRight, 1, 0.7f);
    }
}

//==============================================================================
BlockPicker::BlockPicker (AudioEngine& e, const juce::String& currentName, const juce::String& prompt) : engine (e), promptText (prompt)
{
    const char* names[] { "ALL", "INSTRUMENTS", "MIDI FX", "AUDIO FX" };
    for (int i = 0; i < 4; ++i)
    {
        auto* c = tabs.add (new LcdChip (names[i]));
        c->textSize = 11.0f;
        c->chosen = i == category;
        c->onClick = [this, i] { category = i; for (int k = 0; k < tabs.size(); ++k) tabs[k]->chosen = k == i; repaint(); refilter(); };
        addAndMakeVisible (c);
    }
    find.textSize = 11.0f;
    find.onClick = [this] { if (onFind) onFind(); };
    addAndMakeVisible (find);

    styleSearch (search, utf ("FIND \xe2\x80\xa6"));
    search.onTextChange = [this] { refilter(); };
    search.onReturnKey = [this] { for (auto& it : shown) if (! it.header) { pick (it); return; } };
    addAndMakeVisible (search);

    list.setModel (this);
    list.setRowHeight (24);
    list.setColour (juce::ListBox::backgroundColourId, Pal::lcd);
    list.setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (list);
    gather (currentName);
    refilter();
}

void BlockPicker::paint (juce::Graphics& g)
{
    g.setColour (Pal::lcdDim);
    g.setFont (DeckLookAndFeel::get().mono (11.0f));
    g.drawText (promptText, 0, 34, 140, 30, juce::Justification::centredLeft);
}

void BlockPicker::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (28);
    find.setBounds (top.removeFromRight (std::min (130, find.idealWidth())));
    for (auto* c : tabs) { c->setBounds (top.removeFromLeft (c->idealWidth())); top.removeFromLeft (5); }
    r.removeFromTop (6);
    auto s = r.removeFromTop (30);
    s.removeFromLeft (140);
    search.setBounds (s);
    r.removeFromTop (6);
    list.setBounds (r);
}

int BlockPicker::categoryOf (const juce::PluginDescription& d)
{
    if (d.isInstrument) return instruments;
    auto c = d.category + " " + d.fileOrIdentifier;
    if (c.containsIgnoreCase ("midi") || c.containsIgnoreCase ("event") || c.containsIgnoreCase ("aumi")
        || c.containsIgnoreCase ("note-effect")) return midiFx;
    return audioFx;
}

void BlockPicker::gather (const juce::String& current)
{
    items.clear();
    auto add = [&] (Item it) { it.current = ! it.header && it.name.equalsIgnoreCase (current); items.push_back (it); };
    auto head = [&] (const juce::String& text) { Item it; it.header = true; it.name = text; items.push_back (it); };

    head (utf ("TAPE\xc2\xb7" "FOUR"));
    for (int k = 0; k < BuiltIn::numSynths; ++k) { Item it; it.name = BuiltIn::synthName (k); it.sub = "INSTRUMENT"; it.category = instruments; it.kind = Kind::synth; it.index = k; add (it); }
    for (int k = 0; k < BuiltIn::numFx; ++k)     { Item it; it.name = BuiltIn::fxName (k);    it.sub = "AUDIO FX";   it.category = audioFx;     it.kind = Kind::fx;    it.index = k; add (it); }

    bool anyOut = false;
    for (auto& o : engine.getMidiOutputs())
    {
        if (! o.active) continue;
        if (! anyOut) { head ("HARDWARE SYNTHS"); anyOut = true; }
        Item it;
        it.name = engine.midiOutputLabel (o.slot);
        it.sub = "EXTERNAL";
        it.category = instruments;
        it.kind = Kind::external;
        it.index = o.slot;
        add (it);
    }

    auto types = engine.knownPlugins.getTypes();

    // A plug-in installed in several formats is listed once, in the best of them.
    {
        auto key = [] (const juce::PluginDescription& d) { return d.manufacturerName.toLowerCase().trim() + "|" + d.name.toLowerCase().trim(); };
        std::map<juce::String, int> best;
        for (auto& d : types)
        {
            auto k = key (d);
            auto pref = formatPreference (d.pluginFormatName);
            if (auto it = best.find (k); it == best.end() || pref < it->second) best[k] = pref;
        }
        types.removeIf ([&] (const juce::PluginDescription& d) { return formatPreference (d.pluginFormatName) != best[key (d)]; });
    }
    std::sort (types.begin(), types.end(), [] (const auto& a, const auto& b)
    {
        auto ma = a.manufacturerName.isEmpty() ? juce::String ("~") : a.manufacturerName.toLowerCase();
        auto mb = b.manufacturerName.isEmpty() ? juce::String ("~") : b.manufacturerName.toLowerCase();
        return ma != mb ? ma < mb : a.name.compareIgnoreCase (b.name) < 0;
    });
    // Plug-ins that share a name are all listed; where they came from tells them apart.
    juce::StringArray seen, repeated;
    for (auto& d : types)
    {
        auto key = d.name.toLowerCase();
        if (seen.contains (key)) repeated.addIfNotAlreadyThere (key); else seen.add (key);
    }
    juce::String maker = "\x01";
    for (auto& d : types)
    {
        auto m = d.manufacturerName.isNotEmpty() ? d.manufacturerName : juce::String ("Unknown maker");
        if (m != maker) { head (m.toUpperCase()); maker = m; }
        Item it;
        it.name = d.name.toUpperCase();
        it.category = categoryOf (d);
        it.sub = (it.category == instruments ? "INSTRUMENT" : it.category == midiFx ? "MIDI FX" : "AUDIO FX") + juce::String (" ") + d.pluginFormatName;
        if (repeated.contains (d.name.toLowerCase()))
        {
            // its folder and file, whichever system wrote the path
            auto parts = juce::StringArray::fromTokens (d.fileOrIdentifier.replaceCharacter ('\\', '/'), "/", {});
            parts.removeEmptyStrings();
            it.sub = parts.size() >= 2 ? parts[parts.size() - 2] + "/" + parts[parts.size() - 1] : d.fileOrIdentifier;
        }
        it.kind = Kind::plugin;
        it.desc = d;
        add (it);
    }
}

void BlockPicker::refilter()
{
    auto q = search.getText().trim().toLowerCase();
    shown.clear();
    const Item* pendingHeader = nullptr;
    for (auto& it : items)
    {
        if (it.header) { pendingHeader = &it; continue; }
        if (category != all && it.category != category) continue;
        bool match = q.isEmpty() || it.name.toLowerCase().contains (q) || it.sub.toLowerCase().contains (q)
                     || (pendingHeader != nullptr && pendingHeader->name.toLowerCase().contains (q));
        if (! match) continue;
        if (pendingHeader != nullptr) { shown.push_back (*pendingHeader); pendingHeader = nullptr; }
        shown.push_back (it);
    }
    list.updateContent();
    list.repaint();
}

int BlockPicker::getNumRows() { return (int) shown.size(); }

void BlockPicker::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool)
{
    if (! juce::isPositiveAndBelow (row, (int) shown.size())) return;
    auto& it = shown[(size_t) row];
    if (it.header)
    {
        g.setColour (Pal::lcdDim);
        g.setFont (DeckLookAndFeel::get().mono (10.0f));
        g.drawText (it.name, 4, 0, w - 8, h, juce::Justification::bottomLeft);
        return;
    }
    paintRow (g, w, h, it.current, it.name, it.current ? juce::String ("LOADED") : it.sub);
}

void BlockPicker::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    if (juce::isPositiveAndBelow (row, (int) shown.size()) && ! shown[(size_t) row].header)
        pick (shown[(size_t) row]);
}

void BlockPicker::pick (Item it)   // a copy: picking rebuilds the editor
{
    juce::Component::SafePointer<BlockPicker> safe (this);
    juce::MessageManager::callAsync ([safe, it] { if (safe != nullptr && safe->onPick) safe->onPick (it); });
}

//==============================================================================
ParamPicker::ParamPicker (juce::AudioProcessor& p)
{
    auto ps = p.getParameters();
    for (int i = 0; i < ps.size(); ++i)
        names.add (ps[i]->getName (40).toUpperCase());
    styleSearch (search, utf ("FIND A PARAMETER \xe2\x80\xa6"));
    search.onTextChange = [this] { refilter(); };
    addAndMakeVisible (search);
    list.setModel (this);
    list.setRowHeight (24);
    list.setColour (juce::ListBox::backgroundColourId, Pal::lcd);
    list.setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (list);
    refilter();
}

void ParamPicker::highlight (int index)
{
    chosen = index;
    for (int r = 0; r < (int) shown.size(); ++r)
        if (shown[(size_t) r] == index) list.scrollToEnsureRowIsOnscreen (r);
    list.repaint();
}

void ParamPicker::paint (juce::Graphics& g)
{
    g.setColour (Pal::lcdText);
    g.setFont (DeckLookAndFeel::get().mono (12.0f));
    g.drawFittedText ("LEARN: TOUCH A CONTROL IN THE PLUG-IN'S WINDOW, OR PICK ONE, THEN MOVE A KNOB ON YOUR KEYBOARD",
                      getLocalBounds().removeFromTop (30), juce::Justification::centredLeft, 2, 0.8f);
}

void ParamPicker::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (34);
    search.setBounds (r.removeFromTop (30));
    r.removeFromTop (6);
    list.setBounds (r);
}

void ParamPicker::refilter()
{
    auto q = search.getText().trim().toLowerCase();
    shown.clear();
    for (int i = 0; i < names.size(); ++i)
        if (q.isEmpty() || names[i].toLowerCase().contains (q))
            shown.push_back (i);
    list.updateContent();
    list.repaint();
}

int ParamPicker::getNumRows() { return (int) shown.size(); }

void ParamPicker::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool)
{
    if (! juce::isPositiveAndBelow (row, (int) shown.size())) return;
    auto i = shown[(size_t) row];
    paintRow (g, w, h, i == chosen, names[i], "PARAM " + juce::String (i + 1));
}

void ParamPicker::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    if (juce::isPositiveAndBelow (row, (int) shown.size()) && onPick)
    {
        highlight (shown[(size_t) row]);
        onPick (shown[(size_t) row]);
    }
}

} // namespace tf
