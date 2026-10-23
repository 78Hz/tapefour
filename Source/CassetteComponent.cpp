#include "CassetteComponent.h"

namespace tf
{
namespace
{
    // All geometry in tenths of a millimetre: the shell is 1004 x 638.
    constexpr float shellW = 1004.0f, shellH = 638.0f;
    constexpr float hubR = 104.0f;                     // 21 mm hub
    constexpr float minPackD = 216.0f;                 // leader wound on an empty hub
    constexpr float cy = 300.0f, lx = 292.0f, rx = 712.0f;
    constexpr float guideY = 586.0f, guideR = 32.0f, wrapR = 30.0f, guideLX = 118.0f, guideRX = 886.0f;
    constexpr float unitsPerTapeSecond = 476.0f;       // 47.6 mm/s

    juce::Colour rgba (int r, int g, int b, float a) { return juce::Colour ((juce::uint8) r, (juce::uint8) g, (juce::uint8) b, a); }

    /** How each tape model's shell is moulded and printed. */
    struct Style
    {
        juce::Colour shell, pack0, pack1, tape;
        juce::Colour hub, hubRing, hubInner, hubInnerRing, hubTeeth, spindleTeeth, hubCore, accent;
        bool accents;
        juce::Colour label, labelStripe, ink;
        juce::Colour lower, lowerEdge, hole, screw, outline;
        float sheen;
    };

    Style styleFor (int model)
    {
        switch (model)
        {
            case 0: // BASIC: milky white shell, black and yellow print
                return { juce::Colour (0xffd9d5ca), juce::Colour (0xff5a3a22), juce::Colour (0xff2e1c0f), juce::Colour (0xff3a2416),
                         juce::Colour (0xfffbfaf7), juce::Colour (0xffc9c5bb), juce::Colour (0xffdcd8cf), juce::Colour (0xffb5b1a7),
                         juce::Colours::black.withAlpha (0.18f), juce::Colours::white, juce::Colour (0xffd6d3cc), juce::Colour (0xff1a1917), false,
                         juce::Colour (0xfff6d35a), juce::Colour (0xff1a1917), juce::Colour (0xff4a4a4a),
                         juce::Colour (0xffcfcabe), juce::Colours::white.withAlpha (0.7f), juce::Colour (0xff1a1917), juce::Colour (0xffa9a59b),
                         juce::Colours::white.withAlpha (0.65f), 0.35f };
            case 2: // XR: blue-smoked clear shell, silver and blue print
                return { rgba (46, 66, 98, 0.58f), juce::Colour (0xff2b211c), juce::Colour (0xff0e0a08), juce::Colour (0xff24180f),
                         juce::Colour (0xffdfe3e8), juce::Colour (0xffa7aeb8), juce::Colour (0xffc3c9d1), juce::Colour (0xff9aa3ad),
                         juce::Colours::black.withAlpha (0.3f), juce::Colour (0xfff5f7fa), juce::Colour (0xffbec6cf), juce::Colour (0xff3f7fe0), true,
                         juce::Colour (0xffe9edf1), juce::Colour (0xff3f7fe0), juce::Colour (0xff1d2b5e),
                         rgba (10, 16, 28, 0.5f), juce::Colours::white.withAlpha (0.2f), juce::Colour (0xff0a0a0b), juce::Colour (0xff14161c),
                         rgba (223, 232, 245, 0.4f), 0.24f };
            case 3: // MX: black body, clear window, copper print
                return { rgba (30, 29, 28, 0.7f), juce::Colour (0xff2a2a2c), juce::Colour (0xff0b0b0c), juce::Colour (0xff1c1c1e),
                         juce::Colour (0xff3b3936), juce::Colour (0xff5a5753), juce::Colour (0xff4d4a46), juce::Colour (0xff2a2826),
                         juce::Colours::black.withAlpha (0.45f), juce::Colour (0xff4a4743), juce::Colour (0xff6f6c67), juce::Colour (0xffc98a4b), true,
                         juce::Colour (0xfff1ebdd), juce::Colour (0xffc98a4b), juce::Colour (0xff1a1917),
                         juce::Colour (0xff0b0b0b), juce::Colour (0xffc98a4b).withAlpha (0.35f), juce::Colours::black, juce::Colour (0xff2a2826),
                         juce::Colour (0xff6a6156).withAlpha (0.6f), 0.14f };
            case 1: default: // SD: smoked clear shell, orange and gold print
                return { rgba (74, 70, 88, 0.58f), juce::Colour (0xff3a2618), juce::Colour (0xff160c06), juce::Colour (0xff2c1a0e),
                         juce::Colour (0xffecebe6), juce::Colour (0xffb9b6ad), juce::Colour (0xffcfccc3), juce::Colour (0xffa9a69e),
                         juce::Colours::black.withAlpha (0.32f), juce::Colour (0xfffafaf8), juce::Colour (0xffc9ccd0), juce::Colour (0xffe8702a), true,
                         juce::Colour (0xfff6f3ea), juce::Colour (0xffe8702a), juce::Colour (0xff26357a),
                         rgba (18, 17, 22, 0.5f), juce::Colours::white.withAlpha (0.18f), juce::Colour (0xff0a0a0b), juce::Colour (0xff1a1a1e),
                         rgba (230, 235, 245, 0.38f), 0.22f };
        }
    }

    // Point where the tape leaves a pack (centre c, radius r1) on the outside, tangent to the guide roller.
    std::pair<juce::Point<float>, juce::Point<float>> tangent (juce::Point<float> c, float r1, juce::Point<float> gc, float r2, int side)
    {
        auto d = gc - c;
        auto len = d.getDistanceFromOrigin();
        auto th = std::atan2 (d.y, d.x);
        auto a = std::acos (juce::jlimit (-1.0f, 1.0f, (r1 - r2) / len));
        juce::Point<float> n1 (std::cos (th + a), std::sin (th + a)), n2 (std::cos (th - a), std::sin (th - a));
        auto n = (side < 0) == (n1.x < n2.x) ? n1 : n2;
        return { c + n * r1, gc + n * r2 };
    }

    void text (juce::Graphics& g, const juce::String& s, juce::Font f, juce::Colour c, float x, float baseline,
               juce::Justification j = juce::Justification::left, float kerning = 0.0f)
    {
        f = f.withExtraKerningFactor (kerning);
        g.setFont (f);
        g.setColour (c);
        auto w = juce::GlyphArrangement::getStringWidth (f, s) + 4.0f;
        auto top = baseline - f.getAscent();
        auto r = j == juce::Justification::right ? juce::Rectangle<float> (x - w, top, w, f.getHeight())
                                                 : juce::Rectangle<float> (x, top, w, f.getHeight());
        g.drawText (s, r, j == juce::Justification::right ? juce::Justification::topRight : juce::Justification::topLeft, false);
    }
}

CassetteComponent::CassetteComponent()
{
    nameLabel.setEditable (false, true, false);
    nameLabel.setJustificationType (juce::Justification::centredLeft);
    nameLabel.setFont (DeckLookAndFeel::get().hand (22.0f));
    nameLabel.setColour (juce::Label::textColourId, Pal::ink);
    nameLabel.setColour (juce::Label::textWhenEditingColourId, Pal::ink);
    nameLabel.setColour (juce::TextEditor::backgroundColourId, Pal::paper);
    nameLabel.setColour (juce::TextEditor::textColourId, Pal::ink);
    nameLabel.setColour (juce::TextEditor::highlightColourId, Pal::rule);
    nameLabel.setTooltip ("Double-click to write on the label");
    nameLabel.onTextChange = [this] { if (onNameChanged) onNameChanged (nameLabel.getText()); };
    addAndMakeVisible (nameLabel);
    setOpaque (false);
}

void CassetteComponent::resized()
{
    // The write-on label, in component pixels.
    auto sx = getWidth() / shellW, sy = getHeight() / shellH;
    nameLabel.setBounds (juce::Rectangle<float> (122.0f * sx, 402.0f * sy, 760.0f * sx, 62.0f * sy).toNearestInt());
}

void CassetteComponent::setTape (const Tape* tape)
{
    hasTape = tape != nullptr;
    nameLabel.setVisible (hasTape);
    if (tape != nullptr)
    {
        nameLabel.setText (tape->name, juce::dontSendNotification);
        model = tape->getModel();
        minutes = tape->getTypeInfo().minutes;
        nameLabel.setColour (juce::Label::textColourId, styleFor (model).ink);
        nameLabel.setColour (juce::Label::textWhenEditingColourId, styleFor (model).ink);
    }
    repaint();
}

float CassetteComponent::packDiameter (float f) const
{
    // Wound tape area is conserved between the spools.
    return std::sqrt (minPackD * minPackD + (fullPackUnits * fullPackUnits - minPackD * minPackD) * juce::jlimit (0.0f, 1.0f, f));
}

void CassetteComponent::update (const Tape* tape, double pos)
{
    // Still, and nothing new: nothing to draw.
    if ((tape != nullptr) == hasTape && tape == lastTape && pos == lastPos)
        return;
    lastTape = tape;
    hasTape = tape != nullptr;
    if (! hasTape) { repaint(); return; }

    fullPackUnits = tape->getTypeInfo().fullPackMm * 10.0f;
    fraction = (float) (pos / (double) tape->getLengthCells());

    if (lastPos >= 0.0)
    {
        auto moved = (float) ((pos - lastPos) / cellRate) * unitsPerTapeSecond;
        auto rL = packDiameter (1.0f - fraction) * 0.5f, rR = packDiameter (fraction) * 0.5f;
        // Tape moving left to right turns both spools anticlockwise.
        angleL -= moved / rL;
        angleR -= moved / rR;
        angleL = std::fmod (angleL, juce::MathConstants<float>::twoPi);
        angleR = std::fmod (angleR, juce::MathConstants<float>::twoPi);
    }
    lastPos = pos;
    repaint();
}

void CassetteComponent::drawPack (juce::Graphics& g, juce::Point<float> c, float d) const
{
    auto st = styleFor (model);
    auto r = juce::Rectangle<float> (d, d).withCentre (c);
    g.setGradientFill (juce::ColourGradient (st.pack0, c.x, c.y, st.pack1, c.x + d * 0.5f, c.y, true));
    g.fillEllipse (r);
    g.setColour (juce::Colours::white.withAlpha (0.045f));
    for (float rr = d * 0.5f - 10.0f; rr > hubR; rr -= 26.0f)
        g.drawEllipse (juce::Rectangle<float> (rr * 2, rr * 2).withCentre (c), 2.0f);
}

void CassetteComponent::drawHub (juce::Graphics& g, juce::Point<float> c, float angle) const
{
    auto st = styleFor (model);
    auto rot = [&] (float extra) { return juce::AffineTransform::rotation (angle + extra).translated (c); };
    auto circle = [&] (float r) { return juce::Rectangle<float> (r * 2, r * 2).withCentre (c); };

    g.setColour (st.hub);
    g.fillEllipse (circle (hubR));
    g.setColour (st.hubRing);
    g.drawEllipse (circle (hubR - 1), 2.0f);

    if (st.accents)
    {
        juce::Path arcs;
        arcs.addCentredArc (0, 0, 102, 102, 0, -1.875f, -0.582f, true);
        arcs.addCentredArc (0, 0, 102, 102, 0, 1.266f, 2.559f, true);
        g.setColour (st.accent);
        g.strokePath (arcs, juce::PathStrokeType (13.0f), rot (0));
    }

    g.setColour (st.hubInner);
    g.drawEllipse (circle (80), 3.0f);

    juce::Path tooth;
    tooth.addRoundedRectangle (-5.0f, -101.0f, 10.0f, 20.0f, 2.0f);
    juce::Path spline;
    spline.addRoundedRectangle (-6.0f, -58.0f, 12.0f, 15.0f, 2.0f);
    for (int k = 0; k < 6; ++k)
    {
        g.setColour (st.hubTeeth);
        g.fillPath (tooth, rot (k * juce::MathConstants<float>::pi / 3.0f));
    }

    g.setColour (st.hubCore.interpolatedWith (st.hub, 0.5f));
    g.fillEllipse (circle (58));
    g.setColour (st.hubInnerRing);
    g.drawEllipse (circle (58), 3.0f);
    g.setColour (st.spindleTeeth);
    for (int k = 0; k < 6; ++k)
        g.fillPath (spline, rot (k * juce::MathConstants<float>::pi / 3.0f));
    g.setColour (st.hubCore);
    g.fillEllipse (circle (28));
}

void CassetteComponent::drawPrint (juce::Graphics& g) const
{
    auto& lf = DeckLookAndFeel::get();
    const auto& tm = tapeModels[model];
    auto title = juce::String (tm.name) + (model == 0 ? " " : "") + juce::String (minutes * 2);
    auto brandWord = [&] (float x, float baseline, float size, juce::Colour c, juce::Colour dot)
    {
        juce::AttributedString a;
        a.append ("TAPE", lf.brand (size), c);
        a.append (utf ("\xc2\xb7"), lf.brand (size), dot);
        a.append ("FOUR", lf.brand (size), c);
        juce::TextLayout tl;
        tl.createLayout (a, 600.0f);
        auto w = tl.getWidth();
        tl.draw (g, { x - w, baseline - lf.brand (size).getAscent(), w + 2, size * 1.3f });
    };

    switch (model)
    {
        case 0:
        {
            g.setColour (juce::Colour (0xfff2c230));
            g.fillRect (30.0f, 30.0f, 944.0f, 54.0f);
            text (g, "EVERYDAY RECORDING", lf.cond (40.0f, true), juce::Colour (0xff1a1917), 56, 72, juce::Justification::left, 0.2f);
            brandWord (944, 72, 40, juce::Colour (0xff1a1917), juce::Colour (0xff1a1917));
            text (g, title, lf.brand (96.0f), juce::Colour (0xff1a1917), 56, 178);
            text (g, "NORMAL BIAS " + utf ("\xc2\xb7") + " TYPE I", lf.cond (28.0f, true), juce::Colour (0xff3a3833), 918, 134, juce::Justification::right, 0.07f);
            text (g, juce::String (minutes) + " MIN PER SIDE", lf.cond (26.0f), juce::Colour (0xff3a3833), 918, 168, juce::Justification::right, 0.07f);
            text (g, "A", lf.cond (40.0f, true), juce::Colour (0xff1a1917), 60, 300);
            break;
        }
        case 2:
        {
            g.setColour (rgba (12, 20, 34, 0.8f));
            g.fillRoundedRectangle (30.0f, 26.0f, 944.0f, 178.0f, 14.0f);
            g.setColour (juce::Colour (0xff3f7fe0));
            g.fillRect (30.0f, 176.0f, 944.0f, 8.0f);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe9eef5), 0, 60, juce::Colour (0xff9aa8b8), 0, 150, false));
            g.setFont (lf.brand (108.0f));
            g.drawText (title, juce::Rectangle<float> (74, 150 - lf.brand (108.0f).getAscent(), 300, 120), juce::Justification::topLeft, false);
            text (g, "CHROME", lf.cond (30.0f, true), juce::Colour (0xff7fb2ff), 352, 96, juce::Justification::left, 0.2f);
            text (g, "EXTRA RANGE " + utf ("\xc2\xb7") + " HIGH BIAS " + utf ("\xc2\xb7") + " 70 " + utf ("\xc2\xb5") + "s EQ",
                  lf.cond (26.0f), juce::Colour (0xffb9c3cf), 352, 128, juce::Justification::left, 0.1f);
            text (g, "IEC II / TYPE II   HIGH POSITION", lf.cond (28.0f, true), juce::Colour (0xffb9c3cf), 352, 160, juce::Justification::left, 0.07f);
            brandWord (944, 150, 46, juce::Colour (0xffe6ecf3), juce::Colour (0xff7fb2ff));
            break;
        }
        case 3:
        {
            g.setColour (juce::Colour (0xffc98a4b).withAlpha (0.6f));
            g.fillRect (30.0f, 34.0f, 944.0f, 2.0f);
            g.fillRect (30.0f, 190.0f, 944.0f, 2.0f);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff2c08a), 0, 60, juce::Colour (0xff8a5326), 0, 160, false));
            g.setFont (lf.brand (112.0f));
            g.drawText (title, juce::Rectangle<float> (74, 156 - lf.brand (112.0f).getAscent(), 300, 120), juce::Justification::topLeft, false);
            text (g, "METAL", lf.cond (32.0f, true), juce::Colour (0xffd9a46e), 352, 96, juce::Justification::left, 0.44f);
            text (g, "REFERENCE " + utf ("\xc2\xb7") + " MAXIMUM OUTPUT " + utf ("\xc2\xb7") + " 70 " + utf ("\xc2\xb5") + "s EQ",
                  lf.cond (26.0f), juce::Colour (0xffb9a48c), 352, 130, juce::Justification::left, 0.1f);
            text (g, "IEC IV / TYPE IV   METAL POSITION", lf.cond (28.0f, true), juce::Colour (0xffb9a48c), 352, 162, juce::Justification::left, 0.07f);
            brandWord (944, 156, 46, juce::Colour (0xffece4cf), juce::Colour (0xffc98a4b));
            text (g, "A", lf.cond (34.0f, true), juce::Colour (0xffc98a4b), 60, 300);
            break;
        }
        case 1: default:
        {
            g.setColour (rgba (26, 24, 32, 0.74f));
            g.fillRoundedRectangle (30.0f, 26.0f, 944.0f, 178.0f, 14.0f);
            text (g, title, lf.brand (112.0f), Pal::orange, 74, 152);
            juce::Path rule;
            rule.startNewSubPath (300, 166); rule.lineTo (334, 88); rule.lineTo (830, 88);
            g.setColour (juce::Colour (0xffcdb57c));
            g.strokePath (rule, juce::PathStrokeType (3.0f));
            text (g, "STUDIO DYNAMIC " + utf ("\xc2\xb7") + " LOW NOISE " + utf ("\xc2\xb7") + " WIDE RANGE", lf.cond (27.0f), juce::Colour (0xffcdb57c), 350, 120, juce::Justification::left, 0.11f);
            text (g, "IEC I / TYPE I   NORMAL POSITION", lf.cond (31.0f, true), juce::Colour (0xffcdb57c), 350, 156, juce::Justification::left, 0.07f);
            brandWord (944, 156, 46, juce::Colour (0xffece4cf), Pal::orange);
            break;
        }
    }
}

void CassetteComponent::paint (juce::Graphics& g)
{
    auto& lf = DeckLookAndFeel::get();
    g.addTransform (juce::AffineTransform::scale (getWidth() / shellW, getHeight() / shellH));
    auto shell = juce::Rectangle<float> (shellW, shellH);
    auto st = styleFor (model);

    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRoundedRectangle (shell.translated (6, 14), 26.0f);

    if (! hasTape)
        return;

    g.setColour (st.shell);
    g.fillRoundedRectangle (shell, 24.0f);

    // tape packs and the tape path round the guides
    auto dL = packDiameter (1.0f - fraction), dR = packDiameter (fraction);
    drawPack (g, { lx, cy }, dL);
    drawPack (g, { rx, cy }, dR);

    auto tl = tangent ({ lx, cy }, dL * 0.5f, { guideLX, guideY }, wrapR, -1);
    auto tr = tangent ({ rx, cy }, dR * 0.5f, { guideRX, guideY }, wrapR, 1);
    juce::Path tape;
    tape.startNewSubPath (tl.first);
    tape.lineTo (tl.second);
    // JUCE arc angles run clockwise from 12 o'clock; atan2 runs clockwise from 3 o'clock (y is down).
    auto aL = std::atan2 (tl.second.y - guideY, tl.second.x - guideLX);
    if (aL < 0) aL += juce::MathConstants<float>::twoPi;
    tape.addCentredArc (guideLX, guideY, wrapR, wrapR, 0, aL + juce::MathConstants<float>::halfPi, juce::MathConstants<float>::pi);
    tape.lineTo (guideRX, guideY + wrapR);
    auto aR = std::atan2 (tr.second.y - guideY, tr.second.x - guideRX);
    tape.addCentredArc (guideRX, guideY, wrapR, wrapR, 0, juce::MathConstants<float>::pi, aR + juce::MathConstants<float>::halfPi);
    tape.lineTo (tr.first);
    g.setColour (st.tape);
    g.strokePath (tape, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved));

    drawHub (g, { lx, cy }, angleL);
    drawHub (g, { rx, cy }, angleR);

    // the body over the works
    if (model == 0)
    {
        // milky plastic, clearer in the window
        juce::Path veil;
        veil.addRoundedRectangle (shell, 24.0f);
        veil.addRoundedRectangle (160.0f, 222.0f, 684.0f, 156.0f, 12.0f);
        veil.setUsingNonZeroWinding (false);
        g.setColour (juce::Colour (0xffece9e1).withAlpha (0.72f));
        g.fillPath (veil);
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.drawRoundedRectangle (160.0f, 222.0f, 684.0f, 156.0f, 12.0f, 2.0f);
    }
    else if (model == 3)
    {
        // Opaque body with a window and two hub holes. They overlap, so cut each one out in turn.
        {
            juce::Graphics::ScopedSaveState state (g);
            auto cut = [&] (const juce::Path& hole)
            {
                juce::Path outside;
                outside.addRectangle (shell.expanded (60));
                outside.addPath (hole);
                outside.setUsingNonZeroWinding (false);
                g.reduceClipRegion (outside);
            };
            juce::Path window, holeL, holeR;
            window.addRoundedRectangle (150.0f, 214.0f, 704.0f, 172.0f, 20.0f);
            holeL.addEllipse (juce::Rectangle<float> (220, 220).withCentre ({ lx, cy }));
            holeR.addEllipse (juce::Rectangle<float> (220, 220).withCentre ({ rx, cy }));
            cut (window); cut (holeL); cut (holeR);
            g.setColour (juce::Colour (0xff121110));
            g.fillRoundedRectangle (shell, 24.0f);
        }
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.fillRoundedRectangle (150.0f, 214.0f, 704.0f, 172.0f, 20.0f);
        g.setColour (st.accent.withAlpha (0.55f));
        g.drawRoundedRectangle (150.0f, 214.0f, 704.0f, 172.0f, 20.0f, 2.0f);
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawEllipse (juce::Rectangle<float> (220, 220).withCentre ({ lx, cy }), 4.0f);
        g.drawEllipse (juce::Rectangle<float> (220, 220).withCentre ({ rx, cy }), 4.0f);
    }
    else
    {
        // clear window band
        g.setColour (juce::Colours::white.withAlpha (0.04f));
        g.fillRect (34.0f, 206.0f, 936.0f, 188.0f);
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.fillRect (34.0f, 205.0f, 936.0f, 2.0f);
        g.fillRect (34.0f, 393.0f, 936.0f, 2.0f);
    }

    drawPrint (g);

    // write-on label
    g.setColour (st.label);
    g.fillRoundedRectangle (68.0f, 398.0f, 868.0f, 80.0f, 3.0f);
    g.setColour (st.labelStripe);
    g.fillRect (68.0f, 468.0f, 868.0f, 10.0f);
    g.setColour (st.ink.withAlpha (0.85f));
    g.setFont (lf.cond (40.0f, true));
    g.drawText ("A", juce::Rectangle<float> (88, 400, 36, 66), juce::Justification::centredLeft);

    // lower section with the head openings
    juce::Path lower;
    lower.startNewSubPath (168, shellH); lower.lineTo (200, 494); lower.lineTo (804, 494); lower.lineTo (836, shellH); lower.closeSubPath();
    g.setColour (st.lower);
    g.fillPath (lower);
    g.setColour (st.lowerEdge);
    g.strokePath (lower, juce::PathStrokeType (2.0f));
    g.setColour (model == 0 ? juce::Colours::black.withAlpha (0.07f) : juce::Colours::white.withAlpha (0.08f));
    for (int k = 0; k < 6; ++k)
        g.fillRect (210.0f - 2 * k, 507.0f + 10 * k, 584.0f + 4 * k, 2.0f);
    g.setColour (st.hole);
    g.fillEllipse (248, 582, 36, 36);
    g.fillEllipse (720, 582, 36, 36);
    g.fillRoundedRectangle (352, 580, 36, 42, 6);
    g.fillRoundedRectangle (616, 580, 36, 42, 6);
    g.fillRoundedRectangle (466, 592, 72, 46, 4);
    g.setColour (juce::Colour (0xffc8b89c));
    g.fillRoundedRectangle (480, 600, 44, 14, 2);

    // guide rollers
    for (auto x : { guideLX, guideRX })
    {
        auto r = juce::Rectangle<float> (guideR * 2, guideR * 2).withCentre ({ x, guideY });
        g.setColour (model == 3 ? juce::Colour (0xff1c1b1a) : juce::Colour (0xffe6e6e6).withAlpha (model == 0 ? 1.0f : 0.55f));
        g.fillEllipse (r);
        g.setColour (model == 3 ? st.accent.withAlpha (0.35f) : juce::Colours::white.withAlpha (0.4f));
        g.drawEllipse (r, 2.0f);
        g.setColour (juce::Colour (0xff9aa0a6));
        g.fillEllipse (juce::Rectangle<float> (20, 20).withCentre ({ x, guideY }));
    }

    // screws, write-protect tabs and the position-detection holes
    auto screwY = model == 0 ? 114.0f : 44.0f;
    for (auto p : std::initializer_list<juce::Point<float>> { { 48, screwY }, { 956, screwY }, { 502, 562 } })
    {
        auto big = p.y != 562.0f;
        auto r = juce::Rectangle<float> (big ? 34.0f : 22.0f, big ? 34.0f : 22.0f).withCentre (p);
        g.setColour (st.screw);
        g.fillEllipse (r);
        g.setColour (model == 3 ? st.accent.withAlpha (0.45f) : juce::Colours::white.withAlpha (model == 0 ? 0.5f : 0.25f));
        g.drawEllipse (r, 2.0f);
        if (big)
        {
            g.setColour (juce::Colour (0xff6b6e74));
            g.drawLine (p.x - 8, p.y, p.x + 8, p.y, 3.0f);
            g.drawLine (p.x, p.y - 8, p.x, p.y + 8, 3.0f);
        }
    }
    g.setColour (juce::Colours::black.withAlpha (model == 3 ? 1.0f : 0.4f));
    g.fillRoundedRectangle (150, 0, 58, 24, 3);
    g.fillRoundedRectangle (796, 0, 58, 24, 3);
    if (tapeModels[model].iecType >= 2)
    {
        g.fillRoundedRectangle (226, 0, 22, 24, 3);
        g.fillRoundedRectangle (756, 0, 22, 24, 3);
    }
    if (tapeModels[model].iecType == 4)
    {
        g.fillRoundedRectangle (420, 0, 22, 24, 3);
        g.fillRoundedRectangle (562, 0, 22, 24, 3);
    }

    // the plastic over everything
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (st.sheen), 0, 0,
                                             juce::Colours::white.withAlpha (0.0f), shellW * 0.6f, shellH * 0.6f, false));
    g.fillRoundedRectangle (shell, 24.0f);
    g.setColour (st.outline);
    g.drawRoundedRectangle (shell.reduced (1.5f), 23.0f, 3.0f);
}
} // namespace tf
