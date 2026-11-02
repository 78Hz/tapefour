#include "PhraseSamplerPanel.h"

namespace tf
{
namespace
{
    using namespace iw;
    using namespace sampler;
    const char* const tabNames[] { "PLAY", "SAMPLE", "EDIT", "SONGS", "SETUP" };

    juce::String padName (int index) { return juce::String::charToString ((juce::juce_wchar) ('A' + index / padsPerBank)) + juce::String (index % padsPerBank + 1); }
    juce::String noteName (int note) { return note < 0 ? juce::String ("NONE") : juce::MidiMessage::getMidiNoteName (note, true, true, 3); }   // Yamaha's octaves: 60 is C3
    juce::String secs (double s) { return juce::String (s, 1) + " S"; }

    // the front panel's own colours: the SU10's grey case, its olive LCD
    const juce::Colour caseTop { 0xff3a3935 }, caseEdge { 0xff26251f }, keyFace { 0xff4f4d47 }, keyFaceDown { 0xff5f5c55 },
                       padFace { 0xff57554f }, lcdGlass { 0xffa9b78a }, lcdInk { 0xff26301d }, recFace { 0xff8a3027 }, ledOff { 0xff2a2925 };
}

//==============================================================================
/** A setting that isn't a parameter (the input gain, say), drawn like iw::Bar. */
class PhraseSamplerPanel::ValueBar : public juce::Component
{
public:
    ValueBar (juce::String c, int lo, int hi, std::function<int()> g, std::function<void (int)> s, std::function<juce::String (int)> f)
        : caption (std::move (c)), low (lo), high (hi), get (std::move (g)), set (std::move (s)), fmt (std::move (f)) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        text (g, caption, r.removeFromLeft (captionW), Pal::lcdDim);
        text (g, fmt (get()), r.removeFromRight (valueW), Pal::lcdText, 11.0f, juce::Justification::centredRight);
        bar = r.reduced (2, 0).withSizeKeepingCentre (r.getWidth() - 4, 6);
        g.setColour (Pal::lcdRule);
        g.fillRoundedRectangle (bar.toFloat(), 3.0f);
        g.setColour (Pal::lcdText);
        g.fillRoundedRectangle (bar.toFloat().withWidth ((float) bar.getWidth() * (float) (get() - low) / (float) std::max (1, high - low)), 3.0f);
    }
    void mouseDown (const juce::MouseEvent& e) override { mouseDrag (e); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (bar.getWidth() <= 0) return;
        set (low + juce::roundToInt (juce::jlimit (0.0f, 1.0f, (float) (e.x - bar.getX()) / (float) bar.getWidth()) * (float) (high - low)));
        repaint();
    }

private:
    juce::String caption;
    int low, high;
    std::function<int()> get;
    std::function<void (int)> set;
    std::function<juce::String (int)> fmt;
    juce::Rectangle<int> bar;
};

//==============================================================================
/** The front panel, as the owner's manual draws it: the LCD in its compartments, the value keys, the
    ribbon, SHIFT SCRATCH HOLD REV BANK, the 12 pads with SHIFT's functions printed above them, and the
    transport. SHIFT, SCRATCH and REV stay down when clicked (a mouse can't hold one key and press another),
    or hold the computer's Shift key while clicking a pad. */
class PhraseSamplerPanel::FrontView : public juce::Component,
                                      private juce::Timer
{
public:
    FrontView (Processor& p) : s (p), f (p.front()) {}
    ~FrontView() override { latchOff(); }

    struct Key { juce::Rectangle<float> r; juce::String label, name; int pad = -1; bool led = false; };

    void resized() override
    {
        keys.clear();
        const float w = (float) getWidth();
        lcd = { 16.0f, 14.0f, 430.0f, 104.0f };
        readouts = { 462.0f, 14.0f, w - 462.0f - 16.0f - 2.0f * 84.0f, 104.0f };
        const float kx = w - 16.0f - 2.0f * 78.0f - 6.0f;
        const char* value[] { "EXIT", "ENTER", "-1", "+1", "<", ">" };
        for (int i = 0; i < 6; ++i)
            keys.push_back ({ { kx + (float) (i % 2) * 84.0f, 14.0f + (float) (i / 2) * 36.0f, 78.0f, 30.0f }, value[i], value[i] });
        ribbon = { 16.0f, 130.0f, w - 32.0f, 36.0f };
        const char* left[] { "SHIFT", "SCRATCH", "HOLD", "REV", "BANK" };
        for (int i = 0; i < 5; ++i)
            keys.push_back ({ { 16.0f + (float) (i % 2) * 84.0f, 200.0f + (float) (i / 2) * 48.0f, 78.0f, 40.0f }, left[i], left[i], -1, i < 4 });
        const float padX = 200.0f, padW = (w - 16.0f - padX - 5.0f * 8.0f) / 6.0f;
        for (int i = 0; i < 12; ++i)
            keys.push_back ({ { padX + (float) (i % 6) * (padW + 8.0f), 202.0f + (float) (i / 6) * 96.0f, padW, 80.0f }, juce::String (i + 1), {}, i });
        const char* transport[] { "REC", "TOP", "BWD", "STOP", "PLAY", "FWD" };
        for (int i = 0; i < 6; ++i)
            keys.push_back ({ { 16.0f + (float) i * 62.0f, 392.0f, 56.0f, 32.0f }, transport[i], transport[i], -1, i == 0 || i == 4 });
    }

    bool lit (const Key& k, const PanelState& st, const Front::Lcd& l) const
    {
        const auto& n = k.name;
        if (n == "SHIFT") return f.shiftHeld();
        if (n == "SCRATCH") return st.scratchPad >= 0 || st.scratchArmed;
        if (n == "HOLD") return st.hold;
        if (n == "REV") return st.reverse;
        if (n == "REC") return l.rec;
        if (n == "PLAY") return st.songPlaying;
        return false;
    }

    void paint (juce::Graphics& g) override
    {
        auto& lf = DeckLookAndFeel::get();
        const auto st = s.panel();
        const auto l = f.lcd();
        auto body = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (caseTop);
        g.fillRoundedRectangle (body, 8.0f);
        g.setColour (caseEdge);
        g.drawRoundedRectangle (body, 8.0f, 1.5f);

        // ---- the LCD, in its compartments
        g.setColour (lcdGlass);
        g.fillRoundedRectangle (lcd, 4.0f);
        const auto off = lcdInk.withAlpha (0.22f);
        auto cell = [&] (float& x, const char* cap, const juce::String& v, float minW)
        {
            g.setFont (lf.mono (7.0f));
            g.setColour (off);
            g.drawText (cap, juce::Rectangle<float> (x, lcd.getY() + 4.0f, 60.0f, 9.0f), juce::Justification::centredLeft);
            g.setFont (lf.mono (12.0f));
            g.setColour (lcdInk);
            g.drawText (v, juce::Rectangle<float> (x, lcd.getY() + 12.0f, 80.0f, 16.0f), juce::Justification::centredLeft);
            x += std::max (minW, juce::GlyphArrangement::getStringWidth (lf.mono (12.0f), v) + 10.0f);
        };
        float x = lcd.getX() + 10.0f;
        cell (x, "GRADE", l.grade, 40.0f);
        cell (x, "MODE", l.mode, 40.0f);
        cell (x, "LOOP", l.loop, 48.0f);
        cell (x, "", l.format, 50.0f);
        cell (x, "INPUT", l.input, 40.0f);
        cell (x, "BANK", l.bank, 26.0f);
        cell (x, "", l.effect, 38.0f);
        cell (x, "SONG", l.song + (l.songRepeat ? juce::String (juce::CharPointer_UTF8 (" \xe2\x86\xbb")) : juce::String()), 30.0f);
        g.setColour (off);
        g.fillRect (juce::Rectangle<float> (lcd.getX() + 8.0f, lcd.getY() + 32.0f, lcd.getWidth() - 16.0f, 1.0f));
        g.setColour (lcdInk);
        g.setFont (lf.mono (13.0f));
        g.drawText (l.top, juce::Rectangle<float> (lcd.getX() + 12.0f, lcd.getY() + 36.0f, 220.0f, 18.0f), juce::Justification::centredLeft, true);
        g.setFont (lf.mono (19.0f));
        g.drawText (l.bottom, juce::Rectangle<float> (lcd.getX() + 12.0f, lcd.getY() + 54.0f, 236.0f, 26.0f), juce::Justification::centredLeft, true);
        float ix = lcd.getRight() - 190.0f;
        for (auto [name, on] : { std::pair<const char*, bool> { "MIDI", l.midi }, { "TIME", l.time }, { "TEMPO", l.tempo }, { "REC", l.rec } })
        {
            g.setFont (lf.mono (9.0f));
            g.setColour (on ? lcdInk : off);
            g.drawText (name, juce::Rectangle<float> (ix, lcd.getY() + 38.0f, 46.0f, 11.0f), juce::Justification::centredLeft);
            ix += 46.0f;
        }
        g.setColour (lcdInk);
        g.setFont (lf.mono (8.0f));
        g.drawText (l.meterLabel, juce::Rectangle<float> (lcd.getRight() - 190.0f, lcd.getY() + 54.0f, 80.0f, 10.0f), juce::Justification::centredLeft);
        g.setFont (lf.mono (24.0f));
        g.drawText (l.meter, juce::Rectangle<float> (lcd.getRight() - 160.0f, lcd.getY() + 54.0f, 148.0f, 28.0f), juce::Justification::centredRight);
        float bar = l.bar;
        if (bar < -1.5f)
        {
            // the input level, while recording is set up
            inputLevel = std::max (inputLevel * 0.85f, s.takeInputPeak());
            bar = juce::jlimit (0.0f, 1.0f, inputLevel > 1.0e-5f ? (20.0f * std::log10 (inputLevel) + 48.0f) / 48.0f : 0.0f);
        }
        if (bar >= 0.0f)
        {
            auto b = juce::Rectangle<float> (lcd.getX() + 12.0f, lcd.getBottom() - 14.0f, lcd.getWidth() - 24.0f, 6.0f);
            g.setColour (lcdInk);
            g.drawRect (b, 1.0f);
            g.fillRect (b.reduced (2.0f, 1.5f).withWidth ((b.getWidth() - 4.0f) * bar));
        }

        // ---- what TAPE·FOUR adds: the memory and voices
        auto ro = readouts.toNearestInt();
        {
            auto r = ro.removeFromTop (34);
            const double total = memoryNibbles / clockHz (s.grade(), s.pitchPercent());
            text (g, "MEMORY", r.removeFromTop (14), Pal::faint, 9.5f);
            text (g, juce::String (s.usedSeconds(), 1) + " / " + juce::String (total, 1) + " S", r.withY (r.getY() - 14).withHeight (14), Pal::cream, 9.5f, juce::Justification::centredRight);
            auto b = r.withSizeKeepingCentre (r.getWidth(), 8).toFloat();
            g.setColour (Pal::dark);
            g.fillRoundedRectangle (b, 3.0f);
            g.setColour (Pal::lcdText);
            g.fillRoundedRectangle (b.withWidth (b.getWidth() * juce::jlimit (0.0f, 1.0f, (float) (s.usedSeconds() / total))), 3.0f);
        }
        {
            auto r = ro.removeFromTop (34);
            text (g, "VOICES", r.removeFromTop (14), Pal::faint, 9.5f);
            for (int v = 0; v < 4; ++v)
            {
                const auto dot = juce::Rectangle<float> ((float) r.getX() + (float) v * 16.0f, (float) r.getCentreY() - 5.0f, 10.0f, 10.0f);
                g.setColour (v >= st.maxVoices ? Pal::dark : (v < st.voices ? Pal::lamp : ledOff));
                g.fillEllipse (dot);
            }
            text (g, juce::String (st.voices) + " OF " + juce::String (st.maxVoices), r, Pal::faint, 9.0f, juce::Justification::centredRight);
        }
        text (g, "PLAYED HERE IT'S HEARD, ARMED OR NOT;", ro.removeFromTop (13), Pal::faint, 8.0f);
        text (g, "ARM THE TRACK TO RECORD IT TO TAPE", ro.removeFromTop (13), Pal::faint, 8.0f);

        // ---- the ribbon
        g.setColour (Pal::darker);
        g.fillRoundedRectangle (ribbon, 5.0f);
        g.setColour (Pal::line);
        g.drawRoundedRectangle (ribbon, 5.0f, 1.0f);
        const float rx = ribbon.getX() + 14.0f + (ribbon.getWidth() - 28.0f) * s.ribbonPosition();
        g.setColour (touching ? Pal::cream : Pal::cream.withAlpha (0.55f));
        g.fillEllipse (rx - 10.0f, ribbon.getCentreY() - 10.0f, 20.0f, 20.0f);
        juce::String help = "RIBBON";
        switch (st.effect)
        {
            case Effect::filter: case Effect::extFilter: help << " \xc2\xb7 FILTER: DRAG FOR THE CUTOFF"; break;
            case Effect::pitch: help << " \xc2\xb7 PITCH: -20 % TO +10 %"; break;
            case Effect::crossfade: help << " \xc2\xb7 CROSSFADER: SAMPLES TO INPUT"; break;
            case Effect::extScratch: help << " \xc2\xb7 EXT SCRATCH: HOLD AND DRAG THROUGH WHAT JUST CAME IN"; break;
            case Effect::scale: help << " \xc2\xb7 SCALE: THE PADS PLAY ONE SAMPLE IN SEMITONES"; break;
            case Effect::none: default: help << (st.scratchPad >= 0 ? " \xc2\xb7 SCRATCH: HOLD AND DRAG" : " \xc2\xb7 SHIFT + A PAD CHOOSES AN EFFECT"); break;
        }
        text (g, utf (help.toRawUTF8()), { (int) ribbon.getX(), (int) ribbon.getBottom() + 4, (int) ribbon.getWidth(), 14 }, Pal::faint, 9.0f, juce::Justification::centred);

        // ---- keys and pads
        const bool shifted = f.shiftHeld();
        for (auto& k : keys)
        {
            const bool down = &k == pressed;
            if (k.pad >= 0)
            {
                const int index = st.bank * padsPerBank + k.pad;
                const auto p = s.pad (index);
                const bool sounding = (st.padsSounding >> index) & 1;
                g.setColour (shifted ? Pal::lamp : Pal::faint.withAlpha (0.9f));
                g.setFont (lf.cond (10.5f, true));
                g.drawText (Front::shiftLabel (k.pad), k.r.withY (k.r.getY() - 15.0f).withHeight (13.0f), juce::Justification::bottomLeft);
                g.setColour (down ? keyFaceDown : padFace);
                g.fillRoundedRectangle (k.r, 6.0f);
                if (sounding)
                {
                    g.setColour (Pal::lamp.withAlpha (0.45f));
                    g.fillRoundedRectangle (k.r, 6.0f);
                }
                g.setColour (caseEdge);
                g.drawRoundedRectangle (k.r, 6.0f, 1.0f);
                auto r = k.r.reduced (9.0f, 7.0f).toNearestInt();
                g.setColour (Pal::cream);
                g.setFont (lf.cond (15.0f, true));
                g.drawText (k.label, r.removeFromTop (18), juce::Justification::topLeft);
                g.setColour (p.used ? Pal::cream.withAlpha (0.85f) : Pal::faint.withAlpha (0.5f));
                g.setFont (lf.mono (9.5f));
                g.drawText (p.used ? p.name : juce::String ("empty"), r.removeFromBottom (14), juce::Justification::bottomLeft, true);
                continue;
            }
            const bool isRec = k.name == "REC";
            g.setColour (isRec ? recFace : (down ? keyFaceDown : keyFace));
            g.fillRoundedRectangle (k.r, 4.0f);
            g.setColour (caseEdge);
            g.drawRoundedRectangle (k.r, 4.0f, 1.0f);
            if (k.led)
            {
                g.setColour (lit (k, st, l) ? (isRec ? Pal::red : Pal::lamp) : ledOff);
                g.fillEllipse (k.r.getCentreX() - 3.0f, k.r.getY() + 5.0f, 6.0f, 6.0f);
            }
            g.setColour (Pal::cream);
            g.setFont (lf.cond (13.0f, true));
            g.drawText (k.label, k.r.withTrimmedTop (k.led ? 8.0f : 0.0f).toNearestInt(), juce::Justification::centred, true);
        }
        text (g, "PADS ON MIDI C1-B4", { getWidth() - 276, 394, 260, 14 }, Pal::faint, 9.0f, juce::Justification::centredRight);
        text (g, "SHIFT + PAD: THE LABELS ABOVE THE PADS", { getWidth() - 276, 410, 260, 14 }, Pal::faint, 9.0f, juce::Justification::centredRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        pressed = nullptr;
        if (ribbon.contains (e.position)) { touching = true; s.setRibbon (ribbonAt (e.position.x), true); repaint(); return; }
        for (auto& k : keys)
            if (k.r.contains (e.position))
            {
                pressed = &k;
                if (k.pad >= 0)
                {
                    // the computer's Shift key works as SHIFT while clicking
                    const bool tempShift = e.mods.isShiftDown() && ! f.shiftHeld();
                    if (tempShift) f.key ("SHIFT", true);
                    // the pads aren't velocity sensitive on the unit; here the lower the click, the louder
                    const float v = juce::jlimit (0.3f, 1.0f, 0.3f + 0.7f * (e.position.y - k.r.getY()) / k.r.getHeight() * 1.25f);
                    const bool shifted = f.shiftHeld();
                    padDown = k.pad;
                    f.pad (k.pad, true, v);
                    if (tempShift) { f.key ("SHIFT", false); padDown = -1; }
                    else if (shifted && s.panel().effect != Effect::filter) { f.key ("SHIFT", false); padDown = -1; }   // SHIFT does one function
                    if (scratchLatched) { f.key ("SCRATCH", false); scratchLatched = false; }
                }
                else if (k.name == "SHIFT") f.key ("SHIFT", ! f.shiftHeld());
                else if (k.name == "SCRATCH")
                {
                    if (s.panel().scratchPad >= 0) { f.key ("SCRATCH", true); f.key ("SCRATCH", false); }
                    else if (scratchLatched) { f.key ("SCRATCH", false); scratchLatched = false; }
                    else { f.key ("SCRATCH", true); scratchLatched = true; }
                }
                else if (k.name == "REV")
                {
                    revLatched = ! revLatched;
                    f.key ("REV", revLatched);
                }
                else
                {
                    f.key (k.name, true, 0);
                    if (k.name == "+1" || k.name == "-1" || k.name == "<" || k.name == ">")
                    {
                        repeatCount = 0;
                        repeatName = k.name;
                        startTimer (400);
                    }
                }
                break;
            }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (touching) { s.setRibbon (ribbonAt (e.position.x), true); repaint(); }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        stopTimer();
        repeatName.clear();
        if (touching) { touching = false; s.setRibbon (ribbonAt (e.position.x), false); }
        if (pressed != nullptr)
        {
            if (pressed->pad >= 0) { if (padDown >= 0) f.pad (padDown, false); padDown = -1; }
            else if (pressed->name != "SHIFT" && pressed->name != "SCRATCH" && pressed->name != "REV") f.key (pressed->name, false);
        }
        pressed = nullptr;
        repaint();
    }

private:
    void timerCallback() override
    {
        // a held key repeats, faster and faster
        if (repeatName.isEmpty()) { stopTimer(); return; }
        f.key (repeatName, true, ++repeatCount);
        startTimer (std::max (25, (int) (220.0 * std::pow (0.88, repeatCount))));
        repaint();
    }
    void latchOff()
    {
        if (f.shiftHeld()) f.key ("SHIFT", false);
        if (scratchLatched) f.key ("SCRATCH", false);
        if (revLatched) f.key ("REV", false);
    }
    float ribbonAt (float x) const { return juce::jlimit (0.0f, 1.0f, (x - ribbon.getX() - 14.0f) / (ribbon.getWidth() - 28.0f)); }

    Processor& s;
    Front& f;
    std::vector<Key> keys;
    juce::Rectangle<float> lcd, readouts, ribbon;
    const Key* pressed = nullptr;
    int padDown = -1, repeatCount = 0;
    juce::String repeatName;
    bool touching = false, scratchLatched = false, revLatched = false;
    float inputLevel = 0.0f;
};

//==============================================================================
/** A button that repeats while held, faster the longer it's held: onRepeat gets the count (0 first). */
class PhraseSamplerPanel::RepeatChip : public LcdChip,
                                       private juce::Timer
{
public:
    explicit RepeatChip (const juce::String& label) : LcdChip (label) { textSize = 11.0f; }
    std::function<void (int)> onRepeat;

    void mouseDown (const juce::MouseEvent& e) override
    {
        LcdChip::mouseDown (e);
        count = 0;
        if (onRepeat) onRepeat (0);
        startTimer (400);
    }
    void mouseUp (const juce::MouseEvent& e) override { stopTimer(); LcdChip::mouseUp (e); }

private:
    void timerCallback() override
    {
        if (! isMouseButtonDown()) { stopTimer(); return; }
        if (onRepeat) onRepeat (++count);
        startTimer (std::max (25, (int) (220.0 * std::pow (0.88, count))));
    }
    int count = 0;
};

//==============================================================================
/** EDIT's fine tuning: the start, loop and end points to the sample (or a step of 1 or 10 ms), and the
    loop's tempo, large, with -/+ that move the end a hundredth of a BPM at a time. */
class PhraseSamplerPanel::FineTune : public juce::Component
{
public:
    FineTune (Processor& p, int index, std::function<void()> changed) : s (p), pad (index), onChange (std::move (changed))
    {
        const char* labels[] { "<<", "<", ">", ">>" };
        const int steps[] { -10, -1, 1, 10 };
        for (int point = 0; point < 3; ++point)
            for (int k = 0; k < 4; ++k)
            {
                auto* b = nudges.add (new RepeatChip (labels[k]));
                b->onRepeat = [this, point, n = steps[k]] (int) { movePoint (point, n); };
                addAndMakeVisible (b);
            }
        const char* stepLabels[] { "1 SMP", "1 MS", "10 MS" };
        for (int i = 0; i < 3; ++i)
        {
            auto* c = stepChips.add (new LcdChip (stepLabels[i]));
            c->textSize = 10.5f;
            c->onClick = [this, i] { stepMode = i; refresh(); };
            addAndMakeVisible (c);
        }
        for (int k = 0; k < 2; ++k)
        {
            auto* b = tempoKeys.add (new RepeatChip (k == 0 ? "-" : "+"));
            b->onRepeat = [this, k] (int) { if (s.nudgeTempo (pad, k == 0 ? -1 : 1)) notify(); };
            addAndMakeVisible (b);
        }
        refresh();
    }

    void resized() override
    {
        for (int point = 0; point < 3; ++point)
            for (int k = 0; k < 4; ++k)
                nudges[point * 4 + k]->setBounds (8 + point * 150 + k * 33, 66, 29, 22);
        int x = 456;
        for (auto* c : stepChips) { c->setBounds (x, 40, c->idealWidth(), 22); x += c->idealWidth() + 4; }
        tempoKeys[0]->setBounds (tempoX + 6, 66, 40, 22);
        tempoKeys[1]->setBounds (tempoX + 50, 66, 40, 22);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setColour (Pal::lcdRule);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 1.0f);
        const auto p = s.pad (pad);
        const double clock = clockHz (s.grade(), s.pitchPercent());
        const char* names[] { "START", "LOOP", "END" };
        const int at[] { p.start, p.loopPoint, p.end };
        for (int point = 0; point < 3; ++point)
        {
            const bool off = point == 1 && p.loop != Loop::partialLoop;
            const auto c = point == 1 ? Pal::lamp : Pal::lcdText;
            text (g, names[point], { 8 + point * 150, 8, 140, 12 }, (off ? Pal::lcdDim : c).withAlpha (off ? 0.5f : 1.0f), 9.0f);
            text (g, juce::String (at[point] / clock, 3) + " S", { 8 + point * 150, 24, 140, 20 }, (off ? Pal::lcdDim : c).withAlpha (off ? 0.5f : 1.0f), 15.0f);
        }
        text (g, "STEP", { 456, 8, 100, 12 }, Pal::lcdDim, 9.0f);
        text (g, "< > ONE STEP, << >> TEN", { 456, 66, 160, 11 }, Pal::lcdDim, 8.0f);
        text (g, "HOLD TO REPEAT, FASTER", { 456, 78, 160, 11 }, Pal::lcdDim, 8.0f);
        g.setColour (Pal::lcdRule);
        g.fillRect (tempoX - 8, 8, 1, getHeight() - 16);
        const auto t = s.tempo (pad);
        const int from = p.loop == Loop::partialLoop ? p.loopPoint : p.start;
        text (g, "LOOP TEMPO", { tempoX + 6, 8, 120, 12 }, Pal::lcdDim, 9.0f);
        text (g, s.loops (pad) ? juce::String ((p.end - from) / clock, 3) + " S LOOPED" : juce::String ("NOT LOOPED"),
              { tempoX, 8, getWidth() - tempoX - 10, 12 }, Pal::lcdDim, 9.0f, juce::Justification::centredRight);
        text (g, "BPM", { tempoX + 6, 30, 60, 12 }, Pal::lcdDim, 9.0f);
        text (g, "- +: THE END", { tempoX + 6, 44, 80, 11 }, Pal::lcdDim, 8.0f);
        text (g, t.shown ? t.text() : juce::String (s.loops (pad) ? "---" : ""), { tempoX + 40, 20, getWidth() - tempoX - 50, 42 },
              Pal::lcdText, 32.0f, juce::Justification::centredRight);
        text (g, "0.01 BPM A PRESS", { tempoX + 96, 66, 140, 11 }, Pal::lcdDim, 8.0f);
        text (g, "HOLD: REPEATS, FASTER", { tempoX + 96, 78, 140, 11 }, Pal::lcdDim, 8.0f);
    }

    static inline int stepMode = 1;   // 1 sample, 1 ms, 10 ms (kept while the app runs)
    static constexpr int tempoX = 640;

private:
    void refresh()
    {
        for (int i = 0; i < stepChips.size(); ++i) { stepChips[i]->chosen = i == stepMode; stepChips[i]->repaint(); }
        const bool partial = s.pad (pad).loop == Loop::partialLoop;
        for (int k = 4; k < 8; ++k) nudges[k]->setEnabled (partial);
        for (auto* b : tempoKeys) b->setEnabled (s.loops (pad));
    }
    void notify() { repaint(); if (onChange) onChange(); }
    void movePoint (int point, int n)
    {
        auto p = s.pad (pad);
        if (! p.used) return;
        const double clock = clockHz (s.grade(), s.pitchPercent());
        const int step = stepMode == 0 ? 1 : std::max (1, (int) std::lround (clock * (stepMode == 1 ? 0.001 : 0.01)));
        const int d = n * step;
        if (point == 0) { p.start = std::clamp (p.start + d, 0, p.end - 1); p.loopPoint = std::max (p.loopPoint, p.start); }
        else if (point == 1) p.loopPoint = std::clamp (p.loopPoint + d, p.start, p.end - 1);
        else { p.end = std::clamp (p.end + d, p.start + 1, p.frames); p.loopPoint = std::min (p.loopPoint, p.end - 1); }
        s.setPad (pad, p);
        notify();
    }

    Processor& s;
    int pad;
    std::function<void()> onChange;
    juce::OwnedArray<RepeatChip> nudges, tempoKeys;
    juce::OwnedArray<LcdChip> stepChips;
};

//==============================================================================
/** A pad's sample, with its start, loop and end points to drag. */
class PhraseSamplerPanel::Wave : public juce::Component
{
public:
    Wave (Processor& p, int index) : s (p), pad (index)
    {
        const auto audio = s.padAudio (pad);
        const auto info = s.pad (pad);
        frames = info.frames;
        const int ch = info.stereo ? 2 : 1;
        peaks.assign (600, { 0.0f, 0.0f });
        for (int f = 0; f < frames; ++f)
        {
            auto& pk = peaks[(size_t) (f * 600 / std::max (1, frames))];
            for (int c = 0; c < ch; ++c)
            {
                const float v = audio[(size_t) (f * ch + c)] / 32768.0f;
                pk.first = std::min (pk.first, v); pk.second = std::max (pk.second, v);
            }
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Pal::lcdRule);
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        auto plot = r.reduced (8.0f, 14.0f);
        if (frames <= 0)
        {
            text (g, "PAD EMPTY: RECORD OR IMPORT INTO IT ON THE SAMPLE TAB", getLocalBounds(), Pal::lcdDim, 10.0f, juce::Justification::centred);
            return;
        }
        const auto p = s.pad (pad);
        g.setColour (Pal::lcdDim.withAlpha (0.75f));
        for (int x = 0; x < (int) plot.getWidth(); ++x)
        {
            const auto& pk = peaks[(size_t) (x * 600 / (int) plot.getWidth())];
            const float top = plot.getCentreY() - pk.second * plot.getHeight() * 0.5f, bottom = plot.getCentreY() - pk.first * plot.getHeight() * 0.5f;
            g.fillRect (plot.getX() + (float) x, top, 1.0f, std::max (1.0f, bottom - top));
        }
        g.setColour (Pal::lcd.withAlpha (0.6f));
        g.fillRect (juce::Rectangle<float> (plot.getX(), r.getY() + 1.0f, xOf (p.start) - plot.getX(), r.getHeight() - 2.0f));
        g.fillRect (juce::Rectangle<float> (xOf (p.end), r.getY() + 1.0f, plot.getRight() - xOf (p.end), r.getHeight() - 2.0f));
        auto marker = [&] (int at, const juce::String& label, juce::Colour c, bool right)
        {
            const float x = xOf (at);
            g.setColour (c);
            g.fillRect (x - 0.75f, r.getY() + 2.0f, 1.5f, r.getHeight() - 4.0f);
            text (g, label, { (int) x + (right ? -64 : 4), (int) r.getY() + 2, 60, 12 }, c, 8.5f, right ? juce::Justification::centredRight : juce::Justification::centredLeft);
        };
        if (p.loop == Loop::partialLoop)
        {
            g.setColour (Pal::lamp.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> (xOf (p.loopPoint), r.getY() + 2.0f, xOf (p.end) - xOf (p.loopPoint), 2.0f));
            marker (p.loopPoint, "LOOP", Pal::lamp, false);
        }
        marker (p.start, "START", Pal::lcdText, false);
        marker (p.end, "END", Pal::lcdText, true);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto p = s.pad (pad);
        dragging = -1;
        if (frames <= 0) return;
        float best = 10.0f;
        const int at[] { p.start, p.loopPoint, p.end };
        for (int i = 0; i < 3; ++i)
        {
            if (i == 1 && p.loop != Loop::partialLoop) continue;
            const float d = std::abs (xOf (at[i]) - e.position.x);
            if (d < best) { best = d; dragging = i; }
        }
        if (dragging < 0) { s.playPad (pad, true); auditioning = true; }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        auto p = s.pad (pad);
        const int f = frameAt (e.position.x);
        if (dragging == 0) p.start = f; else if (dragging == 1) p.loopPoint = f; else p.end = f;
        s.setPad (pad, p);
        repaint();
        if (onChange) onChange();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (auditioning) s.playPad (pad, false);
        auditioning = false;
        dragging = -1;
    }

    std::function<void()> onChange;

private:
    juce::Rectangle<float> plotArea() const { return getLocalBounds().toFloat().reduced (8.0f, 14.0f); }
    float xOf (int frame) const { const auto p = plotArea(); return p.getX() + p.getWidth() * (float) frame / (float) std::max (1, frames); }
    int frameAt (float x) const { const auto p = plotArea(); return juce::jlimit (0, frames, juce::roundToInt ((x - p.getX()) / p.getWidth() * (float) frames)); }

    Processor& s;
    int pad, frames = 0, dragging = -1;
    bool auditioning = false;
    std::vector<std::pair<float, float>> peaks;
};

//==============================================================================
PhraseSamplerPanel::PhraseSamplerPanel (Processor& p, int& t) : sampler (p), tab (t)
{
    for (int i = 0; i < 5; ++i)
    {
        auto* c = tabChips.add (new LcdChip (tabNames[i]));
        c->textSize = 11.5f;
        c->onClick = [this, i] { tab = i; build(); };
        addAndMakeVisible (c);
    }
    startTimerHz (30);
}

PhraseSamplerPanel::~PhraseSamplerPanel() = default;

iw::Chips& PhraseSamplerPanel::chips (const juce::String& caption, juce::StringArray labels, std::function<int()> chosen, std::function<void (int)> pick)
{
    auto* c = new iw::Chips (caption, labels, std::move (chosen), std::move (pick));
    parts.add (c);
    chipRows.push_back (c);
    addAndMakeVisible (c);
    return *c;
}

iw::Chips& PhraseSamplerPanel::paramChips (const juce::String& id, const juce::String& caption, juce::StringArray labels)
{
    auto* p = sampler.param (id);
    return chips (caption, labels,
                  [p] { return juce::roundToInt (p->convertFrom0to1 (p->getValue())); },
                  [p] (int i) { p->beginChangeGesture(); p->setValueNotifyingHost (p->convertTo0to1 ((float) i)); p->endChangeGesture(); });
}

iw::Bar& PhraseSamplerPanel::bar (const juce::String& id, const juce::String& caption, std::function<juce::String (float)> fmt)
{
    auto* b = new iw::Bar (*sampler.param (id), caption, std::move (fmt));
    parts.add (b);
    live.push_back (b);
    addAndMakeVisible (b);
    return *b;
}

LcdChip& PhraseSamplerPanel::button (const juce::String& label, std::function<void()> click)
{
    auto* c = new LcdChip (label);
    c->textSize = 11.0f;
    c->onClick = std::move (click);
    parts.add (c);
    addAndMakeVisible (c);
    return *c;
}

void PhraseSamplerPanel::setRecord (std::function<void (Processor::RecordSettings&)> change)
{
    auto r = sampler.recordSettings();
    change (r);
    sampler.setRecordSettings (r);
}

void PhraseSamplerPanel::build()
{
    parts.clear();
    chipRows.clear();
    dimRows.clear();
    live.clear();
    notes.clear();
    recordButton = nullptr;
    meterArea = memoryArea = headArea = songArea = confirmArea = {};
    builtTab = tab;
    builtPad = sampler.editorPad;
    builtPadFrames = sampler.pad (sampler.editorPad).frames;
    for (int i = 0; i < tabChips.size(); ++i) tabChips[i]->chosen = i == tab;

    auto area = getLocalBounds().withTrimmedTop (chipH + 10);
    const int w = area.getWidth();
    int y = area.getY();
    auto placeChips = [&] (iw::Chips& c) { const int h = c.layout (w); c.setBounds (area.getX(), y, w, h); y += h + gapY - 2; };
    auto placeBar = [&] (juce::Component& b) { b.setBounds (area.getX(), y, w, rowH); y += rowH + gapY; };
    auto add = [this] (ValueBar* b) { parts.add (b); live.push_back (b); addAndMakeVisible (b); return b; };
    auto confirmed = [this] (int what, const juce::String& question) -> bool
    {
        // a second click within three seconds does it
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (deleteArmedWhat == what && now < deleteArmedUntil) { deleteArmedWhat = -1; confirmText.clear(); repaint(); return true; }
        deleteArmedWhat = what; deleteArmedUntil = now + 3000.0;
        confirmText = question;
        repaint();
        return false;
    };
    juce::StringArray nums;
    for (int i = 1; i <= padsPerBank; ++i) nums.add (juce::String (i));

    switch (tab)
    {
        case 0:   // PLAY
        {
            auto* f = new FrontView (sampler);
            parts.add (f);
            live.push_back (f);
            addAndMakeVisible (f);
            f->setBounds (area.withHeight (std::min (area.getHeight(), 436)));
            break;
        }
        case 1:   // SAMPLE
        {
            // the sources: this track's own input, the other tracks' outputs, the mix, a file
            const int own = sampler.ownTrack();
            juce::StringArray labels { "THIS TRACK IN" };
            std::vector<int> trackOf { -1 };
            for (int t = 0; t < 4; ++t) if (t != own) { labels.add ("TRACK " + juce::String (t + 1) + " OUT"); trackOf.push_back (t); }
            labels.add ("MIX");
            labels.add (utf ("FILE\xe2\x80\xa6"));
            const int mixAt = labels.size() - 2, fileAt = labels.size() - 1;
            placeChips (chips ("SOURCE", labels,
                               [this, trackOf, mixAt]
                               {
                                   const auto r = sampler.recordSettings();
                                   if (r.source == Source::mix) return mixAt;
                                   if (r.source == Source::track) for (int k = 1; k < (int) trackOf.size(); ++k) if (trackOf[(size_t) k] == r.track) return k;
                                   return 0;
                               },
                               [this, trackOf, mixAt, fileAt] (int i)
                               {
                                   if (i == fileAt) { importFile(); return; }
                                   setRecord ([i, trackOf, mixAt] (auto& r)
                                   {
                                       if (i == mixAt) r.source = Source::mix;
                                       else if (i == 0) r.source = Source::input;
                                       else { r.source = Source::track; r.track = trackOf[(size_t) i]; }
                                   });
                                   build();
                               }));
            placeChips (chips ("TAKEN", { "BEFORE THE FADER", "AFTER THE FADER" }, [this] { return sampler.recordSettings().postFader ? 1 : 0; },
                               [this] (int i) { setRecord ([i] (auto& r) { r.postFader = i == 1; }); }));
            placeChips (paramChips ("grade", "GRADE (ALL PADS)", { "HI", "STD", "LONG", "EXLG" }));
            placeChips (chips ("FORMAT", { "STEREO", "MONO" }, [this] { return sampler.recordSettings().stereo ? 0 : 1; },
                               [this] (int i) { setRecord ([i] (auto& r) { r.stereo = i == 0; }); }));
            placeChips (chips ("START", { "MANUAL", "LEVEL 1", "2", "3", "4", "5", "6", "7" }, [this] { return sampler.recordSettings().trigger; },
                               [this] (int i) { setRecord ([i] (auto& r) { r.trigger = i; }); }));
            placeChips (chips ("NEW SAMPLES LOOP", { "ONE SHOT", "WHOLE LOOP", "PARTIAL LOOP", "REVERSE SHOT", "REVERSE LOOP" },
                               [this] { return (int) sampler.recordSettings().defaultLoop; },
                               [this] (int i) { setRecord ([i] (auto& r) { r.defaultLoop = (Loop) i; }); }));
            placeBar (*add (new ValueBar ("GAIN", 1, 16, [this] { return sampler.recordSettings().gain; },
                                          [this] (int v) { setRecord ([v] (auto& r) { r.gain = v; }); },
                                          [] (int v) { return juce::String (v) + " OF 16" + (v == 8 ? " (UNITY)" : ""); })));
            meterArea = { area.getX(), y, w, rowH };
            y += rowH + gapY;
            placeChips (chips ("TO BANK", { "A", "B", "C", "D" }, [this] { return sampler.recordSettings().toPad / padsPerBank; },
                               [this] (int i) { setRecord ([i] (auto& r) { r.toPad = i * padsPerBank + r.toPad % padsPerBank; }); }));
            auto& toPad = chips ("TO PAD", nums, [this] { return sampler.recordSettings().toPad % padsPerBank; },
                                 [this] (int i) { setRecord ([i] (auto& r) { r.toPad = (r.toPad / padsPerBank) * padsPerBank + i; }); });
            placeChips (toPad);
            dimRows.push_back ({ &toPad, [this] (int i) { return sampler.pad ((sampler.recordSettings().toPad / padsPerBank) * padsPerBank + i).used; } });
            note ({ area.getX() + captionW, y - 4, w - captionW, 12 }, "DIMMED: THE PAD ALREADY HOLDS A SAMPLE");
            y += 10;
            memoryArea = { area.getX(), y + 2, w, 58 };
            y += 66;
            auto& rec = button ("RECORD", [this]
            {
                const auto st = sampler.panel();
                if (st.armed || st.recording) { sampler.stopRecord(); return; }
                juce::String error;
                if (! sampler.armRecord (error)) say (error);
            });
            rec.setBounds (area.getX(), y, 130, chipH + 4);
            recordButton = &rec;
            button (utf ("IMPORT FILE\xe2\x80\xa6"), [this] { importFile(); }).setBounds (area.getX() + 140, y, 130, chipH + 4);
            y += chipH + 12;
            note ({ area.getX(), y, w, 14 }, "A FILE OR A TRACK IS RECORDED AS IF THROUGH THE INPUT: AT THE GRADE, THROUGH ITS FILTER, AS ADPCM, INTO THE SAME MEMORY.");
            note ({ area.getX(), y + 16, w, 14 }, "ON THE PANEL: REC, A PAD, ENTER; < > THROUGH THE SAME SETTINGS; PLAY STARTS, STOP ENDS.");
            break;
        }
        case 2:   // EDIT
        {
            const int index = sampler.editorPad;
            placeChips (chips ("BANK", { "A", "B", "C", "D" }, [this] { return sampler.editorPad / padsPerBank; },
                               [this] (int i) { sampler.editorPad = i * padsPerBank + sampler.editorPad % padsPerBank; build(); }));
            auto& padRow = chips ("PAD", nums, [this] { return sampler.editorPad % padsPerBank; },
                                  [this] (int i) { sampler.editorPad = (sampler.editorPad / padsPerBank) * padsPerBank + i; build(); });
            placeChips (padRow);
            dimRows.push_back ({ &padRow, [this] (int i) { return ! sampler.pad ((sampler.editorPad / padsPerBank) * padsPerBank + i).used; } });
            note ({ area.getX() + captionW, y - 4, w - captionW, 12 }, "DIMMED: EMPTY");
            y += 10;
            headArea = { area.getX(), y, w, 16 };
            y += 20;
            auto* wave = new Wave (sampler, index);
            parts.add (wave);
            addAndMakeVisible (wave);
            wave->setBounds (area.getX(), y, w, 100);
            y += 106;
            const auto p = sampler.pad (index);
            if (! p.used)
            {
                placeBar (*add (new ValueBar ("MIDI NOTE", -1, 127, [this, index] { return sampler.pad (index).note; },
                                              [this, index] (int v) { auto q = sampler.pad (index); q.note = v; sampler.setPad (index, q); }, noteName)));
                break;
            }
            auto* fine = new FineTune (sampler, index, [this, wave] { wave->repaint(); repaint (headArea); });
            parts.add (fine);
            live.push_back (fine);
            addAndMakeVisible (fine);
            fine->setBounds (area.getX(), y, w, 96);
            wave->onChange = [this, fine] { fine->repaint(); repaint (headArea); };
            y += 102;
            auto setPad = [this, index] (std::function<void (Pad&)> change) { auto q = sampler.pad (index); change (q); sampler.setPad (index, q); build(); };
            placeChips (chips ("LOOP", { "ONE SHOT", "WHOLE LOOP", "PARTIAL LOOP", "REVERSE SHOT", "REVERSE LOOP" },
                               [this, index] { return (int) sampler.pad (index).loop; }, [setPad] (int i) { setPad ([i] (Pad& q) { q.loop = (Loop) i; }); }));
            placeChips (chips ("PAD PLAYS", { "NOTE (WHILE HELD)", "TOGGLE", "TRIGGER" },
                               [this, index] { return (int) sampler.pad (index).sounding; }, [setPad] (int i) { setPad ([i] (Pad& q) { q.sounding = (Sounding) i; }); }));
            placeChips (chips ("ALTERNATE GROUP", { "OFF", "1", "2", "3" },
                               [this, index] { return sampler.pad (index).group; }, [setPad] (int i) { setPad ([i] (Pad& q) { q.group = i; }); }));
            auto setQuiet = [this, index] (std::function<void (Pad&)> change) { auto q = sampler.pad (index); change (q); sampler.setPad (index, q); repaint(); };
            placeBar (*add (new ValueBar ("VOLUME", 0, 127, [this, index] { return sampler.pad (index).volume; },
                                          [setQuiet] (int v) { setQuiet ([v] (Pad& q) { q.volume = v; }); }, [] (int v) { return juce::String (v) + " OF 127"; })));
            placeBar (*add (new ValueBar ("MIDI NOTE", -1, 127, [this, index] { return sampler.pad (index).note; },
                                          [setQuiet] (int v) { setQuiet ([v] (Pad& q) { q.note = v; }); }, noteName)));
            int x = area.getX();
            auto act = [&] (const juce::String& label, std::function<void()> f)
            {
                auto& b = button (label, std::move (f));
                b.setBounds (x, y, b.idealWidth() + 8, chipH + 2);
                x += b.idealWidth() + 14;
            };
            act (utf ("COPY TO\xe2\x80\xa6"), [this, index] { choosePad ("COPY", [this, index] (int to) { juce::String e; if (! sampler.copyPad (index, to, e)) say (e); else { sampler.editorPad = to; build(); } }); });
            act (utf ("MOVE TO\xe2\x80\xa6"), [this, index] { choosePad ("MOVE", [this, index] (int to) { juce::String e; if (! sampler.movePad (index, to, e)) say (e); else { sampler.editorPad = to; build(); } }); });
            act (utf ("SPLIT AT END\xe2\x80\xa6"), [this, index] { choosePad ("SPLIT", [this, index] (int to) { juce::String e; if (! sampler.splitPad (index, to, e)) say (e); else build(); }); });
            act ("COMPACT", [this, index] { if (sampler.compactPad (index)) { say ("COMPACTED: THE MEMORY'S BACK"); build(); } else say ("NOTHING TO TRIM: MOVE THE START OR END IN FIRST"); });
            act ("DELETE " + Front::padName (index), [this, index, confirmed]
            {
                if (! confirmed (index, "DELETE " + Front::padName (index) + "? CLICK AGAIN TO DELETE")) return;
                sampler.deletePad (index);
                say ("DELETED " + Front::padName (index));
                build();
            });
            confirmArea = { x + 4, y, w - x, chipH + 2 };
            y += chipH + 12;
            note ({ area.getX(), y, w, 14 }, "DRAG THE MARKERS; CLICK ELSEWHERE ON THE SAMPLE TO HEAR IT. DELETE FREES THE PAD AND GIVES ITS MEMORY BACK.");
            note ({ area.getX(), y + 16, w, 14 }, "TEMPO: THE LOOPED PART, 80.00-159.99 BPM; WHOLE BEATS FOR REVERSE AND VERY LONG LOOPS (OVER 6 S); NONE UNDER 0.375 S.");
            note ({ area.getX(), y + 32, w, 14 }, "- AND + MOVE THE END TO MAKE THE LOOP 0.01 BPM FASTER OR SLOWER, TO THE NEAREST SAMPLE. HELD, THEY REPEAT, EVER FASTER.");
            break;
        }
        case 3:   // SONGS
        {
            for (int s = 0; s < numSongs; ++s)
            {
                const int rowY = y;
                int x = area.getX() + captionW + 270;
                auto act = [&] (const juce::String& label, std::function<void()> f)
                {
                    auto& b = button (label, std::move (f));
                    b.setBounds (x, rowY, b.idealWidth() + 8, chipH);
                    x += b.idealWidth() + 14;
                };
                const juce::String key = "SONG " + juce::String (s + 1);
                auto select = [this, key, s] { if (sampler.panel().songSelected != s) sampler.pressKey (key); };
                act ("PLAY", [this, select] { select(); sampler.pressKey ("PLAY"); });
                act ("RECORD", [this, select, s]
                {
                    if (sampler.songEvents (s) > 0) { say ("DELETE THE SONG FIRST: A SONG IS RECORDED IN ONE GO"); return; }
                    select(); sampler.pressKey ("REC");
                    say ("RECORDING STARTS AT THE FIRST PAD; STOP ENDS IT");
                });
                act ("STOP", [this] { sampler.pressKey ("STOP"); });
                act ("DELETE", [this, s, confirmed]
                {
                    if (! confirmed (100 + s, "DELETE SONG " + juce::String (s + 1) + "? CLICK AGAIN")) return;
                    sampler.deleteSong (s);
                    say ("SONG " + juce::String (s + 1) + " DELETED");
                });
                y += chipH + 10;
            }
            songArea = { area.getX(), area.getY(), w, y - area.getY() };
            y += 4;
            placeChips (chips ("REPEAT", { "OFF", "ON" }, [this] { return sampler.songRepeat() ? 1 : 0; }, [this] (int i) { sampler.setSongRepeat (i == 1); }));
            meterArea = { area.getX(), y, w, rowH };   // the song memory
            y += rowH + 8;
            confirmArea = { area.getX(), y, w, 14 };
            y += 18;
            note ({ area.getX(), y, w, 14 }, "TO RECORD: RECORD, THEN PLAY THE PADS (HERE, ON THE PLAY TAB OR FROM MIDI); STOP ENDS IT.");
            note ({ area.getX(), y + 16, w, 14 }, "A SONG KEEPS THE PADS, THEIR TIMING AND VELOCITY, REV AND HOLD: NOT THE RIBBON, AS ON THE ORIGINAL. 1000 EVENTS IN ALL.");
            note ({ area.getX(), y + 32, w, 14 }, "ON THE PANEL: SHIFT + SONG 1-4 CHOOSES; PLAY, STOP; TOP, BWD, FWD MOVE THROUGH IT; PLAY WITH HOLD ON REPEATS.");
            break;
        }
        case 4: default:   // SETUP
        {
            placeBar (bar ("pitch", "PITCH (THE CLOCK)", [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " %"; }));
            placeBar (bar ("volume", "VOLUME", [] (float v) { return juce::String (juce::roundToInt (v)) + " OF 127"; }));
            placeChips (paramChips ("filterType", "FILTER", { "LPF", "BPF", "HPF", "BYPASS" }));
            placeChips (paramChips ("resonance", "RESONANCE", { "FLAT", "WEAK", "STRONG" }));
            placeBar (bar ("channel", "MIDI CHANNEL", [] (float v) { return v < 0.5f ? juce::String ("ANY") : juce::String (juce::roundToInt (v)); }));
            placeChips (paramChips ("ribbonSource", "RIBBON FROM MIDI", { "OFF", "PITCH BEND", "A CONTROLLER" }));
            placeBar (bar ("ribbonCc", "RIBBON CONTROLLER", [] (float v) { return "CC " + juce::String (juce::roundToInt (v)); }));
            auto& reset = button (utf ("PADS ON C1\xe2\x80\x93" "B4"), [this]
            {
                for (int i = 0; i < numPads; ++i) { auto q = sampler.pad (i); q.note = 36 + i; sampler.setPad (i, q); }
                say ("THE 48 PADS ON C1-B4 AGAIN");
            });
            reset.setBounds (area.getX() + captionW, y, reset.idealWidth() + 8, chipH);
            note ({ area.getX(), y, captionW, chipH }, "MIDI NOTES");
            note ({ area.getX() + captionW + reset.idealWidth() + 20, y, w, chipH }, "EACH PAD'S OWN NOTE IS ON THE EDIT TAB");
            y += chipH + 10;
            note ({ area.getX(), y, captionW, chipH }, "MEMORY");
            auto& all = button ("DELETE ALL SAMPLES", [this, confirmed]
            {
                if (! confirmed (200, "EVERY SAMPLE AND SONG GOES: CLICK AGAIN")) return;
                sampler.deleteAllSamples();
                say ("ALL SAMPLES AND SONGS DELETED");
            });
            all.setBounds (area.getX() + captionW, y, all.idealWidth() + 8, chipH);
            auto& init = button ("RESET EVERYTHING", [this, confirmed]
            {
                if (! confirmed (201, "SAMPLES, SONGS AND SETTINGS, AS IT CAME: CLICK AGAIN")) return;
                sampler.resetEverything();
                say ("RESET: AS IT CAME");
                build();
            });
            init.setBounds (all.getRight() + 10, y, init.idealWidth() + 8, chipH);
            confirmArea = { init.getRight() + 12, y, w - init.getRight() - 12, chipH };
            note (confirmArea, "THE UNIT'S INIT DATA AND INIT ALL");
            y += chipH + 14;
            note ({ area.getX(), y, w, 14 }, "IN EXT FILTER, PADS 1-4 CHOOSE THE FILTER AND 7-9 ITS RESONANCE, AS ON THE ORIGINAL (IN FILTER, WITH SHIFT HELD).");
            note ({ area.getX(), y + 16, w, 14 }, "PITCH MOVES THE SAMPLE CLOCK, RECORDING AND PLAYING: HI AT +10 % IS 44.1 KHZ.");
            note ({ area.getX(), y + 32, w, 14 }, "ON THE PANEL: SHIFT + UTILITY; < > THROUGH THE MIDI SETTINGS, SONG DELETE, INIT DATA AND INIT ALL.");
            break;
        }
    }
    for (auto& [row, dimmed] : dimRows)
        for (int i = 0; i < row->chips.size(); ++i) row->chips[i]->dim = dimmed (i);
    repaint();
}

void PhraseSamplerPanel::importFile()
{
    const auto r = sampler.recordSettings();
    if (sampler.pad (r.toPad).used) { say (padName (r.toPad) + " ISN'T EMPTY: CHOOSE ANOTHER PAD, OR DELETE IT"); return; }
    chooser = std::make_unique<juce::FileChooser> ("A sound into " + padName (r.toPad), juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    juce::Component::SafePointer<PhraseSamplerPanel> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe, r] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File()) return;
        juce::String error;
        if (safe->sampler.importFile (fc.getResult(), r.toPad, error))
        {
            safe->sampler.editorPad = r.toPad;
            safe->say (padName (r.toPad) + ": " + fc.getResult().getFileName().toUpperCase() + (error.isNotEmpty() ? " (" + error + ")" : juce::String()));
        }
        else safe->say (error);
        safe->build();
    });
}

void PhraseSamplerPanel::choosePad (const juce::String& what, std::function<void (int)> then)
{
    juce::PopupMenu m;
    m.addSectionHeader (what + " TO AN EMPTY PAD");
    for (int b = 0; b < banks; ++b)
    {
        juce::PopupMenu sub;
        for (int i = 0; i < padsPerBank; ++i)
        {
            const int index = b * padsPerBank + i;
            sub.addItem (padName (index), ! sampler.pad (index).used, false, [then, index] { then (index); });
        }
        m.addSubMenu ("BANK " + juce::String::charToString ((juce::juce_wchar) ('A' + b)), sub);
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

void PhraseSamplerPanel::resized()
{
    int x = 0;
    const int w = (getWidth() - 4 * 7) / 5;
    for (auto* c : tabChips) { c->setBounds (x, 0, w, chipH + 2); x += w + 7; }
    build();
}

void PhraseSamplerPanel::timerCallback()
{
    const auto info = sampler.pad (sampler.editorPad);
    if (builtTab != tab || (tab == 2 && (builtPad != sampler.editorPad || builtPadFrames != info.frames))) { build(); return; }
    for (auto* c : chipRows) c->refresh();
    for (auto& [row, dimmed] : dimRows)
        for (int i = 0; i < row->chips.size(); ++i)
            if (row->chips[i]->dim != dimmed (i)) { row->chips[i]->dim = dimmed (i); row->chips[i]->repaint(); }
    for (auto* l : live) l->repaint();
    if (confirmText.isNotEmpty() && juce::Time::getMillisecondCounterHiRes() > deleteArmedUntil) { confirmText.clear(); repaint(); }
    if (tab == 1)
    {
        const float pk = sampler.takeInputPeak();
        meterShown = std::max (pk, meterShown * 0.85f);
        if (pk >= meterHeld || --meterHold <= 0) { meterHeld = pk; meterHold = 45; }
        const auto st = sampler.panel();
        if (recordButton != nullptr)
        {
            const auto label = st.armed || st.recording ? juce::String ("STOP") : juce::String ("RECORD");
            if (recordButton->getButtonText() != label) recordButton->setButtonText (label);
            recordButton->chosen = st.armed || st.recording;
            recordButton->repaint();
        }
        repaint();
    }
    if (tab == 2 || tab == 3) repaint();
}

void PhraseSamplerPanel::paint (juce::Graphics& g)
{
    for (auto& [r, t] : notes)
        if (t.isNotEmpty() && ! (confirmText.isNotEmpty() && r == confirmArea)) text (g, t, r, Pal::lcdDim.withAlpha (0.75f), 9.5f);
    if (confirmText.isNotEmpty() && ! confirmArea.isEmpty()) text (g, confirmText, confirmArea, Pal::lamp, 9.5f);
    const juce::String dot = utf (" \xc2\xb7 ");

    if (! meterArea.isEmpty() && tab == 1)
    {
        // the input after the gain: 24 segments, 3 dB each, the top four for the last 6 dB before clipping
        auto r = meterArea;
        text (g, "INPUT", r.removeFromLeft (captionW), Pal::lcdDim);
        const auto st = sampler.panel();
        const auto rs = sampler.recordSettings();
        const juce::String from = rs.source == Source::track ? "TRACK " + juce::String (rs.track + 1) + " OUT" + (rs.postFader ? ", AFTER ITS FADER" : ", BEFORE ITS FADER")
                                : rs.source == Source::mix ? juce::String ("THE MIX") + (rs.postFader ? ", AFTER THE FADERS" : ", BEFORE THE FADERS")
                                                           : juce::String ("THIS TRACK'S INPUT");
        text (g, st.clipped ? juce::String ("CLIPPED") : from, r.removeFromRight (valueW + 40), st.clipped ? Pal::red : Pal::lcdDim, 9.0f, juce::Justification::centredRight);
        const int n = 24;
        r.removeFromRight (8);
        const float segW = ((float) r.getWidth() - 4.0f) / (float) n;
        const float db = meterShown > 1.0e-6f ? 20.0f * std::log10 (meterShown) : -100.0f;
        const float hdb = meterHeld > 1.0e-6f ? 20.0f * std::log10 (meterHeld) : -100.0f;
        for (int i = 0; i < n; ++i)
        {
            const float top = -72.0f + 3.0f * (float) (i + 1);
            const bool on = db >= top - 3.0f, held = hdb >= top - 3.0f && hdb < top;
            const auto c = i >= n - 2 ? Pal::red : (i >= n - 6 ? Pal::lamp : Pal::green);
            g.setColour (on || held ? c : Pal::lcdRule);
            g.fillRect (juce::Rectangle<float> ((float) r.getX() + (float) i * segW, (float) r.getCentreY() - 5.0f, segW - 2.0f, 10.0f));
        }
    }

    if (! memoryArea.isEmpty())
    {
        auto r = memoryArea;
        g.setColour (Pal::lcdRule);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 1.0f);
        const auto grade = sampler.grade();
        const double total = memoryNibbles / clockHz (grade, sampler.pitchPercent());
        text (g, "MEMORY" + dot + "384 KB, SHARED BY ALL 48 PADS" + dot + "AT " + gradeName (grade) + ": " + secs (total) + " MONO IN ALL",
              r.reduced (8, 4).removeFromTop (12), Pal::lcdDim, 9.0f);
        auto strip = r.reduced (8, 0).withTrimmedTop (20).withHeight (22);
        g.setColour (Pal::lcdRule.withAlpha (0.5f));
        g.fillRect (strip);
        const int to = sampler.recordSettings().toPad;
        for (int i = 0; i < numPads; ++i)
        {
            const auto p = sampler.pad (i);
            if (! p.used) continue;
            const int len = p.frames * (p.stereo ? 2 : 1);
            const float x0 = (float) strip.getX() + (float) strip.getWidth() * (float) p.offset / (float) memoryNibbles;
            const float x1 = (float) strip.getX() + (float) strip.getWidth() * (float) (p.offset + len) / (float) memoryNibbles;
            g.setColour (i == sampler.editorPad ? Pal::lamp : Pal::lcdText);
            g.fillRect (juce::Rectangle<float> (x0, (float) strip.getY(), std::max (1.0f, x1 - x0 - 1.0f), (float) strip.getHeight()));
            if (x1 - x0 > 26.0f) text (g, padName (i), juce::Rectangle<float> (x0 + 3.0f, (float) strip.getY(), x1 - x0 - 4.0f, (float) strip.getHeight()).toNearestInt(), Pal::lcd, 8.5f);
        }
        text (g, secs (sampler.usedSeconds()) + " USED" + dot + secs (sampler.freeSeconds()) + " FREE" + dot + "RECORDING INTO " + padName (to)
                 + (sampler.pad (to).used ? juce::String (" (NOT EMPTY)") : juce::String()),
              r.reduced (8, 4).removeFromBottom (12), Pal::lcdText, 9.0f, juce::Justification::centredRight);
    }

    if (tab == 1 && recordButton != nullptr)
    {
        // the record status beside the buttons
        const auto st = sampler.panel();
        const auto r = sampler.recordSettings();
        juce::String t;
        if (st.armed) t = "ARMED: WAITS FOR LEVEL " + juce::String (r.trigger) + ", THEN RECORDS UNTIL STOP OR THE MEMORY RUNS OUT";
        else if (st.recording) t = "RECORDING " + secs (st.recordSeconds) + " INTO " + padName (r.toPad) + ": STOP ENDS IT";
        else t = r.trigger == 0 ? "MANUAL: RECORDING STARTS AT ONCE" : "LEVEL " + juce::String (r.trigger) + ": RECORD ARMS IT, THE SOUND STARTS IT";
        text (g, t, recordButton->getBounds().withX (recordButton->getRight() + 156).withWidth (getWidth() - recordButton->getRight() - 156),
              st.recording ? Pal::lamp : Pal::lcdDim, 9.5f);
    }

    if (! headArea.isEmpty())
    {
        const int index = sampler.editorPad;
        const auto p = sampler.pad (index);
        text (g, "PAD " + padName (index) + (p.used ? dot + p.name : juce::String (dot + "EMPTY")), headArea, Pal::lcdText, 12.0f);
        if (p.used)
        {
            const double len = (p.end - p.start) / clockHz (sampler.grade(), sampler.pitchPercent());
            juce::String info = juce::String (p.stereo ? "STEREO" : "MONO") + dot + "MADE AT " + gradeName (p.grade) + dot + secs (len);
            text (g, info, headArea, Pal::lcdDim, 10.0f, juce::Justification::centredRight);
        }
    }

    if (! songArea.isEmpty())
    {
        const auto st = sampler.panel();
        for (int s = 0; s < numSongs; ++s)
        {
            const juce::Rectangle<int> row (songArea.getX(), songArea.getY() + s * (chipH + 10), songArea.getWidth(), chipH);
            const bool on = st.songSelected == s;
            text (g, "SONG " + juce::String (s + 1), row.withWidth (captionW), on ? Pal::lcdText : Pal::lcdDim);
            juce::String t = sampler.songEvents (s) == 0 ? juce::String ("EMPTY")
                                                         : juce::String (sampler.songEvents (s)) + " EVENTS" + dot + secs (sampler.songLength (s));
            if (on && st.songPlaying) t << dot << "PLAYING " << secs (st.songSeconds);
            if (on && st.songRecording) t << dot << "RECORDING";
            text (g, t, row.withX (row.getX() + captionW).withWidth (262), on && (st.songPlaying || st.songRecording) ? Pal::lamp : Pal::lcdText, 10.0f);
        }
        if (! meterArea.isEmpty())
        {
            auto r = meterArea;
            const int used = sampler.songEventsTotal();
            text (g, "SONG MEMORY", r.removeFromLeft (captionW), Pal::lcdDim);
            text (g, juce::String (used) + " OF " + juce::String (songEventLimit) + " EVENTS", r.removeFromRight (valueW), Pal::lcdText, 11.0f, juce::Justification::centredRight);
            auto b = r.reduced (2, 0).withSizeKeepingCentre (r.getWidth() - 4, 6).toFloat();
            g.setColour (Pal::lcdRule);
            g.fillRoundedRectangle (b, 3.0f);
            g.setColour (Pal::lcdText);
            g.fillRoundedRectangle (b.withWidth (b.getWidth() * (float) used / (float) songEventLimit), 3.0f);
        }
    }
}
} // namespace tf
