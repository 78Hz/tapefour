#include "StepPanel.h"

namespace tf
{
namespace
{
    using namespace step;
    const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
    const juce::Colour cellFill { 0xff0e2c28 };

    // TIME as a log scale, 0..1000 for 10 ms..10 s
    int timeToIndex (double ms) { return juce::roundToInt (1000.0 * std::log (std::clamp (ms, shortestMs, longestMs) / shortestMs) / std::log (longestMs / shortestMs)); }
    double indexToTime (int v)  { return shortestMs * std::pow (longestMs / shortestMs, std::clamp (v, 0, 1000) / 1000.0); }
    /** Step times rounded as they're shown: whole ms under a second, 10 ms above. */
    double tidy (double ms) { ms = std::clamp (ms, shortestMs, longestMs); return ms < 1000.0 ? std::round (ms) : std::round (ms / 10.0) * 10.0; }
}

//==============================================================================
/** One setting of the selected step: caption, bar (drag; the wheel steps; double-click: its default), value. */
class StepPanel::Field : public juce::Component
{
public:
    Field (juce::String c, int lo, int hi, int def, std::function<int()> g, std::function<void (int)> s, std::function<juce::String (int)> t)
        : caption (std::move (c)), low (lo), high (hi), deflt (def), get (std::move (g)), set (std::move (s)), text (std::move (t)) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        iw::text (g, caption, r.removeFromLeft (120), Pal::lcdDim, 10.0f);
        iw::text (g, text (get()), r.removeFromRight (110), Pal::lcdText, 10.5f, juce::Justification::centredRight);
        bar = r.reduced (6, 0).withSizeKeepingCentre (r.getWidth() - 12, 6);
        g.setColour (Pal::lcdRule);
        g.fillRoundedRectangle (bar.toFloat(), 3.0f);
        g.setColour (Pal::lcdText);
        const float f = (float) (get() - low) / (float) std::max (1, high - low);
        g.fillRoundedRectangle (bar.toFloat().withWidth (std::max (6.0f, (float) bar.getWidth() * f)), 3.0f);
    }
    void mouseDown (const juce::MouseEvent& e) override { mouseDrag (e); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (bar.getWidth() <= 0) return;
        set (low + juce::roundToInt (juce::jlimit (0.0f, 1.0f, (float) (e.x - bar.getX()) / (float) bar.getWidth()) * (float) (high - low)));
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override { set (juce::jlimit (low, high, get() + (w.deltaY > 0 ? 1 : -1))); }
    void mouseDoubleClick (const juce::MouseEvent&) override { set (deflt); }

private:
    juce::String caption;
    int low, high, deflt;
    std::function<int()> get;
    std::function<void (int)> set;
    std::function<juce::String (int)> text;
    juce::Rectangle<int> bar;
};

//==============================================================================
/** The 16 steps: their numbers (and a lamp on the one playing), the pitch row and the time row. */
class StepPanel::Steps : public juce::Component
{
public:
    Steps (Processor& p, std::function<void()> changedFn) : s (p), onChange (std::move (changedFn)) {}

    static constexpr int numberH = 26, pitchH = 86, gapH = 10, timeH = 70;

    juce::Rectangle<int> column (int i) const
    {
        const float w = (float) getWidth() / (float) numSteps;
        return juce::Rectangle<float> (w * (float) i + 1.0f, 0.0f, w - 2.0f, (float) getHeight()).toNearestInt();
    }
    juce::Rectangle<int> pitchCell (int i) const { return column (i).withY (numberH).withHeight (pitchH); }
    juce::Rectangle<int> timeCell (int i) const  { return column (i).withY (numberH + pitchH + gapH).withHeight (timeH); }

    void paint (juce::Graphics& g) override
    {
        const auto& st = s.state();
        const int playing = s.playingStep();
        for (int i = 0; i < numSteps; ++i)
        {
            const auto& stp = st.steps[(size_t) i];
            const bool inUse = i < st.length, sel = i == s.editorStep;
            const float a = inUse ? 1.0f : 0.35f;
            auto col = column (i);
            iw::text (g, juce::String (i + 1), col.withHeight (14), (i == playing ? Pal::lcdText : Pal::lcdDim).withMultipliedAlpha (a), 9.5f, juce::Justification::centred);
            if (i == playing)
            {
                g.setColour (Pal::lcdText);
                g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ (float) col.getCentreX(), 19.0f }));
            }

            // pitch: the note's name, a line for its height, velocity along the foot
            auto p = pitchCell (i);
            g.setColour (cellFill.withMultipliedAlpha (a));
            g.fillRoundedRectangle (p.toFloat(), 3.0f);
            if (stp.rest)
            {
                iw::text (g, utf ("\xe2\x80\x94"), p.withTrimmedBottom (30), Pal::lcdDim.withMultipliedAlpha (a), 14.0f, juce::Justification::centred);
                iw::text (g, "REST", p.withTrimmedTop (44).withHeight (16), Pal::lcdDim.withMultipliedAlpha (a), 8.5f, juce::Justification::centred);
            }
            else
            {
                iw::text (g, noteName (stp.note), p.withHeight (26), Pal::lcdText.withMultipliedAlpha (a), 12.0f, juce::Justification::centred);
                if (stp.tie) iw::text (g, "TIE", p.withY (p.getY() + 22).withHeight (14), Pal::lcdText.withMultipliedAlpha (a), 8.5f, juce::Justification::centred);
                const float frac = (float) (stp.note - lowestNote) / (float) (highestNote - lowestNote);
                const float y = (float) p.getBottom() - 12.0f - frac * ((float) p.getHeight() - 42.0f);
                g.setColour (Pal::lcdText.withMultipliedAlpha (a));
                g.fillRect (juce::Rectangle<float> ((float) p.getX() + 6.0f, y - 1.0f, (float) p.getWidth() - 12.0f, 2.0f));
                g.setColour (Pal::lcdDim.withMultipliedAlpha (a));
                g.fillRect (juce::Rectangle<float> ((float) p.getX() + 6.0f, (float) p.getBottom() - 5.0f, ((float) p.getWidth() - 12.0f) * (float) stp.velocity / 127.0f, 1.0f));
            }

            // time: a column for the step's time (log, 10 ms..10 s), a thinner one for its gate
            auto t = timeCell (i);
            g.setColour (cellFill.withMultipliedAlpha (a));
            g.fillRoundedRectangle (t.toFloat(), 3.0f);
            iw::text (g, msText (stp.ms), t.withHeight (22), Pal::lcdText.withMultipliedAlpha (a), 9.0f, juce::Justification::centred);
            const float tf = (float) timeToIndex (stp.ms) / 1000.0f;
            const float full = (float) t.getHeight() - 32.0f, base = (float) t.getBottom() - 6.0f;
            const float half = (float) t.getWidth() * 0.5f;
            g.setColour (Pal::lcdText.withMultipliedAlpha (a));
            g.fillRect (juce::Rectangle<float> ((float) t.getX() + 7.0f, base - std::max (2.0f, tf * full), half - 9.0f, std::max (2.0f, tf * full)));
            if (! stp.rest)
            {
                const float gh = std::max (1.0f, tf * full * (float) (stp.tie ? 100 : stp.gate) / 100.0f);
                g.setColour (Pal::lcdDim.withMultipliedAlpha (a));
                g.fillRect (juce::Rectangle<float> ((float) t.getX() + half + 3.0f, base - gh, half - 10.0f, gh));
            }
            if (sel)
            {
                g.setColour (Pal::lcdText);
                g.drawRoundedRectangle (p.toFloat().reduced (0.5f), 3.0f, 1.0f);
                g.drawRoundedRectangle (t.toFloat().reduced (0.5f), 3.0f, 1.0f);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStep = -1;
        for (int i = 0; i < numSteps; ++i)
        {
            const bool inPitch = pitchCell (i).contains (e.getPosition()), inTime = timeCell (i).contains (e.getPosition());
            if (! inPitch && ! inTime) continue;
            s.editorStep = i;
            auto& st = s.state().steps[(size_t) i];
            dragStep = i;
            dragPitch = inPitch;
            startY = e.y;
            startNote = st.rest ? lowestNote - 1 : st.note;
            startMs = st.ms;
            startGate = st.gate;
            if (inPitch && e.mods.isShiftDown() && ! st.rest)
            {
                st.tie = ! st.tie;   // shift-click: tie it into the next step (or not)
                dragStep = -1;
                onChange();
            }
            repaint();
            if (auto* parent = getParentComponent()) { parent->repaint(); parent->grabKeyboardFocus(); }
            return;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragStep < 0) return;
        auto& st = s.state().steps[(size_t) dragStep];
        const int dy = startY - e.y;
        if (dragPitch)
        {
            // 6 px a semitone; below the lowest note it rests
            const int note = std::clamp (startNote + dy / 6, lowestNote - 1, highestNote);
            st.rest = note < lowestNote;
            if (! st.rest) st.note = note;
        }
        else if (e.mods.isShiftDown())
            st.gate = std::clamp (startGate + dy / 2, 1, 100);             // shift-drag: the gate
        else
            st.ms = tidy (startMs * std::pow (2.0, dy / 40.0));            // 40 px doubles or halves it
        onChange();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < numSteps; ++i)
        {
            auto& st = s.state().steps[(size_t) i];
            if (timeCell (i).contains (e.getPosition())) { st.ms = defaultMs; onChange(); return; }
            if (pitchCell (i).contains (e.getPosition())) { st.rest = false; st.note = 36; onChange(); return; }
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const int dir = w.deltaY > 0 ? 1 : -1;
        for (int i = 0; i < numSteps; ++i)
        {
            auto& st = s.state().steps[(size_t) i];
            if (pitchCell (i).contains (e.getPosition()))
            {
                if (st.rest && dir > 0) { st.rest = false; st.note = lowestNote; }
                else if (! st.rest) { if (st.note + dir < lowestNote) st.rest = true; else st.note = std::min (highestNote, st.note + dir); }
                onChange();
                return;
            }
            if (timeCell (i).contains (e.getPosition()))
            {
                st.ms = tidy (indexToTime (timeToIndex (st.ms) + dir * 5));
                onChange();
                return;
            }
        }
    }

private:
    Processor& s;
    std::function<void()> onChange;
    int dragStep = -1, startY = 0, startNote = 36, startGate = 50;
    bool dragPitch = true;
    double startMs = defaultMs;
};

//==============================================================================
StepPanel::StepPanel (Processor& p) : seq (p)
{
    play.textSize = 12.0f;
    play.onClick = [this] { seq.setPlaying (! seq.isPlaying()); };
    addAndMakeVisible (play);
    for (int i = 1; i <= numSteps; ++i)
    {
        auto* c = lengthChips.add (new LcdChip (juce::String (i)));
        c->textSize = 10.5f;
        c->onClick = [this, i] { seq.state().length = i; edited(); };
        addAndMakeVisible (c);
    }
    for (auto* c : { &transposeChip, &keysOff, &homeDown, &homeUp, &halve, &twice }) { c->textSize = 11.0f; addAndMakeVisible (*c); }
    transposeChip.onClick = [this] { seq.state().transpose = true; edited(); };
    keysOff.onClick = [this] { seq.state().transpose = false; edited(); };
    homeDown.onClick = [this] { seq.state().homeNote = std::max (lowestNote, seq.state().homeNote - 1); edited(); };
    homeUp.onClick = [this] { seq.state().homeNote = std::min (highestNote, seq.state().homeNote + 1); edited(); };
    homeDown.setTooltip ("The key that plays the sequence as written: down a semitone");
    homeUp.setTooltip ("The key that plays the sequence as written: up a semitone");
    auto scaleAll = [this] (double by)
    {
        for (auto& st : seq.state().steps) st.ms = tidy (st.ms * by);
        edited();
    };
    halve.onClick = [scaleAll] { scaleAll (0.5); };
    twice.onClick = [scaleAll] { scaleAll (2.0); };
    halve.setTooltip ("Every step's time halved: twice as fast, the same shape");
    twice.setTooltip ("Every step's time doubled: half as fast, the same shape");

    steps = std::make_unique<Steps> (seq, [this] { edited(); });
    addAndMakeVisible (*steps);

    auto sel = [this] () -> Step& { return seq.state().steps[(size_t) std::clamp (seq.editorStep, 0, numSteps - 1)]; };
    fields.add (new Field ("PITCH", lowestNote - 1, highestNote, 36,
                           [sel] { auto& s = sel(); return s.rest ? lowestNote - 1 : s.note; },
                           [this, sel] (int v) { auto& s = sel(); s.rest = v < lowestNote; if (! s.rest) s.note = v; edited(); },
                           [] (int v) { return v < lowestNote ? juce::String ("REST") : noteName (v); }));
    fields.add (new Field ("TIME", 0, 1000, timeToIndex (defaultMs),
                           [sel] { return timeToIndex (sel().ms); },
                           [this, sel] (int v) { sel().ms = tidy (indexToTime (v)); edited(); },
                           [sel] (int) { return msText (sel().ms); }));   // the step's own time, not the bar's rounding
    fields.add (new Field ("GATE", 1, 100, 50, [sel] { return sel().gate; }, [this, sel] (int v) { sel().gate = v; edited(); },
                           [sel] (int v) { return sel().tie ? juce::String ("TIED") : juce::String (v) + "%"; }));
    fields.add (new Field ("VELOCITY", 1, 127, 100, [sel] { return sel().velocity; }, [this, sel] (int v) { sel().velocity = v; edited(); },
                           [] (int v) { return juce::String (v); }));
    for (auto* f : fields) addAndMakeVisible (f);
    setWantsKeyboardFocus (true);
    startTimerHz (30);
}

StepPanel::~StepPanel() = default;

void StepPanel::edited()
{
    seq.changed();
    repaint();
    steps->repaint();
    for (auto* f : fields) f->repaint();
}

void StepPanel::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (30);
    play.setBounds (top.removeFromLeft (90));
    top.removeFromLeft (14);
    cycleArea = top.removeFromLeft (150);
    top.removeFromLeft (8);
    auto len = top;
    juce::ignoreUnused (len);
    const int lx = top.getX() + 64;
    const int cw = (top.getRight() - lx - 15 * 4) / numSteps;
    for (int i = 0; i < numSteps; ++i)
        lengthChips[i]->setBounds (lx + i * (cw + 4), top.getY() + 2, cw, 26);
    r.removeFromTop (10);

    auto keys = r.removeFromTop (26);
    keysArea = keys.removeFromLeft (46);
    transposeChip.setBounds (keys.removeFromLeft (transposeChip.idealWidth())); keys.removeFromLeft (6);
    keysOff.setBounds (keys.removeFromLeft (keysOff.idealWidth())); keys.removeFromLeft (12);
    homeDown.setBounds (keys.removeFromLeft (26)); keys.removeFromLeft (4);
    homeUp.setBounds (keys.removeFromLeft (26));
    timesArea = keys.withTrimmedLeft (8);
    twice.setBounds (keys.removeFromRight (44)); keys.removeFromRight (6);
    halve.setBounds (keys.removeFromRight (44));
    r.removeFromTop (14);

    steps->setBounds (r.removeFromTop (Steps::numberH + Steps::pitchH + Steps::gapH + Steps::timeH));
    r.removeFromTop (6);
    helpArea = r.removeFromTop (30);
    r.removeFromTop (8);
    selArea = r.removeFromTop (22);
    for (auto* f : fields) { r.removeFromTop (6); f->setBounds (r.removeFromTop (22)); }
    r.removeFromTop (10);
    chainArea = r.removeFromTop (30);
}

void StepPanel::paint (juce::Graphics& g)
{
    const auto& st = seq.state();
    iw::text (g, "CYCLE", cycleArea.removeFromLeft (52), Pal::lcdDim, 10.0f);
    iw::text (g, msText (st.cycleMs()), cycleArea, Pal::lcdText, 13.0f);
    iw::text (g, "LENGTH", { lengthChips[0]->getX() - 64, lengthChips[0]->getY(), 60, 26 }, Pal::lcdDim, 10.0f);
    iw::text (g, "KEYS", keysArea, Pal::lcdDim, 10.0f);
    const int shiftNow = seq.transposition();
    const juce::String keysText = st.transpose ? "FROM " + noteName (st.homeNote) + dot + "NOW " + (shiftNow > 0 ? "+" : "") + juce::String (shiftNow)
                                               : juce::String ("THE KEYBOARD IS IGNORED");
    iw::text (g, keysText, { homeUp.getRight() + 10, timesArea.getY(), halve.getX() - homeUp.getRight() - 150, 26 }, Pal::lcdText, 10.5f);
    iw::text (g, "STEP TIMES", { halve.getX() - 96, halve.getY(), 92, 26 }, Pal::lcdDim, 10.0f, juce::Justification::centredRight);

    g.setColour (Pal::lcdRule);
    g.fillRect (0, steps->getY() - 7, getWidth(), 1);
    iw::text (g, utf ("PITCH: DRAG UP OR DOWN \xc2\xb7 BELOW THE LOWEST NOTE: REST \xc2\xb7 SHIFT-CLICK: TIE (HELD INTO THE NEXT; A MONO SYNTH GLIDES)"),
              helpArea.withHeight (15), Pal::lcdDim, 9.0f);
    iw::text (g, utf ("TIME: DRAG (10 MS TO 10 S) \xc2\xb7 SHIFT-DRAG: ITS GATE (THE THIN COLUMN) \xc2\xb7 DOUBLE-CLICK: 180 MS \xc2\xb7 THE WHEEL: FINE"),
              helpArea.withTrimmedTop (15), Pal::lcdDim, 9.0f);
    g.fillRect (0, selArea.getY() - 5, getWidth(), 1);

    const auto& s = st.steps[(size_t) std::clamp (seq.editorStep, 0, numSteps - 1)];
    auto sa = selArea;
    iw::text (g, "STEP " + juce::String (seq.editorStep + 1), sa.removeFromLeft (80), Pal::lcdText, 12.5f);
    iw::text (g, (s.rest ? juce::String ("REST") : noteName (s.note) + (s.tie ? " TIED" : "")) + dot + msText (s.ms)
                     + (s.rest || s.tie ? juce::String() : dot + "GATE " + juce::String (s.gate) + "%") + dot + "VELOCITY " + juce::String (s.velocity)
                     + dot + utf ("\xe2\x86\x90 / \xe2\x86\x92: THE STEPS EITHER SIDE"),
              sa, Pal::lcdDim, 10.0f);

    g.fillRect (0, chainArea.getY() - 5, getWidth(), 1);
    iw::text (g, utf ("IN A CHAIN: TF STEP \xe2\x80\xba EXTERNAL INSTRUMENT (A HARDWARE SYNTH), OR TF STEP \xe2\x80\xba ANY INSTRUMENT"),
              chainArea.withHeight (15), Pal::lcdText, 9.5f);
    iw::text (g, "ITS OWN PLAY, NOT THE TAPE'S. HEARD WHILE ITS WINDOW IS OPEN; RECORDED WHEN THE TRACK IS ARMED.",
              chainArea.withTrimmedTop (15), Pal::lcdDim, 9.0f);
}

bool StepPanel::keyPressed (const juce::KeyPress& k)
{
    // the arrows: the steps either side
    if (k.getKeyCode() == juce::KeyPress::leftKey || k.getKeyCode() == juce::KeyPress::rightKey)
    {
        const int dir = k.getKeyCode() == juce::KeyPress::leftKey ? -1 : 1;
        seq.editorStep = (seq.editorStep + dir + numSteps) % numSteps;
        repaint();
        return true;
    }
    return false;
}

void StepPanel::timerCallback()
{
    const auto& st = seq.state();
    const bool on = seq.isPlaying();
    const juce::String label = on ? "STOP" : "PLAY";
    if (play.getButtonText() != label || play.chosen != on) { play.setButtonText (label); play.chosen = on; play.repaint(); }
    for (int i = 0; i < lengthChips.size(); ++i)
        if (lengthChips[i]->chosen != (i + 1 == st.length)) { lengthChips[i]->chosen = i + 1 == st.length; lengthChips[i]->repaint(); }
    if (transposeChip.chosen != st.transpose) { transposeChip.chosen = st.transpose; keysOff.chosen = ! st.transpose; transposeChip.repaint(); keysOff.repaint(); }
    homeDown.setVisible (st.transpose);
    homeUp.setVisible (st.transpose);
    steps->repaint();
    repaint (0, 0, getWidth(), 70);
    static_cast<juce::Component*> (this)->repaint (selArea);
    for (auto* f : fields) f->repaint();
}
} // namespace tf
