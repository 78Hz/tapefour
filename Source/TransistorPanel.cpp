#include "TransistorPanel.h"

namespace tf
{
namespace
{
    using namespace iw;
    const char* const tabNames[] { "PANEL", "TUNING", "GENERATORS", "KEYS", "VIBRATO" };
    juce::String ms (float v) { return v < 0.05f ? juce::String ("NONE") : juce::String (v, 1) + " MS"; }
}

//==============================================================================
/** The organ's six drawbars, pulled down to bring a sound in: four pitches (light tips), two tones (dark tips). */
class TransistorPanel::Drawbars : public juce::Component
{
public:
    explicit Drawbars (organ::Processor& o) : organ (o) {}

    static constexpr int count = 6;
    static const char* id (int i) { static const char* ids[] { "d16", "d8", "d4", "dIV", "flute", "reed" }; return ids[i]; }
    static juce::String label (int i) { static const char* l[] { "16'", "8'", "4'", "IV", "~", "M" }; return utf (l[i]); }

    juce::Rectangle<float> slot (int i) const
    {
        const float w = 30.0f, gap = 30.0f, extra = i >= 4 ? 26.0f : 0.0f;
        return { 16.0f + (float) i * (w + gap) + extra, 4.0f, w, (float) getHeight() - 40.0f };
    }

    void paint (juce::Graphics& g) override
    {
        for (int i = 0; i < count; ++i)
        {
            auto* p = organ.param (id (i));
            const float v = p->convertFrom0to1 (p->getValue());
            const auto r = slot (i);
            const bool tone = i >= 4;
            const auto ink = tone ? Pal::lamp : Pal::lcdText;
            g.setColour (Pal::lcdRule);
            g.drawRoundedRectangle (r, 3.0f, 1.0f);
            const float step = (r.getHeight() - 14.0f) / 8.0f;
            for (int k = 0; k <= 8; ++k)
                text (g, juce::String (k), { (int) r.getX() - 14, (int) (r.getY() + 8.0f + step * (float) k) - 6, 12, 12 }, Pal::lcdDim.withAlpha (0.6f), 8.0f);
            const float h = 8.0f + step * v;
            g.setColour (ink);
            g.fillRect (juce::Rectangle<float> (r.getX() + 4.0f, r.getY() + 2.0f, r.getWidth() - 8.0f, h));
            g.fillRoundedRectangle (juce::Rectangle<float> (r.getX() + 1.0f, r.getY() + h - 2.0f, r.getWidth() - 2.0f, 9.0f), 2.0f);
            text (g, label (i), { (int) r.getX() - 10, (int) r.getBottom() + 4, (int) r.getWidth() + 20, 14 }, ink, 12.0f, juce::Justification::centred);
            text (g, juce::String (v, 1), { (int) r.getX() - 10, (int) r.getBottom() + 18, (int) r.getWidth() + 20, 12 }, Pal::lcdDim, 9.0f, juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = -1;
        for (int i = 0; i < count; ++i)
            if (slot (i).expanded (12.0f, 10.0f).contains (e.position)) dragging = i;
        if (dragging < 0) return;
        organ.param (id (dragging))->beginChangeGesture();
        mouseDrag (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        const auto r = slot (dragging);
        const float v = juce::jlimit (0.0f, 8.0f, (e.position.y - r.getY() - 8.0f) / ((r.getHeight() - 14.0f) / 8.0f));
        auto* p = organ.param (id (dragging));
        p->setValueNotifyingHost (p->convertTo0to1 (v));
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0) organ.param (id (dragging))->endChangeGesture();
        dragging = -1;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < count; ++i)
            if (slot (i).expanded (12.0f, 10.0f).contains (e.position))
            {
                auto* p = organ.param (id (i));
                p->setValueNotifyingHost (p->getDefaultValue());
            }
        repaint();
    }

private:
    organ::Processor& organ;
    int dragging = -1;
};

//==============================================================================
TransistorPanel::TransistorPanel (organ::Processor& o, int& t) : organ (o), tab (t)
{
    for (int i = 0; i < 5; ++i)
    {
        auto* c = tabChips.add (new LcdChip (tabNames[i]));
        c->textSize = 11.5f;
        c->onClick = [this, i] { tab = i; build(); };
        addAndMakeVisible (c);
    }
    startTimerHz (15);
}

TransistorPanel::~TransistorPanel() = default;

iw::Bar& TransistorPanel::bar (const juce::String& id, const juce::String& caption, std::function<juce::String (float)> fmt)
{
    auto* b = new iw::Bar (*organ.param (id), caption, std::move (fmt));
    parts.add (b);
    bars.push_back (b);
    addAndMakeVisible (b);
    return *b;
}

iw::Chips& TransistorPanel::chips (const juce::String& caption, juce::StringArray labels, std::function<int()> chosen, std::function<void (int)> pick)
{
    auto* c = new iw::Chips (caption, labels, std::move (chosen), std::move (pick));
    parts.add (c);
    chipRows.push_back (c);
    addAndMakeVisible (c);
    return *c;
}

iw::Chips& TransistorPanel::paramChips (const juce::String& id, const juce::String& caption, juce::StringArray labels)
{
    auto* p = organ.param (id);
    return chips (caption, labels,
                  [p] { return juce::roundToInt (p->convertFrom0to1 (p->getValue())); },
                  [p] (int i) { p->beginChangeGesture(); p->setValueNotifyingHost (p->convertTo0to1 ((float) i)); p->endChangeGesture(); });
}

void TransistorPanel::build()
{
    parts.clear();
    bars.clear();
    chipRows.clear();
    notes.clear();
    soundArea = chartArea = driftArea = traceArea = contactArea = wobbleArea = {};
    builtTab = tab;
    builtSystem = organ.param ("system")->getCurrentValueAsText().hashCode();
    for (int i = 0; i < tabChips.size(); ++i) tabChips[i]->chosen = i == tab;

    auto area = getLocalBounds().withTrimmedTop (chipH + 10);
    const int w = area.getWidth();
    int y = area.getY();
    auto placeBar = [&] (iw::Bar& b) { b.setBounds (area.getX(), y, w, rowH); y += rowH + gapY; };
    auto placeChips = [&] (iw::Chips& c) { const int h = c.layout (w); c.setBounds (area.getX(), y, w, h); y += h + gapY; };

    switch (tab)
    {
        case 0:   // PANEL
        {
            auto* d = new Drawbars (organ);
            parts.add (d);
            addAndMakeVisible (d);
            auto left = area.removeFromLeft (420);
            d->setBounds (left.withTrimmedBottom (14));
            note (left.removeFromBottom (14).withTrimmedLeft (8), utf ("IV = 2\xe2\x85\x94' 2' 1\xe2\x85\x97' 1' \xc2\xb7 ~ FLUTE \xc2\xb7 M REED"));
            auto right = area.withTrimmedLeft (12);
            auto& vib = paramChips ("vibrato", "VIBRATO", { "OFF", "ON" });
            const int h = vib.layout (right.getWidth());
            vib.setBounds (right.getX(), right.getY(), right.getWidth(), h);
            soundArea = right.withTrimmedTop (h + 10);
            break;
        }
        case 1:   // TUNING
        {
            placeChips (paramChips ("system", "SYSTEM", organ::systemChoices()));
            const auto sys = organ.tuningSettings().system;
            if (sys == tuning::System::scala)
                placeChips (chips ("FILE", { utf ("LOAD .SCL\xe2\x80\xa6") }, [] { return -1; }, [this] (int) { loadScale(); }));
            else if (sys != tuning::System::equal && sys != tuning::System::custom)
                placeChips (paramChips ("key", "KEY", { "C", "C#", "D", "EB", "E", "F", "F#", "G", "AB", "A", "BB", "B" }));
            placeBar (bar ("a4", "A4", [] (float v) { return juce::String (v, 1) + " HZ"; }));
            note ({ area.getX(), y, w, 14 }, "TWELVE OSCILLATORS AND EXACT OCTAVES: NO EDO OR OCTAVE STRETCH HERE, AND A SCALA FILE NEEDS 12 NOTES AND A 2/1 OCTAVE.");
            chartArea = area.withTop (y + 18);
            break;
        }
        case 2:   // GENERATORS
        {
            placeBar (bar ("drift", "DRIFT", [] (float v) { return v < 0.05f ? juce::String ("NONE: RIGHT ON") : utf ("\xc2\xb1") + juce::String (v, 1) + " CENTS, WANDERING OVER MINUTES"; }));
            auto* tune = new LcdChip ("TUNE THEM UP");
            tune->textSize = 11.0f;
            tune->onClick = [this] { organ.tuneUp(); if (onMessage) onMessage ("TRANSISTOR: THE 12 OSCILLATORS TUNED UP"); };
            parts.add (tune);
            addAndMakeVisible (tune);
            tune->setBounds (area.getX() + captionW, y, tune->idealWidth() + 10, chipH);
            note ({ area.getX() + captionW + tune->idealWidth() + 20, y, w, chipH }, "A TECHNICIAN RETUNES THE 12 COILS: THE DRIFT STARTS AGAIN FROM ZERO.");
            y += chipH + gapY + 4;
            auto rest = area.withTop (y);
            driftArea = rest.removeFromLeft (rest.getWidth() * 9 / 20).withTrimmedRight (8);
            traceArea = rest;
            break;
        }
        case 3:   // KEYS
        {
            placeBar (bar ("click", "CLICK", [] (float v) { return v > 0.97f ? juce::String ("INSTANT, AS BUILT") : "SOFTENED OVER " + juce::String ((1.0f - v) * 5.0f, 1) + " MS"; }));
            placeBar (bar ("bounce", "BOUNCE", ms));
            placeBar (bar ("spread", "CONTACT SPREAD", [] (float v) { return v < 0.05f ? juce::String ("ALL FOUR TOGETHER") : "UP TO " + juce::String (v, 1) + " MS APART"; }));
            contactArea = area.withTop (y + 2).withTrimmedBottom (16);
            note ({ area.getX(), area.getBottom() - 14, w, 14 }, "EACH CONTACT SWITCHES ITS TONES STRAIGHT ON, MID-CYCLE: THAT'S THE CLICK. NO VELOCITY: AN ORGAN KEY IS ON OR OFF.");
            break;
        }
        case 4: default:   // VIBRATO
        {
            placeBar (bar ("vibHz", "SPEED", [] (float v) { return juce::String (v, 1) + " HZ"; }));
            placeBar (bar ("vibDepth", "DEPTH", [] (float v) { return utf ("\xc2\xb1") + juce::String (v, 1) + " CENTS"; }));
            note ({ area.getX(), y, w, 14 }, "INSIDE THE CASE ON THE REAL ORGAN; THE PANEL SWITCH TURNS IT ON. ONE WOBBLE FOR ALL 12 OSCILLATORS: EVERY NOTE MOVES TOGETHER.");
            wobbleArea = area.withTop (y + 18);
            break;
        }
    }
    repaint();
}

void TransistorPanel::resized()
{
    int x = 0;
    const int w = (getWidth() - 4 * 7) / 5;
    for (auto* c : tabChips) { c->setBounds (x, 0, w, chipH + 2); x += w + 7; }
    build();
}

void TransistorPanel::timerCallback()
{
    if (builtTab != tab || (tab == 1 && organ.param ("system")->getCurrentValueAsText().hashCode() != builtSystem)) { build(); return; }
    for (auto* c : chipRows) c->refresh();
    for (auto* b : bars) b->repaint();
    for (auto* p : parts) if (dynamic_cast<Drawbars*> (p) != nullptr) p->repaint();
    repaint();
}

void TransistorPanel::loadScale()
{
    chooser = std::make_unique<juce::FileChooser> ("A Scala scale (12 notes, 2/1 octave)", juce::File(), "*.scl");
    juce::Component::SafePointer<TransistorPanel> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File()) return;
        const auto textOf = fc.getResult().loadFileAsString().toStdString();
        std::string err;
        tuning::Scale sc;
        if (tuning::parseScala (textOf, sc, err) && safe->organ.setScale (sc, textOf, err))
        {
            auto* p = safe->organ.param ("system");
            p->setValueNotifyingHost (p->convertTo0to1 (8.0f));
        }
        else if (safe->onMessage) safe->onMessage ("SCALA: " + juce::String (err));
        safe->repaint();
    });
}

void TransistorPanel::mouseDown (const juce::MouseEvent& e)
{
    dragPitchClass = -1;
    if (tab != 1 || organ.tuningSettings().system != tuning::System::custom || ! chartArea.contains (e.getPosition())) return;
    const auto chart = chartArea.withTrimmedTop (16).withTrimmedBottom (16);
    dragPitchClass = juce::jlimit (0, 11, (e.x - chart.getX()) * 12 / std::max (1, chart.getWidth()));
    mouseDrag (e);
}

void TransistorPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (dragPitchClass < 0) return;
    const auto chart = chartArea.withTrimmedTop (16).withTrimmedBottom (16);
    const float mid = (float) chart.getCentreY();
    auto* p = organ.param ("cents" + juce::String (dragPitchClass));
    p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (-50.0f, 50.0f, (mid - (float) e.y) / ((float) chart.getHeight() * 0.5f) * 50.0f)));
    repaint();
}

void TransistorPanel::paint (juce::Graphics& g)
{
    const auto s = organ.settings();
    for (auto& [r, t] : notes) text (g, t, r, Pal::lcdDim.withAlpha (0.75f), 9.5f);
    auto frame = [&g] (juce::Rectangle<int> r, const juce::String& title)
    {
        g.setColour (Pal::lcdRule);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 1.0f);
        text (g, title, r.reduced (8, 4).removeFromTop (12), Pal::lcdDim, 9.0f);
        return r.reduced (10, 6).withTrimmedTop (16);
    };

    if (! soundArea.isEmpty())
    {
        // middle C, pitch by pitch: what each contact's tones add (light), and how much of it is the flute tone (amber)
        auto r = frame (soundArea, "WHAT MIDDLE C SOUNDS, BY PITCH");
        const char* labels[] { "16'", "8'", "4'", "2\xe2\x85\x94'", "2'", "1\xe2\x85\x97'", "1'" };
        const double hz[] { 130.8, 261.6, 523.3, 784.0, 1046.5, 1318.5, 2093.0 };
        const double foot[] { organ::drawbarGain (s.drawbar16), organ::drawbarGain (s.drawbar8), organ::drawbarGain (s.drawbar4) };
        const double iv = organ::drawbarGain (s.drawbarIV) * 0.5;
        const double gR = organ::drawbarGain (s.reed), gF = organ::drawbarGain (s.flute) * organ::fluteMakeup;
        auto names = r.removeFromBottom (14);
        const float cw = (float) r.getWidth() / 7.0f;
        for (int i = 0; i < 7; ++i)
        {
            const double level = i < 3 ? foot[i] : iv;
            const double lp = 1.0 / (1.0 + std::pow (hz[i] / organ::fluteCornerHz, 2.0));   // |H|^2 of one pole = both poles' |H|   // the flute filter's two poles, on the fundamental
            const double reedPart = level * gR, flutePart = level * gF * lp;
            auto toH = [&r] (double a) { return a <= 1.0e-6 ? 0.0f : (float) juce::jlimit (0.0, 1.0, (20.0 * std::log10 (a) + 36.0) / 36.0) * (float) r.getHeight(); };
            const float x = (float) r.getX() + cw * (float) i + 6.0f;
            const float hTotal = toH (reedPart + flutePart), hFlute = toH (flutePart) * (reedPart + flutePart > 0.0 ? (float) (flutePart / (reedPart + flutePart)) : 0.0f);
            g.setColour (Pal::lcdText);
            g.fillRect (juce::Rectangle<float> (x, (float) r.getBottom() - hTotal, cw * 0.55f, hTotal));
            g.setColour (Pal::lamp);
            g.fillRect (juce::Rectangle<float> (x, (float) r.getBottom() - hFlute, cw * 0.55f, hFlute));
            text (g, utf (labels[i]), { (int) (x - 6.0f), names.getY(), (int) cw, 14 }, Pal::lcdDim, 9.0f, juce::Justification::centred);
        }
    }

    if (! chartArea.isEmpty())
    {
        auto chart = chartArea;
        const auto ts = organ.tuningSettings();
        if (ts.system == tuning::System::scala && organ.scaleDescription().isEmpty())
            text (g, "NO SCALE LOADED: LOAD A .SCL FILE (12 NOTES, A 2/1 OCTAVE)", chart.removeFromTop (16), Pal::lcdDim, 9.5f);
        else
            text (g, ts.system == tuning::System::custom ? "THE 12 OSCILLATORS: CENTS FROM EQUAL: DRAG A NOTE'S BAR UP OR DOWN"
                                                         : "THE 12 OSCILLATORS: CENTS FROM EQUAL (A KEPT AT THE REFERENCE)", chart.removeFromTop (16), Pal::lcdDim, 9.5f);
        const auto d = tuning::centsFromEqual (ts);
        double range = ts.system == tuning::System::custom ? 50.0 : 15.0;
        if (ts.system != tuning::System::custom) for (auto c : d) range = std::max (range, std::abs (c) * 1.15);
        iw::paintCentsChart (g, chart, d, range);
    }

    if (! driftArea.isEmpty())
    {
        auto r = frame (driftArea, "THE 12 OSCILLATORS NOW: CENTS FROM THEIR TUNING");
        const auto drift = organ.driftNow();
        double range = std::max (4.0, (double) s.driftCents * 2.5);
        iw::paintCentsChart (g, r, drift, range);
    }

    if (! traceArea.isEmpty())
    {
        auto r = frame (traceArea, utf ("MIDDLE C ON ITS WAY: DIVIDER \xe2\x80\xba KEY FILTER \xe2\x80\xba FLUTE FILTER"));
        auto foot = r.removeFromBottom (12);
        text (g, "12 LC OSCILLATORS, EACH HALVED SIX TIMES: EVERY C COMES FROM ONE COIL", foot, Pal::lcdDim.withAlpha (0.7f), 8.5f);
        // the same filters the organ uses, run on a square wave
        const double f = 261.63, rate = f * 200.0;
        std::vector<double> sq, reed, flute;
        double y1 = 0, f1 = 0, f2 = 0;
        const double aR = 1.0 - std::exp (-2.0 * 3.14159265358979 * organ::reedCornerRatio * f / rate);
        const double aF = 1.0 - std::exp (-2.0 * 3.14159265358979 * organ::fluteCornerHz / rate);
        for (int i = 0; i < 200 * 8; ++i)
        {
            const double x = (i % 200) < 100 ? 1.0 : -1.0;
            y1 += aR * (x - y1); f1 += aF * (y1 - f1); f2 += aF * (f1 - f2);
            if (i >= 200 * 5) { sq.push_back (x); reed.push_back (y1); flute.push_back (f2); }
        }
        const int bw = r.getWidth() / 3;
        auto draw = [&] (const std::vector<double>& v, int idx, const juce::String& label, juce::Colour c)
        {
            auto box = juce::Rectangle<int> (r.getX() + idx * bw, r.getY(), bw - 10, r.getHeight());
            auto plot = box.withTrimmedBottom (14).toFloat().reduced (0.0f, 4.0f);
            double peak = 1.0e-9;
            for (auto x : v) peak = std::max (peak, std::abs (x));
            juce::Path p;
            for (size_t i = 0; i < v.size(); ++i)
            {
                const float xx = plot.getX() + plot.getWidth() * (float) i / (float) v.size();
                const float yy = plot.getCentreY() - (float) (v[i] / peak) * plot.getHeight() * 0.45f;
                if (i == 0) p.startNewSubPath (xx, yy); else p.lineTo (xx, yy);
            }
            g.setColour (c);
            g.strokePath (p, juce::PathStrokeType (1.6f));
            text (g, label, box.removeFromBottom (14), Pal::lcdDim, 8.5f);
        };
        draw (sq, 0, "SQUARE (THE DIVIDER)", Pal::lcdText);
        draw (reed, 1, "REED (ITS RC FILTER)", Pal::lcdText);
        draw (flute, 2, "FLUTE (TWO MORE)", Pal::lamp);
    }

    if (! contactArea.isEmpty())
    {
        auto r = frame (contactArea, "MIDDLE C GOING DOWN, THEN UP: ITS FOUR CONTACTS");
        const char* names[] { "16'", "8'", "4'", "IV" };
        const double span = std::max (0.004, (s.spreadMs + s.bounceMs) * 0.001 * 1.4 + 0.001);
        const int rowsH = r.getHeight() / 4;
        auto plotArea = r.withTrimmedLeft (40);
        const int half = plotArea.getWidth() / 2;
        for (int c = 0; c < 4; ++c)
        {
            auto row = juce::Rectangle<int> (r.getX(), r.getY() + c * rowsH, r.getWidth(), rowsH);
            text (g, utf (names[c]), row.removeFromLeft (40), Pal::lcdDim, 10.0f);
            juce::Path p;
            const float base = (float) row.getBottom() - 4.0f, top = (float) row.getY() + 4.0f;
            for (int i = 0; i < plotArea.getWidth(); ++i)
            {
                const bool down = i < half;
                const double t = down ? span * i / half : span + span * (i - half) / half;
                const double u = down ? -1.0 : span * (i - half) / half;
                const bool closed = organ::contactClosed (s, organ.seed(), 60, c, t, u);
                const float yy = closed ? top : base;
                if (i == 0) p.startNewSubPath ((float) row.getX(), yy); else p.lineTo ((float) (row.getX() + i), yy);
            }
            g.setColour (Pal::lcdText);
            g.strokePath (p, juce::PathStrokeType (1.4f));
        }
        g.setColour (Pal::lcdRule);
        g.drawVerticalLine (plotArea.getX() + half, (float) r.getY(), (float) r.getBottom());
        text (g, "KEY UP", { plotArea.getX() + half + 4, r.getY() - 14, 80, 12 }, Pal::lcdDim, 8.5f);
        text (g, juce::String (span * 1000.0, 1) + " MS EACH SIDE", { plotArea.getRight() - 120, r.getY() - 14, 120, 12 }, Pal::lcdDim, 8.5f, juce::Justification::centredRight);
    }

    if (! wobbleArea.isEmpty())
    {
        auto r = frame (wobbleArea, "PITCH OVER ONE SECOND: A SQUARE WAVE ROUNDED BY AN RC FILTER");
        g.setColour (Pal::lcdRule);
        g.drawHorizontalLine (r.getCentreY(), (float) r.getX(), (float) r.getRight());
        juce::Path p;
        for (int i = 0; i < r.getWidth(); ++i)
        {
            const double t = (double) i / r.getWidth();
            const float yy = (float) r.getCentreY() - (float) (organ::vibratoShape (t, s.vibratoHz) * (s.vibratoCents / 60.0)) * (float) r.getHeight() * 0.48f;
            if (i == 0) p.startNewSubPath ((float) r.getX(), yy); else p.lineTo ((float) (r.getX() + i), yy);
        }
        g.setColour (s.vibrato ? Pal::lamp : Pal::lcdDim);
        g.strokePath (p, juce::PathStrokeType (1.8f));
        text (g, utf ("\xc2\xb1") + "60 CENTS FULL SCALE" + (s.vibrato ? juce::String() : utf ("  \xc2\xb7  OFF ON THE PANEL")), r.removeFromBottom (12), Pal::lcdDim, 8.5f, juce::Justification::centredRight);
    }
}
} // namespace tf
