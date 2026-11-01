#include "SpringPanel.h"

namespace tf
{
namespace
{
    using namespace iw;
    using namespace spring;
    constexpr double pictureRate = 16000.0, pictureSeconds = 0.6, pictureTopHz = 6000.0;
}

SpringEditor::SpringEditor (spring::Processor& p) : juce::AudioProcessorEditor (p), proc (p)
{
    auto paramChips = [this] (const juce::String& id, const juce::String& caption, juce::StringArray labels)
    {
        auto* prm = proc.param (id);
        auto* c = new iw::Chips (caption, labels,
                                 [prm] { return juce::roundToInt (prm->convertFrom0to1 (prm->getValue())); },
                                 [prm] (int i) { prm->beginChangeGesture(); prm->setValueNotifyingHost (prm->convertTo0to1 ((float) i)); prm->endChangeGesture(); });
        parts.add (c);
        chipRows.push_back (c);
        addAndMakeVisible (c);
    };
    juce::StringArray tanks;
    for (int t = 0; t < numTanks; ++t)
        tanks.add (juce::String (tankInfo ((Tank) t).label) + utf (" \xc2\xb7 ") + tankInfo ((Tank) t).code);
    paramChips ("tank", "TANK", tanks);
    paramChips ("model", "MODEL", { "SIMULATED SPRINGS", "WAVEGUIDE" });
    auto bar = [this] (const juce::String& id, const juce::String& caption, std::function<juce::String (float)> fmt)
    {
        auto* b = new iw::Bar (*proc.param (id), caption, std::move (fmt));
        parts.add (b);
        bars.push_back (b);
        addAndMakeVisible (b);
    };
    bar ("dwell", "DWELL", [] (float v)
    {
        const juce::String s = juce::String (v, 1) + " OF 10";
        return v > 8.5f ? s + utf (" \xc2\xb7 THE 12AT7 CLIPPING") : v > 6.5f ? s + utf (" \xc2\xb7 THE 12AT7 STARTING TO CLIP") : s;
    });
    bar ("tone", "TONE", [] (float v) { return juce::String (v, 1) + " OF 10"; });
    setSize (width, height);
    startTimerHz (15);
}

SpringEditor::~SpringEditor() = default;

void SpringEditor::resized()
{
    auto r = getLocalBounds().reduced (14, 10);
    r.removeFromTop (26);
    tankArea = r.removeFromTop (106);
    r.removeFromTop (14);
    for (auto* c : chipRows)
    {
        const int h = c->layout (r.getWidth());
        c->setBounds (r.removeFromTop (h));
        r.removeFromTop (gapY);
    }
    for (auto* b : bars)
    {
        b->setBounds (r.removeFromTop (rowH));
        r.removeFromTop (gapY);
    }
    r.removeFromTop (6);
    pictureArea = r;
}

void SpringEditor::timerCallback()
{
    for (auto* c : chipRows) c->refresh();
    for (auto* b : bars) b->repaint();
    const int key = (int) proc.tank() * 2 + (int) proc.model();
    if (key != pictureFor) { makePicture(); repaint(); }
}

void SpringEditor::makePicture()
{
    pictureFor = (int) proc.tank() * 2 + (int) proc.model();
    // a click through this tank, as a picture of frequency against time (a short FFT, hop by hop)
    const auto x = Processor::click (proc.tank(), proc.model(), pictureRate, pictureSeconds);
    constexpr int order = 8, size = 1 << order, hop = 24;
    juce::dsp::FFT fft (order);
    const int cols = ((int) x.size() - size) / hop, rows = (int) (pictureTopHz / pictureRate * size);
    std::vector<float> mags ((size_t) (cols * rows));
    std::vector<float> buf (2 * size);
    float peak = 1.0e-12f;
    for (int c = 0; c < cols; ++c)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            buf[(size_t) i] = x[(size_t) (c * hop + i)] * (0.5f - 0.5f * std::cos (6.2831853f * (float) i / size));
        fft.performFrequencyOnlyForwardTransform (buf.data());
        for (int r = 0; r < rows; ++r)
        {
            const float m = buf[(size_t) r];
            mags[(size_t) (c * rows + r)] = m;
            peak = std::max (peak, m);
        }
    }
    picture = juce::Image (juce::Image::RGB, std::max (1, cols), std::max (1, rows), false);
    for (int c = 0; c < cols; ++c)
        for (int r = 0; r < rows; ++r)
        {
            const float db = 20.0f * std::log10 (std::max (1.0e-9f, mags[(size_t) (c * rows + r)] / peak));
            const float v = juce::jlimit (0.0f, 1.0f, (db + 50.0f) / 50.0f);
            picture.setPixelAt (c, rows - 1 - r, Pal::lcd.interpolatedWith (Pal::lcdText, v * v));
        }
}

void SpringEditor::paintTank (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto& info = tankInfo (proc.tank());
    g.setColour (Pal::lcdRule);
    g.drawRoundedRectangle (area.toFloat().reduced (0.5f), 3.0f, 1.0f);
    auto in = area.reduced (10, 6);
    text (g, juce::String ("ACCUTRONICS ") + info.code + utf (" \xc2\xb7 ") + utf (info.description), in.removeFromTop (14), Pal::lcdDim, 9.5f);
    // the chassis from above: the transducers at the ends, the springs between, joined in the middle
    const auto footer = in.removeFromBottom (13);
    auto chassis = in.reduced (16, 3).toFloat();
    const float scale = (float) (info.inches / 16.75);   // the short tank drawn shorter
    chassis = chassis.withWidth (chassis.getWidth() * scale);
    g.setColour (Pal::lcdRule.brighter (0.2f));
    g.drawRect (chassis, 1.0f);
    auto inBox = chassis.removeFromLeft (44.0f).reduced (4.0f, 6.0f);
    auto outBox = chassis.removeFromRight (44.0f).reduced (4.0f, 6.0f);
    g.setColour (Pal::lcdRule);
    g.fillRect (inBox);
    g.fillRect (outBox);
    text (g, "IN", inBox.toNearestInt(), Pal::lcdText, 9.0f, juce::Justification::centred);
    text (g, "OUT", outBox.toNearestInt(), Pal::lcdText, 9.0f, juce::Justification::centred);
    const int springs = info.paths;
    const bool pairs = info.inches > 12.0;   // the 17" tanks: each spring a counter-wound pair, joined in the middle
    for (int s = 0; s < springs; ++s)
    {
        const float y = chassis.getY() + chassis.getHeight() * (float) (s + 1) / (float) (springs + 1);
        const float turns = 0.9f;
        juce::Path p;
        for (int i = 0; i <= (int) chassis.getWidth(); ++i)
        {
            const float x = chassis.getX() + (float) i;
            const float yy = y + 4.0f * std::sin ((float) i * turns);
            if (i == 0) p.startNewSubPath (x, yy); else p.lineTo (x, yy);
        }
        g.setColour (s == 0 ? Pal::lcdText : Pal::lcdDim);
        g.strokePath (p, juce::PathStrokeType (1.0f));
        if (pairs)
        {
            g.setColour (Pal::lamp);
            g.fillEllipse (chassis.getCentreX() - 3.0f, y - 3.0f, 6.0f, 6.0f);
        }
    }
    const auto tank = proc.tank();
    const auto delays = [&info, tank]
    {
        juce::String d;
        for (int s = 0; s < info.paths; ++s)
            d << (s > 0 ? " / " : "") << juce::String (juce::roundToInt (tankPhysics (tank, s).roundTripSeconds() * 1000.0));
        return d;
    }();
    text (g, juce::String ("ECHOES EVERY ") + delays + " MS" + utf (" \xc2\xb7 ") + "DECAY " + info.decayClass
             + (proc.model() == Model::simulated ? utf (" \xc2\xb7 ") + juce::String (proc.modesInUse() > 0 ? proc.modesInUse() : 0) + " MODES" : juce::String()),
          footer, Pal::lcdDim.withAlpha (0.8f), 8.5f, juce::Justification::centredRight);
}

void SpringEditor::paint (juce::Graphics& g)
{
    g.fillAll (Pal::lcd);
    auto head = getLocalBounds().reduced (14, 10).removeFromTop (26);
    text (g, "TF SPRING REVERB", head, Pal::lcdDim, 11.0f);
    text (g, "MIX IS ON THE CHAIN", head, Pal::lcdDim.withAlpha (0.7f), 9.5f, juce::Justification::centredRight);
    paintTank (g, tankArea);
    // the click
    g.setColour (Pal::lcdRule);
    g.drawRoundedRectangle (pictureArea.toFloat().reduced (0.5f), 3.0f, 1.0f);
    auto in = pictureArea.reduced (10, 6);
    text (g, "A CLICK THROUGH THE TANK: FREQUENCY (UP, TO 6 KHZ) OVER 0.6 S", in.removeFromTop (14), Pal::lcdDim, 9.0f);
    if (picture.isValid())
        g.drawImage (picture, in.toFloat(), juce::RectanglePlacement::stretchToFit);
    const float fc = (float) tankPhysics (proc.tank(), 0).transitionHz();
    const float y = (float) in.getBottom() - (float) in.getHeight() * fc / (float) pictureTopHz;
    g.setColour (Pal::lamp.withAlpha (0.6f));
    g.drawHorizontalLine ((int) y, (float) in.getX(), (float) in.getRight());
    text (g, "TRANSITION " + juce::String (fc / 1000.0f, 1) + " KHZ", { in.getRight() - 160, (int) y - 14, 160, 12 }, Pal::lamp, 8.5f, juce::Justification::centredRight);
}
} // namespace tf
