#include "PatchBay.h"

namespace tf
{
namespace
{
    constexpr int pad = 16, headTop = 54, rowsTop = 92, rowH = 22, headingH = 18, footH = 70;
    constexpr int colX = 440, colW = 94;
    const juce::Colour frame { 0xff3d3b36 };
}

PatchBay::PatchBay (AudioEngine& e) : engine (e)
{
    setOpaque (false);
    setWantsKeyboardFocus (true);
    closeX.onClick = [this] { if (onClose) onClose(); };
    closeX.setTooltip ("Close (Esc)");
    addAndMakeVisible (closeX);
    rebuild();
    setSize (cardWidth, wantedHeight());
    startTimerHz (10);
}

juce::String PatchBay::signature() const
{
    juce::String s = engine.routeSignature();
    for (int t = 0; t < numTracks; ++t)
        s << "|" << engine.track[t].feed.load() << engine.track[t].inputChannel.load() << (int) engine.track[t].inputPair.load();
    s << "|" << engine.getActiveInputNames().joinIntoString (",");
    return s;
}

void PatchBay::timerCallback()
{
    if (signature() != lastSignature)
    {
        rebuild();
        repaint();
    }
}

void PatchBay::rebuild()
{
    lastSignature = signature();
    rows.clear();
    auto heading = [this] (const juce::String& text) { Row r; r.kind = Kind::heading; r.name = text; rows.push_back (r); };

    heading ("INTERFACE INPUTS");
    auto names = engine.getActiveInputNames();
    for (int i = 0; i < names.size() && i < AudioEngine::maxInputs; ++i)
    {
        Row r; r.kind = Kind::input; r.input = i;
        r.name = "IN " + juce::String (i + 1);
        r.sub = names[i].toUpperCase();
        rows.push_back (r);
    }
    for (int i = 0; i + 1 < names.size() && i + 1 < AudioEngine::maxInputs; i += 2)
    {
        Row r; r.kind = Kind::input; r.input = i; r.pair = true;
        r.name = "IN " + juce::String (i + 1) + "+" + juce::String (i + 2);
        r.sub = "MONO SUM";
        rows.push_back (r);
    }
    if (names.isEmpty())
    {
        Row r; r.kind = Kind::heading; r.name = utf ("    NONE ACTIVE: SEE SETUP \xe2\x80\xba AUDIO");
        rows.push_back (r);
    }

    auto sources = engine.getRouteSources();
    int lastBlock = -1;
    for (auto& src : sources)
    {
        if (src.blockId == 0) continue;
        if (src.blockId != lastBlock)
        {
            heading ("TRK " + juce::String (src.track + 1) + utf (" \xc2\xb7 ") + src.owner);
            lastBlock = src.blockId;
        }
        Row r; r.kind = Kind::output; r.src = src;
        r.name = src.name.isEmpty() ? juce::String ("MAIN") : src.name;
        r.sub = src.channels == 1 ? "MONO" : "STEREO";
        rows.push_back (r);
    }

    heading (utf ("CHAIN OUTS \xc2\xb7 A COPY OF EACH TRACK'S FINISHED SOUND"));
    for (auto& src : sources)
        if (src.blockId == 0)
        {
            Row r; r.kind = Kind::chainOut; r.src = src;
            r.name = src.name;
            if ((AudioEngine::Feed) engine.track[src.track].feed.load() == AudioEngine::Feed::bounce)
                r.sub = "BOUNCES: CAN'T BE SENT ON";
            rows.push_back (r);
        }

    if (hoverRow >= (int) rows.size()) hoverRow = -1;
    const int h = wantedHeight();
    if (getHeight() != h && getParentComponent() != nullptr)
    {
        setSize (cardWidth, h);
        getParentComponent()->repaint();
    }
    int content = 0;
    for (auto& r : rows) content += r.kind == Kind::heading ? headingH : rowH;
    scroll = juce::jlimit (0, std::max (0, content - rowsArea().getHeight()), scroll);
}

int PatchBay::wantedHeight() const
{
    int content = 0;
    for (auto& r : rows) content += r.kind == Kind::heading ? headingH : rowH;
    return std::min (maxHeight, rowsTop + content + 8 + footH);
}

juce::Rectangle<int> PatchBay::rowsArea() const
{
    return { pad, rowsTop, cardWidth - 2 * pad, getHeight() - footH - rowsTop - 4 };
}

void PatchBay::resized()
{
    closeX.setBounds (cardWidth - pad - 26, 12, 26, 26);
}

//==============================================================================
PatchBay::Point PatchBay::pointAt (const Row& r, int k, juce::String* why) const
{
    const bool bounceTo = (AudioEngine::Feed) engine.track[k].feed.load() == AudioEngine::Feed::bounce;
    auto barred = [&] (const juce::String& reason) { if (why != nullptr) *why = reason; return Point::barred; };
    auto loopWhy = [&] (int from)
    {
        if ((AudioEngine::Feed) engine.track[from].feed.load() == AudioEngine::Feed::bounce)
            return "TRK " + juce::String (from + 1) + " BOUNCES FROM TAPE: ITS SOUND CAN'T BE SENT ON";
        return "TRK " + juce::String (k + 1) + " ALREADY FEEDS TRK " + juce::String (from + 1) + ": IT WOULD MAKE A LOOP";
    };
    switch (r.kind)
    {
        case Kind::heading:
            return Point::none;

        case Kind::input:
        {
            if (bounceTo) return barred ("TRK " + juce::String (k + 1) + " BOUNCES FROM TAPE: IT TAKES NO INPUT");
            const auto& p = engine.track[k];
            return p.inputChannel.load() == r.input && p.inputPair.load() == r.pair ? Point::audioIn : Point::open;
        }

        case Kind::output:
            if (k == r.src.track) return r.src.home ? Point::home : Point::homeAway;
            if (r.src.dest == k) return Point::sent;
            return engine.routeAllowed (r.src.track, k) ? Point::open : barred (loopWhy (r.src.track));

        case Kind::chainOut:
        {
            if (k == r.src.track) return barred ("A TRACK CAN'T FEED ITSELF");
            if (engine.track[r.src.track].chainOutMask.load() & (1 << k)) return Point::sent;
            return engine.routeAllowed (r.src.track, k) ? Point::open : barred (loopWhy (r.src.track));
        }
    }
    return Point::none;
}

void PatchBay::click (const Row& r, int k)
{
    juce::String why;
    const auto pt = pointAt (r, k, &why);
    if (pt == Point::barred) { if (onMessage) onMessage ("NOT ALLOWED: " + why); return; }
    switch (r.kind)
    {
        case Kind::heading: return;
        case Kind::input:
            if (pt == Point::audioIn) engine.setTrackInput (k, -1);
            else                      engine.setTrackInput (k, r.input, r.pair);
            break;
        case Kind::output:
        {
            const auto& s = r.src;
            if (k == s.track)
            {
                // the ring: stays here too (a copy) or not (moved)
                if (s.dest < 0 && ! s.canBeOff) { if (onMessage) onMessage ("SEND IT TO A TRACK FIRST: THEN IT CAN LEAVE ITS OWN"); return; }
                engine.setBlockRoute (s.track, s.blockId, s.output, s.dest, ! s.home);
            }
            else if (pt == Point::sent) engine.setBlockRoute (s.track, s.blockId, s.output, -1, s.home);
            else                        engine.setBlockRoute (s.track, s.blockId, s.output, k, s.dest >= 0 ? s.home : false);   // first sent: moved
            break;
        }
        case Kind::chainOut:
            engine.setChainOut (r.src.track, k, pt != Point::sent);
            break;
    }
    engine.saveSettings();
    rebuild();
    repaint();
}

juce::String PatchBay::describe (const Row& r) const
{
    const auto dot = utf (" \xc2\xb7 ");
    switch (r.kind)
    {
        case Kind::heading: return {};
        case Kind::input:
        {
            juce::StringArray on;
            for (int k = 0; k < numTracks; ++k) if (pointAt (r, k) == Point::audioIn) on.add ("TRK " + juce::String (k + 1));
            return r.name + (on.isEmpty() ? juce::String (": NOT A TRACK'S INPUT") : ": THE INPUT OF " + on.joinIntoString (", "))
                   + dot + "A TRACK TAKES ONE: CLICKING ANOTHER REPLACES IT";
        }
        case Kind::output:
        {
            const auto& s = r.src;
            const auto own = "TRK " + juce::String (s.track + 1);
            if (s.dest < 0)
                return r.name + (s.home ? ": ON " + own + " ONLY" : juce::String (": OFF, NOT HEARD")) + dot + "CLICK A TRACK TO SEND IT THERE";
            const auto to = "TRK " + juce::String (s.dest + 1);
            return r.name + (s.home ? ": A COPY ON " + to + " (IT STAYS ON " + own + ")" + dot + "CLICK ITS RING ON " + own + " TO MOVE IT INSTEAD"
                                    : ": MOVED TO " + to + dot + "CLICK ITS RING ON " + own + " TO KEEP A COPY THERE");
        }
        case Kind::chainOut:
        {
            juce::StringArray on;
            for (int k = 0; k < numTracks; ++k) if (pointAt (r, k) == Point::sent) on.add ("TRK " + juce::String (k + 1));
            return r.name + (on.isEmpty() ? juce::String (": NOT SENT") : ": A COPY ON " + on.joinIntoString (", ")) + dot + "THE TRACK KEEPS ITS OWN SOUND";
        }
    }
    return {};
}

//==============================================================================
int PatchBay::rowAt (juce::Point<int> p) const
{
    if (! rowsArea().contains (p)) return -1;
    int y = rowsTop - scroll;
    for (int i = 0; i < (int) rows.size(); ++i)
    {
        const int h = rows[(size_t) i].kind == Kind::heading ? headingH : rowH;
        if (p.y >= y && p.y < y + h) return rows[(size_t) i].kind == Kind::heading ? -1 : i;
        y += h;
    }
    return -1;
}

int PatchBay::columnAt (juce::Point<int> p) const
{
    if (p.x < colX) return -1;
    const int k = (p.x - colX) / colW;
    return k < numTracks ? k : -1;
}

void PatchBay::mouseMove (const juce::MouseEvent& e)
{
    const int r = rowAt (e.getPosition());
    if (r != hoverRow) { hoverRow = r; repaint(); }
}

void PatchBay::mouseExit (const juce::MouseEvent&)
{
    if (hoverRow != -1) { hoverRow = -1; repaint(); }
}

void PatchBay::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    const int r = rowAt (e.getPosition()), k = columnAt (e.getPosition());
    if (r >= 0 && k >= 0)
        click (rows[(size_t) r], k);
}

void PatchBay::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    int content = 0;
    for (auto& r : rows) content += r.kind == Kind::heading ? headingH : rowH;
    const int most = std::max (0, content - rowsArea().getHeight());
    scroll = juce::jlimit (0, most, scroll - juce::roundToInt (w.deltaY * 120.0f));
    repaint();
}

bool PatchBay::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { if (onClose) onClose(); return true; }
    return false;
}

//==============================================================================
void PatchBay::paint (juce::Graphics& g)
{
    auto& lf = DeckLookAndFeel::get();
    auto body = getLocalBounds().toFloat();
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (body, 7.0f);
    g.setColour (frame);
    g.fillRoundedRectangle (body.reduced (1.0f), 6.5f);
    g.setColour (Pal::lcd);
    g.fillRoundedRectangle (body.reduced (3.0f), 5.0f);

    // header and columns
    g.setColour (Pal::lcdText);
    g.setFont (lf.mono (15.0f));
    g.drawText ("PATCH BAY", pad, 12, 200, 26, juce::Justification::centredLeft);
    g.setColour (Pal::lcdDim);
    g.setFont (lf.mono (10.0f));
    g.drawText (utf ("EVERY SOURCE, AND THE TRACKS IT GOES TO \xc2\xb7 CLICK A POINT TO CONNECT OR DISCONNECT"),
                220, 12, cardWidth - 220 - pad - 36, 26, juce::Justification::centred);
    for (int k = 0; k < numTracks; ++k)
    {
        const int cx = colX + k * colW;
        g.setColour (Pal::lcdText);
        g.setFont (lf.mono (12.0f));
        g.drawText ("TRK " + juce::String (k + 1), cx, headTop, colW, 16, juce::Justification::centred);
        if ((AudioEngine::Feed) engine.track[k].feed.load() == AudioEngine::Feed::bounce)
        {
            g.setColour (Pal::lcdDim);
            g.setFont (lf.mono (9.0f));
            g.drawText ("BOUNCE", cx, headTop + 16, colW, 12, juce::Justification::centred);
        }
    }
    g.setColour (Pal::lcdRule);
    g.fillRect (pad, rowsTop - 8, cardWidth - 2 * pad, 1);

    auto drawPoint = [&g] (Point pt, juce::Point<float> c)
    {
        const auto r6 = juce::Rectangle<float> (12.0f, 12.0f).withCentre (c);
        switch (pt)
        {
            case Point::none: break;
            case Point::open:     g.setColour (Pal::lcdRule); g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (c)); break;
            case Point::sent:     g.setColour (Pal::lcdText); g.fillEllipse (r6); break;
            case Point::home:     g.setColour (Pal::lcdText); g.drawEllipse (r6.reduced (0.5f), 1.2f); break;
            case Point::homeAway: g.setColour (Pal::lcdRule); g.drawEllipse (r6.reduced (0.5f), 1.2f); break;
            case Point::audioIn:  g.setColour (Pal::lcdText); g.fillRect (r6); break;
            case Point::barred:
                g.setColour (Pal::lcdRule);
                for (float dy = -6.0f; dy <= 6.0f; dy += 3.0f) g.fillRect (juce::Rectangle<float> (c.x - 7.0f, c.y + dy, 14.0f, 1.0f));
                break;
        }
    };

    // the rows
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (rowsArea().withLeft (0).withRight (cardWidth));
        int y = rowsTop - scroll;
        for (int i = 0; i < (int) rows.size(); ++i)
        {
            const auto& r = rows[(size_t) i];
            if (r.kind == Kind::heading)
            {
                g.setColour (Pal::lcdDim);
                g.setFont (lf.mono (10.0f));
                g.drawText (r.name, pad, y, cardWidth - 2 * pad, headingH, juce::Justification::centredLeft);
                y += headingH;
                continue;
            }
            if (i == hoverRow)
            {
                g.setColour (Pal::lcdDim);
                g.drawRoundedRectangle (juce::Rectangle<float> ((float) pad, (float) y + 1.0f, (float) (cardWidth - 2 * pad), (float) rowH - 2.0f), 2.0f, 1.0f);
            }
            g.setColour (Pal::lcdText);
            g.setFont (lf.mono (12.0f));
            g.drawText (r.name, pad + 12, y, 170, rowH, juce::Justification::centredLeft);
            g.setColour (Pal::lcdDim);
            g.setFont (lf.mono (9.5f));
            g.drawText (r.sub, pad + 190, y, colX - pad - 196, rowH, juce::Justification::centredLeft);
            for (int k = 0; k < numTracks; ++k)
                drawPoint (pointAt (r, k), { (float) (colX + k * colW + colW / 2), (float) y + rowH * 0.5f });
            y += rowH;
        }
    }

    // the key, and what the row under the mouse does
    const int fy = getHeight() - footH;
    g.setColour (Pal::lcdRule);
    g.fillRect (pad, fy, cardWidth - 2 * pad, 1);
    g.setFont (lf.mono (9.5f));
    int x = pad + 8;
    for (auto [pt, text] : { std::pair<Point, const char*> { Point::sent, "SENT" }, { Point::home, "STAYS ON ITS OWN TRACK" },
                             { Point::homeAway, "MOVED AWAY" }, { Point::audioIn, "THE TRACK'S AUDIO IN (ONE A TRACK)" },
                             { Point::barred, "NOT ALLOWED (ITSELF, A LOOP, A BOUNCE)" } })
    {
        drawPoint (pt, { (float) x, (float) fy + 18.0f });
        g.setColour (Pal::lcdDim);
        const juce::String s (text);
        const int w = (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), s);
        g.drawText (s, x + 12, fy + 10, w + 4, 16, juce::Justification::centredLeft);
        x += w + 30;
    }
    g.setColour (hoverRow >= 0 ? Pal::lcdText : Pal::lcdDim);
    g.setFont (lf.mono (11.0f));
    g.drawFittedText (hoverRow >= 0 ? describe (rows[(size_t) hoverRow]) : utf ("POINT AT A ROW TO SEE WHERE IT GOES \xc2\xb7 ESC: DONE"),
                      pad, fy + 34, cardWidth - 2 * pad, 22, juce::Justification::centredLeft, 1, 0.7f);

    // more below (or above): a thin bar at the right
    int content = 0;
    for (auto& r : rows) content += r.kind == Kind::heading ? headingH : rowH;
    const auto area = rowsArea();
    if (content > area.getHeight())
    {
        const float frac = (float) area.getHeight() / (float) content;
        const float top = (float) area.getY() + (float) area.getHeight() * (float) scroll / (float) content;
        g.setColour (Pal::lcdDim);
        g.fillRect (juce::Rectangle<float> ((float) cardWidth - 8.0f, top, 2.0f, (float) area.getHeight() * frac));
    }
}
} // namespace tf
