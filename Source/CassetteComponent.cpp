#include "CassetteComponent.h"
#include "../Formats/Zip.h"

namespace tf
{
namespace
{
    // All geometry in tenths of a millimetre: the shell is 1004 x 638.
    constexpr float shellW = 1004.0f, shellH = 638.0f;
    constexpr float hubR = 104.0f;                     // 21 mm hub
    constexpr float minPackD = (float) (Geometry::hubDiameterMm * 10.0);   // the hub's winding surface (Geometry.h)
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
    auto sx = (float) getWidth() / shellW, sy = (float) getHeight() / shellH;
    nameLabel.setBounds (juce::Rectangle<float> (122.0f * sx, 402.0f * sy, 760.0f * sx, 62.0f * sy).toNearestInt());
    nameLabel.setFont (DeckLookAndFeel::get().hand (22.0f * (float) getHeight() / 255.0f));   // 22 at the deck's size
}

void CassetteComponent::setSide (int s)
{
    if (s == side) return;
    side = s;
    repaint();
}

void CassetteComponent::setLabelEditable (bool editable)
{
    nameLabel.setEditable (false, editable, false);
    nameLabel.setInterceptsMouseClicks (editable, false);
    nameLabel.setTooltip (editable ? "Double-click to write on the label" : juce::String());
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
    if ((tape != nullptr) == hasTape && tape == lastTape && juce::exactlyEqual (pos, lastPos))
        return;
    lastTape = tape;
    hasTape = tape != nullptr;
    if (! hasTape) { repaint(); return; }

    fullPackUnits = tape->fullPackMm() * 10.0f;
    fraction = (float) tape->rightFraction (pos);

    if (lastPos >= 0.0)
    {
        auto moved = (float) ((pos - lastPos) / cellRate) * unitsPerTapeSecond;
        auto rL = packDiameter (1.0f - fraction) * 0.5f, rR = packDiameter (fraction) * 0.5f;
        // Tape moving left to right turns both spools anticlockwise.
        stepL = -moved / rL;
        stepR = -moved / rR;
        angleL += stepL;
        angleR += stepR;
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
    constexpr float pi = juce::MathConstants<float>::pi;

    // The leader clamp: a moulded frame set into the rim, about 52 degrees of it, its outer edge the
    // rim itself (it never stands proud: the tape winds over it), open in the middle.
    constexpr float clampSpan = 0.91f, clampIn = 70.0f, clampOut = hubR - 0.5f, frameW = 7.0f;
    auto sector = [] (float r0, float r1, float a0, float a1)
    {
        juce::Path p;
        p.addCentredArc (0, 0, r1, r1, 0, a0, a1, true);
        p.addCentredArc (0, 0, r0, r0, 0, a1, a0, false);
        p.closeSubPath();
        return p;
    };
    // its outline
    auto clampOutline = [&] (float inset)
    {
        const float a0 = -clampSpan * 0.5f + inset / clampOut, a1 = clampSpan * 0.5f - inset / clampOut;
        const float rOut = clampOut - inset, rInA = clampIn + inset, rInB = clampIn + inset;
        juce::Path p;
        p.addCentredArc (0, 0, rOut, rOut, 0, a0, a1, true);
        p.addCentredArc (0, 0, rInB, rInB, 0, a1, 0.0f, false);
        p.addCentredArc (0, 0, rInA, rInA, 0, 0.0f, a0, false);
        p.closeSubPath();
        return p;
    };
    const auto clampWindow = clampOutline (frameW);

    // the hub's body, with the clamp's window open through it
    juce::Path body;
    body.setUsingNonZeroWinding (false);
    body.addEllipse (-hubR, -hubR, hubR * 2, hubR * 2);
    body.addPath (clampWindow);
    {
        // hollow sections: arc-shaped openings between the rim and the inner ring, ribs between them
        const float gapR0 = 80.0f, gapR1 = 90.0f;
        const int n = 6;
        const float rib = 0.16f;
        for (int k = 0; k < n; ++k)
        {
            float a0 = (float) k * 2.0f * pi / (float) n + rib * 0.5f, a1 = (float) (k + 1) * 2.0f * pi / (float) n - rib * 0.5f;
            const float c0 = -clampSpan * 0.5f - 0.05f + 2.0f * pi, c1 = clampSpan * 0.5f + 0.05f;   // not under the clamp
            if (a1 > c0) a1 = c0;
            if (a0 < c1) a0 = c1;
            if (a1 - a0 > 0.05f) body.addPath (sector (gapR0, gapR1, a0, a1));
        }
    }
    g.setColour (st.hub);
    g.fillPath (body, rot (0));
    g.setColour (st.hubRing);
    g.drawEllipse (circle (hubR - 1), 2.0f);
    {
        // the inner ring's edge, not across the clamp's opening
        const float rr = 78.0f;
        juce::Path ring;
        ring.addCentredArc (0, 0, rr, rr, 0, clampSpan * 0.5f, 2.0f * pi - clampSpan * 0.5f, true);
        g.setColour (st.hubInner);
        g.strokePath (ring, juce::PathStrokeType (3.0f), rot (0));
    }

    {
        const auto col = st.accents ? st.accent : st.hub.darker (0.08f);
        juce::Path frame = clampOutline (0.0f);
        frame.setUsingNonZeroWinding (false);
        frame.addPath (clampWindow);
        g.setColour (col);
        g.fillPath (frame, rot (0));
        g.setColour (col.darker (0.4f));
        g.strokePath (clampOutline (0.6f), juce::PathStrokeType (1.2f), rot (0));
        g.strokePath (clampWindow, juce::PathStrokeType (1.2f), rot (0));
        // the moulding's lit inner edge
        g.setColour (col.brighter (0.4f).withAlpha (0.6f));
        g.strokePath (clampOutline (frameW - 1.5f), juce::PathStrokeType (1.0f), rot (0));
    }

    juce::Path spline;
    spline.addRoundedRectangle (-6.0f, -58.0f, 12.0f, 15.0f, 2.0f);

    g.setColour (st.hubCore.interpolatedWith (st.hub, 0.5f));
    g.fillEllipse (circle (58));
    g.setColour (st.hubInnerRing);
    g.drawEllipse (circle (58), 3.0f);
    g.setColour (st.spindleTeeth);
    for (int k = 0; k < 6; ++k)
        g.fillPath (spline, rot ((float) k * pi / 3.0f));
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
            text (g, sideLetter(), lf.cond (40.0f, true), juce::Colour (0xff1a1917), 60, 300);
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
            text (g, sideLetter(), lf.cond (34.0f, true), juce::Colour (0xffc98a4b), 60, 300);
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
    g.addTransform (juce::AffineTransform::scale ((float) getWidth() / shellW, (float) getHeight() / shellH));
    auto shell = juce::Rectangle<float> (shellW, shellH);

    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRoundedRectangle (shell.translated (6, 14), 26.0f);

    if (! hasTape)
        return;

    // The same layers a cassette's look is saved in (docs/formats/cass-v1.md, "The look").
    paintBack (g);
    paintWorks (g);
    paintFront (g, {});   // the label's writing is the label component's, over this
}

void CassetteComponent::paintBack (juce::Graphics& g) const
{
    auto shell = juce::Rectangle<float> (shellW, shellH);
    auto st = styleFor (model);
    g.setColour (st.shell);
    g.fillRoundedRectangle (shell, 24.0f);
}

void CassetteComponent::paintWorks (juce::Graphics& g) const
{
    auto st = styleFor (model);
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

    // Winding, the hubs blur (drawn at the angles they passed through since the last frame).
    auto blurred = [&] (juce::Point<float> c, float angle, float step)
    {
        const auto spread = std::min (std::abs (step), juce::MathConstants<float>::twoPi);
        const int copies = spread < 0.12f ? 1 : juce::jlimit (2, 12, (int) std::ceil (spread / 0.1f));
        if (copies == 1) { drawHub (g, c, angle); return; }
        for (int n = 0; n < copies; ++n)
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.beginTransparencyLayer (1.0f / (float) (n + 1));
            drawHub (g, c, angle - (step < 0 ? -1.0f : 1.0f) * spread * (float) n / (float) (copies - 1));
            g.endTransparencyLayer();
        }
    };
    blurred ({ lx, cy }, angleL, stepL);
    blurred ({ rx, cy }, angleR, stepR);

}

void CassetteComponent::paintFront (juce::Graphics& g, const juce::String& writtenLabel) const
{
    auto& lf = DeckLookAndFeel::get();
    auto shell = juce::Rectangle<float> (shellW, shellH);
    auto st = styleFor (model);

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
    g.drawText (sideLetter(), juce::Rectangle<float> (88, 400, 36, 66), juce::Justification::centredLeft);

    // lower section with the head openings
    juce::Path lower;
    lower.startNewSubPath (168, shellH); lower.lineTo (200, 494); lower.lineTo (804, 494); lower.lineTo (836, shellH); lower.closeSubPath();
    g.setColour (st.lower);
    g.fillPath (lower);
    g.setColour (st.lowerEdge);
    g.strokePath (lower, juce::PathStrokeType (2.0f));
    g.setColour (model == 0 ? juce::Colours::black.withAlpha (0.07f) : juce::Colours::white.withAlpha (0.08f));
    for (int k = 0; k < 6; ++k)
        g.fillRect (210.0f - 2.0f * (float) k, 507.0f + 10.0f * (float) k, 584.0f + 4.0f * (float) k, 2.0f);
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
        auto big = ! juce::exactlyEqual (p.y, 562.0f);
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

    // The label's writing, as the label component shows it (22 px at the deck's 255 px height).
    if (writtenLabel.isNotEmpty())
    {
        g.setColour (st.ink);
        g.setFont (lf.hand (22.0f * shellH / 255.0f));
        g.drawText (writtenLabel, juce::Rectangle<float> (122.0f + 12.5f, 402.0f + 2.5f, 760.0f - 25.0f, 62.0f - 5.0f),
                    juce::Justification::centredLeft, true);
    }
}

//==============================================================================
namespace
{
    // Which version of the TF-4's drawings a look was made by: a change to the drawings raises it,
    // and cassettes get their looks redrawn the next time they're saved.
    constexpr const char* artRevision = "4";   // 2: looks written blank on Windows by revision 1 are redrawn; 3: real pack sizes (Geometry.h); 4: hollow hubs, leader clamp
    constexpr float artScale = 2.0f;   // image pixels per tenth of a mm

    std::string hex (juce::Colour c)
    {
        return ("#" + juce::String::toHexString ((int) c.getRed()).paddedLeft ('0', 2) + juce::String::toHexString ((int) c.getGreen()).paddedLeft ('0', 2)
                + juce::String::toHexString ((int) c.getBlue()).paddedLeft ('0', 2) + juce::String::toHexString ((int) c.getAlpha()).paddedLeft ('0', 2)).toStdString();
    }

    juce::MemoryBlock png (const juce::Image& image)
    {
        juce::MemoryOutputStream out;
        juce::PNGImageFormat().writeImageToStream (image, out);
        return out.getMemoryBlock();
    }

    std::string designOf (int model)
    {
        return "tapefour." + juce::String (tapeModels[juce::jlimit (0, numTapeModels - 1, model)].name).toLowerCase().toStdString()
               + "/" + artRevision;
    }
}

CassetteComponent::ArtSpec CassetteComponent::ArtSpec::of (const Tape& tape)
{
    return { tape.getModel(), tape.getTypeInfo().minutes, tape.fullPackMm(), tape.name };
}

juce::String CassetteComponent::ArtSpec::key() const
{
    return juce::String (designOf (model)) + "|" + juce::String (minutes) + "|" + juce::String (fullPackMm) + "|" + label;
}

CassetteComponent::ArtImages CassetteComponent::drawArt (const ArtSpec& spec)
{
    CassetteComponent c;
    c.model = spec.model;
    c.minutes = spec.minutes;
    c.hasTape = true;
    const auto st = styleFor (c.model);
    const auto w = juce::roundToInt (shellW * artScale), h = juce::roundToInt (shellH * artScale);
    // Drawn in memory, and the drawing finished (the Graphics gone) before the image is read:
    // on Windows a native image is drawn by the graphics card, and isn't there until then.
    auto layer = [&] (std::function<void (juce::Graphics&)> draw)
    {
        juce::Image image (juce::Image::ARGB, w, h, true, juce::SoftwareImageType());
        {
            juce::Graphics g (image);
            g.addTransform (juce::AffineTransform::scale (artScale));
            draw (g);
        }
        return image;
    };

    ArtImages art;
    art.back = layer ([&] (juce::Graphics& g) { c.paintBack (g); });
    c.side = 0;
    art.frontA = layer ([&] (juce::Graphics& g) { c.paintFront (g, spec.label); });
    c.side = 1;
    art.frontB = layer ([&] (juce::Graphics& g) { c.paintFront (g, spec.label); });

    constexpr float hubBox = 224.0f;
    {
        const auto px = juce::roundToInt (hubBox * artScale);
        juce::Image image (juce::Image::ARGB, px, px, true, juce::SoftwareImageType());
        {
            juce::Graphics g (image);
            g.addTransform (juce::AffineTransform::scale (artScale));
            c.drawHub (g, { hubBox * 0.5f, hubBox * 0.5f }, 0.0f);
        }
        art.hub = image;
    }

    auto& l = art.look;
    l.design = designOf (c.model);
    l.labelText = spec.label.toStdString();
    l.width = shellW;
    l.height = shellH;
    l.scale = artScale;
    l.back = "art/back.png";
    l.frontA = "art/front-a.png";
    l.frontB = "art/front-b.png";
    l.hub = "art/hub.png";
    l.hubSize = hubBox;
    l.reels = { { { lx, cy }, { rx, cy } } };
    l.pack.minDiameter = minPackD;
    l.pack.fullDiameter = spec.fullPackMm * 10.0f;
    l.pack.inner = hex (st.pack0);
    l.pack.outer = hex (st.pack1);
    l.pack.ring = hex (juce::Colours::white.withAlpha (0.045f));
    l.pack.ringWidth = 2;
    l.pack.ringSpacing = 26;
    l.pack.ringInset = 10;
    l.pack.ringStop = hubR;
    l.tape.colour = hex (st.tape);
    l.tape.width = 4;
    l.tape.guides = { { { guideLX, guideY }, { guideRX, guideY } } };
    l.tape.wrapRadius = wrapR;
    l.label = { 68, 398, 868, 80 };
    return art;
}

CassetteComponent::Art CassetteComponent::renderArt (const ArtSpec& spec)
{
    auto drawn = drawArt (spec);
    Art art;
    art.look = drawn.look;
    art.back = png (drawn.back);
    art.frontA = png (drawn.frontA);
    art.frontB = png (drawn.frontB);
    art.hub = png (drawn.hub);
    return art;
}

bool CassetteComponent::artIsCurrent (const ArtSpec& spec, const juce::File& folder)
{
    tfmt::Look look;
    std::string err;
    if (! tfmt::readLook (tfmt::pathFromUtf8 (folder.getFullPathName().toStdString()), look, err)) return false;
    // Only the TF-4's own drawings are redrawn: a cassette that came with another look keeps it.
    if (look.design.rfind ("tapefour.", 0) != 0) return true;
    return look.design == designOf (spec.model) && look.labelText == spec.label.toStdString()
           && tfmt::sameNumber (look.pack.fullDiameter, (double) (spec.fullPackMm * 10.0f))
           && folder.getChildFile (look.back).existsAsFile() && folder.getChildFile (look.frontA).existsAsFile()
           && folder.getChildFile (look.hub).existsAsFile();
}

bool CassetteComponent::writeArt (const ArtSpec& spec, const juce::File& folder)
{
    auto art = renderArt (spec);
    auto dir = folder.getChildFile ("art");
    if (! dir.createDirectory()) return false;
    auto put = [&] (const std::string& name, const void* data, size_t size)
    {
        auto file = folder.getChildFile (juce::String (name));
        auto tmp = file.getSiblingFile (file.getFileName() + ".tmp");
        return tmp.replaceWithData (data, size) && tmp.moveFileTo (file);
    };
    // The images first, the description last: a reader never finds a look naming images that aren't there.
    auto json = tfmt::toJson (art.look).dump() + "\n";
    return put (art.look.back, art.back.getData(), art.back.getSize())
           && put (art.look.frontA, art.frontA.getData(), art.frontA.getSize())
           && put (art.look.frontB, art.frontB.getData(), art.frontB.getSize())
           && put (art.look.hub, art.hub.getData(), art.hub.getSize())
           && put (tfmt::lookFile, json.data(), json.size());
}
} // namespace tf
