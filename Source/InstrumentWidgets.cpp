#include "InstrumentWidgets.h"

namespace tf::iw
{
void paintCentsChart (juce::Graphics& g, juce::Rectangle<int> chart, const std::array<double, 12>& d, double range)
{
    static const char* const names[] { "C", "C#", "D", "EB", "E", "F", "F#", "G", "AB", "A", "BB", "B" };
    auto labels = chart.removeFromBottom (16);
    const float mid = (float) chart.getCentreY();
    g.setColour (Pal::lcdRule);
    g.drawHorizontalLine ((int) mid, (float) chart.getX(), (float) chart.getRight());
    const float cw = (float) chart.getWidth() / 12.0f;
    for (int i = 0; i < 12; ++i)
    {
        const float x = (float) chart.getX() + cw * (float) i;
        const float h = (float) (d[(size_t) i] / range) * ((float) chart.getHeight() * 0.5f);
        g.setColour (Pal::lcdText);
        g.fillRect (juce::Rectangle<float> (x + 4.0f, std::min (mid, mid - h), cw * 0.5f, std::max (1.0f, std::abs (h))));
        text (g, (d[(size_t) i] >= 0.0 ? "+" : "") + juce::String (d[(size_t) i], 1), { (int) (x + cw * 0.5f + 6.0f), (int) mid - 7, (int) (cw * 0.5f), 14 }, Pal::lcdDim, 8.5f);
        text (g, names[i], { (int) x + 4, labels.getY(), (int) cw, 16 }, Pal::lcdDim, 9.5f);
    }
}
} // namespace tf::iw
