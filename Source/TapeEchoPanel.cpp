#include "TapeEchoPanel.h"

namespace tf
{
namespace
{
    using namespace echo;
    const juce::Colour cell { 0xff0a221f };
    const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));

    void section (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, const juce::String& sub)
    {
        g.setColour (cell);
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        iw::text (g, title, r.reduced (10, 0).withHeight (24), Pal::lcdText, 12.0f);
        if (sub.isNotEmpty()) iw::text (g, sub, r.reduced (10, 0).withHeight (24), Pal::lcdDim, 9.0f, juce::Justification::centredRight);
    }

    juce::String msText (double s)
    {
        return s < 1.0 ? juce::String (juce::roundToInt (s * 1000.0)) + " MS" : juce::String (s, 2) + " S";
    }

    /** A place on the tape as note values of the tempo ("1/8", "3/16", "2 BARS"), or empty. */
    juce::String noteText (double seconds, double bpm)
    {
        if (bpm <= 0.0) return {};
        const double quarters = seconds * bpm / 60.0;
        const int n32 = juce::roundToInt (quarters * 8.0);   // in 32nds
        if (n32 <= 0 || std::abs (quarters * 8.0 - n32) > 0.02) return {};
        if (n32 % 32 == 0) return juce::String (n32 / 32) + (n32 == 32 ? " BAR" : " BARS");
        int num = n32, den = 32;
        while (num % 2 == 0 && den > 1) { num /= 2; den /= 2; }
        return juce::String (num) + "/" + juce::String (den);
    }
}

//==============================================================================
/** A knob on a parameter: drag up or down, the wheel for fine steps, double-click its default. */
class TapeEchoPanel::Knob : public juce::Component,
                            public juce::SettableTooltipClient
{
public:
    Knob (juce::RangedAudioParameter& p, juce::String l, std::function<juce::String (float)> f, int r)
        : param (p), label (std::move (l)), fmt (std::move (f)), radius (r) {}

    void paint (juce::Graphics& g) override
    {
        const float v = param.getValue();
        const auto c = juce::Point<float> ((float) getWidth() * 0.5f, (float) radius + 4.0f);
        constexpr float a0 = juce::MathConstants<float>::pi * 0.75f, span = juce::MathConstants<float>::pi * 1.5f;
        const float h = juce::MathConstants<float>::halfPi, am = a0 + span * v;
        juce::Path arc;
        arc.addCentredArc (c.x, c.y, (float) radius, (float) radius, 0.0f, a0 + h, a0 + span + h, true);
        g.setColour (Pal::lcdRule);
        g.strokePath (arc, juce::PathStrokeType (3.0f));
        if (v > 0.0f)
        {
            juce::Path on;
            on.addCentredArc (c.x, c.y, (float) radius, (float) radius, 0.0f, a0 + h, am + h, true);
            g.setColour (Pal::lcdText);
            g.strokePath (on, juce::PathStrokeType (3.0f));
        }
        g.setColour (Pal::lcdText);
        g.drawLine (c.x + std::cos (am) * ((float) radius - 9.0f), c.y + std::sin (am) * ((float) radius - 9.0f),
                    c.x + std::cos (am) * ((float) radius - 2.0f), c.y + std::sin (am) * ((float) radius - 2.0f), 2.0f);
        const int y = 2 * radius + 8;
        iw::text (g, label, { 0, y, getWidth(), 13 }, Pal::lcdDim, 9.0f, juce::Justification::centred);
        iw::text (g, fmt (param.convertFrom0to1 (v)), { 0, y + 12, getWidth(), 14 }, Pal::lcdText, 10.0f, juce::Justification::centred);
    }
    void mouseDown (const juce::MouseEvent&) override { from = param.getValue(); param.beginChangeGesture(); }
    void mouseUp (const juce::MouseEvent&) override { param.endChangeGesture(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        param.setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, from - (float) e.getDistanceFromDragStartY() / 200.0f));
        changed();
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        param.setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, param.getValue() + (w.deltaY > 0 ? 0.005f : -0.005f)));
        changed();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { param.setValueNotifyingHost (param.getDefaultValue()); changed(); }
    std::function<void()> onChange;

private:
    void changed() { repaint(); if (onChange) onChange(); }
    juce::RangedAudioParameter& param;
    juce::String label;
    std::function<juce::String (float)> fmt;
    int radius;
    float from = 0.0f;
};

//==============================================================================
/** A key that repeats while held, ever faster (as the SU10's -/+). */
class TapeEchoPanel::Nudge : public LcdChip,
                             private juce::Timer
{
public:
    explicit Nudge (const juce::String& l) : LcdChip (l) { textSize = 12.0f; }
    std::function<void()> onStep;
    void mouseDown (const juce::MouseEvent& e) override { LcdChip::mouseDown (e); count = 0; if (onStep) onStep(); startTimer (400); }
    void mouseUp (const juce::MouseEvent& e) override { stopTimer(); LcdChip::mouseUp (e); }

private:
    void timerCallback() override
    {
        if (! isMouseButtonDown()) { stopTimer(); return; }
        ++count;
        if (onStep) onStep();
        startTimer (std::max (25, (int) (220.0 * std::pow (0.88, count))));
    }
    int count = 0;
};

//==============================================================================
/** The tape path: the whole loop above, a strip of it below to zoom along and drag the heads on. */
class TapeEchoPanel::TapePath : public juce::Component
{
public:
    explicit TapePath (Processor& p) : proc (p) {}
    bool snap = true;
    std::function<void()> onChange;
    std::function<void (const juce::String&)> onMessage;

    static constexpr int overviewY = 16, overviewH = 12, stripY = 66, margin = 22;

    double viewFrom = 0.0, viewTo = 1.2;   // the strip, in seconds of tape at 1 7/8 (nominal)

    juce::Rectangle<int> overviewBar() const { return { 0, overviewY, getWidth(), overviewH }; }
    int stripLeft() const  { return margin; }
    int stripRight() const { return getWidth() - margin; }
    float xOf (double nominal) const { return (float) stripLeft() + (float) (stripRight() - stripLeft()) * (float) ((nominal - viewFrom) / (viewTo - viewFrom)); }
    double nominalAt (float x) const { return viewFrom + (viewTo - viewFrom) * (double) (x - (float) stripLeft()) / (double) (stripRight() - stripLeft()); }
    float overviewX (double nominal) const { return (float) getWidth() * (float) (nominal / proc.loopNominal()); }
    juce::Rectangle<float> headBox (int h) const { return juce::Rectangle<float> (22.0f, 20.0f).withCentre ({ xOf (proc.headPos (h)), (float) stripY + 16.0f }); }
    juce::Rectangle<float> timeBox (int h) const
    {
        // Each head's time under it; heads close together stack theirs a row lower, so none overlap.
        int order[numHeads] { 0, 1, 2 };
        std::sort (order, order + numHeads, [this] (int a, int b) { return proc.headPos (a) < proc.headPos (b); });
        float rowEnd[3] { -1e9f, -1e9f, -1e9f };
        int row = 0;
        for (int k : order)
        {
            const float x = xOf (proc.headPos (k));
            int r = 0;
            while (r < 2 && x - 34.0f < rowEnd[r] + 4.0f) ++r;
            rowEnd[r] = x + 34.0f;
            if (k == h) row = r;
        }
        const auto b = headBox (h);
        return { b.getCentreX() - 34.0f, b.getBottom() + 6.0f + (float) row * 25.0f, 68.0f, 24.0f };
    }

    /** The grid SNAP uses: a note value of the tempo that's at least 8 pixels wide here (nominal seconds). */
    double gridStep() const
    {
        const double bpm = proc.bpm();
        if (bpm <= 0.0) return 0.0;
        const double quarterNominal = proc.toNominal (60.0 / bpm);
        for (double q : { 0.125, 0.25, 0.5, 1.0, 4.0, 16.0, 64.0 })
            if (std::abs (xOf (q * quarterNominal) - xOf (0.0)) >= 8.0f) return q * quarterNominal;
        return 64.0 * quarterNominal;
    }

    void paint (juce::Graphics& g) override
    {
        const double len = proc.loopNominal();
        // ---- the whole loop ----
        iw::text (g, "THE WHOLE LOOP", { 0, 0, 200, 12 }, Pal::lcdDim, 8.5f);
        auto bar = overviewBar().toFloat();
        g.setColour (Pal::lcd);
        g.fillRect (bar);
        g.setColour (Pal::lcdRule);
        g.drawRect (bar, 1.0f);
        const float ex = overviewX (len - eraseGapSeconds);
        if (proc.get ("erase") > 0.5f)
            for (float x = ex; x < bar.getRight() - 1.0f; x += 3.0f) g.drawLine (x, bar.getY() + 1.0f, x + 1.5f, bar.getBottom() - 1.0f, 1.0f);
        g.setColour (Pal::lcdText);
        g.fillRect (bar.getX(), bar.getY() - 3.0f, 2.0f, bar.getHeight() + 6.0f);
        g.fillRect (ex, bar.getY() - 3.0f, 1.0f, bar.getHeight() + 6.0f);
        g.drawRect (juce::Rectangle<float> (overviewX (viewFrom), bar.getY() - 1.0f, std::max (3.0f, overviewX (viewTo) - overviewX (viewFrom)), bar.getHeight() + 2.0f), 1.0f);
        for (int h = 0; h < numHeads; ++h)
            if (proc.headOn (h)) g.fillRect (overviewX (proc.headPos (h)) - 1.0f, bar.getY() + 2.0f, 2.0f, bar.getHeight() - 4.0f);
        g.setColour (Pal::lcdDim);
        const float sx = overviewX (0.0) + 0.5f;   // the splice: where the loop's ends were joined (the tape's start)
        juce::ignoreUnused (sx);
        const int ly = overviewY + overviewH + 3;
        iw::text (g, "R", { 1, ly, 20, 12 }, Pal::lcdDim, 8.0f);
        iw::text (g, "ERASED E", { (int) ex - 100, ly, 104, 12 }, Pal::lcdDim, 8.0f, juce::Justification::centredRight);
        // heads beyond the strip, named on the overview
        for (int h = 0; h < numHeads; ++h)
        {
            const double at = proc.headPos (h);
            if (at < viewFrom || at > viewTo)
                iw::text (g, juce::String (h + 1) + ": " + msText (proc.toSeconds (at)), { (int) overviewX (at) - 20, ly, 90, 12 }, proc.headOn (h) ? Pal::lcdText : Pal::lcdDim, 8.0f);
        }
        iw::text (g, "VIEW " + juce::String (proc.toSeconds (viewFrom), 2) + "-" + juce::String (proc.toSeconds (viewTo), 2) + " S",
                  { getWidth() - 200, 0, 200, 12 }, Pal::lcdText, 8.5f, juce::Justification::centredRight);

        // ---- the strip ----
        const float y = (float) stripY;
        g.setColour (Pal::lcdDim);
        g.drawLine ((float) stripLeft(), y, (float) stripRight(), y, 2.0f);
        for (float ax : { 0.2f, 0.55f, 0.9f })
        {
            const float x = (float) stripLeft() + ax * (float) (stripRight() - stripLeft());
            juce::Path p;
            p.addTriangle (x, y - 4.0f, x, y + 4.0f, x + 8.0f, y);
            g.fillPath (p);
        }
        // the erased stretch, if it's in view
        if (proc.get ("erase") > 0.5f && viewTo > len - eraseGapSeconds)
        {
            const float x0 = std::max ((float) stripLeft(), xOf (len - eraseGapSeconds)), x1 = std::min ((float) stripRight(), xOf (len));
            g.setColour (Pal::lcdRule);
            for (float x = x0; x < x1; x += 4.0f) g.drawLine (x, y - 6.0f, x + 3.0f, y + 6.0f, 1.0f);
            iw::text (g, "ERASED", { (int) x0, (int) y - 22, 80, 12 }, Pal::lcdDim, 8.0f);
        }
        // the grid SNAP uses, labelled in note values
        if (const double step = gridStep(); step > 0.0)
        {
            const auto first = (long long) std::ceil (viewFrom / step);
            for (auto k = first; (double) k * step <= viewTo && k - first < 400; ++k)
            {
                const double at = (double) k * step;
                const float x = xOf (at);
                const auto label = noteText (proc.toSeconds (at), proc.bpm());
                const bool big = k % 2 == 0;
                g.setColour (big ? Pal::lcdDim : Pal::lcdRule);
                g.fillRect (x, y - (big ? 8.0f : 4.0f), 1.0f, big ? 8.0f : 4.0f);
                if (big && k > 0 && label.isNotEmpty() && k % 4 == 0)
                    iw::text (g, label, { (int) x - 30, (int) y - 22, 60, 12 }, Pal::lcdDim, 8.0f, juce::Justification::centred);
            }
        }
        // the record head, and the playback heads
        auto drawHead = [&] (juce::Rectangle<float> b, const juce::String& label, bool on, bool impossible)
        {
            if (impossible)
            {
                if (on) { g.setColour (Pal::lcdDim); g.fillRoundedRectangle (b, 2.0f); }
                g.setColour (Pal::lcdText);
                const float dash[] { 2.0f, 2.0f };
                juce::Path p;
                p.addRectangle (b);
                juce::Path dashed;
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, p, dash, 2);
                g.fillPath (dashed);
            }
            else
            {
                g.setColour (Pal::lcdText);
                if (on) g.fillRoundedRectangle (b, 2.0f);
                else g.drawRoundedRectangle (b, 2.0f, 1.0f);
            }
            iw::text (g, label, b.toNearestInt(), on ? Pal::lcd : Pal::lcdText, 10.0f, juce::Justification::centred);
        };
        if (viewFrom <= 0.0)
        {
            const auto b = juce::Rectangle<float> (22.0f, 20.0f).withCentre ({ xOf (0.0), y + 16.0f });
            drawHead (b, "R", true, false);
            iw::text (g, "REC", { (int) b.getX() - 10, (int) b.getBottom() + 6, (int) b.getWidth() + 20, 12 }, Pal::lcdDim, 8.5f, juce::Justification::centred);
        }
        for (int h = 0; h < numHeads; ++h)
        {
            const double at = proc.headPos (h);
            const bool on = proc.headOn (h), close = proc.tooClose (h);
            if (at < viewFrom || at > viewTo)
            {
                // off the strip: an arrow at its edge
                const bool right = at > viewTo;
                iw::text (g, right ? juce::String (h + 1) + " > " + msText (proc.toSeconds (at)) : "< " + juce::String (h + 1),
                          { right ? stripRight() - 120 : 0, (int) y + 8, 120, 16 }, on ? Pal::lcdText : Pal::lcdDim, 9.0f,
                          right ? juce::Justification::centredRight : juce::Justification::centredLeft);
                continue;
            }
            const auto b = headBox (h);
            drawHead (b, juce::String (h + 1), on, close);
            const auto t = timeBox (h);
            if (t.getY() > b.getBottom() + 10.0f)
            {
                g.setColour (Pal::lcdDim);
                g.drawLine (b.getCentreX(), b.getBottom(), b.getCentreX(), t.getY(), 1.0f);
            }
            iw::text (g, msText (proc.toSeconds (at)), t.withHeight (12.0f).toNearestInt(), on ? Pal::lcdText : Pal::lcdDim, 9.0f, juce::Justification::centred);
            iw::text (g, noteText (proc.toSeconds (at), proc.bpm()), t.withTrimmedTop (12.0f).toNearestInt(), Pal::lcdDim, 8.0f, juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = -1;
        scrolling = false;
        if (overviewBar().expanded (0, 4).contains (e.getPosition()))
        {
            // a place on the whole loop: the strip goes there
            const double w = viewTo - viewFrom, at = proc.loopNominal() * (double) e.x / getWidth();
            setView (at - w * 0.5, at + w * 0.5);
            return;
        }
        for (int h = numHeads - 1; h >= 0; --h)
            if (proc.headPos (h) >= viewFrom && proc.headPos (h) <= viewTo && headBox (h).expanded (3.0f).contains (e.position))
            {
                dragging = h;
                grabOffset = e.position.x - headBox (h).getCentreX();
                return;
            }
        scrolling = true;
        scrollFrom = viewFrom;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging >= 0)
        {
            double at = nominalAt (e.position.x - grabOffset);
            // SNAP: heads 2 and 3 to note values of the tempo (head 1 sets the tempo, so it moves freely)
            if (snap && dragging > 0)
                if (const double step = gridStep(); step > 0.0) at = std::round (at / step) * step;
            proc.setHeadPos (dragging, at);
            repaint();
            if (onChange) onChange();
        }
        else if (scrolling)
        {
            const double w = viewTo - viewFrom;
            const double from = scrollFrom - (double) e.getDistanceFromDragStartX() / (double) (stripRight() - stripLeft()) * w;
            setView (from, from + w);
        }
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = -1; scrolling = false; }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        // zoom around the pointer
        const double at = nominalAt (e.position.x), k = w.deltaY > 0 ? 1.0 / 1.25 : 1.25;
        setView (at - (at - viewFrom) * k, at + (viewTo - at) * k);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        for (int h = 0; h < numHeads; ++h)
        {
            if (proc.headPos (h) < viewFrom || proc.headPos (h) > viewTo) continue;
            if (headBox (h).expanded (3.0f).contains (e.position))
            {
                proc.setHeadOn (h, ! proc.headOn (h));
                repaint();
                if (onChange) onChange();
                return;
            }
            if (timeBox (h).contains (e.position)) { typeTime (h); return; }
        }
    }

    void setView (double from, double to)
    {
        const double len = proc.loopNominal();
        double w = juce::jlimit (0.02, len, to - from);
        from = juce::jlimit (0.0, len - w, from);
        viewFrom = from;
        viewTo = from + w;
        repaint();
    }

private:
    void typeTime (int h)
    {
        editor = std::make_unique<juce::TextEditor>();
        editor->setFont (DeckLookAndFeel::get().mono (11.0f));
        editor->setColour (juce::TextEditor::backgroundColourId, Pal::lcd);
        editor->setColour (juce::TextEditor::textColourId, Pal::lcdText);
        editor->setColour (juce::TextEditor::outlineColourId, Pal::lcdDim);
        editor->setInputRestrictions (8, "0123456789.");
        editor->setJustification (juce::Justification::centred);
        editor->setText (juce::String (juce::roundToInt (proc.toSeconds (proc.headPos (h)) * 1000.0)), false);
        editor->setBounds (timeBox (h).withHeight (16.0f).toNearestInt());
        addAndMakeVisible (*editor);
        editor->selectAll();
        editor->grabKeyboardFocus();
        auto done = [this, h] (bool apply)
        {
            if (editor == nullptr) return;
            if (apply)
            {
                const double ms = editor->getText().getDoubleValue();
                if (ms > 0.0)
                {
                    proc.setHeadPos (h, proc.toNominal (ms * 0.001));
                    if (onMessage) onMessage ("HEAD " + juce::String (h + 1) + ": " + msText (proc.toSeconds (proc.headPos (h))));
                }
            }
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<TapePath> (this)] { if (safe != nullptr) { safe->editor.reset(); safe->repaint(); if (safe->onChange) safe->onChange(); } });
        };
        editor->onReturnKey = [done] { done (true); };
        editor->onEscapeKey = [done] { done (false); };
        editor->onFocusLost = [done] { done (true); };
    }

    Processor& proc;
    int dragging = -1;
    float grabOffset = 0.0f;
    bool scrolling = false;
    double scrollFrom = 0.0;
    std::unique_ptr<juce::TextEditor> editor;
};

//==============================================================================
TapeEchoPanel::TapeEchoPanel (Processor& p) : proc (p)
{
    path = std::make_unique<TapePath> (proc);
    path->onChange = [this] { repaint(); };
    path->onMessage = [this] (const juce::String& m) { if (onMessage) onMessage (m); };
    path->setView (0.0, proc.toNominal (1.2));
    addAndMakeVisible (*path);

    holdChip = &chip ("HOLD", [this] { return proc.get ("hold") > 0.5f; }, [this]
    {
        const bool now = proc.get ("hold") < 0.5f;
        proc.set ("hold", now ? 1.0f : 0.0f);
        if (onMessage) onMessage (now ? "HOLD: THE LOOP KEEPS WHAT'S ON IT" : "RECORDING ONTO THE LOOP AGAIN");
    }, 11.0f);
    chip ("SNAP", [this] { return path->snap; }, [this] { path->snap = ! path->snap; });

    auto pct = [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; };
    auto db = [] (float v) { return std::abs (v) < 0.05f ? juce::String ("0.0 DB") : (v > 0.0f ? "+" : "") + juce::String (v, 1) + " DB"; };
    knob ("rate", "RATE", [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "% " + juce::String (1.875 * v, 2) + " IPS"; }, 26);
    knob ("intensity", "INTENSITY", pct);
    knob ("bass", "BASS", db);
    knob ("treble", "TREBLE", db);
    knob ("spread", "SPREAD", pct);
    knob ("age", "AGE", [] (float v) { return v < 0.15f ? juce::String ("NEW") : v < 0.45f ? juce::String ("USED") : v < 0.75f ? juce::String ("WORN") : juce::String ("TIRED"); });
    knob ("wow", "WOW+FLUT", [] (float v) { return juce::String (v * 0.6f + 0.05f, 2) + "%"; });
    knob ("drive", "DRIVE", [] (float v) { return std::abs (v) < 0.05f ? juce::String ("0.0 VU") : (v > 0.0f ? "+" : "") + juce::String (v, 1) + " VU"; });

    for (int k = 0; k < 2; ++k)
    {
        auto* b = nudges.add (new Nudge (k == 0 ? "-" : "+"));
        b->onStep = [this, k]
        {
            const double now = std::round (proc.bpm() * 100.0) / 100.0;
            proc.setBpm (now + (k == 0 ? -0.01 : 0.01));
            refresh();
        };
        addAndMakeVisible (b);
    }
    for (int v = 0; v < numNoteValues; ++v)
        chip (noteValues[v].label, [this, v] { return proc.noteValue() == v; }, [this, v] { proc.setNoteValue (v); }, 8.5f);
    for (int k = 0; k < numPlacements; ++k)
        chip (placements[k].label, [] { return false; }, [this, k]
        {
            proc.place (k);
            if (onMessage) onMessage (juce::String ("HEADS PLACED ") + placements[k].label);
        }, 8.5f);
    const char* tapes[] { "NORMAL", "CHROME", "METAL" };
    for (int t = 0; t < 3; ++t)
        chip (tapes[t], [this, t] { return juce::roundToInt (proc.get ("tape")) == t; }, [this, t] { proc.set ("tape", (float) t); }, 8.5f);
    for (int k = 0; k < numLoops; ++k)
        chip (juce::String ((int) loopSeconds[k]) + " S", [this, k] { return juce::roundToInt (proc.get ("loop")) == k; }, [this, k]
        {
            proc.set ("loop", (float) k);
            path->setView (path->viewFrom, path->viewTo);
        }, 9.0f);
    chip ("ERASE", [this] { return proc.get ("erase") > 0.5f; }, [this] { proc.set ("erase", proc.get ("erase") > 0.5f ? 0.0f : 1.0f); }, 9.0f);
    chip ("SPLICE TICK", [this] { return proc.get ("splice") > 0.5f; }, [this] { proc.set ("splice", proc.get ("splice") > 0.5f ? 0.0f : 1.0f); }, 9.0f);

    refresh();
    startTimerHz (30);
}

TapeEchoPanel::~TapeEchoPanel() = default;

LcdChip& TapeEchoPanel::chip (const juce::String& label, std::function<bool()> lit, std::function<void()> click, float size)
{
    auto* c = chips.add (new LcdChip (label));
    c->textSize = size;
    c->onClick = [this, act = std::move (click)] { act(); refresh(); };
    chipLit.push_back (std::move (lit));
    addAndMakeVisible (c);
    return *c;
}

TapeEchoPanel::Knob& TapeEchoPanel::knob (const juce::String& id, const juce::String& label, std::function<juce::String (float)> fmt, int radius)
{
    auto* k = knobs.add (new Knob (*proc.param (id), label, std::move (fmt), radius));
    k->onChange = [this] { repaint(); path->repaint(); };
    addAndMakeVisible (k);
    return *k;
}

void TapeEchoPanel::resized()
{
    pathArea = { 0, 24, 520, 190 };
    rateArea = { 530, 24, 302, 190 };
    echoArea = { 0, 224, 410, 150 };
    tapeArea = { 420, 224, 412, 150 };
    loopArea = { 0, 384, 832, 84 };
    nowArea = { 0, 478, 832, 62 };

    holdChip->setBounds (panelWidth - 100, 0, 100, 20);
    path->setBounds (pathArea.getX() + 10, pathArea.getY() + 22, pathArea.getWidth() - 20, pathArea.getHeight() - 42);
    chips[1]->setBounds (pathArea.getRight() - 64, pathArea.getBottom() - 22, 54, 18);   // SNAP

    auto place = [] (Knob* k, int cx, int cy, int r) { k->setBounds (cx - 45, cy - r - 4, 90, 2 * r + 36); };
    place (knobs[0], rateArea.getX() + 52, rateArea.getY() + 58, 26);
    for (int i = 0; i < 4; ++i) place (knobs[1 + i], echoArea.getX() + 46 + i * 100, echoArea.getY() + 52, 17);
    for (int i = 0; i < 3; ++i) place (knobs[5 + i], tapeArea.getX() + 44 + i * 90, tapeArea.getY() + 72, 17);

    nudges[0]->setBounds (rateArea.getX() + 118, rateArea.getY() + 78, 34, 22);
    nudges[1]->setBounds (rateArea.getX() + 156, rateArea.getY() + 78, 34, 22);
    int k = 2;
    for (int v = 0; v < numNoteValues; ++v, ++k) chips[k]->setBounds (rateArea.getX() + 10 + v * 41, rateArea.getY() + 134, 38, 20);
    int x = echoArea.getX() + 60;
    for (int i = 0; i < numPlacements; ++i, ++k) { chips[k]->setBounds (x, echoArea.getY() + 112, chips[k]->idealWidth(), 20); x = chips[k]->getRight() + 5; }
    x = tapeArea.getX() + 10;
    for (int t = 0; t < 3; ++t, ++k) { chips[k]->setBounds (x, tapeArea.getY() + 22, chips[k]->idealWidth(), 20); x = chips[k]->getRight() + 5; }
    x = loopArea.getX() + 60;
    for (int i = 0; i < numLoops; ++i, ++k) { chips[k]->setBounds (x, loopArea.getY() + 22, chips[k]->idealWidth(), 20); x = chips[k]->getRight() + 5; }
    chips[k]->setBounds (loopArea.getX() + 316, loopArea.getY() + 22, chips[k]->idealWidth(), 20); ++k;
    chips[k]->setBounds (loopArea.getX() + 384, loopArea.getY() + 22, chips[k]->idealWidth(), 20);
    vuArea = { tapeArea.getRight() - 102, tapeArea.getY() + 32, 92, 72 };
}

void TapeEchoPanel::paint (juce::Graphics& g)
{
    iw::text (g, "AN ENDLESS CASSETTE AT 1 7/8 IPS, ONE RECORD HEAD, THREE PLAYBACK HEADS" + dot + "THE BLOCK'S MIX SETS WET AND DRY",
              { 0, 0, panelWidth - 110, 20 }, Pal::lcdDim, 9.0f);

    section (g, pathArea, "TAPE PATH", "LOOP " + juce::String (proc.toSeconds (proc.loopNominal()), 2) + " S AT THIS SPEED");
    iw::text (g, "WHEEL: ZOOM" + dot + "DRAG: SCROLL, OR MOVE A HEAD" + dot + "DOUBLE-CLICK A HEAD: ON/OFF, ITS TIME: TYPE IT" + dot + "DASHED: TOO CLOSE TO BUILD",
              { pathArea.getX() + 10, pathArea.getBottom() - 20, pathArea.getWidth() - 84, 16 }, Pal::lcdDim, 7.5f);

    section (g, rateArea, "RATE", "THE MOTOR");
    iw::text (g, "BPM", { rateArea.getX() + 118, rateArea.getY() + 24, 60, 14 }, Pal::lcdDim, 9.0f);
    iw::text (g, juce::String (proc.bpm(), 2), { rateArea.getX() + 110, rateArea.getY() + 30, rateArea.getWidth() - 120, 44 }, Pal::lcdText, 34.0f, juce::Justification::centredRight);
    iw::text (g, "0.01 BPM A PRESS", { rateArea.getX() + 196, rateArea.getY() + 78, 100, 22 }, Pal::lcdDim, 8.0f);
    iw::text (g, "HEAD 1 IS", { rateArea.getX() + 10, rateArea.getY() + 116, 100, 14 }, Pal::lcdDim, 9.0f);
    iw::text (g, "TURNING RATE BENDS THE REPEATS" + dot + "HOLD -/+ TO REPEAT, FASTER", { rateArea.getX() + 10, rateArea.getY() + 162, rateArea.getWidth() - 20, 14 }, Pal::lcdDim, 7.5f);

    section (g, echoArea, "ECHO", juce::String());
    iw::text (g, "PLACE", { echoArea.getX() + 10, echoArea.getY() + 112, 50, 20 }, Pal::lcdDim, 9.0f);
    iw::text (g, "PLACE: PUTS THE HEADS THERE, THEN DRAG AWAY" + dot + "INTENSITY PAST 100%: IT RUNS AWAY", { echoArea.getX() + 10, echoArea.getBottom() - 18, echoArea.getWidth() - 20, 14 }, Pal::lcdDim, 7.5f);

    section (g, tapeArea, "TAPE", "THE LOOP CASSETTE");
    iw::text (g, "AGE: TREBLE LOSS, DROPOUTS, HISS" + dot + "DRIVE: HOW HARD THE LOOP IS RECORDED", { tapeArea.getX() + 10, tapeArea.getBottom() - 18, tapeArea.getWidth() - 20, 14 }, Pal::lcdDim, 7.5f);
    {
        // the record level, as segments: lit to where it is, outlined above 0 VU
        auto v = vuArea.toFloat();
        g.setColour (Pal::lcd);
        g.fillRect (v);
        g.setColour (Pal::lcdRule);
        g.drawRect (v, 1.0f);
        iw::text (g, "REC LEVEL", vuArea.withHeight (18), Pal::lcdDim, 8.0f, juce::Justification::centred);
        for (int s = 0; s < 12; ++s)
        {
            const float segDb = -20.0f + (float) s * 26.0f / 11.0f;
            const auto r = juce::Rectangle<float> (v.getX() + 8.0f + (float) s * 6.5f, v.getY() + 26.0f, 4.5f, 22.0f);
            g.setColour (s >= 9 ? Pal::lcdText : Pal::lcdDim);
            if (vuDb >= segDb) g.fillRect (r);
            else g.drawRect (r, 1.0f);
        }
        iw::text (g, "-20", { vuArea.getX() + 6, vuArea.getBottom() - 18, 30, 12 }, Pal::lcdDim, 7.0f);
        iw::text (g, "0", { vuArea.getX() + 58, vuArea.getBottom() - 18, 20, 12 }, Pal::lcdDim, 7.0f);
        iw::text (g, "+6", { vuArea.getRight() - 24, vuArea.getBottom() - 18, 20, 12 }, Pal::lcdDim, 7.0f);
    }

    section (g, loopArea, "LOOP", "AN ENDLESS CASSETTE");
    iw::text (g, "LENGTH", { loopArea.getX() + 10, loopArea.getY() + 22, 50, 20 }, Pal::lcdDim, 9.0f);
    iw::text (g, "LENGTH AT 1 7/8 IPS; A SLOWER RATE MAKES A LONGER LOOP", { loopArea.getX() + 500, loopArea.getY() + 22, 330, 20 }, Pal::lcdDim, 8.0f);
    iw::text (g, "ERASE OFF: NOTHING IS WIPED, SO EACH TURN OF THE LOOP PLAYS WHAT WAS THERE BEFORE, A LITTLE QUIETER, UNDER WHAT'S NEW (SOUND ON SOUND).",
              { loopArea.getX() + 10, loopArea.getY() + 46, loopArea.getWidth() - 20, 12 }, Pal::lcdDim, 7.5f);
    iw::text (g, "HOLD: STOPS RECORDING ONTO THE LOOP; WHAT'S ON IT KEEPS GOING ROUND. SPLICE TICK: THE JOIN IN THE LOOP PASSING A HEAD, ONCE A TURN.",
              { loopArea.getX() + 10, loopArea.getY() + 62, loopArea.getWidth() - 20, 12 }, Pal::lcdDim, 7.5f);

    section (g, nowArea, "ON THE LOOP NOW", juce::String());
    {
        const auto r = nowArea.reduced (10, 0).withTrimmedTop (24).withTrimmedBottom (6).toFloat();
        const float mid = r.getCentreY(), half = r.getHeight() * 0.5f;
        g.setColour (Pal::lcdDim);
        const float w = r.getWidth() / (float) overviewBins;
        for (int b = 0; b < overviewBins; ++b)
        {
            const float lv = std::min (1.0f, proc.overview (b) / (tapeRefPeak * 2.0f));
            const float h = std::max (0.5f, lv * half);
            g.fillRect (r.getX() + (float) b * w, mid - h, std::max (1.0f, w - 0.5f), 2.0f * h);
        }
        const float rx = r.getX() + r.getWidth() * (float) proc.recordAt();
        g.setColour (Pal::lcdText);
        g.fillRect (rx - 1.0f, r.getY() - 4.0f, 2.0f, r.getHeight() + 8.0f);
        iw::text (g, proc.get ("hold") > 0.5f ? "HELD" : "REC", { (int) rx + 5, (int) r.getY() - 6, 40, 12 }, Pal::lcdText, 8.0f);
    }
}

void TapeEchoPanel::refresh()
{
    for (int i = 0; i < chips.size(); ++i)
        if (const bool on = chipLit[(size_t) i](); chips[i]->chosen != on) { chips[i]->chosen = on; chips[i]->repaint(); }
    for (auto* k : knobs) k->repaint();
    path->repaint();
    repaint();
}

void TapeEchoPanel::timerCallback()
{
    // Settings changed elsewhere (a learned control, a preset): show them.
    juce::String now;
    for (auto* p : proc.getParameters()) now << p->getValue() << ",";
    if (now != shown) { shown = now; refresh(); }
    const float peak = proc.takeRecordPeakDb();
    vuDb = std::max (peak, vuDb - 1.5f);
    repaint (vuArea);
    repaint (nowArea);
}
} // namespace tf
