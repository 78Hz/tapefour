#include "N5Panel.h"

namespace tf
{
namespace
{
    using namespace iw;
    using namespace n5;
    const char* const tabNames[] { "OSC", "PITCH", "VDF", "VDA", "CONTROL", "WAVES", "EFFECTS", "COMBI", "ARP" };
    constexpr int numTabs = 9, row = 20, rowGap = 4;
    const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
}

//==============================================================================
/** One setting: its caption, a bar (or chips, or four switches) and its value. Drag to set; double-click: initial value. */
class N5Panel::FieldRow : public juce::Component
{
public:
    FieldRow (const Field& f, std::function<int()> g, std::function<void (int)> s, std::function<int (int)> q = nullptr)
        : field (f), get (std::move (g)), set (std::move (s)), quadGet (std::move (q)) {}

    static constexpr int captionWidth = 150, valueWidth = 116;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        text (g, field.caption, r.removeFromLeft (captionWidth), Pal::lcdDim, 10.0f);
        if (field.fmt == Fmt::quad)
        {
            // AT DT ST RT: each OFF, + or -
            static const char* stages[] { "AT", "DT", "ST", "RT" };
            for (int k = 0; k < 4; ++k)
            {
                auto cell = cellArea (k);
                const int v = quadGet (k);
                g.setColour (v == 0 ? Pal::lcdRule : Pal::lcdText);
                g.drawRoundedRectangle (cell.toFloat().reduced (0.5f), 3.0f, 1.0f);
                if (v != 0) { g.setColour (Pal::lcdText.withAlpha (0.18f)); g.fillRoundedRectangle (cell.toFloat().reduced (1.0f), 3.0f); }
                text (g, juce::String (stages[k]) + " " + fieldText (field, v), cell, v == 0 ? Pal::lcdDim : Pal::lcdText, 9.5f, juce::Justification::centred);
            }
            return;
        }
        const int v = get();
        if (isChips())
        {
            const auto labels = juce::StringArray::fromTokens (field.labels, "|", "");
            for (int k = 0; k < labels.size(); ++k)
            {
                auto cell = cellArea (k);
                const bool on = v - field.lo == k;
                g.setColour (on ? Pal::lcdText : Pal::lcdRule);
                if (on) g.fillRoundedRectangle (cell.toFloat().reduced (0.5f), 3.0f);
                else g.drawRoundedRectangle (cell.toFloat().reduced (0.5f), 3.0f, 1.0f);
                text (g, labels[k], cell, on ? Pal::lcd : Pal::lcdText, 9.5f, juce::Justification::centred);
            }
            return;
        }
        text (g, fieldText (field, v), r.removeFromRight (valueWidth), Pal::lcdText, 10.0f, juce::Justification::centredRight);
        bar = r.reduced (4, 0).withSizeKeepingCentre (r.getWidth() - 12, 5);
        g.setColour (Pal::lcdRule);
        g.fillRoundedRectangle (bar.toFloat(), 2.5f);
        g.setColour (Pal::lcdText);
        const float frac = (float) (v - field.lo) / (float) std::max (1, field.hi - field.lo);
        if (field.lo < 0 && field.hi > 0)
        {
            const float mid = (float) bar.getX() + (float) bar.getWidth() * (float) -field.lo / (float) (field.hi - field.lo);
            const float at = (float) bar.getX() + (float) bar.getWidth() * frac;
            g.fillRoundedRectangle (juce::Rectangle<float> (std::min (mid, at), (float) bar.getY(), std::abs (at - mid) + 1.0f, 5.0f), 2.5f);
            g.setColour (Pal::lcdDim);
            g.fillRect (juce::Rectangle<float> (mid - 0.5f, (float) bar.getY() - 3.0f, 1.0f, 11.0f));
        }
        else g.fillRoundedRectangle (bar.toFloat().withWidth ((float) bar.getWidth() * frac), 2.5f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (field.fmt == Fmt::quad)
        {
            for (int k = 0; k < 4; ++k) if (cellArea (k).contains (e.getPosition())) quadSet (k);
            return;
        }
        if (isChips())
        {
            const auto labels = juce::StringArray::fromTokens (field.labels, "|", "");
            for (int k = 0; k < labels.size(); ++k) if (cellArea (k).contains (e.getPosition())) { set (field.lo + k); repaint(); }
            return;
        }
        dragFrom = get();
        mouseDrag (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (field.fmt == Fmt::quad || isChips() || bar.getWidth() <= 0) return;
        const float frac = juce::jlimit (0.0f, 1.0f, (float) (e.x - bar.getX()) / (float) bar.getWidth());
        set (field.lo + juce::roundToInt (frac * (float) (field.hi - field.lo)));
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        if (field.fmt == Fmt::quad || isChips()) return;
        set (juce::jlimit (field.lo, field.hi, get() + (w.deltaY > 0 ? 1 : -1)));
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (field.fmt == Fmt::quad || isChips()) return;
        set (field.def);
        repaint();
    }

    std::function<void (int)> quadSetter;

private:
    bool isChips() const { return field.fmt == Fmt::choice && field.labels != nullptr && juce::StringArray::fromTokens (field.labels, "|", "").size() <= 8; }
    juce::Rectangle<int> cellArea (int k) const
    {
        const int n = field.fmt == Fmt::quad ? 4 : juce::StringArray::fromTokens (field.labels, "|", "").size();
        const int x0 = captionWidth, w = (getWidth() - x0) / std::max (1, n);
        return { x0 + k * w, 0, w - 4, getHeight() };
    }
    void quadSet (int k) { if (quadSetter) quadSetter (k); repaint(); }

    const Field& field;
    std::function<int()> get;
    std::function<void (int)> set;
    std::function<int (int)> quadGet;
    juce::Rectangle<int> bar;
    int dragFrom = 0;
};

//==============================================================================
/** An EG drawn as the N5 shapes it: drag a point (its time across, its level up and down). */
class N5Panel::EnvelopeGraph : public juce::Component
{
public:
    struct Handle { int timeField, levelField; const char* label; };

    EnvelopeGraph (std::vector<Handle> h, int startLevelField, bool bipolar_, std::function<int*()> values, std::function<void()> changedFn)
        : handles (std::move (h)), startField (startLevelField), bipolar (bipolar_), get (std::move (values)), changed (std::move (changedFn)) {}

    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (14.0f, 22.0f).withTrimmedTop (4.0f); }

    float levelY (int v) const
    {
        const auto p = plot();
        const float f = bipolar ? ((float) v + 128.0f) / 255.0f : (float) v / 127.0f;
        return p.getBottom() - p.getHeight() * f;
    }

    std::vector<juce::Point<float>> points() const
    {
        const auto p = plot();
        const int* L = get();
        std::vector<juce::Point<float>> pts;
        float x = p.getX();
        pts.push_back ({ x, startField >= 0 ? levelY (L[startField]) : levelY (0) });
        const float unit = (p.getWidth() - 60.0f) / (float) std::max<size_t> (1, handles.size());
        for (size_t k = 0; k < handles.size(); ++k)
        {
            const auto& h = handles[k];
            if (k == handles.size() - 1) x += 60.0f;   // the hold, until the key's let go
            x += 6.0f + (unit - 6.0f) * (h.timeField >= 0 ? (float) L[h.timeField] / 127.0f : 0.5f);
            pts.push_back ({ x, h.levelField >= 0 ? levelY (L[h.levelField]) : levelY (0) });
        }
        return pts;
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Pal::lcdRule);
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        text (g, "EG: DRAG A POINT (TIME ACROSS, LEVEL UP AND DOWN)", getLocalBounds().reduced (10, 4).removeFromTop (12), Pal::lcdDim, 8.5f);
        const auto p = plot();
        g.setColour (Pal::lcdRule);
        g.drawHorizontalLine ((int) levelY (0), p.getX(), p.getRight());
        const auto pts = points();
        // the key coming up: before the last segment
        const float keyOff = pts[pts.size() - 2].x + 60.0f;
        g.setColour (Pal::lcdRule.brighter (0.3f));
        g.drawVerticalLine ((int) keyOff, p.getY(), p.getBottom());
        text (g, "KEY OFF", { (int) keyOff + 4, (int) p.getY() - 2, 60, 10 }, Pal::lcdDim, 8.0f);
        juce::Path path;
        path.startNewSubPath (pts[0]);
        for (size_t k = 1; k < pts.size(); ++k)
        {
            if (k == pts.size() - 1) path.lineTo (keyOff, pts[k - 1].y);
            path.lineTo (pts[k]);
        }
        g.setColour (Pal::lcdText);
        g.strokePath (path, juce::PathStrokeType (1.8f));
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const bool draggable = k == 0 ? startField >= 0 : true;
            if (! draggable) continue;
            g.setColour ((int) k == dragging ? Pal::lamp : Pal::lcdText);
            g.fillEllipse (pts[k].x - 4.5f, pts[k].y - 4.5f, 9.0f, 9.0f);
            if (k > 0) text (g, handles[k - 1].label, { (int) pts[k].x - 40, (int) p.getBottom() + 4, 80, 12 }, Pal::lcdDim, 8.0f, juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = -1;
        const auto pts = points();
        float best = 14.0f;
        for (size_t k = 0; k < pts.size(); ++k)
        {
            if (k == 0 && startField < 0) continue;
            const float d = pts[k].getDistanceFrom (e.position);
            if (d < best) { best = d; dragging = (int) k; }
        }
        if (dragging >= 0)
        {
            int* L = get();
            fromTime = dragging > 0 && handles[(size_t) dragging - 1].timeField >= 0 ? L[handles[(size_t) dragging - 1].timeField] : 0;
            fromLevel = dragging == 0 ? L[startField] : handles[(size_t) dragging - 1].levelField >= 0 ? L[handles[(size_t) dragging - 1].levelField] : 0;
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        int* L = get();
        const auto p = plot();
        const float unit = (p.getWidth() - 60.0f) / (float) std::max<size_t> (1, handles.size());
        const int range = bipolar ? 255 : 127;
        const int dLevel = juce::roundToInt (-(float) e.getDistanceFromDragStartY() / p.getHeight() * (float) range);
        if (dragging == 0) L[startField] = juce::jlimit (-128, 127, fromLevel + dLevel);
        else
        {
            const auto& h = handles[(size_t) dragging - 1];
            if (h.timeField >= 0) L[h.timeField] = juce::jlimit (0, 127, fromTime + juce::roundToInt ((float) e.getDistanceFromDragStartX() / (unit - 6.0f) * 127.0f));
            if (h.levelField >= 0) L[h.levelField] = juce::jlimit (bipolar ? -128 : 0, 127, fromLevel + dLevel);
        }
        changed();
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = -1; repaint(); }

private:
    std::vector<Handle> handles;
    int startField;
    bool bipolar;
    std::function<int*()> get;
    std::function<void()> changed;
    int dragging = -1, fromTime = 0, fromLevel = 0;
};

//==============================================================================
/** One cycle of a drawn wave, drawn with the mouse. */
class N5Panel::WaveCanvas : public juce::Component
{
public:
    WaveCanvas (n5::Processor& p, int slot_) : synth (p), slot (slot_), points (p.drawnWave (slot_)) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Pal::lcdRule);
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        text (g, "WAVE " + juce::String (slot + 1).paddedLeft ('0', 2) + dot + "\"" + synth.drawnName (slot) + "\"" + dot + "DRAW WITH THE MOUSE" + dot + "ONE CYCLE, 256 POINTS",
              getLocalBounds().reduced (10, 4).removeFromTop (12), Pal::lcdDim, 9.0f);
        const auto p = plot();
        g.setColour (Pal::lcdRule.withAlpha (0.5f));
        for (int k = 1; k < 8; ++k) g.drawVerticalLine ((int) (p.getX() + p.getWidth() * (float) k / 8.0f), p.getY(), p.getBottom());
        g.setColour (Pal::lcdRule);
        g.drawHorizontalLine ((int) p.getCentreY(), p.getX(), p.getRight());
        juce::Path path;
        for (int i = 0; i < drawnPoints; ++i)
        {
            const float x = p.getX() + p.getWidth() * (float) i / (float) (drawnPoints - 1);
            const float y = p.getCentreY() - points[(size_t) i] * p.getHeight() * 0.5f;
            if (i == 0) path.startNewSubPath (x, y); else path.lineTo (x, y);
        }
        g.setColour (Pal::lcdText);
        g.strokePath (path, juce::PathStrokeType (2.0f));
    }

    void mouseDown (const juce::MouseEvent& e) override { last = -1; mouseDrag (e); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto p = plot();
        const int i = juce::jlimit (0, drawnPoints - 1, juce::roundToInt ((e.position.x - p.getX()) / p.getWidth() * (drawnPoints - 1)));
        const float v = juce::jlimit (-1.0f, 1.0f, (p.getCentreY() - e.position.y) / (p.getHeight() * 0.5f));
        // fill in between the last point and this one, so a quick stroke leaves no gaps
        if (last >= 0 && last != i)
        {
            const int a = std::min (last, i), b = std::max (last, i);
            for (int k = a; k <= b; ++k)
            {
                const float f = (float) (k - last) / (float) (i - last);
                points[(size_t) k] = lastValue + (v - lastValue) * f;
            }
        }
        points[(size_t) i] = v;
        last = i; lastValue = v;
        synth.setDrawnWave (slot, points);
        if (onChange) onChange();
        repaint();
    }

    void set (const std::array<float, drawnPoints>& pts) { points = pts; synth.setDrawnWave (slot, points); repaint(); if (onChange) onChange(); }
    const std::array<float, drawnPoints>& get() const { return points; }
    std::function<void()> onChange;

private:
    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (14.0f, 10.0f).withTrimmedTop (14.0f); }
    n5::Processor& synth;
    int slot;
    std::array<float, drawnPoints> points;
    int last = -1;
    float lastValue = 0.0f;
};

//==============================================================================
/** An effect's parameter, as the manual shows it. */
class N5Panel::EffectRow : public juce::Component
{
public:
    EffectRow (const EffectParam& p, std::function<int()> g, std::function<void (int)> s) : param (p), get (std::move (g)), set (std::move (s)) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        text (g, juce::String (param.name) + (param.dynamic ? juce::String (juce::CharPointer_UTF8 (" \xe2\x97\x8f")) : juce::String()), r.removeFromLeft (150), Pal::lcdDim, 9.5f);
        text (g, effectValueText (param, get()), r.removeFromRight (84), Pal::lcdText, 9.5f, juce::Justification::centredRight);
        bar = r.reduced (4, 0).withSizeKeepingCentre (r.getWidth() - 10, 5);
        g.setColour (Pal::lcdRule);
        g.fillRoundedRectangle (bar.toFloat(), 2.5f);
        g.setColour (Pal::lcdText);
        g.fillRoundedRectangle (bar.toFloat().withWidth ((float) bar.getWidth() * (float) (get() - param.lo) / (float) std::max (1, param.hi - param.lo)), 2.5f);
    }
    void mouseDown (const juce::MouseEvent& e) override { mouseDrag (e); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (bar.getWidth() <= 0) return;
        set (param.lo + juce::roundToInt (juce::jlimit (0.0f, 1.0f, (float) (e.x - bar.getX()) / (float) bar.getWidth()) * (float) (param.hi - param.lo)));
        repaint();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { set (param.def); repaint(); }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override { set (juce::jlimit (param.lo, param.hi, get() + (w.deltaY > 0 ? 1 : -1))); repaint(); }

private:
    const EffectParam& param;
    std::function<int()> get;
    std::function<void (int)> set;
    juce::Rectangle<int> bar;
};

//==============================================================================
/** The combination's eight timbres: drag a number up or down, click the program to choose it. */
class N5Panel::TimbreTable : public juce::Component
{
public:
    TimbreTable (n5::Processor& p, std::function<void()> changedFn) : synth (p), changed (std::move (changedFn)) {}

    struct Col { const char* title; int field; int x, w; };
    static const std::vector<Col>& cols()
    {
        static const std::vector<Col> c { { "", -1, 0, 30 }, { "PROGRAM", tProgram, 36, 190 }, { "VOL", tVolume, 232, 46 }, { "PAN", tPan, 284, 50 },
                                          { "TRANS", tTranspose, 340, 50 }, { "FINE", tFine, 396, 54 }, { "C", tSendC, 456, 40 }, { "D", tSendD, 502, 40 },
                                          { "KEYS", tNoteLo, 548, 56 }, { "", tNoteHi, 606, 56 }, { "VELOCITY", tVelLo, 670, 48 }, { "", tVelHi, 720, 48 } };
        return c;
    }
    static constexpr int rowH = 26, headH = 18;

    void paint (juce::Graphics& g) override
    {
        auto& c = synth.bank().combis[(size_t) synth.bank().combi];
        for (auto& col : cols()) text (g, col.title, { col.x, 0, col.w + 60, headH }, Pal::lcdDim, 9.0f);
        for (int t = 0; t < numTimbres; ++t)
        {
            const int y = headH + t * rowH;
            const bool sel = t == synth.editorTimbre, on = c.timbre[t][tProgram] >= 0;
            if (sel) { g.setColour (Pal::lcdText.withAlpha (0.08f)); g.fillRect (0, y, getWidth(), rowH - 2); }
            for (auto& col : cols())
            {
                const juce::Rectangle<int> cell (col.x, y, col.w, rowH - 4);
                if (col.field < 0)
                {
                    g.setColour (sel ? Pal::lcdText : Pal::lcdRule);
                    if (sel) g.fillRoundedRectangle (cell.toFloat().withWidth (26), 3.0f); else g.drawRoundedRectangle (cell.toFloat().withWidth (26).reduced (0.5f), 3.0f, 1.0f);
                    text (g, juce::String (t + 1), cell.withWidth (26), sel ? Pal::lcd : Pal::lcdText, 10.0f, juce::Justification::centred);
                    continue;
                }
                const int v = c.timbre[t][col.field];
                juce::String s;
                if (col.field == tProgram) s = v < 0 ? juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94 OFF")) : juce::String (v).paddedLeft ('0', 2) + " " + synth.programName (v);
                else s = fieldText (timbreField (col.field), v);
                text (g, s, cell, on || col.field == tProgram ? (on ? Pal::lcdText : Pal::lcdDim.withAlpha (0.6f)) : Pal::lcdDim.withAlpha (0.4f), 10.0f);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int t = (e.y - headH) / rowH;
        if (t < 0 || t >= numTimbres) return;
        synth.editorTimbre = t;
        dragCol = -1;
        for (size_t k = 0; k < cols().size(); ++k)
            if (e.x >= cols()[k].x && e.x < cols()[k].x + cols()[k].w) dragCol = (int) k;
        if (dragCol >= 0 && cols()[(size_t) dragCol].field == tProgram)
        {
            juce::PopupMenu m;
            m.addItem (1, "OFF");
            for (int i = 0; i < numPrograms; ++i) m.addItem (100 + i, juce::String (i).paddedLeft ('0', 2) + " " + synth.programName (i));
            juce::Component::SafePointer<TimbreTable> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [safe, t] (int r)
            {
                if (safe == nullptr || r == 0) return;
                safe->synth.bank().combis[(size_t) safe->synth.bank().combi].timbre[t][tProgram] = r == 1 ? -1 : r - 100;
                safe->changed();
                safe->repaint();
            });
            dragCol = -1;
        }
        else if (dragCol >= 0 && cols()[(size_t) dragCol].field >= 0)
            dragFrom = synth.bank().combis[(size_t) synth.bank().combi].timbre[t][cols()[(size_t) dragCol].field];
        dragRow = t;
        repaint();
        if (onSelect) onSelect();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragCol < 0 || cols()[(size_t) dragCol].field < 0) return;
        const int f = cols()[(size_t) dragCol].field;
        const auto& fd = timbreField (f);
        const float perPixel = (float) (fd.hi - fd.lo) / 160.0f;
        synth.bank().combis[(size_t) synth.bank().combi].timbre[dragRow][f] = juce::jlimit (fd.lo, fd.hi, dragFrom + juce::roundToInt (-(float) e.getDistanceFromDragStartY() * perPixel));
        changed();
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (dragCol < 0 || cols()[(size_t) dragCol].field <= 0) return;
        const int f = cols()[(size_t) dragCol].field;
        synth.bank().combis[(size_t) synth.bank().combi].timbre[dragRow][f] = timbreField (f).def;
        changed();
        repaint();
    }

    std::function<void()> onSelect;

private:
    n5::Processor& synth;
    std::function<void()> changed;
    int dragCol = -1, dragRow = 0, dragFrom = 0;
};

//==============================================================================
N5Panel::N5Panel (n5::Processor& p) : synth (p)
{
    for (int i = 0; i < numTabs; ++i)
    {
        auto* c = tabChips.add (new LcdChip (tabNames[i]));
        c->textSize = 10.5f;
        c->onClick = [this, i] { synth.editorTab = i; build(); };
        addAndMakeVisible (c);
    }
    startTimerHz (15);
}

N5Panel::~N5Panel() = default;

n5::ProgramData& N5Panel::program()
{
    auto& b = synth.bank();
    if (b.combiMode != 0)
    {
        // in COMBI, the program pages edit the selected timbre's program
        const int pr = b.combis[(size_t) b.combi].timbre[std::clamp (synth.editorTimbre, 0, numTimbres - 1)][tProgram];
        return b.programs[(size_t) std::clamp (pr, 0, numPrograms - 1)];
    }
    return b.programs[(size_t) b.program];
}

n5::FxSettings& N5Panel::fx()
{
    auto& b = synth.bank();
    return b.combiMode != 0 ? b.combis[(size_t) b.combi].fx : b.programs[(size_t) b.program].fx;
}

int* N5Panel::layerValues() { return program().layer[program().global[gOscMode] == 1 ? std::clamp (synth.editorLayer, 0, 1) : 0]; }

void N5Panel::edited() { synth.changed(); }

N5Panel::FieldRow& N5Panel::layerRow (int f, juce::Rectangle<int> r)
{
    const auto& field = layerField (f);
    auto* fieldRow = new FieldRow (field, [this, f] { return layerValues()[f]; },
                              [this, f] (int v) { layerValues()[f] = v; edited(); },
                              [this, f] (int k) { return layerValues()[f + k]; });
    fieldRow->quadSetter = [this, f] (int k) { auto& v = layerValues()[f + k]; v = (v + 1) % 3; edited(); };
    parts.add (fieldRow);
    live.push_back (fieldRow);
    addAndMakeVisible (fieldRow);
    fieldRow->setBounds (r);
    return *fieldRow;
}

N5Panel::FieldRow& N5Panel::globalRow (int f, juce::Rectangle<int> r)
{
    auto* fieldRow = new FieldRow (globalField (f), [this, f] { return program().global[f]; }, [this, f] (int v) { program().global[f] = v; edited(); if (f == gOscMode) buildSoon(); });
    parts.add (fieldRow);
    live.push_back (fieldRow);
    addAndMakeVisible (fieldRow);
    fieldRow->setBounds (r);
    return *fieldRow;
}

N5Panel::FieldRow& N5Panel::arpRow (int f, juce::Rectangle<int> r)
{
    auto* fieldRow = new FieldRow (arpField (f), [this, f] { return synth.bank().arp[f]; }, [this, f] (int v) { synth.bank().arp[f] = v; edited(); });
    parts.add (fieldRow);
    live.push_back (fieldRow);
    addAndMakeVisible (fieldRow);
    fieldRow->setBounds (r);
    return *fieldRow;
}

LcdChip& N5Panel::button (const juce::String& label, std::function<void()> click)
{
    auto* c = new LcdChip (label);
    c->textSize = 10.5f;
    c->onClick = std::move (click);
    parts.add (c);
    addAndMakeVisible (c);
    return *c;
}

void N5Panel::chooseProgram()
{
    auto& b = synth.bank();
    juce::PopupMenu m;
    if (b.combiMode != 0) for (int i = 0; i < numCombis; ++i) m.addItem (100 + i, juce::String (i).paddedLeft ('0', 2) + " " + synth.combiName (i), true, i == b.combi);
    else for (int i = 0; i < numPrograms; ++i) m.addItem (100 + i, juce::String (i).paddedLeft ('0', 2) + " " + synth.programName (i), true, i == b.program);
    juce::Component::SafePointer<N5Panel> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [safe] (int r)
    {
        if (safe == nullptr || r < 100) return;
        auto& bk = safe->synth.bank();
        if (bk.combiMode != 0) bk.combi = r - 100; else bk.program = r - 100;
        safe->edited();
        safe->build();
    });
}

void N5Panel::rename()
{
    auto& b = synth.bank();
    nameEditor = std::make_unique<juce::TextEditor>();
    nameEditor->setFont (DeckLookAndFeel::get().mono (12.0f));
    nameEditor->setColour (juce::TextEditor::backgroundColourId, Pal::lcd);
    nameEditor->setColour (juce::TextEditor::textColourId, Pal::lcdText);
    nameEditor->setColour (juce::TextEditor::outlineColourId, Pal::lcdText);
    nameEditor->setInputRestrictions (10);
    nameEditor->setText (b.combiMode != 0 ? synth.combiName (b.combi) : synth.programName (b.program), false);
    addAndMakeVisible (*nameEditor);
    nameEditor->setBounds (0, 0, 260, 24);
    nameEditor->selectAll();
    nameEditor->grabKeyboardFocus();
    auto finish = [this] (bool keep)
    {
        if (nameEditor == nullptr) return;
        auto& bk = synth.bank();
        if (keep) { if (bk.combiMode != 0) synth.setCombiName (bk.combi, nameEditor->getText().toUpperCase()); else synth.setProgramName (bk.program, nameEditor->getText().toUpperCase()); }
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<N5Panel> (this)] { if (safe != nullptr) { safe->nameEditor.reset(); safe->repaint(); } });
    };
    nameEditor->onReturnKey = [finish] { finish (true); };
    nameEditor->onEscapeKey = [finish] { finish (false); };
    nameEditor->onFocusLost = [finish] { finish (true); };
}

void N5Panel::chooseEffect (int unit)
{
    juce::PopupMenu m;
    juce::String group;
    juce::PopupMenu sub;
    m.addItem (1000, "00 NO EFFECT");
    for (int t = 1; t < numEffectTypes; ++t)
    {
        const auto& et = effectType (t);
        if (group != et.group)
        {
            if (group.isNotEmpty()) m.addSubMenu (group, sub);
            sub = {};
            group = et.group;
        }
        sub.addItem (1000 + t, juce::String (t).paddedLeft ('0', 2) + " " + et.name, true, fx().unit[unit].type == t);
    }
    m.addSubMenu (group, sub);
    juce::Component::SafePointer<N5Panel> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [safe, unit] (int r)
    {
        if (safe == nullptr || r < 1000) return;
        safe->fx().setType (unit, r - 1000);   // the type's initial values, as the N5 does
        safe->edited();
        safe->build();
    });
}

void N5Panel::drawnTool (int which)
{
    WaveCanvas* canvas = nullptr;
    for (auto* p : parts) if (auto* c = dynamic_cast<WaveCanvas*> (p)) canvas = c;
    if (canvas == nullptr) return;
    auto pts = canvas->get();
    switch (which)
    {
        case 0: for (int pass = 0; pass < 3; ++pass) { auto c = pts; for (int i = 0; i < drawnPoints; ++i) pts[(size_t) i] = 0.25f * c[(size_t) ((i + drawnPoints - 1) % drawnPoints)] + 0.5f * c[(size_t) i] + 0.25f * c[(size_t) ((i + 1) % drawnPoints)]; } break;
        case 1: { float peak = 1.0e-6f; for (auto v : pts) peak = std::max (peak, std::abs (v)); for (auto& v : pts) v /= peak; break; }
        case 2: for (auto& v : pts) v = -v; break;
        case 3: std::reverse (pts.begin(), pts.end()); break;
        case 4: std::fill (pts.begin(), pts.end(), 0.0f); break;
        default: break;
    }
    canvas->set (pts);
}

//==============================================================================
void N5Panel::buildSoon()
{
    if (rebuildPending) return;
    rebuildPending = true;
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<N5Panel> (this)]
    {
        if (safe != nullptr) { safe->rebuildPending = false; safe->build(); }
    });
}

void N5Panel::build()
{
    parts.clear();
    live.clear();
    notes.clear();
    diagramArea = harmonicsArea = {};
    auto& b = synth.bank();
    const int tab = std::clamp (synth.editorTab, 0, numTabs - 1);
    builtTab = tab; builtLayer = synth.editorLayer; builtMode = b.combiMode;
    builtOsc = program().global[gOscMode];
    for (int i = 0; i < tabChips.size(); ++i) tabChips[i]->chosen = i == tab;

    // the top row: the program or combination, and PROGRAM / COMBI
    {
        const bool combi = b.combiMode != 0;
        auto& sel = button ((combi ? "COMBI " + juce::String (b.combi).paddedLeft ('0', 2) + " " + synth.combiName (b.combi)
                                   : "PROGRAM " + juce::String (b.program).paddedLeft ('0', 2) + " " + synth.programName (b.program)) + "  v", [this] { chooseProgram(); });
        sel.setBounds (0, 0, 260, 24);
        button ("RENAME", [this] { rename(); }).setBounds (266, 0, 70, 24);
        auto& prog = button ("PROGRAM", [this] { synth.bank().combiMode = 0; edited(); buildSoon(); });
        auto& comb = button ("COMBI", [this] { synth.bank().combiMode = 1; edited(); buildSoon(); });
        prog.chosen = ! combi; comb.chosen = combi;
        prog.setBounds (getWidth() - 160, 0, 78, 24);
        comb.setBounds (getWidth() - 78, 0, 78, 24);
        if (combi && tab <= 5)
        {
            const int pr = b.combis[(size_t) b.combi].timbre[std::clamp (synth.editorTimbre, 0, numTimbres - 1)][tProgram];
            note ({ 346, 0, getWidth() - 520, 24 }, "EDITING TIMBRE " + juce::String (synth.editorTimbre + 1) + "'S PROGRAM: "
                                                    + (pr < 0 ? juce::String ("OFF (CHOOSE ONE ON COMBI)") : juce::String (pr).paddedLeft ('0', 2) + " " + synth.programName (pr)));
        }
    }

    const int top = 2 * 24 + 18;
    auto area = getLocalBounds().withTrimmedTop (top);
    const int colW = (area.getWidth() - 16) / 2;
    auto colRect = [&] (int col, int r) { return juce::Rectangle<int> (area.getX() + col * (colW + 16), area.getY() + r * (row + rowGap), colW, row); };
    const bool dbl = program().global[gOscMode] == 1;
    auto layerSwitch = [&]
    {
        if (! dbl) return 0;
        auto& o1 = button ("OSC 1", [this] { synth.editorLayer = 0; buildSoon(); });
        auto& o2 = button ("OSC 2", [this] { synth.editorLayer = 1; buildSoon(); });
        o1.chosen = synth.editorLayer == 0; o2.chosen = synth.editorLayer == 1;
        o1.setBounds (area.getX(), area.getY(), 70, 22);
        o2.setBounds (area.getX() + 76, area.getY(), 70, 22);
        note ({ area.getX() + 160, area.getY(), 400, 22 }, "DOUBLE" + dot + "32 VOICES" + dot + "EDITING OSC " + juce::String (synth.editorLayer + 1));
        return 1;
    };
    auto rows = [&] (int col, int startRow, std::initializer_list<int> fields)
    {
        int r = startRow;
        for (int f : fields)
        {
            if (layerField (f).caption[0] == 0) continue;   // the other three of a set of four switches
            layerRow (f, colRect (col, r++));
        }
        return r;
    };
    auto graph = [&] (int fromRow, std::vector<EnvelopeGraph::Handle> h, int startField, bool bipolar)
    {
        auto* g = new EnvelopeGraph (std::move (h), startField, bipolar, [this] { return layerValues(); }, [this] { edited(); });
        parts.add (g);
        live.push_back (g);
        addAndMakeVisible (g);
        g->setBounds (area.withTrimmedTop (fromRow * (row + rowGap) + 6));
    };

    switch (tab)
    {
        case 0:   // OSC
        {
            globalRow (gOscMode, colRect (0, 0)).setBounds (colRect (0, 0).withWidth (FieldRow::captionWidth + 200));
            area.removeFromTop (row + rowGap + 6);
            const int s = layerSwitch();
            if (s) area.removeFromTop (row + rowGap + 4);
            layerRow (oWave, colRect (0, 0).withWidth (area.getWidth()));
            rows (0, 1, { oDrawn, oOctave, oLevel, oTranspose, oFine, oVelBottom, oVelTop, oDelay, oSlope });
            rows (1, 1, { xPan, xSendC, xSendD });
            note (colRect (1, 5), "PAN: WHERE THE OSCILLATOR SITS (A/B, INTO THE EFFECTS).");
            note (colRect (1, 6), "C AND D: ITS SENDS (SEE EFFECTS).");
            break;
        }
        case 1:   // PITCH
        {
            if (layerSwitch()) area.removeFromTop (row + rowGap + 4);
            note (colRect (0, 0), "PITCH LFO"); note (colRect (1, 0), "PITCH EG");
            rows (0, 1, { plWave, plFreq, plInt, plDelay, plFade, plModInt, plModSpeed, plAftInt, plAftSpeed });
            rows (1, 1, { peStart, peAttackTime, peAttackLevel, peDecayTime, peReleaseTime, peReleaseLevel, peInt, peIntVel, peTimeVel });
            graph (11, { { peAttackTime, peAttackLevel, "ATTACK" }, { peDecayTime, -1, "DECAY" }, { peReleaseTime, peReleaseLevel, "RELEASE" } }, peStart, true);
            break;
        }
        case 2:   // VDF
        {
            if (layerSwitch()) area.removeFromTop (row + rowGap + 4);
            note (colRect (0, 0), "VDF"); note (colRect (1, 0), "VDF EG");
            int r = rows (0, 1, { fCutoff, fColor, fColorVel, fKtMode, fKtKey, fKtInt, fKtEgTime, fKtSwA });
            note (colRect (0, r), "VDF LFO");
            rows (0, r + 1, { flWave, flFreq, flInt, flDelay, flFade });
            rows (1, 1, { feAttackTime, feAttackLevel, feDecayTime, feBreak, feSlopeTime, feSustain, feReleaseTime, feReleaseLevel, feInt, feIntVel, feTimeVel, feTvSwA });
            graph (15, { { feAttackTime, feAttackLevel, "ATTACK" }, { feDecayTime, feBreak, "BREAK POINT" }, { feSlopeTime, feSustain, "SUSTAIN" }, { feReleaseTime, feReleaseLevel, "RELEASE" } }, -1, true);
            break;
        }
        case 3:   // VDA
        {
            if (layerSwitch()) area.removeFromTop (row + rowGap + 4);
            note (colRect (0, 0), "VDA KEYBOARD TRACKING"); note (colRect (1, 0), "VDA EG");
            int r = rows (0, 1, { aKtMode, aKtKey, aKtInt, aKtEgTime, aKtSwA });
            note (colRect (0, r), "VDA LFO");
            rows (0, r + 1, { alWave, alFreq, alInt, alDelay, alFade });
            rows (1, 1, { aeAttackTime, aeAttackLevel, aeDecayTime, aeBreak, aeSlopeTime, aeSustain, aeReleaseTime, aeAmpVel, aeTimeVel, aeTvSwA });
            graph (12, { { aeAttackTime, aeAttackLevel, "ATTACK" }, { aeDecayTime, aeBreak, "BREAK POINT" }, { aeSlopeTime, aeSustain, "SUSTAIN" }, { aeReleaseTime, -1, "RELEASE" } }, -1, false);
            break;
        }
        case 4:   // CONTROL
        {
            int r = 0;
            for (int f : { gMonoPoly, gPortaSw, gPortaTime, gBendRange, gBendVdf, gModVdf }) globalRow (f, colRect (0, r++));
            r = 0;
            for (int f : { gAftBend, gAftVda, gAftVdf, gAftVdfLfo }) globalRow (f, colRect (1, r++));
            note (colRect (0, 7).withWidth (area.getWidth()), "THE REALTIME KNOBS' CONTROLLERS ARE ANSWERED TOO: CC 74 CUTOFF, 73 ATTACK, 75 DECAY, 72 RELEASE (BOTH EGS), 7 VOLUME, 11 EXPRESSION,");
            note (colRect (0, 8).withWidth (area.getWidth()), "1 MOD WHEEL (MOD 1), 16 AND 17 (MOD 2, MOD 3: THE EFFECTS' DYNAMIC MODULATION), 64 DAMPER.");
            break;
        }
        case 5:   // WAVES
        {
            const int s = layerSwitch();
            if (s) area.removeFromTop (row + rowGap + 4);
            layerRow (oWave, colRect (0, 0).withWidth (area.getWidth()));
            // the sixteen drawn waves
            const int y = area.getY() + row + rowGap + 2;
            for (int k = 0; k < numDrawn; ++k)
            {
                auto& c = button (juce::String (k + 1).paddedLeft ('0', 2), [this, k]
                {
                    synth.editorDrawn = k;
                    layerValues()[oDrawn] = k + 1;
                    if (layerValues()[oWave] == drawn) edited();
                    buildSoon();
                });
                c.chosen = k == synth.editorDrawn;
                c.setBounds (area.getX() + FieldRow::captionWidth + k * 40, y, 36, 22);
            }
            note ({ area.getX(), y, FieldRow::captionWidth, 22 }, "DRAWN WAVES");
            auto* canvas = new WaveCanvas (synth, synth.editorDrawn);
            parts.add (canvas);
            addAndMakeVisible (canvas);
            canvas->setBounds (area.getX(), y + 30, area.getWidth(), area.getBottom() - (y + 30) - 34);
            canvas->onChange = [this] { repaint (harmonicsArea); };
            const char* tools[] { "SMOOTH", "NORMALISE", "INVERT", "REVERSE", "CLEAR" };
            int x = area.getX();
            for (int k = 0; k < 5; ++k)
            {
                auto& t = button (tools[k], [this, k] { drawnTool (k); });
                t.setBounds (x, area.getBottom() - 26, t.idealWidth() + 6, 24);
                x += t.idealWidth() + 12;
            }
            auto& copy = button (utf ("COPY FROM\xe2\x80\xa6"), [this]
            {
                juce::PopupMenu m;
                const char* std[] { "SINE", "TRIANGLE", "SAW", "SQUARE", "PULSE 25%", "PULSE 12%" };
                for (int k = 0; k < 6; ++k) m.addItem (1 + k, std[k]);
                m.addSeparator();
                for (int k = 0; k < numDrawn; ++k) if (k != synth.editorDrawn) m.addItem (100 + k, juce::String (k + 1).paddedLeft ('0', 2) + " " + synth.drawnName (k));
                juce::Component::SafePointer<N5Panel> safe (this);
                m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [safe] (int r)
                {
                    if (safe == nullptr || r == 0) return;
                    std::array<float, drawnPoints> pts {};
                    if (r >= 100) pts = safe->synth.drawnWave (r - 100);
                    else
                        for (int i = 0; i < drawnPoints; ++i)
                        {
                            const double t = i / (double) drawnPoints;
                            double v = 0;
                            switch (r - 1)
                            {
                                case 0: v = std::sin (6.283185307 * t); break;
                                case 1: v = t < 0.25 ? 4 * t : t < 0.75 ? 2 - 4 * t : 4 * t - 4; break;
                                case 2: v = 1 - 2 * t; break;
                                case 3: v = t < 0.5 ? 1 : -1; break;
                                case 4: v = t < 0.25 ? 1 : -1; break;
                                default: v = t < 0.125 ? 1 : -1; break;
                            }
                            pts[(size_t) i] = (float) (0.9 * v);
                        }
                    for (auto* p : safe->parts) if (auto* c = dynamic_cast<WaveCanvas*> (p)) c->set (pts);
                });
            });
            copy.setBounds (x, area.getBottom() - 26, copy.idealWidth() + 6, 24);
            x += copy.idealWidth() + 12;
            harmonicsArea = { area.getRight() - 170, area.getBottom() - 30, 170, 30 };
            break;
        }
        case 6:   // EFFECTS
        {
            auto& f = fx();
            static const Field placementField { "placement", "PLACEMENT", 0, 3, 1, Fmt::choice, "SERIAL|PARA.1|PARA.2|PARA.3" };
            auto chipsRow = new FieldRow (placementField,
                                          [this] { return fx().placement; }, [this] (int v) { fx().placement = v; edited(); repaint (diagramArea); buildSoon(); });
            parts.add (chipsRow); live.push_back (chipsRow); addAndMakeVisible (chipsRow);
            chipsRow->setBounds (area.getX(), area.getY(), 560, row);
            diagramArea = { area.getX(), area.getY() + 28, area.getWidth(), 78 };
            const int y0 = diagramArea.getBottom() + 10;
            const int cw = (area.getWidth() - 20) / 2;
            for (int u = 0; u < 2; ++u)
            {
                const int x0 = area.getX() + u * (cw + 20);
                note ({ x0, y0, 70, 24 }, "EFFECT " + juce::String (u + 1));
                auto& type = button (juce::String (f.unit[u].type).paddedLeft ('0', 2) + " " + effectType (f.unit[u].type).name + "  v", [this, u] { chooseEffect (u); });
                type.setBounds (x0 + 70, y0, cw - 140, 24);
                auto& onB = button (f.unit[u].on ? "ON" : "OFF", [this, u] { fx().unit[u].on = fx().unit[u].on ? 0 : 1; edited(); buildSoon(); });
                onB.chosen = f.unit[u].on != 0;
                onB.setBounds (x0 + cw - 62, y0, 62, 24);
                const auto& et = effectType (f.unit[u].type);
                for (int k = 0; k < et.count; ++k)
                {
                    auto* er = new EffectRow (et.params[k], [this, u, k] { return fx().unit[u].p[k]; }, [this, u, k] (int v) { fx().unit[u].p[k] = v; edited(); });
                    parts.add (er); live.push_back (er); addAndMakeVisible (er);
                    er->setBounds (x0, y0 + 32 + k * (row + 2), cw, row);
                }
                // dynamic modulation
                const int ym = y0 + 32 + 8 * (row + 2) + 4;
                static const Field srcField { "modSource", "DYNAMIC MOD", 0, 5, 0, Fmt::choice, "NONE|MOD1|MOD2|MOD3|AFTR.T|VDA EG" };
                static const Field intField { "modInt", "INTENSITY", -15, 15, 0, Fmt::signed_ };
                auto* src = new FieldRow (srcField, [this, u] { return fx().unit[u].modSource; }, [this, u] (int v) { fx().unit[u].modSource = v; edited(); });
                parts.add (src); live.push_back (src); addAndMakeVisible (src);
                src->setBounds (x0, ym, cw, row);
                auto* in = new FieldRow (intField, [this, u] { return fx().unit[u].modInt; }, [this, u] (int v) { fx().unit[u].modInt = v; edited(); });
                parts.add (in); live.push_back (in); addAndMakeVisible (in);
                in->setBounds (x0, ym + row + 4, cw, row);
            }
            // PAN 3 / 4 (or PARA.3's levels)
            const int yp = area.getBottom() - row;
            if (f.placement == 3)
            {
                static const Field lv[4] { { "l1", "LEVEL 1-L", 0, 9, 9, Fmt::plain }, { "r1", "1-R", 0, 9, 9, Fmt::plain }, { "l2", "2-L", 0, 9, 9, Fmt::plain }, { "r2", "2-R", 0, 9, 9, Fmt::plain } };
                for (int k = 0; k < 4; ++k)
                {
                    auto* rr = new FieldRow (lv[k], [this, k] { return fx().level[k]; }, [this, k] (int v) { fx().level[k] = v; edited(); });
                    parts.add (rr); live.push_back (rr); addAndMakeVisible (rr);
                    rr->setBounds (area.getX() + k * (area.getWidth() / 4), yp, area.getWidth() / 4 - 8, row);
                }
            }
            else
            {
                static const Field p3 { "pan3", "PAN 3", -1, 100, 0, Fmt::panOut }, p4 { "pan4", "PAN 4", -1, 100, 100, Fmt::panOut };
                auto* a = new FieldRow (p3, [this] { return fx().pan3; }, [this] (int v) { fx().pan3 = v; edited(); });
                auto* bb = new FieldRow (p4, [this] { return fx().pan4; }, [this] (int v) { fx().pan4 = v; edited(); });
                for (auto* rr : { a, bb }) { parts.add (rr); live.push_back (rr); addAndMakeVisible (rr); }
                a->setBounds (area.getX(), yp, cw, row);
                bb->setBounds (area.getX() + cw + 20, yp, cw, row);
            }
            break;
        }
        case 7:   // COMBI
        {
            if (b.combiMode == 0)
            {
                note ({ area.getX(), area.getY(), area.getWidth(), 20 }, "PROGRAM MODE: PRESS COMBI (TOP RIGHT) TO PLAY AND EDIT A COMBINATION OF UP TO EIGHT PROGRAMS.");
                break;
            }
            auto* table = new TimbreTable (synth, [this] { edited(); });
            parts.add (table); live.push_back (table); addAndMakeVisible (table);
            table->setBounds (area.getX(), area.getY(), area.getWidth(), TimbreTable::headH + numTimbres * TimbreTable::rowH);
            table->onSelect = [this] { buildSoon(); };
            const int y = area.getY() + TimbreTable::headH + numTimbres * TimbreTable::rowH + 10;
            note ({ area.getX(), y, area.getWidth(), 18 }, "TIMBRE " + juce::String (synth.editorTimbre + 1) + dot + "DRAG A NUMBER UP OR DOWN, CLICK A PROGRAM TO CHOOSE IT; THE PROGRAM PAGES EDIT THIS TIMBRE'S PROGRAM");
            int k = 0;
            for (int f : { tRxNoteOn, tRxCc, tRxBend, tRxAft, tRxDamper, tRxPorta })
            {
                const int t = synth.editorTimbre;
                auto* rr = new FieldRow (timbreField (f), [this, t, f] { return synth.bank().combis[(size_t) synth.bank().combi].timbre[t][f]; },
                                         [this, t, f] (int v) { synth.bank().combis[(size_t) synth.bank().combi].timbre[t][f] = v; edited(); });
                parts.add (rr); live.push_back (rr); addAndMakeVisible (rr);
                rr->setBounds (colRect (k % 2, 0).withY (y + 24 + (k / 2) * (row + rowGap)));
                ++k;
            }
            break;
        }
        case 8: default:   // ARP
        {
            int r = 0;
            for (int f : { aOn, aType, aOctave, aSpeed, aLatch }) arpRow (f, colRect (0, r++));
            r = 0;
            for (int f : { aStep, aVelocity, aGate, aSort, aSwing }) arpRow (f, colRect (1, r++));
            note (colRect (0, 6).withWidth (area.getWidth()), "VELOCITY: KEY PLAYS AS YOU PLAY, STEP THE PATTERN'S OWN (SLIDE IT TO EITHER END); GATE: STEP IS THE PATTERN'S.");
            note (colRect (0, 7).withWidth (area.getWidth()), "UP, DOWN, ALT1, ALT2 AND RANDOM ARE AS THE MANUAL DESCRIBES; WHAT ARP 1-6 AND THE B- AND D- PATTERNS PLAY ISN'T PUBLISHED: MADE BY EAR.");
            break;
        }
    }
    repaint();
}

void N5Panel::resized()
{
    int x = 0;
    const int w = (getWidth() - (numTabs - 1) * 5) / numTabs;
    for (auto* c : tabChips) { c->setBounds (x, 30, w, 24); x += w + 5; }
    build();
}

void N5Panel::timerCallback()
{
    auto& b = synth.bank();
    if (builtTab != synth.editorTab || builtMode != b.combiMode || builtOsc != program().global[gOscMode]) { build(); return; }
    for (auto* l : live) l->repaint();
    if (synth.editorTab == 6 || synth.editorTab == 5) repaint();
}

void N5Panel::paint (juce::Graphics& g)
{
    for (auto& [r, t] : notes) text (g, t, r, Pal::lcdDim.withAlpha (0.8f), 9.5f);
    if (! diagramArea.isEmpty())
    {
        // the placement, as the manual draws it
        const auto& f = fx();
        auto r = diagramArea;
        g.setColour (Pal::lcdRule);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 1.0f);
        auto box = [&] (juce::Rectangle<int> bx, const juce::String& t, bool on)
        {
            g.setColour (on ? Pal::lcdRule : Pal::lcd);
            g.fillRoundedRectangle (bx.toFloat(), 3.0f);
            g.setColour (on ? Pal::lcdText : Pal::lcdDim);
            g.drawRoundedRectangle (bx.toFloat().reduced (0.5f), 3.0f, 1.0f);
            text (g, t, bx, Pal::lcdText, 9.0f, juce::Justification::centred);
        };
        auto line = [&] (int x1, int y1, int x2, int y2) { g.setColour (Pal::lcdDim); g.drawLine ((float) x1, (float) y1, (float) x2, (float) y2, 1.0f); };
        const int y1 = r.getY() + 14, y2 = r.getY() + 46, h = 22;
        const juce::String fx1 = "FX 1" + dot + juce::String (f.unit[0].type).paddedLeft ('0', 2) + " " + effectType (f.unit[0].type).name;
        const juce::String fx2 = "FX 2" + dot + juce::String (f.unit[1].type).paddedLeft ('0', 2) + " " + effectType (f.unit[1].type).name;
        const int x0 = r.getX() + 12;
        box ({ x0, y1, 74, h }, "A/B PAN", true);
        box ({ x0, y2, 74, h }, "C/D SEND", true);
        switch (f.placement)
        {
            case 0:
                box ({ x0 + 110, y1, 220, h }, fx1, f.unit[0].on != 0);
                box ({ x0 + 380, y1 + 16, 220, h }, fx2, f.unit[1].on != 0);
                line (x0 + 74, y1 + 11, x0 + 110, y1 + 11); line (x0 + 330, y1 + 11, x0 + 380, y1 + 27); line (x0 + 74, y2 + 11, x0 + 380, y1 + 27);
                box ({ x0 + 640, y1 + 16, 120, h }, "PAN 3/4 > L/R", true); line (x0 + 600, y1 + 27, x0 + 640, y1 + 27);
                break;
            case 2:
                box ({ x0 + 110, y2, 220, h }, fx2, f.unit[1].on != 0);
                box ({ x0 + 380, y1, 220, h }, fx1, f.unit[0].on != 0);
                line (x0 + 74, y1 + 11, x0 + 380, y1 + 11); line (x0 + 74, y2 + 11, x0 + 110, y2 + 11); line (x0 + 330, y2 + 11, x0 + 380, y1 + 11);
                box ({ x0 + 640, y1, 120, h }, "PAN 3/4 > L/R", true); line (x0 + 600, y1 + 11, x0 + 640, y1 + 11);
                break;
            case 3:
                box ({ x0 + 110, y1, 220, h }, "DRY (NO EFFECT)", true);
                box ({ x0 + 380, y1, 140, h }, "C > " + fx1.fromFirstOccurrenceOf (dot, false, false).substring (0, 14), f.unit[0].on != 0);
                box ({ x0 + 380, y2, 140, h }, "D > " + fx2.fromFirstOccurrenceOf (dot, false, false).substring (0, 14), f.unit[1].on != 0);
                line (x0 + 74, y1 + 11, x0 + 110, y1 + 11); line (x0 + 74, y2 + 11, x0 + 380, y2 + 11); line (x0 + 74, y2 + 11, x0 + 380, y1 + 11);
                box ({ x0 + 640, y1 + 16, 120, h }, "LEVELS > L/R", true);
                line (x0 + 330, y1 + 11, x0 + 640, y1 + 27); line (x0 + 520, y1 + 11, x0 + 640, y1 + 27); line (x0 + 520, y2 + 11, x0 + 640, y1 + 27);
                break;
            case 1: default:
                box ({ x0 + 110, y1, 220, h }, fx1, f.unit[0].on != 0);
                box ({ x0 + 110, y2, 220, h }, fx2, f.unit[1].on != 0);
                line (x0 + 74, y1 + 11, x0 + 110, y1 + 11); line (x0 + 74, y2 + 11, x0 + 110, y2 + 11);
                box ({ x0 + 390, y1 + 16, 60, h }, "MIX", true);
                line (x0 + 330, y1 + 11, x0 + 390, y1 + 27); line (x0 + 330, y2 + 11, x0 + 390, y1 + 27);
                box ({ x0 + 500, y1 + 16, 130, h }, "PAN 3/4 > L/R", true); line (x0 + 450, y1 + 27, x0 + 500, y1 + 27);
                break;
        }
    }
    if (! harmonicsArea.isEmpty())
    {
        const auto h = synth.harmonicsOf (synth.editorDrawn);
        auto r = harmonicsArea;
        text (g, "HARMONICS", r.removeFromLeft (70), Pal::lcdDim, 8.5f);
        const float w = (float) r.getWidth() / 16.0f;
        for (int k = 0; k < 16; ++k)
        {
            const float hh = (float) r.getHeight() * h[(size_t) k];
            g.setColour (Pal::lcdText);
            g.fillRect (juce::Rectangle<float> ((float) r.getX() + (float) k * w + 1.0f, (float) r.getBottom() - hh, w - 2.0f, hh));
        }
    }
}
} // namespace tf
