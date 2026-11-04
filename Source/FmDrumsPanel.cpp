#include "FmDrumsPanel.h"

namespace tf
{
namespace
{
    using namespace iw;
    using namespace fmd;
    const char* const tabNames[] { "PATTERN", "FX" };
    const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
}

//==============================================================================
/** One setting: caption, bar (drag; the mouse wheel steps; double-click: its default, or, locking a step,
    the lock off), value. Amber while it's a step's lock. */
class FmDrumsPanel::Field : public juce::Component
{
public:
    Field (juce::String c, int lo, int hi, std::function<int()> g, std::function<void (int)> s, std::function<juce::String (int)> t)
        : caption (std::move (c)), low (lo), high (hi), get (std::move (g)), set (std::move (s)), text (std::move (t)) {}

    std::function<bool()> locked;        // shown amber
    std::function<void()> reset;         // double-click
    juce::String sub;                    // what the macro does on this machine

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        const bool lock = locked && locked();
        auto cap = r.removeFromLeft (capW);
        iw::text (g, caption, cap.removeFromTop (sub.isEmpty() ? r.getHeight() : 12), lock ? Pal::lamp : Pal::lcdDim, 10.0f);
        if (sub.isNotEmpty()) iw::text (g, sub, cap, Pal::lcdDim.withAlpha (0.7f), 8.5f);
        iw::text (g, text (get()), r.removeFromRight (valW), lock ? Pal::lamp : Pal::lcdText, 10.5f, juce::Justification::centredRight);
        bar = r.reduced (6, 0).withSizeKeepingCentre (r.getWidth() - 12, 6);
        g.setColour (Pal::lcdRule);
        g.fillRoundedRectangle (bar.toFloat(), 3.0f);
        g.setColour (lock ? Pal::lamp : Pal::lcdText);
        const float f = (float) (get() - low) / (float) std::max (1, high - low);
        g.fillRoundedRectangle (bar.toFloat().withWidth (std::max (6.0f, (float) bar.getWidth() * f)), 3.0f);
    }
    void mouseDown (const juce::MouseEvent& e) override { mouseDrag (e); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (bar.getWidth() <= 0) return;
        set (low + juce::roundToInt (juce::jlimit (0.0f, 1.0f, (float) (e.x - bar.getX()) / (float) bar.getWidth()) * (float) (high - low)));
        repaint();
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        set (juce::jlimit (low, high, get() + (w.deltaY > 0 ? 1 : -1)));
        repaint();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { if (reset) reset(); repaint(); }

    static constexpr int capW = 120, valW = 86;

private:
    juce::String caption;
    int low, high;
    std::function<int()> get;
    std::function<void (int)> set;
    std::function<juce::String (int)> text;
    juce::Rectangle<int> bar;
};

//==============================================================================
/** The six tracks' steps, 16 at a time. */
class FmDrumsPanel::Grid : public juce::Component
{
public:
    Grid (Processor& p, std::function<void()> changedFn, std::function<void()> selectionFn)
        : s (p), onChange (std::move (changedFn)), onSelect (std::move (selectionFn)) {}

    static constexpr int headW = 178, rowH = 26;

    juce::Rectangle<int> cell (int track, int k) const
    {
        const float w = (float) (getWidth() - headW) / 16.0f;
        return juce::Rectangle<float> ((float) headW + w * (float) k + 2.0f, (float) (track * rowH) + 3.0f, w - 4.0f, (float) rowH - 6.0f).toNearestInt();
    }

    void paint (juce::Graphics& g) override
    {
        auto& st = s.state();
        const auto& pat = st.patterns[(size_t) st.pattern];
        const int page = s.editorPage;
        const int playing = s.playingStep();
        for (int t = 0; t < fmd::numTracks; ++t)
        {
            const bool sel = t == s.editorTrack;
            juce::Rectangle<int> head (0, t * rowH + 2, headW - 8, rowH - 4);
            g.setColour (sel ? Pal::lcdText.withAlpha (0.12f) : juce::Colours::transparentBlack);
            g.fillRoundedRectangle (head.toFloat(), 3.0f);
            // the track's lamp: how loud it is now
            const float lv = juce::jlimit (0.0f, 1.0f, s.trackLevel (t) * 2.0f);
            g.setColour (Pal::lamp.withAlpha (0.15f + 0.85f * lv));
            g.fillEllipse ((float) head.getX() + 6.0f, (float) head.getCentreY() - 4.0f, 8.0f, 8.0f);
            iw::text (g, juce::String (t + 1), head.withX (head.getX() + 20).withWidth (16), sel ? Pal::lcdText : Pal::lcdDim, 11.0f);
            iw::text (g, machineName (st.sound[t][pMachine]), head.withX (head.getX() + 38).withWidth (56), st.mute[t] ? Pal::lcdDim.withAlpha (0.5f) : (sel ? Pal::lcdText : Pal::lcdDim), 11.0f);
            muteBox (t) = { headW - 58, t * rowH + 5, 46, rowH - 10 };
            // the track's own play key: hear it on its own (choosing the track doesn't play it)
            playBox (t) = { headW - 86, t * rowH + 5, 24, rowH - 10 };
            g.setColour (Pal::lcdRule);
            g.drawRoundedRectangle (playBox (t).toFloat(), 3.0f, 1.0f);
            {
                const auto b = playBox (t).toFloat().withSizeKeepingCentre (7.0f, 8.0f);
                juce::Path tri;
                tri.addTriangle (b.getX(), b.getY(), b.getX(), b.getBottom(), b.getRight(), b.getCentreY());
                g.setColour (Pal::lcdText);
                g.fillPath (tri);
            }
            g.setColour (st.mute[t] ? Pal::lamp : Pal::lcdRule);
            g.drawRoundedRectangle (muteBox (t).toFloat(), 3.0f, 1.0f);
            iw::text (g, "MUTE", muteBox (t), st.mute[t] ? Pal::lamp : Pal::lcdDim, 9.0f, juce::Justification::centred);
            for (int k = 0; k < 16; ++k)
            {
                const int stepIndex = page * 16 + k;
                const auto r = cell (t, k).toFloat();
                const bool inPattern = stepIndex < pat.length;
                const auto& stp = pat.steps[t][stepIndex];
                const bool beat = k % 4 == 0;
                g.setColour (! inPattern ? Pal::lcdRule.withAlpha (0.25f) : (beat ? Pal::lcdRule.withAlpha (0.9f) : Pal::lcdRule.withAlpha (0.55f)));
                g.fillRoundedRectangle (r, 3.0f);
                if (stp.on && inPattern)
                {
                    const float v = (float) stp.velocity / 127.0f;
                    g.setColour (Pal::lcdText.withAlpha (0.35f + 0.65f * v));
                    g.fillRoundedRectangle (r.reduced (1.5f), 2.5f);
                }
                if (stepIndex == playing && inPattern)
                {
                    g.setColour (Pal::cream.withAlpha (0.55f));
                    g.drawRoundedRectangle (r.expanded (1.0f), 3.0f, 2.0f);
                }
                if (t == s.editorTrack && stepIndex == s.editorStep)
                {
                    g.setColour (Pal::lamp);
                    g.drawRoundedRectangle (r.expanded (1.5f), 3.0f, 2.0f);
                }
                // marks: locks (a dot), chance (a corner), retrigs (ticks)
                if (stp.hasLocks()) { g.setColour (Pal::lamp); g.fillEllipse (r.getRight() - 7.0f, r.getY() + 3.0f, 4.0f, 4.0f); }
                if (stp.chance < 100) { g.setColour (stp.on ? Pal::lcd : Pal::lcdDim); g.fillRect (r.getX() + 3.0f, r.getBottom() - 4.0f, (r.getWidth() - 6.0f) * (float) stp.chance / 100.0f, 1.5f); }
                for (int q = 0; q < stp.retrig; ++q) { g.setColour (stp.on ? Pal::lcd : Pal::lcdDim); g.fillRect (r.getX() + 3.0f + (float) q * 3.0f, r.getY() + 3.0f, 1.5f, 5.0f); }
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto& st = s.state();
        auto& pat = st.patterns[(size_t) st.pattern];
        const int t = std::clamp (e.y / rowH, 0, fmd::numTracks - 1);
        if (e.x < headW)
        {
            if (muteBox (t).contains (e.getPosition())) { st.mute[t] = ! st.mute[t]; onChange(); repaint(); return; }
            if (playBox (t).contains (e.getPosition())) { s.hit (t, 110); return; }   // heard, nothing else
            // the track's name: chosen, silently (it may be playing: no extra hit while you work on it)
            s.editorTrack = t;
            s.editorStep = -1;
            onSelect();
            repaint();
            return;
        }
        for (int k = 0; k < 16; ++k)
            if (cell (t, k).expanded (2).contains (e.getPosition()))
            {
                const int stepIndex = s.editorPage * 16 + k;
                if (stepIndex >= pat.length) return;
                if (e.mods.isPopupMenu() || e.mods.isAltDown())
                {
                    // the step's sound: locked from here
                    const bool same = s.editorTrack == t && s.editorStep == stepIndex;
                    s.editorTrack = t;
                    s.editorStep = same ? -1 : stepIndex;
                    onSelect();
                }
                else
                {
                    auto& stp = pat.steps[t][stepIndex];
                    stp.on = (int8_t) (stp.on ? 0 : 1);
                    if (s.editorTrack != t) { s.editorTrack = t; s.editorStep = -1; onSelect(); }
                    else if (s.editorStep >= 0) { s.editorStep = stepIndex; onSelect(); }
                    onChange();
                }
                repaint();
                return;
            }
    }

private:
    juce::Rectangle<int>& muteBox (int t) { return mutes[(size_t) t]; }
    juce::Rectangle<int>& playBox (int t) { return plays[(size_t) t]; }
    Processor& s;
    std::function<void()> onChange, onSelect;
    std::array<juce::Rectangle<int>, fmd::numTracks> mutes {}, plays {};
};

//==============================================================================
FmDrumsPanel::FmDrumsPanel (Processor& p) : synth (p)
{
    for (int i = 0; i < 2; ++i)
    {
        auto* c = tabChips.add (new LcdChip (tabNames[i]));
        c->textSize = 11.5f;
        c->onClick = [this, i] { synth.editorTab = i; buildSoon(); };
        addAndMakeVisible (c);
    }
    startTimerHz (30);
}

FmDrumsPanel::~FmDrumsPanel() = default;

void FmDrumsPanel::buildSoon()
{
    if (rebuildPending) return;
    rebuildPending = true;
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<FmDrumsPanel> (this)]
    {
        if (safe != nullptr) { safe->rebuildPending = false; safe->build(); }
    });
}

iw::Chips& FmDrumsPanel::chips (const juce::String& caption, juce::StringArray labels, std::function<int()> chosen, std::function<void (int)> pick)
{
    auto* c = new iw::Chips (caption, labels, std::move (chosen), std::move (pick));
    parts.add (c);
    chipRows.push_back (c);
    addAndMakeVisible (c);
    return *c;
}

LcdChip& FmDrumsPanel::button (const juce::String& label, std::function<void()> click)
{
    auto* c = new LcdChip (label);
    c->textSize = 11.0f;
    c->onClick = std::move (click);
    parts.add (c);
    addAndMakeVisible (c);
    return *c;
}

fmd::Step* FmDrumsPanel::lockedStep()
{
    if (synth.editorStep < 0) return nullptr;
    return &pattern().steps[std::clamp (synth.editorTrack, 0, fmd::numTracks - 1)][std::clamp (synth.editorStep, 0, maxSteps - 1)];
}

FmDrumsPanel::Field& FmDrumsPanel::setting (const juce::String& caption, int lo, int hi, std::function<int()> get, std::function<void (int)> set,
                                            std::function<juce::String (int)> text, juce::Rectangle<int> r)
{
    auto* f = new Field (caption, lo, hi, std::move (get), std::move (set), std::move (text));
    parts.add (f);
    live.push_back (f);
    addAndMakeVisible (f);
    f->setBounds (r);
    return *f;
}

/** A sound setting of the selected track; while a step is selected, that step's lock of it. */
FmDrumsPanel::Field& FmDrumsPanel::field (int param, juce::Rectangle<int> r)
{
    const auto& info = paramInfo (param);
    auto machineNow = [this]
    {
        auto* st = lockedStep();
        return st != nullptr && st->lock[pMachine] >= 0 ? (int) st->lock[pMachine] : synth.state().sound[synth.editorTrack][pMachine];
    };
    auto& f = setting (info.caption, info.lo, info.hi,
        [this, param]
        {
            auto* st = lockedStep();
            return st != nullptr && st->lock[param] >= 0 ? (int) st->lock[param] : synth.state().sound[synth.editorTrack][param];
        },
        [this, param] (int v)
        {
            if (auto* st = lockedStep()) st->lock[param] = (int16_t) v;
            else synth.state().sound[synth.editorTrack][param] = v;
            edited();
        },
        [machineNow, param] (int v) { return valueText (machineNow(), param, v); }, r);
    f.locked = [this, param] { auto* st = lockedStep(); return st != nullptr && st->lock[param] >= 0; };
    f.reset = [this, param]
    {
        if (auto* st = lockedStep()) st->lock[param] = -1;
        else synth.state().sound[synth.editorTrack][param] = paramInfo (param).def;
        edited();
    };
    f.sub = macroMeaning (machineNow(), param);
    return f;
}

void FmDrumsPanel::build()
{
    parts.clear();
    chipRows.clear();
    dimRows.clear();
    live.clear();
    notes.clear();
    playChip = nullptr;
    headArea = {};
    auto& st = synth.state();
    builtTab = synth.editorTab;
    builtTrack = synth.editorTrack;
    builtStep = synth.editorStep;
    builtMachine = st.sound[synth.editorTrack][pMachine];
    for (int i = 0; i < tabChips.size(); ++i) tabChips[i]->chosen = i == synth.editorTab;

    auto area = getLocalBounds().withTrimmedTop (chipH + 10);
    const int w = area.getWidth();
    int y = area.getY();
    auto placeChips = [&] (iw::Chips& c) { const int h = c.layout (w); c.setBounds (area.getX(), y, w, h); y += h + gapY - 2; };

    if (synth.editorTab == 1)
    {
        // ---- FX ----
        placeChips (chips ("DELAY TIME", { "1/16", "1/8", "3/16", "1/4", "3/8", "1/2" }, [this] { return synth.state().delayTime; },
                           [this] (int i) { synth.state().delayTime = i; edited(); }));
        auto row = [&] (const juce::String& cap, int* v, std::function<juce::String (int)> text)
        {
            setting (cap, 0, 127, [v] { return *v; }, [this, v] (int x) { *v = x; edited(); }, std::move (text), { area.getX(), y, w, rowH });
            y += rowH + gapY;
        };
        auto plain = [] (int v) { return juce::String (v); };
        row ("DELAY FEEDBACK", &st.delayFeedback, [] (int v) { return juce::String (juce::roundToInt (v / 127.0 * 90.0)) + " %"; });
        row ("DELAY TONE", &st.delayTone, plain);
        y += 6;
        row ("REVERB SIZE", &st.reverbSize, plain);
        row ("REVERB DECAY", &st.reverbDecay, [] (int v) { return juce::String (0.3 * std::pow (8.0 / 0.3, v / 127.0), 1) + " S"; });
        row ("REVERB TONE", &st.reverbTone, plain);
        y += 6;
        row ("MASTER", &st.master, plain);
        y += 10;
        note ({ area.getX(), y, w, 14 }, "EACH TRACK SENDS TO THE DELAY AND THE REVERB BY ITS DELAY AND REVERB SETTINGS (PATTERN TAB).");
        note ({ area.getX(), y + 16, w, 14 }, "THE DELAY'S TIME FOLLOWS THE TEMPO; IT BOUNCES LEFT AND RIGHT, DARKER EACH TIME AS TONE SAYS.");
        note ({ area.getX(), y + 32, w, 14 }, "MIDI: C1 TO F1 (NOTES 36-41) PLAY TRACKS 1 TO 6, AT THEIR VELOCITY.");
        note ({ area.getX(), y + 48, w, 14 }, "LIKE A DRUM MACHINE PLUGGED INTO THE RECORDER, IT KEEPS ITS OWN TEMPO: ITS PLAY STARTS IT, NOT THE TAPE.");
        repaint();
        return;
    }

    // ---- PATTERN: the transport ----
    auto& play = button (synth.isPlaying() ? "STOP" : "PLAY", [this] { synth.setPlaying (! synth.isPlaying()); });
    play.setBounds (area.getX(), y, 90, chipH + 4);
    playChip = &play;
    const int half = (w - 110) / 2;
    setting ("TEMPO", 400, 2400, [this] { return synth.state().tempo; }, [this] (int v) { synth.state().tempo = v; edited(); },
             [] (int v) { return juce::String (v / 10.0, 1) + " BPM"; }, { area.getX() + 104, y + 3, half, rowH });
    setting ("SWING", 50, 75, [this] { return synth.state().swing; }, [this] (int v) { synth.state().swing = v; edited(); },
             [] (int v) { return juce::String (v) + " %"; }, { area.getX() + 114 + half, y + 3, half - 10, rowH });
    y += chipH + 12;
    juce::StringArray nums;
    for (int i = 1; i <= numPatterns; ++i) nums.add (juce::String (i));
    auto& pats = chips ("PATTERN", nums, [this] { return synth.state().pattern; }, [this] (int i)
    {
        synth.state().pattern = i;
        synth.editorStep = -1;
        edited();
        say (synth.isPlaying() ? "PATTERN " + juce::String (i + 1) + " NEXT: AT THE END OF THE ONE PLAYING" : "PATTERN " + juce::String (i + 1));
        buildSoon();
    });
    placeChips (pats);
    dimRows.push_back ({ &pats, [this] (int i) { return synth.state().patterns[(size_t) i].empty(); } });
    // the length, and which 16 steps are shown
    const int len = pattern().length;
    auto& lenRow = chips ("LENGTH", { "16", "32", "48", "64" }, [this] { const int l = pattern().length; return l % 16 == 0 ? l / 16 - 1 : -1; },
                          [this] (int i) { pattern().length = (i + 1) * 16; synth.editorPage = std::min (synth.editorPage, i); edited(); buildSoon(); });
    lenRow.setBounds (area.getX(), y, w / 2, lenRow.layout (w / 2));
    auto& pageRow = chips ("STEPS", { "1-16", "17-32", "33-48", "49-64" }, [this] { return synth.editorPage; }, [this] (int i) { synth.editorPage = i; repaint(); });
    pageRow.layout (w / 2);
    pageRow.setBounds (area.getX() + w / 2, y, w / 2, chipH);
    dimRows.push_back ({ &pageRow, [this] (int i) { return i * 16 >= pattern().length; } });
    juce::ignoreUnused (len);
    y += chipH + gapY + 2;

    // ---- the grid ----
    auto* grid = new Grid (synth, [this] { edited(); }, [this] { buildSoon(); });
    parts.add (grid);
    live.push_back (grid);
    addAndMakeVisible (grid);
    grid->setBounds (area.getX(), y, w, Grid::rowH * fmd::numTracks);
    y += Grid::rowH * fmd::numTracks + 2;
    note ({ area.getX() + Grid::headW, y, w - Grid::headW, 12 }, utf ("CLICK: A STEP ON OR OFF \xc2\xb7 RIGHT-CLICK (OR ALT-CLICK): LOCK ITS SOUND \xc2\xb7 A TRACK'S NAME: CHOOSE IT \xc2\xb7 \xe2\x96\xb6: HEAR IT"));
    y += 14;

    // ---- the track's sound, or the step's locks ----
    headArea = { area.getX(), y, w, 18 };
    y += 22;
    auto* step = lockedStep();
    placeChips (chips ("MACHINE", { "KICK", "SNARE", "METAL", "PERC", "TONE", "CHORD" },
                       [this] { auto* s = lockedStep(); return s != nullptr && s->lock[pMachine] >= 0 ? (int) s->lock[pMachine] : synth.state().sound[synth.editorTrack][pMachine]; },
                       [this] (int i)
                       {
                           if (auto* s = lockedStep()) s->lock[pMachine] = (int16_t) i;
                           else synth.state().sound[synth.editorTrack][pMachine] = i;
                           edited();
                           buildSoon();
                       }));
    const int colW = (w - 20) / 2;
    const int top = y;
    // the sound on the left, the mix and the LFO on the right (LFO TO and its shape: drag through them)
    for (int p : { pPitch, pDecay, pColor, pShape, pSweep, pContour, pVolume })
    {
        field (p, { area.getX(), y, colW, rowH + 2 });
        y += rowH + 4;
    }
    int y2 = top;
    for (int p : { pDrive, pPan, pDelay, pReverb, pLfoSpeed, pLfoDepth, pLfoDest, pLfoShape })
    {
        field (p, { area.getX() + colW + 20, y2, colW, rowH });
        y2 += rowH + 2;
    }
    y = std::max (y, y2) + 4;
    if (step != nullptr)
    {
        // the step itself: how hard, how often, how many times
        const int third = (w - 40) / 3;
        setting ("VELOCITY", 1, 127, [step] { return (int) step->velocity; }, [this, step] (int v) { step->velocity = (int8_t) v; edited(); },
                 [] (int v) { return juce::String (v); }, { area.getX(), y, third, rowH });
        setting ("CHANCE", 1, 100, [step] { return (int) step->chance; }, [this, step] (int v) { step->chance = (int8_t) v; edited(); },
                 [] (int v) { return juce::String (v) + " %"; }, { area.getX() + third + 20, y, third, rowH });
        setting ("RETRIG", 0, 3, [step] { return (int) step->retrig; }, [this, step] (int v) { step->retrig = (int8_t) v; edited(); },
                 [] (int v) { return v == 0 ? juce::String ("OFF") : "x" + juce::String (v + 1); }, { area.getX() + 2 * third + 40, y, third, rowH });
        y += rowH + 8;
        int x = area.getX();
        auto& clear = button ("CLEAR LOCKS", [this] { if (auto* s = lockedStep()) { const auto keep = *s; *s = Step(); s->on = keep.on; s->velocity = keep.velocity; s->chance = keep.chance; s->retrig = keep.retrig; edited(); buildSoon(); } });
        clear.setBounds (x, y, clear.idealWidth() + 8, chipH);
        x += clear.idealWidth() + 16;
        auto& done = button ("DONE", [this] { synth.editorStep = -1; buildSoon(); });
        done.setBounds (x, y, done.idealWidth() + 8, chipH);
        note ({ x + done.idealWidth() + 20, y, w, chipH }, utf ("AMBER: LOCKED ON THIS STEP \xc2\xb7 DOUBLE-CLICK A SETTING TO UNLOCK IT"));
    }
    for (auto& [row, dimmed] : dimRows)
        for (int i = 0; i < row->chips.size(); ++i) row->chips[i]->dim = dimmed (i);
    repaint();
}

void FmDrumsPanel::resized()
{
    int x = 0;
    for (auto* c : tabChips) { c->setBounds (x, 0, 140, chipH + 2); x += 147; }
    build();
}

void FmDrumsPanel::timerCallback()
{
    const auto& st = synth.state();
    if (builtTab != synth.editorTab || builtTrack != synth.editorTrack || builtStep != synth.editorStep || builtMachine != st.sound[synth.editorTrack][pMachine]) { buildSoon(); return; }
    for (auto* c : chipRows) c->refresh();
    for (auto& [row, dimmed] : dimRows)
        for (int i = 0; i < row->chips.size(); ++i)
            if (row->chips[i]->dim != dimmed (i)) { row->chips[i]->dim = dimmed (i); row->chips[i]->repaint(); }
    for (auto* l : live) l->repaint();
    if (playChip != nullptr)
    {
        const juce::String label = synth.isPlaying() ? "STOP" : "PLAY";
        if (playChip->getButtonText() != label) playChip->setButtonText (label);
        playChip->chosen = synth.isPlaying();
        playChip->repaint();
    }
}

void FmDrumsPanel::paint (juce::Graphics& g)
{
    for (auto& [r, t] : notes) if (t.isNotEmpty()) iw::text (g, t, r, Pal::lcdDim.withAlpha (0.75f), 9.0f);
    if (! headArea.isEmpty())
    {
        const auto& st = synth.state();
        const int t = synth.editorTrack;
        juce::String head = "TRACK " + juce::String (t + 1) + dot + machineName (st.sound[t][pMachine]);
        if (synth.editorStep >= 0) head << dot << "STEP " << (synth.editorStep + 1) << ": ITS LOCKS";
        iw::text (g, head, headArea, synth.editorStep >= 0 ? Pal::lamp : Pal::lcdText, 12.0f);
        iw::text (g, synth.editorStep >= 0 ? juce::String ("RIGHT-CLICK THE STEP AGAIN, OR DONE, TO GO BACK TO THE TRACK") : juce::String ("DOUBLE-CLICK A SETTING FOR ITS START VALUE"),
                  headArea, Pal::lcdDim, 9.0f, juce::Justification::centredRight);
    }
}
} // namespace tf
