#include "MasterDeckCard.h"
#include "TapeShelf.h"

namespace tf
{
namespace
{
    constexpr int pad = 22, leftW = 250, rightX = pad + leftW + 24, labelW = 100, rowH = 40, keyGap = 8;
    const char* gradeNames[] { "NEW", "USED", "WORN", "CHEWED" };

    void smallCaps (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r, juce::Colour c = Pal::muted,
                    juce::Justification j = juce::Justification::centredLeft)
    {
        g.setColour (c);
        g.setFont (DeckLookAndFeel::get().cond (13.0f).withExtraKerningFactor (0.22f));
        g.drawText (text, r, j, false);
    }
}

//==============================================================================
/** The takes on the master tape: click one to hear it off the tape. */
class MasterDeckCard::Takes : public juce::Component,
                              private juce::ListBoxModel
{
public:
    Takes (AudioEngine& e, MasterDeckCard& c) : engine (e), card (c)
    {
        list.setModel (this);
        list.setRowHeight (22);
        list.setColour (juce::ListBox::backgroundColourId, Pal::lcd);
        list.setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (list);
    }

    void update()
    {
        // newest first
        rows.assign (engine.masterDeck.takes.rbegin(), engine.masterDeck.takes.rend());
        list.updateContent();
        list.repaint();
    }

    void resized() override { list.setBounds (getLocalBounds()); }

private:
    int getNumRows() override { return std::max (1, (int) rows.size()); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
    {
        auto& lf = DeckLookAndFeel::get();
        if (rows.empty())
        {
            g.setColour (Pal::lcdDim);
            g.setFont (lf.mono (12.0f));
            g.drawText ("NO MIXES ON THIS TAPE YET: ARM MIXDOWN, THEN PLAY", 10, 0, w - 20, h, juce::Justification::centredLeft);
            return;
        }
        if (! juce::isPositiveAndBelow (row, (int) rows.size())) return;
        const auto& t = rows[(size_t) row];
        g.setColour (row == 0 ? Pal::lcdText : Pal::lcdDim);
        g.setFont (lf.mono (12.5f));
        auto where = t.side < 0 ? juce::String ("--") : juce::String (t.side == 0 ? "A " : "B ") + formatTime (t.start / cellRate);
        juce::String date;
        if (t.created.length() >= 16)
        {
            static const char* months[] { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
            auto m = juce::jlimit (1, 12, t.created.substring (5, 7).getIntValue());
            date = t.created.substring (8, 10) + " " + months[m - 1] + " " + t.created.substring (11, 16);
        }
        int x = 10;
        auto col = [&] (const juce::String& text, int cw) { g.drawText (text, x, 0, cw, h, juce::Justification::centredLeft); x += cw; };
        col ("MIX " + juce::String (t.number), 70);
        col (where, 80);
        col (formatTime (t.length / cellRate), 60);
        col (t.onTape, 150);
        col (t.formats, 170);
        col (date, 130);
        if (t.side >= 0)
        {
            g.setColour (Pal::lcdText);
            g.drawText (engine.masterDeck.isListening() && listening == t.number ? utf ("\xe2\x96\xa0 STOP") : utf ("\xe2\x96\xb6 LISTEN"),
                        w - 90, 0, 80, h, juce::Justification::centredRight);
        }
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        if (! juce::isPositiveAndBelow (row, (int) rows.size())) return;
        const auto& t = rows[(size_t) row];
        if (t.side < 0) return;   // DIRECT: nothing on tape
        if (engine.masterDeck.isListening() && listening == t.number)
        {
            engine.masterDeck.stopListening();
            listening = 0;
        }
        else if (card.onListen)
        {
            listening = t.number;
            card.onListen (t);
        }
        list.repaint();
    }

    AudioEngine& engine;
    MasterDeckCard& card;
    juce::ListBox list;
    std::vector<MixTake> rows;
    int listening = 0;
};

//==============================================================================
MasterDeckCard::MasterDeckCard (AudioEngine& e, ExportFormats f)
    : engine (e), formats (f),
      recLevel ("REC LEVEL", false, [] (double v) { return (v > 0.05 ? "+" : "") + juce::String (v, 1) + " dB"; })
{
    auto& ms = engine.masterDeck.settings;
    auto changed = [this] { if (onSettingsChanged) onSettingsChanged(); refresh(); };

    for (int k = 0; k < numTapeModels; ++k)
        key (tapeKeys, tapeModels[k].name, [this, k, changed] { engine.masterDeck.settings.model = k; changed(); });
    key (tapeKeys, "DIRECT", [this, changed] { engine.masterDeck.settings.model = -1; changed(); });
    tapeKeys.getLast()->setTooltip ("No master tape: the mix goes straight to the files");
    for (int i = 0; i < numTapeTypes; ++i)
        key (lengthKeys, juce::String ("C") + juce::String (tapeTypes[i].minutes * 2), [this, i, changed] { engine.masterDeck.settings.length = i; changed(); });
    key (nrKeys, "OFF", [this, changed] { engine.masterDeck.settings.nr = false; changed(); });
    key (nrKeys, "ON", [this, changed] { engine.masterDeck.settings.nr = true; changed(); });
    for (int g = 0; g < 4; ++g)
    {
        auto grade = juce::String (tfmt::Condition::grades()[(size_t) g]);
        key (gradeKeys, gradeNames[g], [this, grade, changed] { engine.masterDeck.settings.grade = grade; changed(); });
    }
    key (formatKeys, "WAV",  [this] { auto g = formats; g.wav = ! g.wav; if (g.any()) setFormats (g); });
    key (formatKeys, "FLAC", [this] { auto g = formats; g.flac = ! g.flac; if (g.any()) setFormats (g); });
    key (formatKeys, "MP3",  [this] { auto g = formats; g.mp3 = ! g.mp3; if (g.any()) setFormats (g); });
    key (formatKeys, "ALL",  [this] { auto g = formats; g.wav = g.flac = g.mp3 = true; setFormats (g); });
    key (sideKeys, "SIDE A", [this] { if (engine.masterDeck.settings.side != 0 && onFlip) onFlip(); refresh(); });
    key (sideKeys, "SIDE B", [this] { if (engine.masterDeck.settings.side != 1 && onFlip) onFlip(); refresh(); });

    auto depthMenu = [this] (LcdButton& b, std::function<void (juce::PopupMenu&)> fill)
    {
        b.topSize = 14.0f;
        b.onClick = [&b, fill]
        {
            juce::PopupMenu m;
            fill (m);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&b));
        };
        addAndMakeVisible (b);
    };
    depthMenu (wavDepth, [this] (juce::PopupMenu& m)
    {
        for (int b : { 16, 24, 32 })
            m.addItem (b == 32 ? juce::String ("32-bit float") : juce::String (b) + "-bit", true, formats.wavBits == b, [this, b] { auto g = formats; g.wavBits = b; setFormats (g); });
    });
    depthMenu (flacDepth, [this] (juce::PopupMenu& m)
    {
        for (int b : { 16, 24 })
            m.addItem (juce::String (b) + "-bit", true, formats.flacBits == b, [this, b] { auto g = formats; g.flacBits = b; setFormats (g); });
    });
    depthMenu (mp3Rate, [this] (juce::PopupMenu& m)
    {
        for (int k : { 192, 256, 320 })
            m.addItem (juce::String (k) + " kbps", true, formats.mp3Kbps == k, [this, k] { auto g = formats; g.mp3Kbps = k; setFormats (g); });
    });
    nextTape.topSize = 13.0f;
    addAndMakeVisible (nextTape);

    recLevel.stacked = true;
    recLevel.knobSize = 44;
    recLevel.slider.setRange (-12.0, 6.0);
    recLevel.slider.setDoubleClickReturnValue (true, 0.0);
    recLevel.slider.setValue (ms.recLevelDb, juce::dontSendNotification);
    recLevel.slider.onValueChange = [this]
    {
        engine.masterDeck.settings.recLevelDb = (float) recLevel.slider.getValue();
        engine.masterDeck.setRecLevel (engine.masterDeck.settings.recLevelDb);
        recLevel.repaint();
    };
    recLevel.slider.onDragEnd = [this] { if (onSettingsChanged) onSettingsChanged(); };
    recLevel.slider.setTooltip ("How hard the mix drives the master tape. Hotter: more saturation, less hiss.");
    addAndMakeVisible (recLevel);

    closeX.textSize = 20.0f;
    closeX.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeX);
    for (auto* b : { &saveSide, &showFolder, &newTape, &exportCass, &masterTapes })
    {
        b->textSize = 10.5f;
        addAndMakeVisible (b);
    }
    saveSide.onClick = [this] { if (onSaveSide) onSaveSide (engine.masterDeck.settings.side); };
    saveSide.setTooltip ("This side of the master tape, played through the master deck, as one file: the way you'd hear the cassette");
    showFolder.onClick = [this] { if (onShowFolder) onShowFolder(); };
    newTape.onClick = [this] { if (onNewMasterTape) onNewMasterTape(); };
    newTape.setTooltip ("A new blank master tape of the kind chosen above. The old one stays on the shelf with its mixes.");
    exportCass.onClick = [this] { if (onExportCassette) onExportCassette(); };
    exportCass.setTooltip ("The master tape as a .cass file, for any deck that plays cassettes");
    masterTapes.onClick = [this]
    {
        juce::PopupMenu m;
        m.addSectionHeader ("MASTER TAPES ON THE SHELF");
        auto* cur = engine.masterDeck.getTape();
        int n = 0;
        for (auto& entry : TapeShelf::list())
            if (entry.layout == tfmt::Layout::stereoTwoSides)
            {
                auto id = entry.id;
                m.addItem ((entry.name.isNotEmpty() ? entry.name : juce::String ("(no label)")) + utf ("   \xc2\xb7   ") + entry.formulation
                               + juce::String (entry.minutes * 2) + "  " + entry.created,
                           true, cur != nullptr && cur->id == id, [this, id] { if (onLoadMaster) onLoadMaster (id); });
                ++n;
            }
        if (n == 0) m.addItem ("None yet", false, false, nullptr);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&masterTapes));
    };

    takes = std::make_unique<Takes> (engine, *this);
    addAndMakeVisible (*takes);
    refresh();
    startTimerHz (6);
}

MasterDeckCard::~MasterDeckCard() = default;

LedKey* MasterDeckCard::key (juce::OwnedArray<LedKey>& row, const juce::String& text, std::function<void()> click)
{
    auto* k = row.add (new LedKey (text, Pal::amber));
    k->textSize = 11.0f;
    k->onClick = std::move (click);
    addAndMakeVisible (k);
    return k;
}

void MasterDeckCard::setFormats (const ExportFormats& f)
{
    formats = f;
    if (onFormatsChanged) onFormatsChanged (f);
    refresh();
}

void MasterDeckCard::refresh()
{
    const auto& md = engine.masterDeck;
    const auto& ms = md.settings;
    const auto st = md.getState();
    const bool taking = st != MasterDeck::State::off && st != MasterDeck::State::armed;

    for (int k = 0; k < tapeKeys.size(); ++k) tapeKeys[k]->setLed (k < numTapeModels ? ms.model == k : ms.direct());
    for (int i = 0; i < lengthKeys.size(); ++i) lengthKeys[i]->setLed (! ms.direct() && ms.length == i);
    nrKeys[0]->setLed (! ms.nr);
    nrKeys[1]->setLed (ms.nr);
    for (int g = 0; g < gradeKeys.size(); ++g) gradeKeys[g]->setLed (! ms.direct() && ms.grade == juce::String (tfmt::Condition::grades()[(size_t) g]));
    formatKeys[0]->setLed (formats.wav);
    formatKeys[1]->setLed (formats.flac);
    formatKeys[2]->setLed (formats.mp3);
    formatKeys[3]->setLed (formats.all());
    sideKeys[0]->setLed (! ms.direct() && ms.side == 0);
    sideKeys[1]->setLed (! ms.direct() && ms.side == 1);

    // Nothing that changes the tape while a take is running.
    for (auto* row : { &tapeKeys, &lengthKeys, &nrKeys, &gradeKeys, &sideKeys })
        for (auto* k : *row) k->setEnabled (! taking);
    for (auto* k : lengthKeys) k->setEnabled (! taking && ! ms.direct());
    for (auto* k : gradeKeys) k->setEnabled (! taking && ! ms.direct());
    for (auto* k : sideKeys) k->setEnabled (! taking && ! ms.direct());
    for (auto* k : nrKeys) k->setEnabled (! taking && ! ms.direct());
    for (auto* b : { &saveSide, &newTape, &exportCass, &masterTapes }) b->setEnabled (! taking && ! ms.direct());

    wavDepth.top = "WAV";
    wavDepth.sub = (formats.wavBits == 32 ? juce::String ("32-BIT FLOAT") : juce::String (formats.wavBits) + "-BIT") + utf (" \xe2\x96\xbe");
    flacDepth.top = "FLAC";
    flacDepth.sub = juce::String (formats.flacBits) + utf ("-BIT \xe2\x96\xbe");
    mp3Rate.top = "MP3";
    mp3Rate.sub = juce::String (formats.mp3Kbps) + utf (" KBPS \xe2\x96\xbe");
    wavDepth.setAlpha (formats.wav ? 1.0f : 0.45f);
    flacDepth.setAlpha (formats.flac ? 1.0f : 0.45f);
    mp3Rate.setAlpha (formats.mp3 ? 1.0f : 0.45f);

    auto* tape = md.getTape();
    if (ms.direct())
    {
        nextTape.top = "NO TAPE";
        nextTape.sub = "THE MIX GOES STRAIGHT TO THE FILES";
    }
    else
    {
        const auto wanted = juce::String (tapeModels[juce::jlimit (0, numTapeModels - 1, ms.model)].name) + juce::String (tapeTypes[ms.length].minutes * 2)
                            + " " + ms.grade.toUpperCase();
        const auto inDeck = tape != nullptr ? tape->productName() + " " + juce::String (tape->getCondition().grade).toUpperCase() : juce::String ("NONE");
        nextTape.top = "IN THE DECK: " + inDeck;
        nextTape.sub = wanted == inDeck ? formatTime (md.sideLengthSeconds()) + " EACH SIDE" : "NEXT NEW TAPE: " + wanted;
    }
    for (auto* b : { &wavDepth, &flacDepth, &mp3Rate, &nextTape }) b->repaintIfChanged();

    // The side maps and the list, when anything on them has changed.
    juce::String now = juce::String ((int) md.takes.size()) + ":" + juce::String (ms.side) + ":" + juce::String ((int) md.isListening())
                       + ":" + (tape != nullptr ? tape->id : juce::String()) + ":" + juce::String ((int) ms.direct());
    if (now != shown)
    {
        shown = now;
        takes->update();
        repaint();
    }
}

void MasterDeckCard::timerCallback()
{
    refresh();
}

void MasterDeckCard::resized()
{
    closeX.setBounds (width - pad - 44, 16, 44, 40);

    // left: the cassette, its sides, the record level
    cassetteArea = { pad, 72, leftW, 158 };
    auto sides = juce::Rectangle<int> (pad, 242, leftW, 40);
    sideKeys[0]->setBounds (sides.removeFromLeft ((leftW - keyGap) / 2));
    sides.removeFromLeft (keyGap);
    sideKeys[1]->setBounds (sides);
    recLevel.setBounds (pad, 316, leftW, 48);

    // right: what kind of tape, and the files
    labelColumn = { rightX, 72, labelW, 340 };
    auto row = [&] (int y, juce::OwnedArray<LedKey>& keys, int keyW, int separatorBefore = -1)
    {
        int x = rightX + labelW;
        for (int i = 0; i < keys.size(); ++i)
        {
            if (i == separatorBefore) x += 14;
            keys[i]->setBounds (x, y, keyW, rowH);
            x += keyW + keyGap;
        }
        return x;
    };
    row (72, tapeKeys, 74, numTapeModels);
    auto afterLength = row (126, lengthKeys, 74);
    nextTape.setBounds (afterLength, 126, width - pad - afterLength, rowH);
    row (180, nrKeys, 74);
    row (234, gradeKeys, 92);
    row (300, formatKeys, 74);
    auto depthY = 354;
    wavDepth.setBounds (rightX + labelW, depthY, 136, 42);
    flacDepth.setBounds (rightX + labelW + 136 + keyGap, depthY, 136, 42);
    mp3Rate.setBounds (rightX + labelW + 2 * (136 + keyGap), depthY, 136, 42);

    // below: the tape's two sides, and the mixes on it
    auto buttons = juce::Rectangle<int> (pad, 438, width - 2 * pad, 34);
    const int bw = (buttons.getWidth() - 4 * keyGap) / 5;
    for (auto* b : { &saveSide, &showFolder, &newTape, &exportCass, &masterTapes })
    {
        b->setBounds (buttons.removeFromLeft (bw));
        buttons.removeFromLeft (keyGap);
    }
    mapArea = { pad, 486, width - 2 * pad, 62 };
    takes->setBounds (pad, 560, width - 2 * pad, height - 560 - 18);
}

void MasterDeckCard::paint (juce::Graphics& g)
{
    auto& lf = DeckLookAndFeel::get();
    auto r = getLocalBounds().toFloat();
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (r, 10.0f);

    g.setColour (Pal::cream);
    g.setFont (lf.cond (28.0f, true).withExtraKerningFactor (0.08f));
    g.drawText ("MASTER DECK", pad, 16, 220, 40, juce::Justification::centredLeft);
    smallCaps (g, "THE STEREO CASSETTE YOUR MIX GOES TO", { pad + 210, 16, 420, 40 });

    const auto& md = engine.masterDeck;
    const auto& ms = md.settings;
    auto* tape = md.getTape();

    // The cassette: its shell, label and reels, and which side faces up.
    {
        auto bay = cassetteArea.toFloat();
        g.setColour (Pal::bay);
        g.fillRoundedRectangle (bay, 8.0f);
        auto shell = bay.withSizeKeepingCentre (224.0f, 140.0f);
        if (ms.direct() || tape == nullptr)
        {
            smallCaps (g, ms.direct() ? "DIRECT: NO TAPE" : "NO MASTER TAPE", cassetteArea, Pal::faint, juce::Justification::centred);
        }
        else
        {
            const auto& tm = tape->getModelInfo();
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a3e46), shell.getX(), shell.getY(), juce::Colour (0xff2a2d33), shell.getX(), shell.getBottom(), false));
            g.fillRoundedRectangle (shell, 8.0f);
            auto top = shell.reduced (10.0f, 8.0f).removeFromTop (34.0f);
            g.setColour (juce::Colour (0xff24272d));
            g.fillRoundedRectangle (top, 3.0f);
            g.setColour (juce::Colour (tm.accent));
            g.setFont (lf.cond (22.0f, true).italicised());
            g.drawText (tape->productName(), top.reduced (8.0f, 0).toNearestInt(), juce::Justification::centredLeft);
            auto badge = top.removeFromRight (30.0f).reduced (5.0f);
            g.setColour (Pal::orange);
            g.fillRoundedRectangle (badge, 3.0f);
            g.setColour (Pal::body);
            g.setFont (lf.cond (16.0f, true));
            g.drawText (ms.side == 0 ? "A" : "B", badge.toNearestInt(), juce::Justification::centred);
            for (auto cx : { shell.getX() + 57.0f, shell.getRight() - 57.0f })
            {
                auto reel = juce::Rectangle<float> (46.0f, 46.0f).withCentre ({ cx, shell.getY() + 76.0f });
                g.setColour (juce::Colour (0xfff4f2ee));
                g.fillEllipse (reel);
                g.setColour (juce::Colour (tm.accent));
                g.drawEllipse (reel.reduced (6.0f), 2.0f);
                g.setColour (juce::Colour (0xff9a9a9a));
                g.fillEllipse (reel.withSizeKeepingCentre (16.0f, 16.0f));
            }
            auto label = juce::Rectangle<float> (shell.getX() + 10.0f, shell.getBottom() - 30.0f, shell.getWidth() - 20.0f, 20.0f);
            g.setColour (Pal::paper);
            g.fillRoundedRectangle (label, 2.0f);
            g.setColour (Pal::ink);
            g.setFont (lf.hand (17.0f));
            g.drawText (tape->name.isNotEmpty() ? tape->name : juce::String ("master tape"), label.reduced (6.0f, 0).toNearestInt(), juce::Justification::centredLeft);
        }
    }
    smallCaps (g, "CLICK A SIDE TO TURN IT OVER", { pad, 286, leftW, 20 }, Pal::faint, juce::Justification::centred);

    // labels down the right
    smallCaps (g, "TAPE", { rightX, 72, labelW, rowH });
    smallCaps (g, "LENGTH", { rightX, 126, labelW, rowH });
    smallCaps (g, "NR", { rightX, 180, labelW, rowH });
    smallCaps (g, "CONDITION", { rightX, 234, labelW, rowH });
    g.setColour (Pal::line);
    g.drawHorizontalLine (286, (float) rightX, (float) (width - pad));
    smallCaps (g, "FILES", { rightX, 300, labelW, rowH });
    if (! ms.direct())
    {
        static const char* nrNames[] { "2:1 COMPANDER", "SLIDING BAND B", "SLIDING BAND C" };
        smallCaps (g, juce::String (nrNames[juce::jlimit (0, 2, engine.tapeEngine.params.nrType.load())]) + ", AS THE DECK",
                   { rightX + labelW + 2 * (74 + keyGap) + 8, 180, 300, rowH }, Pal::faint);
    }

    smallCaps (g, "WHAT'S ON THE MASTER: EACH MIX GOES AFTER THE LAST, WITH A GAP", { pad, 410, width - 2 * pad, 22 });

    // The two sides, with the mixes on each.
    if (tape != nullptr && ! ms.direct())
    {
        const auto len = (double) tape->getLengthCells();
        for (int side = 0; side < 2; ++side)
        {
            auto line = mapArea.withHeight (26).translated (0, side * 34);
            auto badge = line.removeFromLeft (26).toFloat().reduced (1.0f);
            g.setColour (side == ms.side ? Pal::orange : Pal::line);
            g.fillRoundedRectangle (badge, 3.0f);
            g.setColour (side == ms.side ? Pal::body : Pal::cream);
            g.setFont (lf.cond (17.0f, true));
            g.drawText (side == 0 ? "A" : "B", badge.toNearestInt(), juce::Justification::centred);
            line.removeFromLeft (12);
            auto info = line.removeFromRight (170);
            line.removeFromRight (10);
            g.setColour (Pal::dark);
            g.fillRoundedRectangle (line.toFloat(), 3.0f);
            for (auto& t : md.takes)
            {
                if (t.side != side) continue;
                auto x0 = line.getX() + (int) (line.getWidth() * t.start / len);
                auto x1 = line.getX() + std::max ((int) (line.getWidth() * (t.start + t.length) / len), x0 + 3);
                auto seg = juce::Rectangle<int> (x0, line.getY(), x1 - x0, line.getHeight());
                g.setColour (side == ms.side ? Pal::lcdDim : Pal::lcdDim.withAlpha (0.5f));
                g.fillRect (seg.withTrimmedRight (1));
                if (seg.getWidth() > 46)
                {
                    g.setColour (Pal::lcd);
                    g.setFont (lf.mono (10.5f));
                    g.drawText ("MIX " + juce::String (t.number), seg.reduced (4, 0), juce::Justification::centredLeft);
                }
            }
            const auto used = md.sideUsedSeconds (side);
            g.setColour (side == ms.side ? Pal::lcdText : Pal::lcdDim);
            g.setFont (lf.mono (12.0f));
            g.drawText (formatTime (used) + utf (" \xc2\xb7 ") + formatTime (std::max (0.0, tape->getLengthSeconds() - used)) + " LEFT", info,
                        juce::Justification::centredRight);
        }
    }
    else
    {
        smallCaps (g, ms.direct() ? "DIRECT MIXES GO STRAIGHT TO FILES: NOTHING ON TAPE" : "NO MASTER TAPE IN THE DECK", mapArea, Pal::faint,
                   juce::Justification::centred);
    }
}
} // namespace tf
