#include "LookView.h"

namespace tf
{
namespace
{
    juce::Colour colourOf (const std::string& hex, juce::Colour fallback)
    {
        auto s = juce::String (hex).trimCharactersAtStart ("#");
        if (s.length() != 6 && s.length() != 8) return fallback;
        auto v = (juce::uint32) s.getHexValue64();
        if (s.length() == 6) return juce::Colour (0xff000000u | v);
        return juce::Colour ((juce::uint8) (v >> 24), (juce::uint8) (v >> 16), (juce::uint8) (v >> 8), (juce::uint8) v);
    }

    juce::Image decode (const juce::MemoryBlock& bytes)
    {
        return bytes.isEmpty() ? juce::Image() : juce::ImageFileFormat::loadFrom (bytes.getData(), bytes.getSize());
    }

    // Where the tape leaves a pack (centre c, radius r1) on its outside, and meets the guide (g, r2).
    std::pair<juce::Point<float>, juce::Point<float>> tangent (juce::Point<float> c, float r1, juce::Point<float> g, float r2, int side)
    {
        auto d = g - c;
        auto len = d.getDistanceFromOrigin();
        auto th = std::atan2 (d.y, d.x);
        auto a = std::acos (juce::jlimit (-1.0f, 1.0f, (r1 - r2) / len));
        juce::Point<float> n1 (std::cos (th + a), std::sin (th + a)), n2 (std::cos (th - a), std::sin (th - a));
        auto n = (side < 0) == (n1.x < n2.x) ? n1 : n2;
        return { c + n * r1, g + n * r2 };
    }
}

//==============================================================================
bool CassetteArtwork::isBlank() const
{
    // A 24 x 16 grid over each image: one pixel with any alpha is enough to show it was drawn.
    auto drawn = [] (const juce::Image& im)
    {
        if (! im.isValid()) return false;
        const juce::Image::BitmapData px (im, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 24; ++x)
                if (px.getPixelColour ((x * 2 + 1) * im.getWidth() / 48, (y * 2 + 1) * im.getHeight() / 32).getAlpha() > 0)
                    return true;
        return false;
    };
    return ! drawn (back) && ! drawn (frontA);
}

CassetteArtwork CassetteArtwork::fromParts (const tfmt::Look& look, const std::function<juce::MemoryBlock (const std::string&)>& readFile)
{
    CassetteArtwork a;
    a.look = look;
    a.back = decode (readFile (look.back));
    a.frontA = decode (readFile (look.frontA));
    a.frontB = look.frontB == look.frontA ? a.frontA : decode (readFile (look.frontB));
    a.hub = decode (readFile (look.hub));
    if (! a.frontB.isValid()) a.frontB = a.frontA;
    return a;
}

CassetteArtwork CassetteArtwork::fromPacked (const tfmt::PackedCassette& p)
{
    std::string json, err;
    if (! tfmt::readPackedEntry (p, tfmt::lookFile, json, 1u << 20)) return {};
    auto j = tfmt::Json::parse (json, &err);
    tfmt::Look look;
    if (! j || ! tfmt::fromJson (*j, look, err)) return {};
    return fromParts (look, [&p] (const std::string& name)
    {
        std::string bytes;
        tfmt::readPackedEntry (p, name, bytes, 32u << 20);
        return juce::MemoryBlock (bytes.data(), bytes.size());
    });
}

CassetteArtwork CassetteArtwork::fromFolder (const juce::File& folder)
{
    tfmt::Look look;
    std::string err;
    if (! tfmt::readLook (tfmt::pathFromUtf8 (folder.getFullPathName().toStdString()), look, err)) return {};
    return fromParts (look, [&folder] (const std::string& name)
    {
        juce::MemoryBlock bytes;
        folder.getChildFile (juce::String (name)).loadFileAsData (bytes);
        return bytes;
    });
}

//==============================================================================
void LookView::setArtwork (CassetteArtwork a)
{
    art = std::move (a);
    drawnScale = 0.0f;
    rescale();
    repaint();
}

void LookView::setSide (int s)
{
    if (s == side) return;
    side = s;
    drawnScale = 0.0f;
    rescale();
    repaint();
}

void LookView::setPlayed (double f, double tapeSecondsMoved)
{
    f = juce::jlimit (0.0, 1.0, f);
    if (! juce::exactlyEqual (tapeSecondsMoved, 0.0) && art.isValid())
    {
        // Each hub turns by the tape that passed over its own pack (47.6 mm a second at 1 7/8 ips),
        // anticlockwise as the tape runs from left to right.
        auto moved = (float) (tapeSecondsMoved * 476.0);
        auto rL = (float) tfmt::packDiameter (art.look, 1.0 - f) * 0.5f, rR = (float) tfmt::packDiameter (art.look, f) * 0.5f;
        angleL = std::fmod (angleL - moved / rL, juce::MathConstants<float>::twoPi);
        angleR = std::fmod (angleR - moved / rR, juce::MathConstants<float>::twoPi);
    }
    if (juce::exactlyEqual (f, fraction) && juce::exactlyEqual (tapeSecondsMoved, 0.0)) return;
    fraction = f;
    repaint();
}

juce::Rectangle<float> LookView::shellBounds() const
{
    if (! art.isValid()) return {};
    auto w = (float) art.look.width, h = (float) art.look.height;
    auto scale = juce::jmin ((float) getWidth() / w, (float) getHeight() / h);
    return juce::Rectangle<float> (w * scale, h * scale).withCentre (getLocalBounds().toFloat().getCentre());
}

void LookView::resized()
{
    rescale();
}

void LookView::rescale()
{
    if (! art.isValid() || getWidth() <= 0 || getHeight() <= 0) return;
    const auto shell = shellBounds();
    const auto scale = shell.getWidth() / (float) art.look.width * (float) juce::Desktop::getInstance().getGlobalScaleFactor();
    if (juce::exactlyEqual (scale, drawnScale)) return;
    drawnScale = scale;
    // At the physical display's pixel density, so the cassette stays sharp on high-DPI screens.
    auto density = 1.0f;
    if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) density = (float) d->scale;
    auto resize = [density] (const juce::Image& im, float unitsWide, float pxPerUnit)
    {
        auto w = juce::jmax (1, juce::roundToInt (unitsWide * pxPerUnit * density));
        auto h = juce::jmax (1, juce::roundToInt ((float) im.getHeight() * (float) w / (float) im.getWidth()));
        return im.rescaled (w, h, juce::Graphics::highResamplingQuality);
    };
    const auto pxPerUnit = shell.getWidth() / (float) art.look.width;
    back = resize (art.back, (float) art.look.width, pxPerUnit);
    front = resize (side == 1 ? art.frontB : art.frontA, (float) art.look.width, pxPerUnit);
    hub = resize (art.hub, (float) art.look.hubSize, pxPerUnit);
}

void LookView::paint (juce::Graphics& g)
{
    if (! art.isValid()) return;
    const auto shell = shellBounds();
    const auto& l = art.look;
    const auto k = shell.getWidth() / (float) l.width;   // pixels per tenth of a mm
    auto at = [&] (double x, double y) { return juce::Point<float> (shell.getX() + (float) x * k, shell.getY() + (float) y * k); };

    // 1. the shell behind the works
    g.drawImage (back, shell);

    // 2. the packs
    const juce::Point<float> reels[2] { at (l.reels[0].x, l.reels[0].y), at (l.reels[1].x, l.reels[1].y) };
    const float d[2] { (float) tfmt::packDiameter (l, 1.0 - fraction) * k, (float) tfmt::packDiameter (l, fraction) * k };
    const auto inner = colourOf (l.pack.inner, juce::Colour (0xff3a2618)), outer = colourOf (l.pack.outer, juce::Colour (0xff160c06));
    const auto ring = colourOf (l.pack.ring, juce::Colours::white.withAlpha (0.045f));
    for (int i = 0; i < 2; ++i)
    {
        auto c = reels[i];
        g.setGradientFill (juce::ColourGradient (inner, c.x, c.y, outer, c.x + d[i] * 0.5f, c.y, true));
        g.fillEllipse (juce::Rectangle<float> (d[i], d[i]).withCentre (c));
        g.setColour (ring);
        for (float rr = d[i] * 0.5f - (float) l.pack.ringInset * k; rr > (float) l.pack.ringStop * k; rr -= (float) l.pack.ringSpacing * k)
            g.drawEllipse (juce::Rectangle<float> (rr * 2, rr * 2).withCentre (c), (float) l.pack.ringWidth * k);
    }

    // 3. the tape, off the outside of each pack, round under its guide, straight between the guides
    const auto gl = at (l.tape.guides[0].x, l.tape.guides[0].y), gr = at (l.tape.guides[1].x, l.tape.guides[1].y);
    const auto wrap = (float) l.tape.wrapRadius * k;
    auto tl = tangent (reels[0], d[0] * 0.5f, gl, wrap, -1);
    auto tr = tangent (reels[1], d[1] * 0.5f, gr, wrap, 1);
    juce::Path tape;
    tape.startNewSubPath (tl.first);
    tape.lineTo (tl.second);
    // JUCE arcs run clockwise from 12 o'clock; atan2 from 3 o'clock (y down).
    auto aL = std::atan2 (tl.second.y - gl.y, tl.second.x - gl.x);
    if (aL < 0) aL += juce::MathConstants<float>::twoPi;
    tape.addCentredArc (gl.x, gl.y, wrap, wrap, 0, aL + juce::MathConstants<float>::halfPi, juce::MathConstants<float>::pi);
    tape.lineTo (gr.x, gr.y + wrap);
    auto aR = std::atan2 (tr.second.y - gr.y, tr.second.x - gr.x);
    tape.addCentredArc (gr.x, gr.y, wrap, wrap, 0, juce::MathConstants<float>::pi, aR + juce::MathConstants<float>::halfPi);
    tape.lineTo (tr.first);
    g.setColour (colourOf (l.tape.colour, juce::Colour (0xff2c1a0e)));
    g.strokePath (tape, juce::PathStrokeType ((float) l.tape.width * k, juce::PathStrokeType::curved));

    // 4. the hubs, turned
    const auto hubPx = (float) l.hubSize * k;
    for (int i = 0; i < 2; ++i)
    {
        auto c = reels[i];
        auto t = juce::AffineTransform::scale (hubPx / (float) hub.getWidth())
                     .translated (-hubPx * 0.5f, -hubPx * 0.5f)
                     .rotated (i == 0 ? angleL : angleR)
                     .translated (c.x, c.y);
        g.drawImageTransformed (hub, t);
    }

    // 5. everything in front
    g.drawImage (front, shell);
}
} // namespace tf
