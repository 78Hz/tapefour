#include "CassetteComponent.h"

namespace tf
{
namespace
{
    constexpr float hubD = 84.0f;          // 21 mm hub
    constexpr float minPackD = 86.0f;      // leader wound on an empty hub
    constexpr float cy = 108.0f, lx = 115.0f, rx = 285.0f;
    constexpr float guideY = 236.0f, guideR = 7.0f, guideLX = 40.0f, guideRX = 360.0f;
    constexpr float pxPerTapeSecond = 47.6f * 3.98f;   // 4.76 cm/s at 3.98 px/mm

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
}

CassetteComponent::CassetteComponent()
{
    nameLabel.setEditable (false, true, false);
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setFont (DeckLookAndFeel::get().hand (24.0f));
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
    nameLabel.setBounds (70, 9, 260, 28);
}

void CassetteComponent::setTapeName (const juce::String& name, const juce::String& type)
{
    nameLabel.setText (name, juce::dontSendNotification);
    typeName = type;
    repaint();
}

float CassetteComponent::packDiameter (float f) const
{
    // Wound tape area is conserved between the spools.
    return std::sqrt (minPackD * minPackD + (fullPackPx * fullPackPx - minPackD * minPackD) * juce::jlimit (0.0f, 1.0f, f));
}

void CassetteComponent::update (const Tape* tape, double pos)
{
    hasTape = tape != nullptr;
    if (! hasTape) { repaint(); return; }

    fullPackPx = tape->getTypeInfo().fullPackMm * 3.98f;
    fraction = (float) (pos / (double) tape->getLengthCells());

    if (lastPos >= 0.0)
    {
        auto movedPx = (float) ((pos - lastPos) / cellRate) * pxPerTapeSecond;
        auto rL = packDiameter (1.0f - fraction) * 0.5f, rR = packDiameter (fraction) * 0.5f;
        // Tape moving left to right turns both spools anticlockwise.
        angleL -= movedPx / rL;
        angleR -= movedPx / rR;
        angleL = std::fmod (angleL, juce::MathConstants<float>::twoPi);
        angleR = std::fmod (angleR, juce::MathConstants<float>::twoPi);
    }
    lastPos = pos;
    repaint();
}

void CassetteComponent::drawPack (juce::Graphics& g, juce::Point<float> c, float d) const
{
    auto r = juce::Rectangle<float> (d, d).withCentre (c);
    g.setColour (juce::Colour (0xff4b2d1c));
    g.fillEllipse (r);
    g.setColour (juce::Colour (0xff3a2215));
    for (float rr = d * 0.5f - 1.5f; rr > hubD * 0.5f; rr -= 3.0f)
        g.drawEllipse (juce::Rectangle<float> (rr * 2, rr * 2).withCentre (c), 0.8f);
    g.setColour (juce::Colour (0xff6b4630));
    g.drawEllipse (r.reduced (0.5f), 1.0f);
    juce::ColourGradient sheen (juce::Colours::white.withAlpha (0.10f), r.getX(), r.getY(),
                                juce::Colours::transparentWhite, r.getCentreX(), r.getCentreY(), false);
    g.setGradientFill (sheen);
    g.fillEllipse (r);
}

void CassetteComponent::drawHub (juce::Graphics& g, juce::Point<float> c, float angle) const
{
    auto t = juce::AffineTransform::rotation (angle).translated (c);
    auto s = hubD / 72.0f;
    auto rr = [&] (float x, float y, float w, float h) { juce::Path p; p.addRectangle (x * s - 36 * s, y * s - 36 * s, w * s, h * s); return p; };

    g.setColour (juce::Colour (0xfff2f3f4));
    g.fillEllipse (juce::Rectangle<float> (hubD, hubD).withCentre (c));
    g.setColour (juce::Colour (0xffc4c8cc));
    g.drawEllipse (juce::Rectangle<float> (hubD - 2, hubD - 2).withCentre (c), 2.0f);

    g.setColour (juce::Colour (0xff5a3825));
    g.fillPath (rr (33, 2, 6, 9), t);

    g.setColour (juce::Colour (0xff141516));
    g.fillEllipse (juce::Rectangle<float> (38 * s, 38 * s).withCentre (c));

    g.setColour (juce::Colour (0xfff2f3f4));
    for (int k = 0; k < 6; ++k)
        g.fillPath (rr (33.5f, 17, 5, 8), juce::AffineTransform::rotation (angle + k * juce::MathConstants<float>::pi / 3.0f).translated (c));
}

void CassetteComponent::paint (juce::Graphics& g)
{
    auto& lf = DeckLookAndFeel::get();
    auto shell = getLocalBounds().toFloat();

    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillRoundedRectangle (shell.translated (0, 4), 12.0f);
    g.setColour (juce::Colour::fromRGBA (150, 160, 170, 26));
    g.fillRoundedRectangle (shell, 12.0f);

    if (! hasTape)
        return;

    auto dL = packDiameter (1.0f - fraction), dR = packDiameter (fraction);
    drawPack (g, { lx, cy }, dL);
    drawPack (g, { rx, cy }, dR);

    auto molded = juce::Colour::fromRGBA (225, 232, 240, 77);

    // window
    g.setColour (juce::Colours::white.withAlpha (0.03f));
    g.fillRoundedRectangle (80, 64, 240, 88, 10);
    g.setColour (molded);
    g.drawRoundedRectangle (80, 64, 240, 88, 10, 1.5f);

    // head opening
    juce::Path head;
    head.startNewSubPath (68, 255); head.lineTo (96, 212); head.lineTo (304, 212); head.lineTo (332, 255); head.closeSubPath();
    g.setColour (juce::Colours::black.withAlpha (0.3f));
    g.fillPath (head);
    g.setColour (molded);
    g.strokePath (head, juce::PathStrokeType (1.5f));

    for (auto x : { 112.0f, 288.0f })
    {
        g.setColour (juce::Colour (0xff0a0a0a));
        g.fillEllipse (x - 7, 221, 14, 14);
        g.setColour (molded);
        g.drawEllipse (x - 7, 221, 14, 14, 1.0f);
    }
    g.setColour (juce::Colour (0xff0a0a0a));
    g.fillRect (184.0f, 246.0f, 32.0f, 9.0f);
    g.setColour (juce::Colour (0xffc9cdd2));
    g.fillRoundedRectangle (190, 230, 20, 12, 2);

    // tape path
    auto tl = tangent ({ lx, cy }, dL * 0.5f, { guideLX, guideY }, guideR, -1);
    auto tr = tangent ({ rx, cy }, dR * 0.5f, { guideRX, guideY }, guideR, 1);
    juce::Path tape;
    tape.startNewSubPath (tl.first);
    tape.lineTo (tl.second);
    // JUCE arc angles run clockwise from 12 o'clock; atan2 runs clockwise from 3 o'clock (y is down).
    auto aL = std::atan2 (tl.second.y - guideY, tl.second.x - guideLX);
    if (aL < 0) aL += juce::MathConstants<float>::twoPi;
    tape.addCentredArc (guideLX, guideY, guideR, guideR, 0, aL + juce::MathConstants<float>::halfPi, juce::MathConstants<float>::pi);
    tape.lineTo (guideRX, guideY + guideR);
    auto aR = std::atan2 (tr.second.y - guideY, tr.second.x - guideRX);
    tape.addCentredArc (guideRX, guideY, guideR, guideR, 0, juce::MathConstants<float>::pi, aR + juce::MathConstants<float>::halfPi);
    tape.lineTo (tr.first);
    g.setColour (juce::Colour (0xff5a3825));
    g.strokePath (tape, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved));

    for (auto x : { guideLX, guideRX })
    {
        g.setColour (juce::Colour (0xffe9ecef));
        g.fillEllipse (x - guideR, guideY - guideR, guideR * 2, guideR * 2);
        g.setColour (juce::Colour (0xff9aa0a6));
        g.drawEllipse (x - guideR, guideY - guideR, guideR * 2, guideR * 2, 1.0f);
        g.fillEllipse (x - 2, guideY - 2, 4, 4);
    }

    // screws
    for (auto p : std::initializer_list<juce::Point<float>> { { 16, 16 }, { 384, 16 }, { 16, 239 }, { 384, 239 }, { 200, 190 } })
    {
        g.setColour (juce::Colour (0xff8d9196));
        g.fillEllipse (p.x - 6, p.y - 6, 12, 12);
        g.setColour (juce::Colour (0xff4a4e53));
        g.drawLine (p.x - 4, p.y, p.x + 4, p.y, 1.5f);
        g.drawLine (p.x, p.y - 4, p.x, p.y + 4, 1.5f);
    }

    drawHub (g, { lx, cy }, angleL);
    drawHub (g, { rx, cy }, angleR);

    // the clear shell over everything
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.16f), 0, 0,
                                             juce::Colours::white.withAlpha (0.0f), shell.getWidth() * 0.55f, shell.getHeight() * 0.55f, false));
    g.fillRoundedRectangle (shell, 12.0f);
    g.setColour (juce::Colour::fromRGBA (225, 232, 240, 97));
    g.drawRoundedRectangle (shell.reduced (0.75f), 12.0f, 1.5f);

    // label strip
    g.setColour (juce::Colour::fromRGBA (242, 238, 228, 224));
    g.fillRoundedRectangle (30, 8, 340, 30, 3);
    g.setColour (Pal::orange.withAlpha (0.75f));
    g.fillRect (30.0f, 38.0f, 340.0f, 3.0f);
    g.setColour (juce::Colour (0xff1a1917));
    g.setFont (lf.cond (20.0f, true));
    g.drawText ("A", juce::Rectangle<int> (40, 8, 20, 30), juce::Justification::centredLeft);
    g.setFont (lf.cond (15.0f, true));
    g.drawText (typeName, juce::Rectangle<int> (300, 8, 62, 30), juce::Justification::centredRight);
}
} // namespace tf
