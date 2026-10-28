#include "InlayCard.h"

namespace tf
{
namespace
{
    const juce::Colour paper { 0xfff6f3ea }, band { 0xff26232c }, fold { 0xffb5ad98 }, ink { 0xff1a1917 }, gold { 0xffcdb57c };
    constexpr int headerH = 128, rowH = 32, pad = 22;

    void brandText (juce::Graphics& g, juce::Rectangle<float> r, float size, juce::Justification j, juce::Colour main)
    {
        auto& lf = DeckLookAndFeel::get();
        juce::AttributedString s;
        s.append ("TAPE", lf.brand (size), main);
        s.append (utf ("\xc2\xb7"), lf.brand (size), Pal::cassetteOrange);
        s.append ("FOUR", lf.brand (size), main);
        s.setJustification (j);
        s.draw (g, r);
    }
}

InlayCard::InlayCard (Tape& t) : tape (t)
{
    auto& lf = DeckLookAndFeel::get();

    title = makeEditor (Inlay::title, lf.hand (34.0f), Pal::ink);
    if (title->getText().isEmpty())
        title->setText (tape.name, false);
    title->onTextChange = [this]
    {
        store (Inlay::title, title->getText());
        tape.name = title->getText();
        if (onTitleChanged) onTitleChanged (tape.name);
        repaint();   // the spine
    };

    for (int i = 0; i < Inlay::numSongs; ++i)
    {
        counters[i] = makeEditor (Inlay::songCounter (i), lf.mono (17.0f), Pal::ink);
        counters[i]->setInputRestrictions (3, "0123456789");
        songs[i] = makeEditor (Inlay::songName (i), lf.hand (26.0f), Pal::ink);
    }
    for (int k = 0; k < numTracks; ++k)
        trackNotes[k] = makeEditor (Inlay::trackNote (k), lf.hand (26.0f), Pal::ink);
    speed = makeEditor (Inlay::speed, lf.hand (26.0f), Pal::ink);
    date = makeEditor (Inlay::date, lf.hand (26.0f), Pal::ink);

    auto setNr = [this] (const juce::String& v)
    {
        store (Inlay::nr, v);
        nrOn.chosen = v == "1";
        nrOff.chosen = v == "0";
        nrOn.repaint();
        nrOff.repaint();
    };
    nrOn.textSize = 15.0f;
    nrOff.textSize = 15.0f;
    nrOn.onClick = [setNr] { setNr ("1"); };
    nrOff.onClick = [setNr] { setNr ("0"); };
    nrOn.chosen = tape.trackSheet[Inlay::nr] == "1";
    nrOff.chosen = tape.trackSheet[Inlay::nr] == "0";
    addAndMakeVisible (nrOn);
    addAndMakeVisible (nrOff);

    closeX.textSize = 20.0f;
    closeX.setTooltip ("Put the inlay back in its case");
    closeX.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeX);
}

juce::TextEditor* InlayCard::makeEditor (int field, juce::Font font, juce::Colour colour)
{
    auto* ed = editors.add (new juce::TextEditor());
    ed->setFont (font);
    ed->setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    ed->setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    ed->setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    ed->setColour (juce::TextEditor::textColourId, colour);
    ed->setColour (juce::TextEditor::highlightColourId, Pal::rule);
    ed->setColour (juce::CaretComponent::caretColourId, colour);
    ed->setIndents (2, 2);
    ed->setText (tape.trackSheet[field], false);
    ed->onTextChange = [this, ed, field] { store (field, ed->getText()); };
    addAndMakeVisible (ed);
    return ed;
}

void InlayCard::store (int field, const juce::String& text)
{
    tape.trackSheet.set (field, text);
    tape.markMetaChanged();
}

void InlayCard::resized()
{
    const int fx = flapW + spineW;
    auto x0 = fx + pad, w = frontW - 2 * pad;
    auto y = headerH + 16;

    title->setBounds (x0 + 30, y, w - 30, 42);
    y += 42 + 4 + 12 + 18;

    for (int i = 0; i < Inlay::numSongs; ++i)
    {
        counters[i]->setBounds (x0, y + i * rowH, 70, rowH - 2);
        songs[i]->setBounds (x0 + 82, y + i * rowH, w - 82, rowH - 2);
    }
    y += Inlay::numSongs * rowH + 12 + 18;

    for (int k = 0; k < numTracks; ++k)
        trackNotes[k]->setBounds (x0 + 82, y + k * rowH, w - 82, rowH - 2);
    y += numTracks * rowH + 14;

    auto footY = cardH - 10 - 14 - 34;
    nrOn.setBounds (x0 + 34, footY, 50, 34);
    nrOff.setBounds (x0 + 90, footY, 56, 34);
    speed->setBounds (x0 + 220, footY - 2, 70, 38);
    date->setBounds (x0 + w - 110, footY - 2, 110, 38);

    closeX.setBounds (flapW + spineW + frontW + gap, 0, closeW, closeW);
}

void InlayCard::paint (juce::Graphics& g)
{
    auto& lf = DeckLookAndFeel::get();
    const int fx = flapW + spineW;

    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRect (juce::Rectangle<int> (0, 0, fx + frontW, cardH).toFloat().translated (0, 10).expanded (4));

    // paper, header bands, bottom stripe, fold lines
    g.setColour (paper);
    g.fillRect (0, 0, fx + frontW, cardH);
    g.setColour (band);
    g.fillRect (0, 0, fx + frontW, headerH);
    const auto accent = juce::Colour (tape.getModelInfo().accent);
    g.setColour (accent);
    g.fillRect (0, cardH - 10, fx + frontW, 10);
    g.setColour (fold);
    for (auto x : { flapW, fx })
        for (int y = headerH; y < cardH - 10; y += 8)
            g.fillRect (x, y, 1, 4);

    auto typeName = tape.productName();

    // back flap
    brandText (g, { 12.0f, (float) headerH + 14, (float) flapW - 16, 24 }, 20.0f, juce::Justification::topLeft, ink);
    g.setColour (juce::Colour (0xff5c564a));
    g.setFont (lf.cond (15.0f, true).withExtraKerningFactor (0.08f));
    g.drawMultiLineText (typeName + "\n" + juce::String (tape.getTypeInfo().name) + "\nTYPE " + juce::String (tape.getModelInfo().iecType == 4 ? "IV" : tape.getModelInfo().iecType == 2 ? "II" : "I")
                             + "\n" + juce::String (tape.getTypeInfo().minutes) + " MIN\nSIDE A",
                         12, headerH + 64, flapW - 20);
    g.setColour (Pal::ink);
    g.setFont (lf.hand (22.0f));
    g.drawText (tape.created().toLowerCase(), 10, cardH - 52, flapW - 14, 30, juce::Justification::centredLeft);

    // spine: type at the top, the title written down the length
    {
        juce::Graphics::ScopedSaveState s (g);
        const int len = cardH - headerH - 40;
        g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi)
                            .translated ((float) flapW + spineW * 0.5f, (headerH + cardH - 10) * 0.5f));
        g.setColour (Pal::ink);
        g.setFont (lf.hand (40.0f));
        g.drawFittedText (tape.name, juce::Rectangle<int> (-len / 2, -24, len, 48), juce::Justification::centred, 1, 0.7f);
    }
    {
        juce::Graphics::ScopedSaveState s (g);
        g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi)
                            .translated ((float) flapW + spineW * 0.5f, (float) headerH * 0.5f));
        g.setColour (accent);
        g.setFont (lf.brand (26.0f));
        g.drawFittedText (typeName, juce::Rectangle<int> (-60, -16, 120, 32), juce::Justification::centred, 1, 0.6f);
    }

    // front header
    g.setColour (accent);
    g.setFont (lf.brand (60.0f));
    g.drawText (typeName, fx + pad, 14, 300, 64, juce::Justification::centredLeft);
    brandText (g, { (float) fx + frontW - pad - 200, 26.0f, 200.0f, 36.0f }, 30.0f, juce::Justification::centredRight, Pal::cream);
    g.setColour (gold);
    g.setFont (lf.cond (15.0f, true).withExtraKerningFactor (0.22f));
    g.drawText (utf (tape.getModelInfo().position) + utf (" \xc2\xb7 SIDE A \xc2\xb7 4 TRACKS ONE DIRECTION \xc2\xb7 ") + juce::String (tape.getTypeInfo().minutes) + " MIN",
                fx + pad, headerH - 38, frontW - 2 * pad, 20, juce::Justification::centredLeft);

    // front body
    auto x0 = fx + pad, w = frontW - 2 * pad;
    auto y = headerH + 16;
    g.setColour (ink);
    g.setFont (lf.cond (24.0f, true));
    g.drawText ("A", x0, y, 24, 42, juce::Justification::centredLeft);
    g.fillRect (x0, y + 46, w, 2);
    y += 42 + 4 + 12;

    auto header = [&] (int yy, const char* a, const char* b)
    {
        g.setColour (Pal::menuHead);
        g.setFont (lf.cond (13.0f, true).withExtraKerningFactor (0.2f));
        g.drawText (a, x0, yy, 80, 16, juce::Justification::centredLeft);
        g.drawText (b, x0 + 82, yy, 300, 16, juce::Justification::centredLeft);
    };
    header (y, "COUNTER", "SONGS");
    y += 18;
    g.setColour (Pal::rule);
    for (int i = 0; i < Inlay::numSongs; ++i)
        g.fillRect (x0, y + (i + 1) * rowH - 1, w, 1);
    y += Inlay::numSongs * rowH + 12;

    header (y, "TRACK", "WHAT'S ON IT");
    y += 18;
    for (int k = 0; k < numTracks; ++k)
    {
        g.setColour (Pal::rule);
        g.fillRect (x0, y + (k + 1) * rowH - 1, w, 1);
        g.setColour (ink);
        g.setFont (lf.cond (20.0f, true));
        g.drawText (juce::String (k + 1), x0, y + k * rowH, 40, rowH, juce::Justification::centredLeft);
    }

    auto footY = cardH - 10 - 14 - 34;
    g.setColour (ink);
    g.fillRect (x0, footY - 12, w, 2);
    g.setFont (lf.cond (15.0f, true).withExtraKerningFactor (0.14f));
    g.drawText ("NR", x0, footY, 30, 34, juce::Justification::centredLeft);
    g.drawText ("SPEED", x0 + 166, footY, 54, 34, juce::Justification::centredLeft);
    g.drawText ("DATE", x0 + w - 160, footY, 50, 34, juce::Justification::centredLeft);
}

//==============================================================================
void InlayThumb::paintButton (juce::Graphics& g, bool over, bool)
{
    auto& lf = DeckLookAndFeel::get();
    auto r = getLocalBounds().toFloat().withTrimmedBottom (22.0f);

    // the clear case
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRoundedRectangle (r.translated (0, 5), 4.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.18f), r.getX(), r.getY(),
                                             juce::Colours::white.withAlpha (0.05f), r.getCentreX(), r.getCentreY(), false));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (juce::Colour::fromRGBA (230, 235, 245, over ? 160 : 90));
    g.drawRoundedRectangle (r.reduced (1.0f), 4.0f, 2.0f);

    // the card inside
    auto c = r.reduced (8.0f);
    g.setColour (paper);
    g.fillRect (c);
    auto head = c.removeFromTop (30.0f);
    g.setColour (band);
    g.fillRect (head);
    auto accent = tape != nullptr ? juce::Colour (tape->getModelInfo().accent) : Pal::cassetteOrange;
    g.setColour (accent);
    g.fillRect (c.removeFromBottom (6.0f));
    g.setFont (lf.brand (17.0f));
    g.drawText (tape != nullptr ? tape->productName() : juce::String(), head.reduced (6, 0), juce::Justification::centredLeft);
    if (getWidth() >= 110)
    {
        g.setColour (Pal::cream);
        g.setFont (lf.brand (9.0f));
        g.drawText (utf ("TAPE\xc2\xb7" "FOUR"), head.reduced (6, 0), juce::Justification::centredRight);
    }

    c = c.reduced (7.0f, 6.0f);
    g.setColour (Pal::rule);
    for (float y = c.getY() + 16; y < c.getBottom(); y += 16)
        g.fillRect (c.getX(), y, c.getWidth(), 1.0f);
    if (tape != nullptr)
    {
        g.setColour (Pal::ink);
        g.setFont (lf.hand (16.0f));
        g.drawText (tape->name, c.removeFromTop (16).toNearestInt(), juce::Justification::bottomLeft);
        g.setFont (lf.hand (13.0f));
        for (int i = 0; i < Inlay::numSongs && c.getHeight() >= 16; ++i)
        {
            auto name = tape->trackSheet[Inlay::songName (i)];
            if (name.isEmpty()) continue;
            g.drawText (tape->trackSheet[Inlay::songCounter (i)] + " " + name, c.removeFromTop (16).toNearestInt(),
                        juce::Justification::bottomLeft);
        }
    }

    g.setColour (Pal::muted);
    g.setFont (lf.cond (14.0f, true).withExtraKerningFactor (0.2f));
    g.drawText (utf ("INLAY \xe2\x80\xba"), getLocalBounds().removeFromBottom (18), juce::Justification::centred);
}
} // namespace tf
