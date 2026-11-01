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
/** The front panel, as on the unit: LCD and meters, effect keys, the ribbon, the keys and pads, the transport. */
class PhraseSamplerPanel::Front : public juce::Component
{
public:
    Front (Processor& p, int& editorPad) : s (p), padForEdit (editorPad) {}

    struct Key { juce::Rectangle<float> r; juce::String label, name; int pad = -1; };

    void resized() override
    {
        keys.clear();
        const float w = (float) getWidth();
        lcd = { 16.0f, 14.0f, 360.0f, 100.0f };
        readouts = { 392.0f, 14.0f, 190.0f, 100.0f };
        const char* fx[] { "SCALE", "FILTER", "CROSSFADER", "PITCH", "EXT SCRATCH", "EXT FILTER" };
        const char* fxLabel[] { "SCALE", "FILTER", "CROSS FADER", "PITCH", "EXT SCRATCH", "EXT FILTER" };
        const float fxW = (w - 16.0f - 598.0f - 12.0f) / 3.0f;
        for (int i = 0; i < 6; ++i)
            keys.push_back ({ { 598.0f + (float) (i % 3) * (fxW + 6.0f), 14.0f + (float) (i / 3) * 52.0f, fxW, 46.0f }, fxLabel[i], fx[i] });
        ribbon = { 16.0f, 126.0f, w - 32.0f, 38.0f };
        const char* left[] { "SHIFT", "SCRATCH", "HOLD", "REV", "BANK A", "BANK B", "BANK C", "BANK D" };
        for (int i = 0; i < 8; ++i)
            keys.push_back ({ { 16.0f + (float) (i % 4) * 64.0f, 196.0f + (float) (i / 4) * 44.0f, 58.0f, 38.0f }, left[i], left[i] });
        const float padX = 286.0f, padW = (w - 16.0f - padX - 5.0f * 8.0f) / 6.0f;
        for (int i = 0; i < 12; ++i)
            keys.push_back ({ { padX + (float) (i % 6) * (padW + 8.0f), 196.0f + (float) (i / 6) * 80.0f, padW, 72.0f }, juce::String (i + 1), {}, i });
        const char* transport[] { "REC", "TOP", "BWD", "STOP", "PLAY", "FWD" };
        for (int i = 0; i < 6; ++i)
            keys.push_back ({ { 16.0f + (float) i * 60.0f, 366.0f, 54.0f, 36.0f }, transport[i], transport[i] });
        for (int i = 0; i < 4; ++i)
            keys.push_back ({ { 400.0f + (float) i * 60.0f, 366.0f, 54.0f, 36.0f }, "SONG " + juce::String (i + 1), "SONG " + juce::String (i + 1) });
    }

    bool lit (const Key& k, const PanelState& st) const
    {
        const auto& n = k.name;
        if (n == "SHIFT") return st.shift;
        if (n == "SCRATCH") return st.scratchPad >= 0 || st.scratchArmed;
        if (n == "HOLD") return st.hold;
        if (n == "REV") return st.reverse;
        if (n.startsWith ("BANK")) return st.bank == n.getLastCharacter() - 'A';
        if (n.startsWith ("SONG")) return st.songSelected == n.getTrailingIntValue() - 1 && (! st.songPlaying || (juce::Time::getMillisecondCounter() / 250) % 2 == 0);
        if (n == "REC") return st.armed || st.recording || st.songRecording;
        if (n == "PLAY") return st.songPlaying;
        const Effect fx[] { Effect::scale, Effect::filter, Effect::crossfade, Effect::pitch, Effect::extScratch, Effect::extFilter };
        const char* names[] { "SCALE", "FILTER", "CROSSFADER", "PITCH", "EXT SCRATCH", "EXT FILTER" };
        for (int i = 0; i < 6; ++i) if (n == names[i]) return st.effect == fx[i];
        return false;
    }

    void paint (juce::Graphics& g) override
    {
        auto& lf = DeckLookAndFeel::get();
        const auto st = s.panel();
        auto body = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (caseTop);
        g.fillRoundedRectangle (body, 8.0f);
        g.setColour (caseEdge);
        g.drawRoundedRectangle (body, 8.0f, 1.5f);

        // ---- the LCD
        g.setColour (lcdGlass);
        g.fillRoundedRectangle (lcd, 4.0f);
        auto in = lcd.reduced (12.0f, 8.0f).toNearestInt();
        auto line = [&] (const juce::String& t, juce::Rectangle<int> r, float size)
        {
            g.setColour (lcdInk);
            g.setFont (lf.mono (size));
            g.drawText (t, r, juce::Justification::centredLeft, true);
        };
        line (s.lcdLine (0), in.removeFromTop (18), 11.0f);
        line (s.lcdLine (1), in.removeFromTop (34), 17.0f);
        auto third = in.removeFromTop (22);
        const auto l2 = s.lcdLine (2);
        line (l2, third.removeFromLeft (150), 10.0f);
        if (st.effect == Effect::filter || st.effect == Effect::extFilter || st.effect == Effect::pitch || st.effect == Effect::crossfade || st.scratchPad >= 0)
        {
            auto b = third.withSizeKeepingCentre (third.getWidth(), 8).toFloat();
            g.setColour (lcdInk);
            g.drawRect (b, 1.0f);
            g.fillRect (b.reduced (2.0f).withWidth ((b.getWidth() - 4.0f) * s.ribbonPosition()));
        }

        // ---- memory, voices, input
        auto ro = readouts.toNearestInt();
        auto row = [&] (const juce::String& caption, float frac, const juce::String& value)
        {
            auto r = ro.removeFromTop (26);
            text (g, caption, r.removeFromTop (12), Pal::faint, 9.5f);
            if (value.isNotEmpty()) text (g, value, r.withY (r.getY() - 12).withHeight (12), Pal::cream, 9.5f, juce::Justification::centredRight);
            auto b = r.withSizeKeepingCentre (r.getWidth(), 8).toFloat();
            g.setColour (Pal::dark);
            g.fillRoundedRectangle (b, 3.0f);
            g.setColour (Pal::lcdText);
            g.fillRoundedRectangle (b.withWidth (b.getWidth() * juce::jlimit (0.0f, 1.0f, frac)), 3.0f);
        };
        const double total = memoryNibbles / clockHz (s.grade(), s.pitchPercent());
        row ("MEMORY", (float) (s.usedSeconds() / total), juce::String (s.usedSeconds(), 1) + " / " + juce::String (total, 1) + " S");
        {
            auto r = ro.removeFromTop (26);
            text (g, "VOICES", r.removeFromTop (12), Pal::faint, 9.5f);
            for (int v = 0; v < 4; ++v)
            {
                const auto dot = juce::Rectangle<float> ((float) r.getX() + (float) v * 16.0f, (float) r.getCentreY() - 5.0f, 10.0f, 10.0f);
                g.setColour (v >= st.maxVoices ? Pal::dark : (v < st.voices ? Pal::lamp : ledOff));
                g.fillEllipse (dot);
            }
            text (g, juce::String (st.voices) + " OF " + juce::String (st.maxVoices), r, Pal::faint, 9.0f, juce::Justification::centredRight);
        }
        inputLevel = std::max (inputLevel * 0.85f, s.takeInputPeak());
        const float idb = inputLevel > 1.0e-5f ? 20.0f * std::log10 (inputLevel) : -100.0f;
        row ("INPUT", (idb + 48.0f) / 48.0f, {});

        // ---- the ribbon
        g.setColour (Pal::darker);
        g.fillRoundedRectangle (ribbon, 5.0f);
        g.setColour (Pal::line);
        g.drawRoundedRectangle (ribbon, 5.0f, 1.0f);
        const float rx = ribbon.getX() + 14.0f + (ribbon.getWidth() - 28.0f) * s.ribbonPosition();
        g.setColour (touching ? Pal::cream : Pal::cream.withAlpha (0.55f));
        g.fillEllipse (rx - 10.0f, ribbon.getCentreY() - 10.0f, 20.0f, 10.0f * 2.0f);
        juce::String help = "RIBBON";
        switch (st.effect)
        {
            case Effect::filter: case Effect::extFilter: help << " \xc2\xb7 DRAG: FILTER CUTOFF"; break;
            case Effect::pitch: help << " \xc2\xb7 DRAG: PITCH -20 % TO +10 %"; break;
            case Effect::crossfade: help << " \xc2\xb7 DRAG: SAMPLES TO INPUT"; break;
            case Effect::extScratch: help << " \xc2\xb7 HOLD AND DRAG: SCRATCH WHAT JUST CAME IN"; break;
            case Effect::scale: help << " \xc2\xb7 SCALE: THE PADS PLAY ONE SAMPLE IN SEMITONES"; break;
            case Effect::none: default: help << (st.scratchPad >= 0 ? " \xc2\xb7 HOLD AND DRAG: SCRATCH" : " \xc2\xb7 CHOOSE AN EFFECT, OR SCRATCH AND A PAD"); break;
        }
        text (g, utf (help.toRawUTF8()), { (int) ribbon.getX(), (int) ribbon.getBottom() + 4, (int) ribbon.getWidth(), 14 }, Pal::faint, 9.0f, juce::Justification::centred);

        // ---- keys and pads
        for (auto& k : keys)
        {
            const bool down = &k == pressed;
            if (k.pad >= 0)
            {
                const int index = st.bank * padsPerBank + k.pad;
                const auto p = s.pad (index);
                const bool sounding = (st.padsSounding >> index) & 1;
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
            const bool hasLed = k.name != "TOP" && k.name != "BWD" && k.name != "FWD" && k.name != "STOP" && k.name != "EXIT";
            if (hasLed)
            {
                g.setColour (lit (k, st) ? (isRec ? Pal::red : Pal::lamp) : ledOff);
                g.fillEllipse (k.r.getCentreX() - 3.0f, k.r.getY() + 5.0f, 6.0f, 6.0f);
            }
            g.setColour (Pal::cream);
            g.setFont (lf.cond (k.r.getWidth() < 62.0f ? 11.5f : 12.5f, true));
            g.drawText (k.label, k.r.withTrimmedTop (hasLed ? 8.0f : 0.0f).toNearestInt(), juce::Justification::centred, true);
        }
        text (g, "PADS ON MIDI C1-B4", { (int) getWidth() - 236, 368, 220, 14 }, Pal::faint, 9.0f, juce::Justification::centredRight);
        text (g, "MOUSE ON A PAD: LOWER IS LOUDER", { (int) getWidth() - 236, 384, 220, 14 }, Pal::faint, 9.0f, juce::Justification::centredRight);
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
                    // the pads aren't velocity sensitive on the unit; here the lower the click, the louder
                    const float v = juce::jlimit (0.3f, 1.0f, 0.3f + 0.7f * (e.position.y - k.r.getY()) / k.r.getHeight() * 1.25f);
                    s.pressPad (k.pad, v);
                    padForEdit = s.panel().bank * padsPerBank + k.pad;
                }
                else s.pressKey (k.name);
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
        if (touching) { touching = false; s.setRibbon (ribbonAt (e.position.x), false); }
        if (pressed != nullptr)
        {
            if (pressed->pad >= 0) s.releasePad (pressed->pad);
            else s.releaseKey (pressed->name);
        }
        pressed = nullptr;
        repaint();
    }

private:
    float ribbonAt (float x) const { return juce::jlimit (0.0f, 1.0f, (x - ribbon.getX() - 14.0f) / (ribbon.getWidth() - 28.0f)); }

    Processor& s;
    int& padForEdit;
    std::vector<Key> keys;
    juce::Rectangle<float> lcd, readouts, ribbon;
    const Key* pressed = nullptr;
    bool touching = false;
    float inputLevel = 0.0f;
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
    live.clear();
    notes.clear();
    recordButton = nullptr;
    meterArea = memoryArea = headArea = songArea = {};
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

    switch (tab)
    {
        case 0:   // PLAY
        {
            auto* f = new Front (sampler, sampler.editorPad);
            parts.add (f);
            live.push_back (f);
            addAndMakeVisible (f);
            f->setBounds (area.withHeight (std::min (area.getHeight(), 414)));
            break;
        }
        case 1:   // SAMPLE
        {
            placeChips (chips ("SOURCE", { "TRACK INPUT", "TAPE: TRACK 1", "TAPE: TRACK 2", "TAPE: TRACK 3", "TAPE: TRACK 4", utf ("FILE\xe2\x80\xa6") },
                               [this] { const auto r = sampler.recordSettings(); return r.source == Source::tape ? 1 + r.tapeTrack : 0; },
                               [this] (int i)
                               {
                                   if (i == 5) { importFile(); return; }
                                   setRecord ([i] (auto& r) { r.source = i == 0 ? Source::input : Source::tape; r.tapeTrack = std::max (0, i - 1); });
                               }));
            placeChips (paramChips ("grade", "GRADE (ALL PADS)", { "HI", "STD", "LONG", "EXLG" }));
            placeChips (chips ("FORMAT", { "STEREO", "MONO" }, [this] { return sampler.recordSettings().stereo ? 0 : 1; },
                               [this] (int i) { setRecord ([i] (auto& r) { r.stereo = i == 0; }); }));
            placeChips (chips ("START", { "MANUAL", "LEVEL 1", "2", "3", "4", "5", "6", "7" }, [this] { return sampler.recordSettings().trigger; },
                               [this] (int i) { setRecord ([i] (auto& r) { r.trigger = i; }); }));
            placeBar (*add (new ValueBar ("GAIN", 1, 16, [this] { return sampler.recordSettings().gain; },
                                          [this] (int v) { setRecord ([v] (auto& r) { r.gain = v; }); },
                                          [] (int v) { return juce::String (v) + " OF 16" + (v == 8 ? " (UNITY)" : ""); })));
            meterArea = { area.getX(), y, w, rowH };
            y += rowH + gapY;
            placeChips (chips ("TO BANK", { "A", "B", "C", "D" }, [this] { return sampler.recordSettings().toPad / padsPerBank; },
                               [this] (int i) { setRecord ([i] (auto& r) { r.toPad = i * padsPerBank + r.toPad % padsPerBank; }); }));
            juce::StringArray nums;
            for (int i = 1; i <= padsPerBank; ++i) nums.add (juce::String (i));
            placeChips (chips ("TO PAD", nums, [this] { return sampler.recordSettings().toPad % padsPerBank; },
                               [this] (int i) { setRecord ([i] (auto& r) { r.toPad = (r.toPad / padsPerBank) * padsPerBank + i; }); }));
            memoryArea = { area.getX(), y + 2, w, 64 };
            y += 74;
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
            note ({ area.getX() + 286, y, w - 286, chipH + 4 }, {});   // the record status, painted live
            y += chipH + 14;
            note ({ area.getX(), y, w, 14 }, "A FILE OR THE TAPE IS RECORDED AS IF THROUGH THE INPUT: AT THE GRADE, THROUGH ITS FILTER, AS ADPCM, INTO THE SAME MEMORY.");
            break;
        }
        case 2:   // EDIT
        {
            const int index = sampler.editorPad;
            placeChips (chips ("BANK", { "A", "B", "C", "D" }, [this] { return sampler.editorPad / padsPerBank; },
                               [this] (int i) { sampler.editorPad = i * padsPerBank + sampler.editorPad % padsPerBank; build(); }));
            juce::StringArray nums;
            for (int i = 1; i <= padsPerBank; ++i) nums.add (juce::String (i));
            placeChips (chips ("PAD", nums, [this] { return sampler.editorPad % padsPerBank; },
                               [this] (int i) { sampler.editorPad = (sampler.editorPad / padsPerBank) * padsPerBank + i; build(); }));
            headArea = { area.getX(), y, w, 16 };
            y += 20;
            auto* wave = new Wave (sampler, index);
            parts.add (wave);
            addAndMakeVisible (wave);
            wave->setBounds (area.getX(), y, w, 104);
            wave->onChange = [this] { repaint (headArea); };
            y += 112;
            const auto p = sampler.pad (index);
            if (! p.used)
            {
                placeBar (*add (new ValueBar ("MIDI NOTE", -1, 127, [this, index] { return sampler.pad (index).note; },
                                              [this, index] (int v) { auto q = sampler.pad (index); q.note = v; sampler.setPad (index, q); }, noteName)));
                break;
            }
            auto setPad = [this, index] (std::function<void (Pad&)> change) { auto q = sampler.pad (index); change (q); sampler.setPad (index, q); repaint(); };
            placeChips (chips ("LOOP", { "ONE SHOT", "WHOLE LOOP", "PARTIAL LOOP", "REVERSE SHOT", "REVERSE LOOP" },
                               [this, index] { return (int) sampler.pad (index).loop; }, [setPad] (int i) { setPad ([i] (Pad& q) { q.loop = (Loop) i; }); }));
            placeChips (chips ("PAD PLAYS", { "NOTE (WHILE HELD)", "TOGGLE", "TRIGGER" },
                               [this, index] { return (int) sampler.pad (index).sounding; }, [setPad] (int i) { setPad ([i] (Pad& q) { q.sounding = (Sounding) i; }); }));
            placeChips (chips ("ALTERNATE GROUP", { "OFF", "1", "2", "3" },
                               [this, index] { return sampler.pad (index).group; }, [setPad] (int i) { setPad ([i] (Pad& q) { q.group = i; }); }));
            placeBar (*add (new ValueBar ("VOLUME", 0, 127, [this, index] { return sampler.pad (index).volume; },
                                          [setPad] (int v) { setPad ([v] (Pad& q) { q.volume = v; }); }, [] (int v) { return juce::String (v) + " OF 127"; })));
            placeBar (*add (new ValueBar ("MIDI NOTE", -1, 127, [this, index] { return sampler.pad (index).note; },
                                          [setPad] (int v) { setPad ([v] (Pad& q) { q.note = v; }); }, noteName)));
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
            act ("DELETE", [this, index]
            {
                const double now = juce::Time::getMillisecondCounterHiRes();
                if (deleteArmedWhat == index && now < deleteArmedUntil) { sampler.deletePad (index); deleteArmedWhat = -1; say ("DELETED " + padName (index)); build(); }
                else { deleteArmedWhat = index; deleteArmedUntil = now + 3000.0; say ("DELETE " + padName (index) + "? CLICK AGAIN"); }
            });
            y += chipH + 12;
            note ({ area.getX(), y, w, 14 }, "DRAG THE MARKERS; CLICK ELSEWHERE ON THE SAMPLE TO HEAR IT. COMPACT TRIMS WHAT'S BEFORE THE START AND AFTER THE END, AND GIVES THE MEMORY BACK.");
            break;
        }
        case 3:   // SONGS
        {
            for (int s = 0; s < numSongs; ++s)
            {
                const int rowY = y;
                int x = area.getX() + captionW + 200;
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
                act ("DELETE", [this, s]
                {
                    const double now = juce::Time::getMillisecondCounterHiRes();
                    if (deleteArmedWhat == 100 + s && now < deleteArmedUntil) { sampler.deleteSong (s); deleteArmedWhat = -1; say ("SONG " + juce::String (s + 1) + " DELETED"); }
                    else { deleteArmedWhat = 100 + s; deleteArmedUntil = now + 3000.0; say ("DELETE SONG " + juce::String (s + 1) + "? CLICK AGAIN"); }
                });
                y += chipH + 10;
            }
            songArea = { area.getX(), area.getY(), w, y - area.getY() };
            y += 8;
            meterArea = { area.getX(), y, w, rowH };   // the song memory
            y += rowH + 12;
            note ({ area.getX(), y, w, 14 }, "TO RECORD: RECORD, THEN PLAY THE PADS (HERE, ON THE PLAY TAB OR FROM MIDI); STOP ENDS IT. PLAY WITH HOLD ON REPEATS.");
            note ({ area.getX(), y + 16, w, 14 }, "A SONG KEEPS THE PADS, THEIR TIMING AND VELOCITY, REV AND HOLD: NOT THE RIBBON, AS ON THE ORIGINAL. 1000 EVENTS IN ALL.");
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
            y += chipH + 14;
            note ({ area.getX(), y, w, 14 }, "IN EXT FILTER, PADS 1-4 CHOOSE THE FILTER AND 7-9 ITS RESONANCE, AS ON THE ORIGINAL (IN FILTER, WITH SHIFT HELD).");
            note ({ area.getX(), y + 16, w, 14 }, "PITCH MOVES THE SAMPLE CLOCK, RECORDING AND PLAYING: HI AT +10 % IS 44.1 KHZ.");
            break;
        }
    }
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
    for (auto* l : live) l->repaint();
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
    for (auto& [r, t] : notes) if (t.isNotEmpty()) text (g, t, r, Pal::lcdDim.withAlpha (0.75f), 9.5f);
    const juce::String dot = utf (" \xc2\xb7 ");

    if (! meterArea.isEmpty() && tab == 1)
    {
        // the input after the gain: 24 segments, 3 dB each, the top four for the last 6 dB before clipping
        auto r = meterArea;
        text (g, "INPUT", r.removeFromLeft (captionW), Pal::lcdDim);
        const auto st = sampler.panel();
        text (g, st.clipped ? juce::String ("CLIPPED") : juce::String ("FULL SCALE AT THE RIGHT"), r.removeFromRight (valueW), st.clipped ? Pal::red : Pal::lcdDim, 9.0f, juce::Justification::centredRight);
        const int n = 24;
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
            if (p.loop == Loop::wholeLoop || p.loop == Loop::partialLoop || p.loop == Loop::reverseLoop)
                info << dot << "LOOP " << juce::String (sampler.loopTempo (index), 1) << " BPM";
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
            text (g, t, row.withX (row.getX() + captionW).withWidth (196), on && (st.songPlaying || st.songRecording) ? Pal::lamp : Pal::lcdText, 10.0f);
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
