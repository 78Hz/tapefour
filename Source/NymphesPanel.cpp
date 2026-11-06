#include "NymphesPanel.h"

namespace tf
{
namespace
{
    using namespace nym;
    const juce::Colour cell { 0xff0a221f };
    const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));

    juce::String oscWaveText (int v)
    {
        // continuous: saw, square, triangle
        if (v <= 4) return "SAW";
        if (v >= 123) return "TRIANGLE";
        if (std::abs (v - 64) <= 4) return "SQUARE";
        return v < 64 ? "SAW-SQ" : "SQ-TRI";
    }
    const char* lfoShapes[] { "TRIANGLE", "SAW DOWN", "SAW UP", "SQUARE", "RANDOM" };
    int lfoShape (int v) { return std::clamp (juce::roundToInt (v / 127.0 * 4.0), 0, 4); }

    juce::String valueText (int c, int v)
    {
        const auto& k = control (c);
        if (k.choices != nullptr) return juce::StringArray::fromTokens (k.choices, "|", "")[v];
        if (c == oscWave) return oscWaveText (v);
        if (c == lfo1Wave || c == lfo2Wave) return lfoShapes[lfoShape (v)];
        if (c == oscPw) return juce::String (juce::roundToInt (v / 127.0 * 100.0)) + "%";
        return juce::String (v);
    }

    void section (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, const juce::String& sub)
    {
        g.setColour (cell);
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        iw::text (g, title, r.reduced (10, 0).withHeight (24), Pal::lcdText, 12.0f);
        if (sub.isNotEmpty()) iw::text (g, sub, r.reduced (10, 0).withHeight (24), Pal::lcdDim, 9.0f, juce::Justification::centredRight);
    }

    void path (juce::Graphics& g, const std::vector<juce::Point<float>>& pts, juce::Colour c, float w = 1.6f)
    {
        if (pts.size() < 2) return;
        juce::Path p;
        p.startNewSubPath (pts[0]);
        for (size_t i = 1; i < pts.size(); ++i) p.lineTo (pts[i]);
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (w));
    }
}

//==============================================================================
/** One control: an arc (drag up or down; the wheel: one step; double-click: its default). */
class NymphesPanel::Knob : public juce::Component,
                           public juce::SettableTooltipClient
{
public:
    Knob (Processor& p, int c, int r) : radius (r), syn (p), index (c)
    {
        setTooltip (juce::String (control (c).name) + " (CC " + juce::String (control (c).cc) + ")");
    }
    void paint (juce::Graphics& g) override
    {
        const auto& k = control (index);
        const int v = syn.value (index);
        const auto c = juce::Point<float> ((float) getWidth() * 0.5f, (float) radius + 4.0f);
        constexpr float a0 = juce::MathConstants<float>::pi * 0.75f, span = juce::MathConstants<float>::pi * 1.5f;
        const float am = a0 + span * (float) v / (float) k.hi;
        juce::Path arc;
        arc.addCentredArc (c.x, c.y, (float) radius, (float) radius, 0.0f, a0 + juce::MathConstants<float>::halfPi, a0 + span + juce::MathConstants<float>::halfPi, true);
        g.setColour (Pal::lcdRule);
        g.strokePath (arc, juce::PathStrokeType (3.0f));
        if (v > 0)
        {
            juce::Path on;
            on.addCentredArc (c.x, c.y, (float) radius, (float) radius, 0.0f, a0 + juce::MathConstants<float>::halfPi, am + juce::MathConstants<float>::halfPi, true);
            g.setColour (Pal::lcdText);
            g.strokePath (on, juce::PathStrokeType (3.0f));
        }
        g.setColour (Pal::lcdText);
        g.drawLine (c.x + std::cos (am) * ((float) radius - 9.0f), c.y + std::sin (am) * ((float) radius - 9.0f),
                    c.x + std::cos (am) * ((float) radius - 2.0f), c.y + std::sin (am) * ((float) radius - 2.0f), 2.0f);
        const int y = 2 * radius + 8;
        iw::text (g, label, { 0, y, getWidth(), 13 }, Pal::lcdDim, 9.0f, juce::Justification::centred);
        iw::text (g, valueText (index, v), { 0, y + 12, getWidth(), 14 }, Pal::lcdText, 10.0f, juce::Justification::centred);
    }
    void mouseDown (const juce::MouseEvent&) override { from = syn.value (index); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto& k = control (index);
        const float per = k.hi >= 64 ? 1.5f : 18.0f;   // pixels a step
        syn.setValue (index, std::clamp (from + juce::roundToInt ((float) -e.getDistanceFromDragStartY() / per), 0, k.hi));
        repaint();
        if (onChange) onChange();
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        syn.setValue (index, std::clamp (syn.value (index) + (w.deltaY > 0 ? 1 : -1), 0, control (index).hi));
        repaint();
        if (onChange) onChange();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        syn.setValue (index, control (index).def);
        repaint();
        if (onChange) onChange();
    }
    juce::String label;
    std::function<void()> onChange;
    int radius;

private:
    Processor& syn;
    int index, from = 0;
};

//==============================================================================
/** An ADSR as a graph: drag the attack's peak (its time), the decay's corner (its time, and the
    sustain's level), the release's end (its time). */
class NymphesPanel::Envelope : public juce::Component
{
public:
    Envelope (Processor& p, int firstControl, juce::String l) : syn (p), first (firstControl), label (std::move (l)) {}
    juce::Point<float> point (int k) const
    {
        const auto r = getLocalBounds().toFloat().reduced (6.0f);
        const float seg = r.getWidth() / 4.2f;
        const float a = (float) syn.value (first) / 127.0f, d = (float) syn.value (first + 1) / 127.0f;
        const float s = (float) syn.value (first + 2) / 127.0f, rel = (float) syn.value (first + 3) / 127.0f;
        const juce::Point<float> pa { r.getX() + a * seg, r.getY() };
        const juce::Point<float> pd { pa.x + d * seg, r.getBottom() - s * r.getHeight() };
        const juce::Point<float> ps { pd.x + seg * 0.9f, pd.y };
        const juce::Point<float> pr { ps.x + rel * seg, r.getBottom() };
        const juce::Point<float> all[] { { r.getX(), r.getBottom() }, pa, pd, ps, pr };
        return all[std::clamp (k, 0, 4)];
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (Pal::lcd);
        std::vector<juce::Point<float>> pts;
        for (int k = 0; k < 5; ++k) pts.push_back (point (k));
        path (g, pts, Pal::lcdText, 2.0f);
        for (int k : { 1, 2, 4 })
        {
            const auto r = juce::Rectangle<float> (8.0f, 8.0f).withCentre (point (k));
            g.setColour (Pal::lcd);
            g.fillEllipse (r);
            g.setColour (Pal::lcdText);
            g.drawEllipse (r, 1.2f);
        }
        if (label.isNotEmpty()) iw::text (g, label, getLocalBounds().reduced (8, 4).withHeight (14), Pal::lcdDim, 9.0f, juce::Justification::centredRight);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        // the nearest of the three handles
        grabbed = 1;
        float best = 1e9f;
        for (int k : { 1, 2, 4 })
            if (const float dd = point (k).getDistanceFrom (e.position); dd < best) { best = dd; grabbed = k; }
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto r = getLocalBounds().toFloat().reduced (6.0f);
        const float seg = r.getWidth() / 4.2f;
        auto to127 = [] (float f) { return std::clamp (juce::roundToInt (f * 127.0f), 0, 127); };
        if (grabbed == 1) syn.setValue (first, to127 ((e.position.x - r.getX()) / seg));
        else if (grabbed == 2)
        {
            syn.setValue (first + 1, to127 ((e.position.x - point (1).x) / seg));
            syn.setValue (first + 2, to127 ((r.getBottom() - e.position.y) / r.getHeight()));
        }
        else syn.setValue (first + 3, to127 ((e.position.x - point (3).x) / seg));
        repaint();
        if (onChange) onChange();
    }
    std::function<void()> onChange;

private:
    Processor& syn;
    int first, grabbed = 1;
    juce::String label;
};

//==============================================================================
/** The modulation matrix: a row a source, a column a destination; drag up or down in a cell for its depth. */
class NymphesPanel::Matrix : public juce::Component
{
public:
    explicit Matrix (Processor& p) : syn (p) {}
    static constexpr int labelW = 96, headH = 34, rowH = 30, cellW = 29, groupGap = 10;

    juce::Rectangle<int> cellRect (int s, int d) const
    {
        int x = labelW, groups = 0;
        for (int k = 0; k < d; ++k) if (k > 0 && juce::String (destGroup (k)) != destGroup (k - 1)) ++groups;
        if (d > 0 && juce::String (destGroup (d)) != destGroup (d - 1)) ++groups;
        x += d * cellW + groups * groupGap;
        return { x, headH + s * rowH, cellW - 4, rowH - 8 };
    }
    void paint (juce::Graphics& g) override
    {
        const bool centred = syn.depthsCentred();
        for (int d = 0; d < numDests; ++d)
        {
            const auto c = cellRect (0, d);
            if (d == 0 || juce::String (destGroup (d)) != destGroup (d - 1))
            {
                int last = d;
                while (last + 1 < numDests && juce::String (destGroup (last + 1)) == destGroup (d)) ++last;
                const int x0 = c.getX(), x1 = cellRect (0, last).getRight();
                iw::text (g, destGroup (d), { x0, 0, x1 - x0, 12 }, Pal::lcdDim, 8.5f, juce::Justification::centred);
            }
            iw::text (g, juce::String (destName (d)).substring (0, 5), { c.getX() - 2, 13, cellW, 11 }, Pal::lcdDim, 7.0f, juce::Justification::centred);
            if (destPerNote (d))
            {
                g.setColour (Pal::lcdText);
                g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ (float) c.getCentreX(), 28.0f }));
            }
        }
        for (int s = 0; s < numSources; ++s)
        {
            iw::text (g, sourceName (s), { 0, headH + s * rowH, labelW - 6, rowH - 8 }, Pal::lcdText, 10.0f);
            for (int d = 0; d < numDests; ++d)
            {
                const auto r = cellRect (s, d).toFloat();
                g.setColour (Pal::lcd);
                g.fillRect (r);
                g.setColour (Pal::lcdRule);
                g.drawRect (r, 1.0f);
                if (! destOnSynth (s, d))
                {
                    // not on the synth: struck through
                    juce::Graphics::ScopedSaveState ss (g);
                    g.reduceClipRegion (r.toNearestInt());
                    for (float x = r.getX() - r.getHeight(); x < r.getRight(); x += 5.0f) g.drawLine (x, r.getBottom(), x + r.getHeight(), r.getY(), 1.0f);
                    continue;
                }
                if (destUnsure (s, d))
                {
                    // in the CC chart, not in the manual's list: dashed sides
                    g.setColour (Pal::lcdDim);
                    for (float y = r.getY(); y < r.getBottom(); y += 5.0f)
                    {
                        g.fillRect (r.getX(), y, 1.0f, 2.5f);
                        g.fillRect (r.getRight() - 1.0f, y, 1.0f, 2.5f);
                    }
                }
                const int v = syn.depth (s, d);
                if (v < 0) continue;
                g.setColour (Pal::lcdText);
                const auto inner = r.reduced (3.0f, 2.0f);
                if (centred)
                {
                    const float mid = inner.getCentreY(), y = inner.getBottom() - inner.getHeight() * (float) v / 127.0f;
                    g.fillRect (juce::Rectangle<float> (inner.getX(), std::min (mid, y), inner.getWidth(), std::max (1.0f, std::abs (y - mid))));
                }
                else
                {
                    const float h = std::max (1.0f, inner.getHeight() * (float) v / 127.0f);
                    g.fillRect (inner.withTop (inner.getBottom() - h));
                }
            }
        }
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int s = 0; s < numSources; ++s)
            if (juce::Rectangle<int> (0, headH + s * rowH, labelW - 6, rowH - 8).contains (e.getPosition()))
            {
                // a source's name: clear its row
                for (int d = 0; d < numDests; ++d) syn.setDepth (s, d, -1);
                repaint();
                if (onMessage) onMessage (juce::String (sourceName (s)) + ": ALL ITS ROUTES CLEARED");
                return;
            }
        grabbed = hit (e.getPosition());
        mouseDrag (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (grabbed.x < 0) return;
        const auto r = cellRect (grabbed.x, grabbed.y).toFloat().reduced (3.0f, 2.0f);
        const float f = juce::jlimit (0.0f, 1.0f, (r.getBottom() - e.position.y) / r.getHeight());
        syn.setDepth (grabbed.x, grabbed.y, juce::roundToInt (f * 127.0f));
        repaint();
        if (onChange) onChange();
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const auto h = hit (e.getPosition());
        if (h.x >= 0) { syn.setDepth (h.x, h.y, -1); repaint(); }
        grabbed = { -1, -1 };
    }
    juce::Point<int> hit (juce::Point<int> p) const
    {
        for (int s = 0; s < numSources; ++s)
            for (int d = 0; d < numDests; ++d)
                if (cellRect (s, d).expanded (2).contains (p) && destOnSynth (s, d)) return { s, d };
        return { -1, -1 };
    }
    int width() const { return cellRect (0, numDests - 1).getRight() + 4; }
    std::function<void()> onChange;
    std::function<void (const juce::String&)> onMessage;

private:
    Processor& syn;
    juce::Point<int> grabbed { -1, -1 };
};

//==============================================================================
NymphesPanel::NymphesPanel (Processor& p) : syn (p)
{
    // ---- the top row: play mode, legato; SEND ALL, KEEP IN STEP ----
    const auto modes = juce::StringArray::fromTokens (control (playMode).choices, "|", "");
    for (int i = 0; i < modes.size(); ++i)
        chip (modes[i], [this, i] { return syn.value (playMode) == i; }, [this, i] { syn.setValue (playMode, i); });
    chip ("LEGATO", [this] { return syn.value (legato) == 1; }, [this] { syn.setValue (legato, 1 - syn.value (legato)); });
    chip ("SEND ALL", [] { return false; }, [this] { syn.sendAll(); if (onMessage) onMessage ("EVERYTHING SENT TO THE NYMPHES"); });
    chip ("KEEP IN STEP", [this] { return syn.keepsInStep(); }, [this] { syn.setKeepInStep (! syn.keepsInStep()); });

    // ---- the knobs (placed in resized) ----
    for (int c : { oscWave, oscPw, oscLevel, oscSub, oscNoise, oscDetune, oscChord, oscGlide, oscLfo, oscEg,
                   lpfCutoff, lpfRes, lpfEg, lpfTrack, lpfLfo, hpfCutoff, revSize, revDecay, revFilter, revMix, ampLevel,
                   fegA, fegD, fegS, fegR, aegA, aegD, aegS, aegR,
                   lfo1Rate, lfo1Wave, lfo1Delay, lfo1Fade, lfo2Rate, lfo2Wave, lfo2Delay, lfo2Fade })
        knob (c, {}, 17);

    // ---- the LFOs' type and key sync ----
    for (int l = 0; l < 2; ++l)
    {
        const int typeC = l == 0 ? lfo1Type : lfo2Type, syncC = l == 0 ? lfo1Sync : lfo2Sync;
        const auto types = juce::StringArray::fromTokens (control (typeC).choices, "|", "");
        for (int i = 0; i < types.size(); ++i)
            chip (types[i], [this, typeC, i] { return syn.value (typeC) == i; }, [this, typeC, i] { syn.setValue (typeC, i); });
        chip ("KEY SYNC", [this, syncC] { return syn.value (syncC) == 1; }, [this, syncC] { syn.setValue (syncC, 1 - syn.value (syncC)); });
    }

    // ---- MPE ----
    chip ("OFF", [this] { return ! syn.isMpe(); }, [this] { syn.setMpe (false); });
    chip ("ON: LOWER ZONE", [this] { return syn.isMpe(); }, [this] { syn.setMpe (true); });
    for (int r : { 12, 24, 48 })
        chip (juce::String (juce::CharPointer_UTF8 ("\xc2\xb1")) + juce::String (r), [this, r] { return syn.bendRange() == r; }, [this, r] { syn.setBendRange (r); });
    // ---- the matrix's depths: unsigned, or centred ----
    chip ("0 = NONE", [this] { return ! syn.depthsCentred(); }, [this] { syn.setDepthsCentred (false); });
    chip ("64 = NONE", [this] { return syn.depthsCentred(); }, [this] { syn.setDepthsCentred (true); });

    filterEg = std::make_unique<Envelope> (syn, fegA, "TO CUTOFF BY FILTER > EG");
    ampEg = std::make_unique<Envelope> (syn, aegA, juce::String());
    for (auto* e : { filterEg.get(), ampEg.get() }) { e->onChange = [this] { repaint(); for (auto* k : knobs) k->repaint(); }; addAndMakeVisible (*e); }
    matrix = std::make_unique<Matrix> (syn);
    matrix->onMessage = [this] (const juce::String& m) { if (onMessage) onMessage (m); };
    addAndMakeVisible (*matrix);
    shown = syn.snapshot();
    startTimerHz (20);
}

NymphesPanel::~NymphesPanel() = default;

NymphesPanel::Knob& NymphesPanel::knob (int c, juce::Point<int>, int radius)
{
    auto* k = knobs.add (new Knob (syn, c, radius));
    static const std::map<int, const char*> labels {
        { oscWave, "WAVE" }, { oscPw, "PW" }, { oscLevel, "LEVEL" }, { oscSub, "SUB" }, { oscNoise, "NOISE" }, { oscDetune, "DETUNE" },
        { oscChord, "CHORD" }, { oscGlide, "GLIDE" }, { oscLfo, "LFO 1" }, { oscEg, "EG" },
        { lpfCutoff, "CUTOFF" }, { lpfRes, "RES" }, { lpfEg, "EG" }, { lpfTrack, "TRACK" }, { lpfLfo, "LFO 1" }, { hpfCutoff, "HPF" },
        { revSize, "SIZE" }, { revDecay, "DECAY" }, { revFilter, "FILTER" }, { revMix, "MIX" }, { ampLevel, "LEVEL" },
        { fegA, "ATTACK" }, { fegD, "DECAY" }, { fegS, "SUSTAIN" }, { fegR, "RELEASE" },
        { aegA, "ATTACK" }, { aegD, "DECAY" }, { aegS, "SUSTAIN" }, { aegR, "RELEASE" },
        { lfo1Rate, "RATE" }, { lfo1Wave, "WAVE" }, { lfo1Delay, "DELAY" }, { lfo1Fade, "FADE" },
        { lfo2Rate, "RATE" }, { lfo2Wave, "WAVE" }, { lfo2Delay, "DELAY" }, { lfo2Fade, "FADE" } };
    if (auto it = labels.find (c); it != labels.end()) k->label = it->second;
    k->onChange = [this] { repaint(); filterEg->repaint(); ampEg->repaint(); };
    addAndMakeVisible (k);
    return *k;
}

LcdChip& NymphesPanel::chip (const juce::String& label, std::function<bool()> lit, std::function<void()> click)
{
    auto* c = chips.add (new LcdChip (label));
    c->textSize = 10.0f;
    c->chosen = lit();
    c->onClick = [this, act = std::move (click)] { act(); refresh(); };
    chipLit.push_back (std::move (lit));
    addAndMakeVisible (c);
    return *c;
}

void NymphesPanel::resized()
{
    // the top row
    int x = 76;
    for (int i = 0; i < 7; ++i)
    {
        auto* c = chips[i];
        c->setBounds (x + (i == 6 ? 10 : 0), 0, c->idealWidth(), 24);
        x = c->getRight() + 5;
    }
    chips[8]->setBounds (panelWidth - chips[8]->idealWidth(), 0, chips[8]->idealWidth(), 24);
    chips[7]->setBounds (chips[8]->getX() - chips[7]->idealWidth() - 8, 0, chips[7]->idealWidth(), 24);

    oscArea = { 0, 48, 400, 240 };
    filterArea = { 410, 48, 400, 240 };
    reverbArea = { 820, 48, 392, 240 };
    envArea = { 0, 298, 604, 232 };
    lfo1Area = { 614, 298, 598, 111 };
    lfo2Area = { 614, 419, 598, 111 };
    mpeArea = { 0, 540, 1212, 100 };
    matrixArea = { 0, 650, 1212, 230 };

    auto place = [this] (int c, int cx, int cy, int r = 17)
    {
        for (auto* k : knobs)
            if (k->getTooltip().startsWith (control (c).name))
            {
                k->radius = r;
                k->setBounds (cx - 40, cy - r - 4, 80, 2 * r + 36);
            }
    };
    // oscillator: a wave strip, then two rows of five
    waveView = { oscArea.getX() + 10, oscArea.getY() + 26, 380, 40 };
    const int oscKnobs[] { oscWave, oscPw, oscLevel, oscSub, oscNoise, oscDetune, oscChord, oscGlide, oscLfo, oscEg };
    for (int i = 0; i < 10; ++i) place (oscKnobs[i], oscArea.getX() + 40 + (i % 5) * 80, oscArea.getY() + 100 + (i / 5) * 76);
    // filter: its response, then cutoff and resonance bigger
    filterView = { filterArea.getX() + 10, filterArea.getY() + 26, 380, 56 };
    const int fk[] { lpfCutoff, lpfRes, lpfEg, lpfTrack, lpfLfo, hpfCutoff };
    for (int i = 0; i < 6; ++i) place (fk[i], filterArea.getX() + 46 + (i % 4) * 96, filterArea.getY() + (i < 4 ? 120 : 194), i < 2 ? 19 : 17);
    // reverb, and the amp's level
    reverbView = { reverbArea.getX() + 10, reverbArea.getY() + 26, 372, 56 };
    const int rk[] { revSize, revDecay, revFilter, revMix };
    for (int i = 0; i < 4; ++i) place (rk[i], reverbArea.getX() + 50 + i * 92, reverbArea.getY() + 120);
    place (ampLevel, reverbArea.getX() + 50, reverbArea.getY() + 204, 15);
    // the envelopes
    filterEg->setBounds (envArea.getX() + 10, envArea.getY() + 26, 300, 96);
    ampEg->setBounds (envArea.getX() + 10, envArea.getY() + 160, 300, 64);
    const int fe[] { fegA, fegD, fegS, fegR }, ae[] { aegA, aegD, aegS, aegR };
    for (int i = 0; i < 4; ++i)
    {
        place (fe[i], envArea.getX() + 352 + i * 66, envArea.getY() + 56, 16);
        place (ae[i], envArea.getX() + 352 + i * 66, envArea.getY() + 186, 16);
    }
    // the LFOs
    for (int l = 0; l < 2; ++l)
    {
        const auto a = l == 0 ? lfo1Area : lfo2Area;
        (l == 0 ? lfo1View : lfo2View) = juce::Rectangle<int> (a.getX() + 10, a.getY() + 26, 190, 78);
        const int lk[] { l == 0 ? lfo1Rate : lfo2Rate, l == 0 ? lfo1Wave : lfo2Wave, l == 0 ? lfo1Delay : lfo2Delay, l == 0 ? lfo1Fade : lfo2Fade };
        for (int i = 0; i < 4; ++i) place (lk[i], a.getX() + 236 + i * 60, a.getY() + 52, 15);
        const int base = 9 + l * 5;
        for (int i = 0; i < 4; ++i) chips[base + i]->setBounds (a.getX() + 478 + (i % 2) * 56, a.getY() + 30 + (i / 2) * 25, 52, 22);
        chips[base + 4]->setBounds (a.getX() + 478, a.getY() + 82, 108, 22);
    }
    // MPE
    chips[19]->setBounds (mpeArea.getX() + 66, mpeArea.getY() + 26, chips[19]->idealWidth(), 24);
    chips[20]->setBounds (chips[19]->getRight() + 6, mpeArea.getY() + 26, chips[20]->idealWidth(), 24);
    for (int i = 0; i < 3; ++i) chips[21 + i]->setBounds (mpeArea.getX() + 66 + i * 50, mpeArea.getY() + 68, 46, 24);
    voicesView = { mpeArea.getX() + 626, mpeArea.getY() + 10, 576, 84 };
    // the matrix
    matrix->setBounds (matrixArea.getX() + 10, matrixArea.getY() + 26, matrix->width(), Matrix::headH + numSources * Matrix::rowH);
    chips[24]->setBounds (matrixArea.getRight() - 10 - 80 - 6 - 80, matrixArea.getY() + 4, 80, 20);
    chips[25]->setBounds (matrixArea.getRight() - 10 - 80, matrixArea.getY() + 4, 80, 20);
}

void NymphesPanel::paint (juce::Graphics& g)
{
    iw::text (g, "PLAY MODE", { 0, 0, 74, 24 }, Pal::lcdDim, 9.5f);
    const int ls = syn.lastSent();
    juce::String status = juce::String ("OUT: CHANNEL 1") + (syn.isMpe() ? " (THE MPE ZONE'S MASTER; NOTES ON 2-16 AS PLAYED)" : " (THE EXTERNAL INSTRUMENT BLOCK MOVES IT TO ITS CHANNEL)");
    if (ls >= 0)
    {
        const int cc = ls >> 8, v = ls & 0xff;
        juce::String what = cc == 30 ? juce::String ("MOD SOURCE") : juce::String();
        for (int c = 0; c < numControls; ++c) if (control (c).cc == cc) what = control (c).name;
        for (int d = 0; d < numDests && what.isEmpty(); ++d) if (destCc (d) == cc) what = juce::String (destGroup (d)) + " " + destName (d) + " DEPTH";
        status << dot << "LAST SENT: CC " << cc << " " << what << " = " << v;
    }
    iw::text (g, status, { 0, 28, 820, 14 }, Pal::lcdDim, 9.0f);
    iw::text (g, "KNOBS: DRAG" + dot + "WHEEL: FINE" + dot + "DOUBLE-CLICK: DEFAULT" + dot + "MIDI LEARN: THE BLOCK'S MIDI BADGE", { 820, 28, 392, 14 }, Pal::lcdDim, 9.0f, juce::Justification::centredRight);

    // ---- oscillator ----
    section (g, oscArea, "OSCILLATOR", "PER VOICE");
    g.setColour (Pal::lcd);
    g.fillRect (waveView);
    {
        const double w = syn.value (oscWave) / 127.0 * 2.0;
        std::vector<juce::Point<float>> pts;
        for (int i = 0; i < waveView.getWidth() - 20; ++i)
        {
            const double ph = std::fmod (i / 120.0, 1.0);
            const double saw = 1.0 - 2.0 * ph, sq = ph < 0.5 ? 1.0 : -1.0, tri = 1.0 - 4.0 * std::abs (ph - 0.5);
            const double v = w < 1.0 ? (1.0 - w) * saw + w * sq : (2.0 - w) * sq + (w - 1.0) * tri;
            pts.push_back ({ (float) (waveView.getX() + 10 + i), (float) waveView.getCentreY() - 4.0f - (float) v * 12.0f });
        }
        path (g, pts, Pal::lcdText);
        for (auto [t, x] : { std::pair<const char*, float> { "SAW", 0.08f }, { "SQUARE", 0.5f }, { "TRIANGLE", 0.92f } })
            iw::text (g, t, { waveView.getX() + juce::roundToInt (x * (float) waveView.getWidth()) - 40, waveView.getBottom() - 12, 80, 11 }, Pal::lcdDim, 8.0f, juce::Justification::centred);
    }
    iw::text (g, "LFO 1: LFO 1 TO PITCH" + dot + "EG: FILTER EG TO PITCH" + dot + "CHORD: IN UNI, TRI AND DUO", { oscArea.getX() + 10, oscArea.getBottom() - 18, 380, 12 }, Pal::lcdDim, 8.0f);

    // ---- filter: the response, from the cutoffs and the resonance (a picture, not a measurement) ----
    section (g, filterArea, "FILTER", "LPF 24 dB" + dot + "HPF 6 dB AFTER");
    g.setColour (Pal::lcd);
    g.fillRect (filterView);
    {
        const double lp = 0.08 + 0.9 * syn.value (lpfCutoff) / 127.0, hp = 0.02 + 0.6 * syn.value (hpfCutoff) / 127.0, q = syn.value (lpfRes) / 127.0;
        std::vector<juce::Point<float>> pts;
        for (int i = 0; i < filterView.getWidth(); ++i)
        {
            const double f = (double) i / filterView.getWidth();
            const double hpG = f / std::sqrt (f * f + hp * hp);
            const double x = f / lp;
            const double lpG = 1.0 / std::sqrt (1.0 + std::pow (x, 8.0));
            const double bump = q * 1.2 * std::exp (-std::pow ((f - lp) / 0.035, 2.0));
            const double v = std::min (1.6, hpG * (lpG + bump));
            pts.push_back ({ (float) (filterView.getX() + i), (float) filterView.getBottom() - 4.0f - (float) v * ((float) filterView.getHeight() - 14.0f) / 1.6f });
        }
        path (g, pts, Pal::lcdText);
    }
    iw::text (g, "RES SELF-OSCILLATES AT THE TOP.", { filterArea.getX() + 230, filterArea.getY() + 186, 165, 12 }, Pal::lcdDim, 8.0f);
    iw::text (g, "HPF AFTER THE LPF: A BAND-PASS.", { filterArea.getX() + 230, filterArea.getY() + 200, 165, 12 }, Pal::lcdDim, 8.0f);

    // ---- reverb and amp ----
    section (g, reverbArea, "REVERB", "MENU 3 ON THE SYNTH");
    g.setColour (Pal::lcd);
    g.fillRect (reverbView);
    {
        const double decay = 20.0 + 300.0 * syn.value (revDecay) / 127.0, mix = syn.value (revMix) / 127.0, size = syn.value (revSize) / 127.0;
        g.setColour (Pal::lcdText);
        g.fillRect (reverbView.getX() + 6, reverbView.getY() + 6, 2, reverbView.getHeight() - 10);   // the dry sound
        for (int i = 6; i < reverbView.getWidth() - 10; i += 4)
        {
            const double h = (reverbView.getHeight() - 12) * mix * std::exp (-(i - 6) / decay) * (0.6 + 0.4 * std::pow (std::sin (i * (0.3 + size)), 2.0));
            g.setColour (Pal::lcdDim);
            g.fillRect ((float) (reverbView.getX() + 6 + i), (float) reverbView.getBottom() - 4.0f - (float) h, 1.0f, (float) h);
        }
        iw::text (g, "MIX " + juce::String (juce::roundToInt (mix * 100.0)) + "%", reverbView.reduced (6, 2).withHeight (14), Pal::lcdDim, 8.5f, juce::Justification::centredRight);
    }
    g.setColour (Pal::lcdRule);
    g.fillRect (reverbArea.getX() + 10, reverbArea.getY() + 168, reverbArea.getWidth() - 20, 1);
    iw::text (g, "AMP", { reverbArea.getX() + 10, reverbArea.getY() + 174, 60, 16 }, Pal::lcdText, 11.0f);
    iw::text (g, "THE WHOLE VOICE'S LEVEL; THE TRACK'S", { reverbArea.getX() + 96, reverbArea.getY() + 196, 280, 12 }, Pal::lcdDim, 8.0f);
    iw::text (g, "FADER STILL SETS WHAT YOU RECORD.", { reverbArea.getX() + 96, reverbArea.getY() + 210, 280, 12 }, Pal::lcdDim, 8.0f);

    // ---- envelopes ----
    section (g, envArea, "FILTER EG", "ADSR: DRAG THE POINTS, OR THE KNOBS");
    g.setColour (Pal::lcdRule);
    g.fillRect (envArea.getX() + 10, envArea.getY() + 130, envArea.getWidth() - 20, 1);
    iw::text (g, "AMP EG", { envArea.getX() + 10, envArea.getY() + 136, 100, 20 }, Pal::lcdText, 12.0f);

    // ---- LFOs ----
    for (int l = 0; l < 2; ++l)
    {
        const auto a = l == 0 ? lfo1Area : lfo2Area;
        const auto v = l == 0 ? lfo1View : lfo2View;
        section (g, a, l == 0 ? "LFO 1" : "LFO 2", l == 0 ? "PER VOICE" + dot + "PITCH AND CUTOFF" : "ONE FOR ALL" + dot + "A MOD SOURCE");
        iw::text (g, "TYPE", { a.getX() + 436, a.getY() + 30, 38, 22 }, Pal::lcdDim, 8.5f);
        g.setColour (Pal::lcd);
        g.fillRect (v);
        const int shape = lfoShape (syn.value (l == 0 ? lfo1Wave : lfo2Wave));
        const double delay = syn.value (l == 0 ? lfo1Delay : lfo2Delay) / 127.0 * 0.5, fade = syn.value (l == 0 ? lfo1Fade : lfo2Fade) / 127.0 * 0.5;
        const double cycles = 1.5 + 3.0 * syn.value (l == 0 ? lfo1Rate : lfo2Rate) / 127.0;
        juce::Random rnd (7);
        double held = 0.0, lastPh = 1.0;
        std::vector<juce::Point<float>> pts;
        for (int i = 0; i < v.getWidth(); ++i)
        {
            const double t = (double) i / v.getWidth(), ph = std::fmod (t * cycles, 1.0);
            if (ph < lastPh) held = rnd.nextDouble() * 2.0 - 1.0;
            lastPh = ph;
            double y = 0.0;
            switch (shape)
            {
                case 0: y = 1.0 - 4.0 * std::abs (ph - 0.5); break;
                case 1: y = 1.0 - 2.0 * ph; break;
                case 2: y = 2.0 * ph - 1.0; break;
                case 3: y = ph < 0.5 ? 1.0 : -1.0; break;
                default: y = held; break;
            }
            const double env = t < delay ? 0.0 : std::min (1.0, (t - delay) / std::max (1.0e-3, fade));
            pts.push_back ({ (float) (v.getX() + i), (float) v.getCentreY() - (float) (y * env) * ((float) v.getHeight() * 0.5f - 5.0f) });
        }
        path (g, pts, Pal::lcdText);
        if (delay > 0.0)
        {
            g.setColour (Pal::lcdRule);
            g.fillRect ((float) v.getX() + (float) delay * (float) v.getWidth(), (float) v.getY() + 3.0f, 1.0f, (float) v.getHeight() - 6.0f);
        }
    }

    // ---- MPE, and what's playing ----
    section (g, mpeArea, "MPE" + dot + "EXPRESSION", juce::String());
    iw::text (g, "MPE", { mpeArea.getX() + 10, mpeArea.getY() + 26, 50, 24 }, Pal::lcdDim, 9.5f);
    iw::text (g, "NOTES ON CHANNELS 2-16, THE SOUND'S SETTINGS ON 1: SET THE EXTERNAL INSTRUMENT BLOCK TO AS PLAYED.",
              { mpeArea.getX() + 10, mpeArea.getY() + 52, 600, 12 }, Pal::lcdDim, 8.0f);
    iw::text (g, "BEND", { mpeArea.getX() + 10, mpeArea.getY() + 68, 50, 24 }, Pal::lcdDim, 9.5f);
    iw::text (g, "ON THE NYMPHES TOO: MPE ON (MENU 7, HOLD SHIFT), AND THE SAME BEND (ITS PITCH WHEEL SETTING).",
              { mpeArea.getX() + 222, mpeArea.getY() + 68, 400, 24 }, Pal::lcdDim, 8.0f);
    iw::text (g, "SLIDE (CC 74) IS THE NYMPHES' CUTOFF, PER NOTE.", { mpeArea.getX() + 222, mpeArea.getY() + 30, 380, 14 }, Pal::lcdDim, 8.0f);
    {
        const auto v = voicesView;
        iw::text (g, "PLAYING NOW", { v.getX(), v.getY(), 90, 12 }, Pal::lcdDim, 8.5f);
        const int cols[] { 90, 140, 290, 440 };
        const char* heads[] { "NOTE", "BEND", "PRESSURE", "SLIDE" };
        for (int i = 0; i < 4; ++i) iw::text (g, heads[i], { v.getX() + cols[i], v.getY(), 100, 12 }, Pal::lcdDim, 8.0f);
        static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        for (int i = 0; i < numVoices; ++i)
        {
            const auto& vo = syn.voice (i);
            const int note = vo.note.load(), y = v.getY() + 14 + i * 12;
            const bool on = note >= 0;
            iw::text (g, "VOICE " + juce::String (i + 1), { v.getX(), y, 80, 11 }, on ? Pal::lcdText : Pal::lcdDim, 8.0f);
            iw::text (g, on ? juce::String (names[note % 12]) + juce::String (note / 12 - 1) : juce::String ("-"), { v.getX() + cols[0], y, 40, 11 }, on ? Pal::lcdText : Pal::lcdRule, 8.5f);
            auto bar = [&] (int x, float f, bool fromCentre)
            {
                const juce::Rectangle<float> r ((float) (v.getX() + x), (float) y + 2.0f, 130.0f, 6.0f);
                g.setColour (Pal::lcd);
                g.fillRect (r);
                if (! on) return;
                g.setColour (Pal::lcdText);
                if (fromCentre)
                {
                    const float c = r.getCentreX(), e = c + f * r.getWidth() * 0.5f;
                    g.fillRect (juce::Rectangle<float> (std::min (c, e), r.getY(), std::max (1.0f, std::abs (e - c)), r.getHeight()));
                }
                else g.fillRect (r.withWidth (r.getWidth() * f));
            };
            bar (cols[1], vo.bend.load(), true);
            bar (cols[2], vo.pressure.load(), false);
            bar (cols[3], vo.slide.load(), false);
        }
    }

    // ---- the matrix ----
    section (g, matrixArea, "MODULATION", juce::String());
    iw::text (g, "DEPTHS", { chips[24]->getX() - 60, chips[24]->getY(), 56, 20 }, Pal::lcdDim, 8.5f, juce::Justification::centredRight);
    const int ly = matrix->getBottom() + 4;
    g.setColour (Pal::lcdText);
    g.fillEllipse ((float) matrixArea.getX() + 12.0f, (float) ly + 4.0f, 5.0f, 5.0f);
    iw::text (g, ": PER NOTE WITH MPE OR POLY AFTERTOUCH; THE OTHERS MOVE ALL NOTES. PRESSURE: AFTERTOUCH, EACH NOTE'S OWN WITH MPE.",
              { matrixArea.getX() + 20, ly, 1100, 13 }, Pal::lcdText, 8.5f);
    iw::text (g, "DRAG IN A CELL FOR ITS DEPTH" + dot + "DOUBLE-CLICK: UNSET (NOT SENT)" + dot + "A SOURCE'S NAME: CLEAR ITS ROW" + dot + "EACH CHANGE: CC 30 (THE SOURCE), THEN THE DEPTH",
              { matrixArea.getX() + 10, ly + 15, 1190, 13 }, Pal::lcdDim, 8.5f);
    iw::text (g, "STRUCK THROUGH: NOT ON THE SYNTH (LFO 2 REACHES OSC, FILTER AND THE EGS). DASHED: IN THE CC CHART, NOT IN THE MANUAL'S 28: TRY THEM. "
                 "DEPTHS: WHETHER 0 OR 64 IS NONE ISN'T IN THE MANUAL.",
              { matrixArea.getX() + 10, ly + 30, 1190, 13 }, Pal::lcdDim, 8.5f);
}

void NymphesPanel::refresh()
{
    for (int i = 0; i < chips.size(); ++i)
        if (const bool on = chipLit[(size_t) i](); chips[i]->chosen != on) { chips[i]->chosen = on; chips[i]->repaint(); }
    for (auto* k : knobs) k->repaint();
    filterEg->repaint();
    ampEg->repaint();
    matrix->repaint();
    repaint();
}

void NymphesPanel::timerCallback()
{
    // the sound changed (a CC came back, a learned control, a preset): show it
    const auto now = syn.snapshot();
    const bool changed = now.values != shown.values || now.depth != shown.depth || now.mpe != shown.mpe || now.bendRange != shown.bendRange
                         || now.keepInStep != shown.keepInStep || now.depthCentred != shown.depthCentred;
    if (changed) { shown = now; refresh(); }
    else if (syn.lastSent() != shownSent) { shownSent = syn.lastSent(); repaint (0, 26, panelWidth, 18); }
    juce::String v;
    for (int i = 0; i < numVoices; ++i)
        v << syn.voice (i).note.load() << juce::roundToInt (syn.voice (i).bend.load() * 64) << juce::roundToInt (syn.voice (i).pressure.load() * 64) << juce::roundToInt (syn.voice (i).slide.load() * 64) << ",";
    if (v != voicesShown) { voicesShown = v; repaint (voicesView); }
}
} // namespace tf
