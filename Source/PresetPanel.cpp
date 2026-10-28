#include "PresetPanel.h"
#include "InstrumentWidgets.h"

namespace tf
{
using iw::text;

PresetDisplay::PresetDisplay (presets::Presettable& p) : inst (p)
{
    shown = label();
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    startTimerHz (5);
}

juce::String PresetDisplay::label() const
{
    const auto name = inst.currentPreset();
    if (name.isEmpty()) return "NO PRESET";
    return name.toUpperCase() + (inst.isModified() ? " *" : "");
}

void PresetDisplay::timerCallback()
{
    auto l = label();
    if (l != shown) { shown = l; repaint(); }
}

void PresetDisplay::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (Pal::lcd);
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (open || isMouseOver() ? Pal::lcdText : Pal::lcdRule.brighter (0.4f));
    g.drawRoundedRectangle (r, 3.0f, 1.0f);
    auto t = getLocalBounds().reduced (8, 0);
    text (g, open ? "^" : "v", t.removeFromRight (12), Pal::lcdDim, 11.0f, juce::Justification::centred);
    text (g, shown, t, inst.currentPreset().isEmpty() ? Pal::lcdDim : Pal::lcdText, 12.0f);
}

//==============================================================================
PresetList::PresetList (presets::Presettable& p) : inst (p)
{
    setWantsKeyboardFocus (true);
    const char* labels[] { "SAVE", "SAVE AS NEW", "RENAME", "DELETE", "SHOW FOLDER" };
    for (auto* l : labels)
    {
        auto* c = chips.add (new LcdChip (l));
        addAndMakeVisible (c);
    }
    chips[0]->onClick = [this]
    {
        if (inst.currentPreset().isEmpty()) { setMode (Mode::naming); return; }
        if (! presets::save (inst, inst.currentPreset(), error)) { repaint(); return; }
        error.clear();
        refresh();
        if (onMessage) onMessage ("SAVED " + inst.currentPreset().toUpperCase());
    };
    chips[1]->onClick = [this] { setMode (Mode::naming); };
    chips[2]->onClick = [this] { if (inst.currentPreset().isNotEmpty()) setMode (Mode::renaming); };
    chips[3]->onClick = [this] { if (inst.currentPreset().isNotEmpty()) setMode (Mode::deleting); };
    chips[4]->onClick = [this]
    {
        auto f = presets::folder (inst.presetKind());
        f.createDirectory();
        auto current = presets::fileFor (inst.presetKind(), inst.currentPreset());
        if (inst.currentPreset().isNotEmpty() && current.existsAsFile()) current.revealToUser();
        else f.startAsProcess();
    };

    nameBox.setFont (DeckLookAndFeel::get().mono (12.5f));
    nameBox.setColour (juce::TextEditor::backgroundColourId, Pal::lcd);
    nameBox.setColour (juce::TextEditor::textColourId, Pal::lcdText);
    nameBox.setColour (juce::TextEditor::outlineColourId, Pal::lcdRule.brighter (0.4f));
    nameBox.setColour (juce::TextEditor::focusedOutlineColourId, Pal::lcdText);
    nameBox.setColour (juce::CaretComponent::caretColourId, Pal::lcdText);
    nameBox.setColour (juce::TextEditor::highlightColourId, Pal::lcdText.withAlpha (0.3f));
    nameBox.setInputRestrictions (60, {});
    nameBox.setIndents (8, 5);
    nameBox.onReturnKey = [this] { commitName(); };
    nameBox.onEscapeKey = [this] { setMode (Mode::list); };
    addChildComponent (nameBox);
    refresh();
}

PresetList::~PresetList() = default;

void PresetList::refresh()
{
    entries = presets::list (inst.presetKind());
    const bool has = inst.currentPreset().isNotEmpty();
    chips[2]->dim = chips[3]->dim = ! has;
    chips[0]->chosen = false;
    chips[1]->chosen = mode == Mode::naming;
    chips[2]->chosen = mode == Mode::renaming;
    chips[3]->chosen = mode == Mode::deleting;
    for (auto* c : chips) c->repaint();
    scroll = juce::jlimit (0, std::max (0, (int) entries.size() * rowH - std::max (rowH, listArea.getHeight())), scroll);
    repaint();
}

void PresetList::setMode (Mode m)
{
    mode = m;
    error.clear();
    lineChips.clear();
    nameBox.setVisible (m == Mode::naming || m == Mode::renaming);
    if (nameBox.isVisible())
    {
        nameBox.setText (m == Mode::renaming ? inst.currentPreset() : juce::String(), false);
        auto* ok = lineChips.add (new LcdChip (m == Mode::renaming ? "RENAME" : "SAVE"));
        ok->chosen = true;
        ok->onClick = [this] { commitName(); };
    }
    else if (m == Mode::deleting)
    {
        auto* yes = lineChips.add (new LcdChip ("DELETE IT"));
        yes->chosen = true;
        yes->onClick = [this]
        {
            const auto name = inst.currentPreset();
            if (! presets::remove (inst, error)) { repaint(); return; }
            if (onMessage) onMessage ("DELETED " + name.toUpperCase());
            setMode (Mode::list);
        };
        auto* no = lineChips.add (new LcdChip ("KEEP IT"));
        no->onClick = [this] { setMode (Mode::list); };
    }
    for (auto* c : lineChips) addAndMakeVisible (c);
    layout();
    refresh();
    if (nameBox.isVisible())
    {
        juce::Component::SafePointer<PresetList> safe (this);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr && safe->nameBox.isShowing()) { safe->nameBox.grabKeyboardFocus(); safe->nameBox.selectAll(); } });
    }
}

void PresetList::commitName()
{
    const auto name = nameBox.getText().trim();
    bool ok;
    if (mode == Mode::renaming) ok = presets::rename (inst, name, error);
    else
    {
        if (name.isNotEmpty() && presets::fileFor (inst.presetKind(), name).existsAsFile() && name != inst.currentPreset())
        {
            error = "THERE'S ALREADY ONE CALLED THAT";
            repaint();
            return;
        }
        ok = presets::save (inst, name, error);
    }
    if (! ok) { repaint(); return; }
    if (onMessage) onMessage ((mode == Mode::renaming ? "RENAMED " : "SAVED ") + name.toUpperCase());
    setMode (Mode::list);
}

void PresetList::resized() { layout(); }

void PresetList::layout()
{
    auto r = getLocalBounds().reduced (10, 10);
    int x = r.getX();
    for (auto* c : chips)
    {
        c->setBounds (x, r.getY(), c->idealWidth() + 8, 26);
        x += c->getWidth() + 6;
    }
    r.removeFromTop (36);
    lineArea = {};
    if (mode != Mode::list)
    {
        lineArea = r.removeFromTop (26);
        auto line = lineArea.withTrimmedLeft (84);
        int right = line.getRight();
        juce::Array<LcdChip*> rev;
        for (auto* c : lineChips) rev.add (c);
        if (mode == Mode::deleting)
        {
            int cx = line.getX();
            for (auto* c : lineChips) { c->setBounds (cx, line.getY(), c->idealWidth() + 8, 26); cx += c->getWidth() + 6; }
        }
        else
        {
            right -= 230;   // room for the hint
            for (int i = rev.size(); --i >= 0;) { auto w = rev[i]->idealWidth() + 24; rev[i]->setBounds (right - w, line.getY(), w, 26); right -= w + 8; }
            nameBox.setBounds (line.withRight (right));
        }
        r.removeFromTop (10);
    }
    r.removeFromTop (error.isNotEmpty() ? 4 : 0);
    headArea = r.removeFromTop (22);
    listArea = r;
}

void PresetList::paint (juce::Graphics& g)
{
    auto box = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (Pal::lcdText.withAlpha (0.7f));
    g.drawRoundedRectangle (box, 4.0f, 1.0f);

    if (mode != Mode::list)
    {
        text (g, mode == Mode::deleting ? "DELETE" : "NAME", lineArea.withWidth (80), Pal::lcdDim, 10.5f);
        if (mode == Mode::deleting)
        {
            int cx = lineChips.isEmpty() ? lineArea.getX() + 84 : lineChips.getLast()->getRight() + 12;
            text (g, "\"" + inst.currentPreset().toUpperCase() + "\" FOR GOOD? ITS FILE GOES TOO.", lineArea.withLeft (cx), Pal::lcdText, 10.5f);
        }
        else
        {
            const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
            text (g, "RETURN " + juce::String (mode == Mode::renaming ? "RENAMES" : "SAVES") + dot + "ESC CANCELS",
                  lineArea.withLeft (lineArea.getRight() - 222), Pal::lcdDim.withAlpha (0.8f), 9.5f);
        }
    }

    auto head = headArea;
    g.setColour (Pal::lcdRule);
    g.fillRect (head.getX(), head.getY(), head.getWidth(), 1);
    if (error.isNotEmpty())
        text (g, error, head.withTrimmedTop (2), juce::Colour (0xffff7a6a), 10.5f);
    else
        text (g, "YOUR PRESETS" + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + presets::folder (inst.presetKind()).getRelativePathFrom (juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getParentDirectory()).replaceCharacter ('\\', '/').toUpperCase(),
              head.withTrimmedTop (2), Pal::lcdDim.withAlpha (0.8f), 9.5f);

    g.saveState();
    g.reduceClipRegion (listArea);
    if (entries.empty())
        text (g, "NONE YET. SAVE AS NEW KEEPS THIS " + inst.presetKind().noun + ", EVERY CONTROL AND ITS TUNING, UNDER A NAME.",
              listArea.withHeight (rowH), Pal::lcdDim, 10.5f);
    const bool modified = inst.isModified();
    for (int i = 0; i < (int) entries.size(); ++i)
    {
        auto row = juce::Rectangle<int> (listArea.getX(), listArea.getY() + i * rowH - scroll, listArea.getWidth(), rowH);
        if (! row.intersects (listArea)) continue;
        const auto& e = entries[(size_t) i];
        const bool current = e.name == inst.currentPreset();
        if (current)
        {
            g.setColour (Pal::lcdText);
            g.fillRoundedRectangle (row.toFloat().reduced (0.0f, 1.0f), 2.0f);
        }
        else if (i == hover)
        {
            g.setColour (Pal::lcdText.withAlpha (0.12f));
            g.fillRoundedRectangle (row.toFloat().reduced (0.0f, 1.0f), 2.0f);
        }
        const auto ink = current ? Pal::lcd : Pal::lcdText;
        const auto dim = current ? Pal::lcd.withAlpha (0.8f) : Pal::lcdDim;
        auto t = row.reduced (8, 0);
        const int nameW = std::min (270, t.getWidth() / 3);
        text (g, e.name.toUpperCase() + (current && modified ? " *" : ""), t.removeFromLeft (nameW), ink, 12.0f);
        text (g, e.saved.formatted ("%d %b %Y").toUpperCase(), t.removeFromRight (110), dim, 9.5f, juce::Justification::centredRight);
        text (g, e.summary, t, dim, 9.5f);
    }
    g.restoreState();
}

int PresetList::rowAt (juce::Point<int> p) const
{
    if (! listArea.contains (p)) return -1;
    const int i = (p.y - listArea.getY() + scroll) / rowH;
    return i < (int) entries.size() ? i : -1;
}

void PresetList::mouseMove (const juce::MouseEvent& e)
{
    const int h = rowAt (e.getPosition());
    if (h != hover) { hover = h; repaint(); }
    setMouseCursor (h >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void PresetList::mouseExit (const juce::MouseEvent&) { if (hover >= 0) { hover = -1; repaint(); } }

void PresetList::mouseUp (const juce::MouseEvent& e)
{
    const int i = rowAt (e.getPosition());
    if (i < 0) return;
    const auto entry = entries[(size_t) i];   // a copy: loading may refresh the list
    if (! presets::load (inst, entry.file, error)) { repaint(); return; }
    if (onMessage) onMessage ("LOADED " + entry.name.toUpperCase());
    if (onLoaded) onLoaded();
}

void PresetList::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    const int most = std::max (0, (int) entries.size() * rowH - listArea.getHeight());
    scroll = juce::jlimit (0, most, scroll - juce::roundToInt (w.deltaY * 240.0f));
    repaint();
}

bool PresetList::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey && mode != Mode::list) { setMode (Mode::list); return true; }
    return false;
}
} // namespace tf
