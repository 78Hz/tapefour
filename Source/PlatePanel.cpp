#include "PlatePanel.h"

namespace tf
{
namespace
{
    using namespace plate;
    const juce::Colour cell { 0xff0a221f };
    const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));

    void section (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, const juce::String& sub)
    {
        g.setColour (cell);
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        iw::text (g, title, r.reduced (10, 0).withHeight (24), Pal::lcdText, 12.0f);
        if (sub.isNotEmpty()) iw::text (g, sub, r.reduced (10, 0).withHeight (24), Pal::lcdDim, 9.0f, juce::Justification::centredRight);
    }

    constexpr int picW = 280, picH = 150;
    constexpr double picLowHz = 100.0, picHighHz = 16000.0;

    /** A click through the plate as time (square root, so the arrival and the decay both show) against
        pitch (log), brightness the level. */
    juce::Image makePicture (int plateIndex, bool sim, double decay, double condition, double& seconds, std::function<bool()> stop)
    {
        constexpr double rate = 48000.0;
        seconds = juce::jlimit (0.6, 3.0, decay * 0.9);
        const auto ir = Processor::click (plateIndex, sim, decay, condition, rate, seconds, 0.18f, 0.28f, stop);
        if (stop && stop()) return {};
        constexpr int order = 9, size = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> buf (size * 2);
        std::vector<float> window (size);
        for (int i = 0; i < size; ++i) window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / size);
        std::vector<std::vector<float>> cols ((size_t) picW, std::vector<float> ((size_t) picH, -120.0f));
        float top = -120.0f;
        for (int x = 0; x < picW; ++x)
        {
            const double t = seconds * std::pow ((x + 0.5) / picW, 2.0);
            const int start = (int) (t * rate) - size / 2;
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < size; ++i)
            {
                const int k = start + i;
                if (k >= 0 && k < (int) ir.size()) buf[(size_t) i] = ir[(size_t) k] * window[(size_t) i];
            }
            fft.performFrequencyOnlyForwardTransform (buf.data());
            for (int y = 0; y < picH; ++y)
            {
                const double f0 = picLowHz * std::pow (picHighHz / picLowHz, (double) y / picH);
                const double f1 = picLowHz * std::pow (picHighHz / picLowHz, (double) (y + 1) / picH);
                const int b0 = std::max (1, (int) (f0 / rate * size)), b1 = std::max (b0, (int) (f1 / rate * size));
                float e = 0.0f;
                for (int b = b0; b <= b1 && b < size / 2; ++b) e = std::max (e, buf[(size_t) b]);
                const float db = 20.0f * std::log10 (e + 1.0e-9f);
                cols[(size_t) x][(size_t) y] = db;
                top = std::max (top, db);
            }
        }
        juce::Image img (juce::Image::ARGB, picW, picH, true);
        for (int x = 0; x < picW; ++x)
            for (int y = 0; y < picH; ++y)
            {
                const float a = juce::jlimit (0.0f, 1.0f, (cols[(size_t) x][(size_t) y] - top + 54.0f) / 54.0f);
                img.setPixelAt (x, picH - 1 - y, Pal::lcdText.withAlpha (a * a));
            }
        return img;
    }
}

/** Draws the click's picture on its own thread (it renders seconds of the plate). */
class PicturePainter : public juce::Thread
{
public:
    PicturePainter (int p, bool s, double d, double c, std::function<void (juce::Image, double)> done)
        : juce::Thread ("TF PLATE picture"), plateIndex (p), sim (s), decay (d), condition (c), onDone (std::move (done)) {}
    void run() override
    {
        double seconds = 0.0;
        auto img = makePicture (plateIndex, sim, decay, condition, seconds, [this] { return threadShouldExit(); });
        if (! threadShouldExit()) onDone (img, seconds);
    }

private:
    int plateIndex;
    bool sim;
    double decay, condition;
    std::function<void (juce::Image, double)> onDone;
};

//==============================================================================
PlatePanel::PlatePanel (Processor& p) : proc (p)
{
    chip ("SIMULATED", [this] { return proc.simulated(); }, [this] { proc.set ("engine", 0.0f); });
    chip ("LIGHT", [this] { return ! proc.simulated(); }, [this] { proc.set ("engine", 1.0f); });
    for (int k = 0; k < numPlates; ++k)
        chip (plateInfo (k).label, [this, k] { return proc.plateIndex() == k; }, [this, k] { proc.set ("plate", (float) k); }, 9.0f);
    chip ("MONO", [this] { return proc.get ("stereo") < 0.5f; }, [this] { proc.set ("stereo", 0.0f); }, 9.0f);
    chip ("STEREO (140 ST)", [this] { return proc.get ("stereo") > 0.5f; }, [this] { proc.set ("stereo", 1.0f); }, 9.0f);

    auto db = [] (float v) { return std::abs (v) < 0.05f ? juce::String ("0.0 DB") : (v > 0.0f ? "+" : "") + juce::String (v, 1) + " DB"; };
    knob ("damping", "DAMPING", [this] (float) { return juce::String (proc.decay500() * (0.75 + 0.25 * proc.get ("condition")), 1) + " S AT 500 HZ"; }, 20);
    knob ("condition", "CONDITION", [] (float v)
    {
        const char* tension = v < 0.33f ? "SLACK" : v < 0.66f ? "FIRM" : "TIGHT";
        const char* age = v < 0.33f ? "AGED" : v < 0.8f ? "USED" : "NEW";
        return juce::String (tension) + ", " + age;
    });
    knob ("predelay", "PRE-DELAY", [] (float v) { return juce::String (juce::roundToInt (v)) + " MS"; });
    knob ("width", "WIDTH", [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; });
    knob ("bass", "BASS", db);
    knob ("treble", "TREBLE", db);
    history.assign (400, 0.0f);
    refresh();
    startTimerHz (30);
}

PlatePanel::~PlatePanel()
{
    stopTimer();
    if (painter != nullptr) painter->stopThread (4000);
}

LcdChip& PlatePanel::chip (const juce::String& label, std::function<bool()> lit, std::function<void()> click, float size)
{
    auto* c = chips.add (new LcdChip (label));
    c->textSize = size;
    c->onClick = [this, act = std::move (click)] { act(); refresh(); };
    chipLit.push_back (std::move (lit));
    addAndMakeVisible (c);
    return *c;
}

iw::Knob& PlatePanel::knob (const juce::String& id, const juce::String& label, std::function<juce::String (float)> fmt, int radius)
{
    auto* k = knobs.add (new iw::Knob (*proc.param (id), label, std::move (fmt), radius));
    k->onChange = [this] { refresh(); };
    addAndMakeVisible (k);
    return *k;
}

void PlatePanel::resized()
{
    plateArea = { 0, 24, 520, 236 };
    clickArea = { 530, 24, 302, 236 };
    plateCtl = { 0, 270, 520, 150 };
    pickCtl = { 530, 270, 302, 150 };
    tailArea = { 0, 430, 832, 62 };
    viewArea = plateArea.reduced (14, 0).withTrimmedTop (28).withTrimmedBottom (22);
    graphArea = { clickArea.getX() + 11, clickArea.getY() + 28, picW, picH };
    curveArea = { plateCtl.getX() + 366, plateCtl.getY() + 46, 144, 80 };

    chips[1]->setBounds (panelWidth - chips[1]->idealWidth(), 0, chips[1]->idealWidth(), 20);
    chips[0]->setBounds (chips[1]->getX() - chips[0]->idealWidth() - 5, 0, chips[0]->idealWidth(), 20);
    int x = plateCtl.getX() + 10;
    for (int k = 0; k < numPlates; ++k) { chips[2 + k]->setBounds (x, plateCtl.getY() + 22, chips[2 + k]->idealWidth(), 20); x = chips[2 + k]->getRight() + 5; }
    chips[5]->setBounds (pickCtl.getX() + 10, pickCtl.getY() + 22, chips[5]->idealWidth(), 20);
    chips[6]->setBounds (chips[5]->getRight() + 5, pickCtl.getY() + 22, chips[6]->idealWidth(), 20);

    knobs[0]->place (plateCtl.getX() + 60, plateCtl.getY() + 82);
    knobs[1]->place (plateCtl.getX() + 170, plateCtl.getY() + 82);
    knobs[2]->place (plateCtl.getX() + 280, plateCtl.getY() + 82);
    for (int i = 0; i < 3; ++i) knobs[3 + i]->place (pickCtl.getX() + 52 + i * 98, pickCtl.getY() + 82);
}

juce::Rectangle<float> PlatePanel::plateRect() const
{
    // the plate at its own proportions, in its frame
    const auto& info = plateInfo (proc.plateIndex());
    auto frame = viewArea.toFloat().reduced (24.0f, 18.0f);
    const float aspect = (float) (info.lx / info.ly);
    if (frame.getWidth() / frame.getHeight() > aspect) frame = frame.withSizeKeepingCentre (frame.getHeight() * aspect, frame.getHeight());
    else frame = frame.withSizeKeepingCentre (frame.getWidth(), frame.getWidth() / aspect);
    return frame;
}

juce::Point<float> PlatePanel::pickupAt (int which) const
{
    const auto r = plateRect();
    const float x = proc.get (which == 0 ? "lx" : "rx"), y = proc.get (which == 0 ? "ly" : "ry");
    return { r.getX() + x * r.getWidth(), r.getY() + y * r.getHeight() };
}

void PlatePanel::mouseDown (const juce::MouseEvent& e)
{
    dragging = -1;
    const int n = proc.get ("stereo") > 0.5f ? 2 : 1;
    for (int k = 0; k < n; ++k)
        if (pickupAt (k).getDistanceFrom (e.position) < 14.0f) dragging = k;
}

void PlatePanel::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging < 0) return;
    const auto r = plateRect();
    proc.set (dragging == 0 ? "lx" : "rx", (e.position.x - r.getX()) / r.getWidth());
    proc.set (dragging == 0 ? "ly" : "ry", (e.position.y - r.getY()) / r.getHeight());
    repaint (plateArea);
}

void PlatePanel::paint (juce::Graphics& g)
{
    const auto& info = plateInfo (proc.plateIndex());
    const bool stereo = proc.get ("stereo") > 0.5f;
    iw::text (g, "A PLATE HUNG IN A FRAME: A DRIVER SHAKES IT, PICKUPS HEAR IT, A DAMPER SETS HOW LONG IT RINGS" + dot + "THE BLOCK'S MIX SETS WET AND DRY",
              { 0, 0, panelWidth - 200, 20 }, Pal::lcdDim, 8.5f);
    iw::text (g, "ENGINE", { chips[0]->getX() - 56, 0, 50, 20 }, Pal::lcdDim, 9.0f, juce::Justification::centredRight);

    // ---- the plate from above ----
    section (g, plateArea, info.name, juce::String (info.size) + dot + "FROM ABOVE");
    {
        const auto pr = plateRect();
        const auto fr = pr.expanded (20.0f, 16.0f);
        g.setColour (Pal::lcdDim);
        g.drawRect (fr, 2.0f);
        // the springs it hangs on: corners and the middle of each side
        auto spring = [&] (juce::Point<float> a, juce::Point<float> b)
        {
            juce::Path p;
            p.startNewSubPath (a);
            const auto d = b - a;
            const juce::Point<float> nrm (-d.y, d.x);
            const float len = std::max (1.0f, nrm.getDistanceFromOrigin());
            for (int k = 1; k < 12; ++k)
            {
                const float t = (float) k / 12.0f, off = (k % 2 ? 3.0f : -3.0f) / len;
                p.lineTo (a + d * t + nrm * off);
            }
            p.lineTo (b);
            g.strokePath (p, juce::PathStrokeType (1.0f));
        };
        spring (fr.getTopLeft(), pr.getTopLeft());
        spring (fr.getTopRight(), pr.getTopRight());
        spring (fr.getBottomLeft(), pr.getBottomLeft());
        spring (fr.getBottomRight(), pr.getBottomRight());
        spring ({ fr.getCentreX(), fr.getY() }, { pr.getCentreX(), pr.getY() });
        spring ({ fr.getCentreX(), fr.getBottom() }, { pr.getCentreX(), pr.getBottom() });
        spring ({ fr.getX(), fr.getCentreY() }, { pr.getX(), pr.getCentreY() });
        spring ({ fr.getRight(), fr.getCentreY() }, { pr.getRight(), pr.getCentreY() });
        g.setColour (Pal::lcd);
        g.fillRect (pr);
        g.setColour (Pal::lcdText);
        g.drawRect (pr, 1.0f);
        // the damper over it: closer, the outline nearer the plate's edge
        const float near = (float) (1.0 - std::log (proc.damperMm() / 3.0) / std::log (100.0 / 3.0));
        const auto damper = pr.reduced (4.0f + 14.0f * (1.0f - near), 4.0f + 10.0f * (1.0f - near));
        juce::Path outline;
        outline.addRectangle (damper);
        juce::Path dashed;
        const float dash[] { 5.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dash, 2);
        g.setColour (Pal::lcdDim);
        g.fillPath (dashed);
        iw::text (g, "DAMPER: " + juce::String (juce::roundToInt (proc.damperMm())) + " MM AWAY",
                  damper.reduced (6.0f, 3.0f).withHeight (14.0f).toNearestInt(), Pal::lcdDim, 8.0f, juce::Justification::centredRight);
        // the driver and the pickups
        auto mark = [&] (juce::Point<float> c, const juce::String& l, const juce::String& under, bool on)
        {
            const auto b = juce::Rectangle<float> (20.0f, 20.0f).withCentre (c);
            g.setColour (Pal::lcdText);
            if (on) g.fillEllipse (b); else g.drawEllipse (b, 1.0f);
            iw::text (g, l, b.toNearestInt(), on ? Pal::lcd : Pal::lcdText, 9.5f, juce::Justification::centred);
            iw::text (g, under, { (int) c.x - 50, (int) b.getBottom() + 2, 100, 12 }, Pal::lcdDim, 8.0f, juce::Justification::centred);
        };
        mark ({ pr.getX() + (float) info.driverX * pr.getWidth(), pr.getY() + (float) info.driverY * pr.getHeight() }, "D", "DRIVER", true);
        mark (pickupAt (0), stereo ? "L" : "P", stereo ? "PICKUP L" : "PICKUP", true);
        if (stereo) mark (pickupAt (1), "R", "PICKUP R", true);
        iw::text (g, "DRAG THE PICKUPS ON THE PLATE" + dot + "THE DAMPER MOVES WITH DAMPING" + dot + "MONO: ONE PICKUP, TO BOTH SIDES",
                  { plateArea.getX() + 10, plateArea.getBottom() - 20, plateArea.getWidth() - 20, 16 }, Pal::lcdDim, 7.5f);
    }

    // ---- a click through it ----
    section (g, clickArea, "A CLICK THROUGH IT", "TIME -> (SQUARE ROOT), PITCH ^");
    g.setColour (Pal::lcd);
    g.fillRect (graphArea);
    if (picture.isValid()) g.drawImageAt (picture, graphArea.getX(), graphArea.getY());
    iw::text (g, "16K", { graphArea.getX() + 3, graphArea.getY() + 2, 40, 10 }, Pal::lcdDim, 7.0f);
    iw::text (g, "100", { graphArea.getX() + 3, graphArea.getBottom() - 12, 40, 10 }, Pal::lcdDim, 7.0f);
    iw::text (g, juce::String (juce::jlimit (0.6, 3.0, proc.decay500() * 0.9), 1) + " S", { graphArea.getRight() - 43, graphArea.getBottom() - 12, 40, 10 },
              Pal::lcdDim, 7.0f, juce::Justification::centredRight);
    const double density = modesPerHz (info);
    iw::text (g, "HIGHS ARRIVE FIRST, LOWS FILL IN: NO ECHOES.", { clickArea.getX() + 10, graphArea.getBottom() + 6, 290, 12 }, Pal::lcdDim, 7.5f);
    iw::text (g, "A RESONANCE EVERY " + juce::String (1.0 / density, 2) + " HZ (" + juce::String (density, 1) + " A HERTZ)",
              { clickArea.getX() + 10, graphArea.getBottom() + 20, 290, 12 }, Pal::lcdDim, 7.5f);
    const juce::String engineText = proc.simulated() && proc.modesInUse() > 0
        ? juce::String (proc.modesInUse()) + " MODES TO " + juce::String (proc.modesTopHz() / 1000.0, 1) + " KHZ + A MATCHED TAIL ABOVE"
        : juce::String (proc.simulated() ? "SIMULATED: THE MODES ARE BUILT AS IT PLAYS" : "LIGHT: A NETWORK, TUNED TO THE PLATE");
    iw::text (g, engineText, { clickArea.getX() + 10, graphArea.getBottom() + 34, 290, 12 }, Pal::lcdText, 7.5f);

    // ---- plate and damper ----
    section (g, plateCtl, "PLATE", "AND ITS DAMPER");
    g.setColour (Pal::lcd);
    g.fillRect (curveArea);
    {
        juce::Path p;
        const double t500 = proc.decay500() * (0.75 + 0.25 * proc.get ("condition"));
        const double cond = proc.get ("condition");
        const double maxT = plateInfo (proc.plateIndex()).maxDecay * 1.2;
        for (int i = 0; i <= 100; ++i)
        {
            const double hz = 100.0 * std::pow (100.0, i / 100.0);
            const float x = (float) curveArea.getX() + (float) curveArea.getWidth() * (float) i / 100.0f;
            const float y = (float) curveArea.getBottom() - 4.0f - (float) ((curveArea.getHeight() - 16) * decayAt (hz, t500, cond) / maxT);
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (Pal::lcdText);
        g.strokePath (p, juce::PathStrokeType (1.5f));
        iw::text (g, "DECAY", { curveArea.getX() + 4, curveArea.getY() + 2, 60, 10 }, Pal::lcdDim, 7.0f);
        iw::text (g, "100", { curveArea.getX() + 3, curveArea.getBottom() - 11, 30, 10 }, Pal::lcdDim, 6.5f);
        iw::text (g, "10K", { curveArea.getRight() - 33, curveArea.getBottom() - 11, 30, 10 }, Pal::lcdDim, 6.5f, juce::Justification::centredRight);
    }
    iw::text (g, "DAMPING: THE DAMPER'S DISTANCE (" + juce::String (info.name) + ": " + juce::String (info.minDecay, 1) + " TO " + juce::String (info.maxDecay, 1)
                 + " S AT 500 HZ)" + dot + "CONDITION: SLACK OR AGED PLATES RING DULLER AND SHORTER",
              { plateCtl.getX() + 10, plateCtl.getBottom() - 18, plateCtl.getWidth() - 20, 14 }, Pal::lcdDim, 7.0f);

    // ---- pickups and out ----
    section (g, pickCtl, "PICKUPS", "AND THE OUTPUT");
    iw::text (g, "BASS, TREBLE: THE RECOVERY AMP'S TRIM", { pickCtl.getX() + 10, pickCtl.getBottom() - 18, pickCtl.getWidth() - 20, 14 }, Pal::lcdDim, 7.0f);

    // ---- the tail ----
    section (g, tailArea, "THE TAIL NOW", juce::String());
    {
        const auto r = tailArea.reduced (10, 0).withTrimmedTop (24).withTrimmedBottom (6).toFloat();
        const float w = r.getWidth() / (float) history.size(), mid = r.getCentreY();
        g.setColour (Pal::lcdDim);
        for (size_t i = 0; i < history.size(); ++i)
        {
            const float v = history[(i + (size_t) historyPos) % history.size()];
            const float h = std::max (0.5f, std::min (1.0f, v * 3.0f) * r.getHeight() * 0.5f);
            g.fillRect (r.getX() + (float) i * w, mid - h, std::max (1.0f, w - 0.4f), 2.0f * h);
        }
    }
}

void PlatePanel::startPicture()
{
    const juce::String key = juce::String (proc.plateIndex()) + ":" + juce::String ((int) proc.simulated()) + ":"
                             + juce::String (proc.decay500(), 2) + ":" + juce::String (proc.get ("condition"), 2);
    if (key == pictureKey || (painter != nullptr && painter->isThreadRunning())) return;
    pictureKey = key;
    painter = std::make_unique<PicturePainter> (proc.plateIndex(), proc.simulated(), proc.decay500() * (0.75 + 0.25 * proc.get ("condition")), proc.get ("condition"),
                                                [this] (juce::Image img, double)
                                                {
                                                    const juce::ScopedLock sl (pictureLock);
                                                    madePicture = img;
                                                    pictureReady = true;
                                                });
    painter->startThread();
}

void PlatePanel::waitForPicture()
{
    startPicture();
    if (painter != nullptr) painter->waitForThreadToExit (60000);
    timerCallback();
}

void PlatePanel::refresh()
{
    for (int i = 0; i < chips.size(); ++i)
        if (const bool on = chipLit[(size_t) i](); chips[i]->chosen != on) { chips[i]->chosen = on; chips[i]->repaint(); }
    for (auto* k : knobs) k->repaint();
    repaint();
}

void PlatePanel::timerCallback()
{
    juce::String now;
    for (auto* p : proc.getParameters()) now << p->getValue() << ",";
    if (now != shown) { shown = now; refresh(); }
    if (pictureReady.exchange (false))
    {
        const juce::ScopedLock sl (pictureLock);
        picture = madePicture;
        repaint (clickArea);
    }
    startPicture();
    history[(size_t) historyPos] = proc.levelNow();
    historyPos = (historyPos + 1) % (int) history.size();
    repaint (tailArea);
}
} // namespace tf
