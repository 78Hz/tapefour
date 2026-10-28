#include "ElectricPianoPanel.h"

namespace tf
{
namespace
{
    using namespace iw;
    const char* const tabNames[] { "MODEL", "TUNING", "REEDS", "HAMMERS", "PICKUP", "DAMPERS", "AMP", "PLAYING" };
    const char* const noteNames[] { "C", "C#", "D", "EB", "E", "F", "F#", "G", "AB", "A", "BB", "B" };

    juce::String noteName (int k) { return juce::String (noteNames[((k % 12) + 12) % 12]) + juce::String (k / 12 - 1); }
    juce::String db (float v) { return v <= -89.5f ? juce::String ("OFF") : juce::String (juce::roundToInt (v)) + " dB"; }
    juce::String feltWord (double f)
    {
        return f < 0.25 ? "SOFT" : f < 0.45 ? "MEDIUM" : f < 0.6 ? "SLIGHTLY HARD" : f < 0.8 ? "HARD" : "VERY HARD";
    }
    juce::String dampingWord (double d) { return d < 0.3 ? "WEAK" : d < 0.55 ? "LIGHT" : d < 0.8 ? "FIRM" : "VERY FIRM"; }
    juce::String restWords (float h)
    {
        const double r = ep::restMm (h);
        if (std::abs (r) < 0.005) return "LEVEL WITH THE FINGERS";
        return juce::String (std::abs (r), 2) + (r > 0.0 ? " MM CLEAR OF THE FINGERS" : " MM INSIDE THEM");
    }

}

//==============================================================================
//==============================================================================
ElectricPianoPanel::ElectricPianoPanel (ep::Processor& p, int& t) : piano (p), tab (t)
{
    for (int i = 0; i < 8; ++i)
    {
        auto* c = tabChips.add (new LcdChip (tabNames[i]));
        c->textSize = 11.5f;
        c->onClick = [this, i] { tab = i; build(); };
        addAndMakeVisible (c);
    }
    startTimerHz (10);
}

ElectricPianoPanel::~ElectricPianoPanel() = default;

juce::Rectangle<int> ElectricPianoPanel::content() const { return getLocalBounds().withTrimmedTop (chipH + 10); }

ElectricPianoPanel::Bar& ElectricPianoPanel::bar (const juce::String& id, const juce::String& caption, std::function<juce::String (float)> fmt)
{
    auto* b = new Bar (*piano.param (id), caption, std::move (fmt));
    parts.add (b);
    bars.push_back (b);
    addAndMakeVisible (b);
    return *b;
}

ElectricPianoPanel::Chips& ElectricPianoPanel::chips (const juce::String& caption, juce::StringArray labels, std::function<int()> chosen, std::function<void (int)> pick)
{
    auto* c = new Chips (caption, labels, std::move (chosen), std::move (pick));
    parts.add (c);
    chipRows.push_back (c);
    addAndMakeVisible (c);
    return *c;
}

ElectricPianoPanel::Chips& ElectricPianoPanel::paramChips (const juce::String& id, const juce::String& caption, juce::StringArray labels)
{
    auto* p = piano.param (id);
    return chips (caption, labels,
                  [p] { return juce::roundToInt (p->convertFrom0to1 (p->getValue())); },
                  [p] (int i) { p->beginChangeGesture(); p->setValueNotifyingHost (p->convertTo0to1 ((float) i)); p->endChangeGesture(); });
}

void ElectricPianoPanel::build()
{
    parts.clear();
    bars.clear();
    chipRows.clear();
    notes.clear();
    edoBox.reset();
    chartArea = diagramArea = traceArea = summaryArea = {};
    builtTab = tab;
    const auto ts = piano.tuningSettings();
    builtSystem = (int) ts.system;

    for (int i = 0; i < tabChips.size(); ++i) tabChips[i]->chosen = i == tab;

    auto area = content();
    const int w = area.getWidth();
    int y = area.getY();
    auto placeBar = [&] (Bar& b) { b.setBounds (area.getX(), y, w, rowH); y += rowH + gapY; };
    auto placeChips = [&] (Chips& c) { const int h = c.layout (w); c.setBounds (area.getX(), y, w, h); y += h + gapY; };
    auto pct = [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; };

    switch (tab)
    {
        case 0:   // MODEL
        {
            placeChips (paramChips ("model", "MODEL", { "200", "200A" }));
            placeChips (chips ("CONDITION", { "NEW", "SERVICED", "WORN", "NEGLECTED" }, [this] { return piano.lastCondition(); },
                               [this] (int c) { piano.chooseCondition (c); if (onMessage) onMessage (juce::String ("ELECTRIC PIANO: ") + ep::conditionName (c)); }));
            placeChips (paramChips ("ages", "AGES AS YOU PLAY", { "OFF", "ON" }));
            summaryArea = area.withTop (y + 2).withTrimmedBottom (16);
            break;
        }
        case 1:   // TUNING
        {
            placeChips (paramChips ("system", "SYSTEM", { "EQUAL", "JUST", "PYTHAGOREAN", "MEANTONE", "WERCKMEISTER III", "KIRNBERGER III",
                                                         "VALLOTTI", "PER NOTE", "EDO", "SCALA FILE" }));
            const auto sys = ts.system;
            if (sys == tuning::System::edo)
            {
                auto* row = new juce::Component();
                parts.add (row);
                addAndMakeVisible (row);
                row->setBounds (area.getX(), y, w, chipH);
                edoBox = std::make_unique<juce::TextEditor>();
                auto& e = *edoBox;
                e.setFont (DeckLookAndFeel::get().mono (13.0f));
                e.setJustification (juce::Justification::centred);
                e.setInputRestrictions (2, "0123456789");
                e.setColour (juce::TextEditor::backgroundColourId, Pal::lcd);
                e.setColour (juce::TextEditor::textColourId, Pal::lcdText);
                e.setColour (juce::TextEditor::outlineColourId, Pal::lcdDim);
                e.setColour (juce::TextEditor::focusedOutlineColourId, Pal::lcdText);
                e.setText (juce::String (ts.edoSteps), false);
                auto commit = [this]
                {
                    auto* p = piano.param ("edo");
                    const int n = juce::jlimit (5, 72, edoBox->getText().getIntValue());
                    p->setValueNotifyingHost (p->convertTo0to1 ((float) n));
                    edoBox->setText (juce::String (n), false);
                };
                e.onReturnKey = commit;
                e.onFocusLost = commit;
                addAndMakeVisible (e);
                e.setBounds (area.getX() + captionW, y, 56, chipH);
                for (int d : { -1, 1 })
                {
                    auto* c = new LcdChip (d < 0 ? "-" : "+");
                    parts.add (c);
                    addAndMakeVisible (c);
                    c->setBounds (area.getX() + captionW + (d < 0 ? 62 : 94), y, 28, chipH);
                    c->onClick = [this, d]
                    {
                        auto* p = piano.param ("edo");
                        const int n = juce::jlimit (5, 72, juce::roundToInt (p->convertFrom0to1 (p->getValue())) + d);
                        p->setValueNotifyingHost (p->convertTo0to1 ((float) n));
                        if (edoBox != nullptr) edoBox->setText (juce::String (n), false);
                    };
                }
                note ({ area.getX(), y, captionW, chipH }, "STEPS PER OCTAVE");
                note ({ area.getX() + captionW + 132, y, w - captionW - 132, chipH }, "TYPE A NUMBER (5 TO 72), OR STEP IT");
                y += chipH + gapY;
            }
            else if (sys == tuning::System::scala)
            {
                placeChips (chips ("FILES", { juce::CharPointer_UTF8 ("LOAD .SCL\xe2\x80\xa6"), juce::CharPointer_UTF8 ("LOAD .KBM\xe2\x80\xa6"), "DEFAULT MAP" },
                                   [] { return -1; },
                                   [this] (int i) { if (i == 2) piano.clearKeyboardMap(); else loadScale (i == 1); }));
            }
            else if (sys != tuning::System::equal && sys != tuning::System::custom)
            {
                placeChips (paramChips ("key", "KEY", { "C", "C#", "D", "EB", "E", "F", "F#", "G", "AB", "A", "BB", "B" }));
            }
            placeBar (bar ("a4", "A4", [] (float v) { return juce::String (v, 1) + " HZ"; }));
            placeBar (bar ("stretch", "OCTAVE STRETCH", [] (float v) { return v < 0.05f ? juce::String ("NONE") : "+" + juce::String (v, 1) + " CENTS / OCTAVE"; }));
            chartArea = area.withTop (y + 2);
            break;
        }
        case 2:   // REEDS
        {
            placeBar (bar ("spread", "TUNING SPREAD", [] (float v) { return juce::String (juce::CharPointer_UTF8 ("\xc2\xb1")) + juce::String (v, 1) + " CENTS"; }));
            placeBar (bar ("ring", "RING TIME", [this] (float v)
            {
                const auto r = piano.currentReeds()[60];
                return juce::String (juce::CharPointer_UTF8 ("\xc3\x97")) + juce::String (v, 2) + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 C4 ")) + juce::String (ep::ringToMinus40 (r), 1) + " S";
            }));
            placeBar (bar ("ringers", "SHORT RINGERS", [this] (float v)
            {
                const int extra = ep::ageing (piano.hoursPlayed()).shortRingers;
                return juce::String (juce::roundToInt (v)) + (extra > 0 ? " + " + juce::String (extra) + " WITH AGE" : juce::String()) + " OF 64";
            }));
            chartArea = area.withTop (y + 4);
            break;
        }
        case 3:   // HAMMERS
        {
            placeBar (bar ("felt", "FELT", [] (float v) { return feltWord (v); }));
            placeBar (bar ("strike", "STRIKE POINT", [] (float v) { return v < -0.33f ? juce::String ("TOWARD THE ROOT") : v > 0.33f ? juce::String ("TOWARD THE TIP") : juce::String ("AS SET"); }));
            placeBar (bar ("blow", "BLOW DISTANCE", [] (float v) { return juce::String (v, 3) + " IN"; }));
            placeBar (bar ("letoff", "LET-OFF", [] (float v) { return juce::String (v, 3) + " IN"; }));
            placeBar (bar ("curve", "VELOCITY CURVE", [] (float v) { return v < -0.33f ? juce::String ("HEAVIER") : v > 0.33f ? juce::String ("LIGHTER") : juce::String ("AS THE ACTION"); }));
            note ({ area.getX(), y + 4, w, 16 }, "THE HAMMER STAYS ON THE REED ABOUT ONE CYCLE OF ITS PITCH. HARDER FELT AND A LONGER BLOW: A SHORTER, HARDER HIT, SO THE REED SWINGS FURTHER AND BARKS SOONER.");
            note ({ area.getX(), y + 22, w, 16 }, "THE FACTORY SETTINGS: A BLOW OF 1 5/64 IN, LET-OFF AT 1/8 IN (THE SERVICE MANUAL). A BIG LET-OFF LOSES THE SOFTEST NOTES.");
            break;
        }
        case 4:   // PICKUP
        {
            placeBar (bar ("height", "REED AT REST", [] (float v) { return restWords (v); }));
            placeBar (bar ("gap", "SIDE GAP", [] (float v) { return juce::String (v, 3) + " MM EACH SIDE"; }));
            placeBar (bar ("volts", "POLARISING", [] (float v) { return juce::String (juce::roundToInt (v)) + " V"; }));
            auto rest = area.withTop (y + 4);
            diagramArea = rest.removeFromLeft (rest.getWidth() * 2 / 5).withTrimmedRight (8);
            traceArea = rest;
            break;
        }
        case 5:   // DAMPERS
        {
            placeBar (bar ("damping", "DAMPING", [] (float v) { return dampingWord (v); }));
            placeBar (bar ("damperNoise", "DAMPER NOISE", db));
            placeBar (bar ("actionNoise", "ACTION NOISE", db));
            placeBar (bar ("pedalNoise", "PEDAL NOISE", db));
            note ({ area.getX(), y + 4, w, 16 }, "THE SUSTAIN PEDAL IS YOUR CONTROLLER'S (CC 64). WORN DAMPER FELT DAMPS LESS: AGE TAKES SOME OF IT.");
            break;
        }
        case 6:   // AMP
        {
            placeBar (bar ("volume", "VOLUME", [] (float v) { return juce::String (v, 1); }));
            placeBar (bar ("vibrato", "VIBRATO", [pct] (float v) { return pct (v) + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 5.75 HZ")); }));
            placeBar (bar ("drive", "PREAMP DRIVE", [pct] (float v) { return v < 0.2f ? juce::String ("CLEAN") : pct (v); }));
            placeChips (paramChips ("output", "OUTPUT", { "SPEAKERS", "REAR JACK" }));
            placeBar (bar ("hum", "MAINS HUM", db));
            placeChips (paramChips ("mains", "MAINS", { "50 HZ", "60 HZ" }));
            note ({ area.getX(), y, w, 16 }, juce::CharPointer_UTF8 ("SPEAKERS: THE POWER AMP AND THE TWO 4\xc3\x97" "8 IN OVALS IN THE LID, AS THE PLAYER HEARS IT."));
            note ({ area.getX(), y + 16, w, 16 }, "REAR JACK: THE PREAMP'S OUTPUT, BEFORE THE POWER AMP AND SPEAKERS.");
            break;
        }
        case 7: default:   // PLAYING
        {
            placeChips (paramChips ("mpe", "MPE", { "OFF", "ON" }));
            placeChips (paramChips ("bend", "PER-NOTE BEND", { "OFF", "ON" }));
            placeChips (paramChips ("pressure", "PRESSURE", { "OFF", "VOLUME", "VIBRATO DEPTH", "PREAMP DRIVE", "DAMPER" }));
            placeBar (bar ("pressureAmount", "PRESSURE AMOUNT", pct));
            placeChips (paramChips ("slide", "SLIDE (CC 74)", { "OFF", "HAMMER FELT", "REED HEIGHT" }));
            note ({ area.getX(), y, w, 16 }, juce::CharPointer_UTF8 ("MPE OFF: VELOCITY ONLY, AS THE REAL PIANO. ON: BEND FOLLOWS THE CONTROLLER'S RANGE (SETUP \xe2\x80\xba MIDI)."));
            note ({ area.getX(), y + 16, w, 16 }, "DAMPER: PRESSING HARDER INTO A HELD KEY DAMPS IT, AS A PALM ON THE REEDS WOULD.");
            break;
        }
    }
    repaint();
}

void ElectricPianoPanel::resized()
{
    int x = 0;
    const int w = (getWidth() - 7 * 6) / 8;
    for (auto* c : tabChips) { c->setBounds (x, 0, w, chipH + 2); x += w + 6; }
    build();
}

void ElectricPianoPanel::timerCallback()
{
    const auto ts = piano.tuningSettings();
    if (builtTab != tab || (tab == 1 && (int) ts.system != builtSystem)) { build(); return; }
    for (auto* c : chipRows) c->refresh();
    for (auto* b : bars) b->repaint();
    if (edoBox != nullptr && ! edoBox->hasKeyboardFocus (true))
        if (edoBox->getText().getIntValue() != ts.edoSteps) edoBox->setText (juce::String (ts.edoSteps), false);
    if (! chartArea.isEmpty() || ! summaryArea.isEmpty() || ! traceArea.isEmpty()) repaint();
}

void ElectricPianoPanel::loadScale (bool keyboardMap)
{
    chooser = std::make_unique<juce::FileChooser> (keyboardMap ? "A Scala keyboard map" : "A Scala scale", juce::File(),
                                                   keyboardMap ? "*.kbm" : "*.scl");
    juce::Component::SafePointer<ElectricPianoPanel> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe, keyboardMap] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File()) return;
        const auto textOf = fc.getResult().loadFileAsString().toStdString();
        std::string err;
        if (keyboardMap)
        {
            tuning::KeyboardMap m;
            if (tuning::parseKeyboardMap (textOf, m, err)) safe->piano.setKeyboardMap (m, textOf);
            else if (safe->onMessage) safe->onMessage ("KEYBOARD MAP: " + juce::String (err));
        }
        else
        {
            tuning::Scale sc;
            if (tuning::parseScala (textOf, sc, err))
            {
                safe->piano.setScale (sc, textOf);
                auto* p = safe->piano.param ("system");
                p->setValueNotifyingHost (p->convertTo0to1 ((float) tuning::System::scala));
            }
            else if (safe->onMessage) safe->onMessage ("SCALA: " + juce::String (err));
        }
        safe->repaint();
    });
}

void ElectricPianoPanel::setCustomCents (int pc, float cents)
{
    auto* p = piano.param ("cents" + juce::String (pc));
    p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (-50.0f, 50.0f, cents)));
}

void ElectricPianoPanel::mouseDown (const juce::MouseEvent& e)
{
    dragPitchClass = -1;
    if (tab != 1 || piano.tuningSettings().system != tuning::System::custom || ! chartArea.contains (e.getPosition())) return;
    const auto chart = chartArea.withTrimmedTop (16).withTrimmedBottom (16);
    dragPitchClass = juce::jlimit (0, 11, (e.x - chart.getX()) * 12 / std::max (1, chart.getWidth()));
    mouseDrag (e);
}

void ElectricPianoPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (dragPitchClass < 0) return;
    const auto chart = chartArea.withTrimmedTop (16).withTrimmedBottom (16);
    const float mid = (float) chart.getCentreY();
    setCustomCents (dragPitchClass, (mid - (float) e.y) / ((float) chart.getHeight() * 0.5f) * 50.0f);
    repaint();
}

void ElectricPianoPanel::paint (juce::Graphics& g)
{
    const auto s = piano.settings();
    const auto ts = piano.tuningSettings();
    for (auto& [r, t] : notes) text (g, t, r, Pal::lcdDim.withAlpha (0.75f), 9.5f);

    if (! summaryArea.isEmpty())
    {
        auto r = summaryArea;
        g.setColour (Pal::lcdRule);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 1.0f);
        r.reduce (10, 6);
        const auto reedsNow = piano.currentReeds();
        juce::StringArray shortOnes;
        int shortCount = 0;
        for (int k = ep::lowestKey; k <= ep::highestKey; ++k)
            if (reedsNow[(size_t) k].shortRinger) { ++shortCount; if (shortOnes.size() < 3) shortOnes.add (noteName (k)); }
        const auto age = ep::ageing (piano.hoursPlayed());
        juce::String tuningLine;
        if (ts.system == tuning::System::edo) tuningLine = juce::String (ts.edoSteps) + "-EDO";
        else if (ts.system == tuning::System::scala) tuningLine = "SCALA: " + piano.scaleDescription().toUpperCase();
        else tuningLine = juce::String (tuning::systemName (ts.system))
                        + (ts.system == tuning::System::equal || ts.system == tuning::System::custom ? juce::String() : juce::String (" IN ") + noteNames[ts.key]);
        const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
        const juce::StringArray lines {
            "THIS PIANO: " + juce::String (s.model == 0 ? "200" : "200A") + dot + ep::conditionName (piano.lastCondition())
                + dot + "PLAYED " + juce::String (piano.hoursPlayed(), piano.hoursPlayed() < 10.0 ? 2 : 1) + " HOURS",
            juce::String (juce::CharPointer_UTF8 ("REEDS \xc2\xb1")) + juce::String (s.tuningSpreadCents + age.spreadCents, 1) + " CENTS" + dot
                + "RING " + juce::String (juce::CharPointer_UTF8 ("\xc3\x97")) + juce::String (s.ringTime, 2) + dot
                + juce::String (shortCount) + (shortCount == 1 ? " SHORT RINGER" : " SHORT RINGERS")
                + (shortOnes.isEmpty() ? juce::String() : " (" + shortOnes.joinIntoString (", ") + (shortCount > 3 ? juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6")) : juce::String()) + ")"),
            "HAMMERS " + feltWord (std::min (1.0, s.felt + age.felt)) + dot + "LET-OFF " + juce::String (s.letOffInches, 3) + " IN"
                + dot + "REEDS " + restWords (s.reedHeight) + dot + "SIDE GAP " + juce::String (s.gapMm, 3) + " MM"
                + dot + juce::String (juce::roundToInt (s.volts)) + " V",
            "DAMPERS " + dampingWord (std::max (0.0, s.damping - age.damping)) + dot + tuningLine + dot + "A4 " + juce::String (ts.a4Hz, 1) + " HZ",
            "VIBRATO " + juce::String (juce::roundToInt (s.vibrato * 100.0f)) + " %" + dot + (s.rearJack ? "REAR JACK" : "SPEAKERS") + dot
                + "HUM " + db (s.humDb) + " (" + (s.mains60 ? "60" : "50") + " HZ)" + dot + (s.mpe ? "MPE ON" : "MPE OFF") };
        for (int i = 0; i < lines.size(); ++i)
            text (g, lines[i], r.removeFromTop (i == 0 ? 22 : 19), i == 0 ? Pal::lcdText : Pal::lcdDim, i == 0 ? 11.5f : 10.0f);
        text (g, "A CONDITION SETS THE TECHNICIAN'S CONTROLS AND STARTS ITS PLAYING TIME AGAIN; CHANGE ANY OF THEM AFTERWARDS.",
              summaryArea.withTop (summaryArea.getBottom() + 2).withHeight (14), Pal::lcdDim.withAlpha (0.7f), 9.0f);
    }

    if (! chartArea.isEmpty() && tab == 1)
    {
        auto chart = chartArea;
        if (ts.system == tuning::System::edo || ts.system == tuning::System::scala)
        {
            const auto f = tuning::frequencies (ts);
            juce::String line;
            if (ts.system == tuning::System::edo)
                line = juce::String (ts.edoSteps) + " STEPS OF " + juce::String (1200.0 / ts.edoSteps, 2) + " CENTS" + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + "A4 ON ITS KEY"
                     + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 THE 64 KEYS SPAN ")) + juce::String (63.0 / ts.edoSteps, 2) + " OCTAVES";
            else
                line = piano.scaleDescription().isEmpty() ? juce::String ("NO SCALE LOADED: LOAD A .SCL FILE")
                                                          : piano.scaleDescription().toUpperCase() + (piano.hasKeyboardMap() ? juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ITS KEYBOARD MAP")) : juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 DEFAULT MAP: C4 IS 1/1, A4 AT THE REFERENCE")));
            text (g, line, chart.removeFromTop (18), Pal::lcdDim, 10.0f);
            // the keyboard's frequencies, key by key, as a staircase
            auto plot = chart.reduced (0, 6);
            const double lo = std::log2 (27.5), hi = std::log2 (4200.0);
            g.setColour (Pal::lcdRule);
            g.drawRect (plot);
            const int keys = ep::highestKey - ep::lowestKey + 1;
            for (int k = 0; k < keys; ++k)
            {
                const double hz = f[(size_t) (ep::lowestKey + k)];
                if (hz <= 0.0) continue;
                const float x = (float) plot.getX() + (float) plot.getWidth() * (float) k / (float) keys;
                const float yy = (float) plot.getBottom() - (float) plot.getHeight() * (float) ((std::log2 (hz) - lo) / (hi - lo));
                g.setColour (Pal::lcdText);
                g.fillRect (x, yy - 1.0f, (float) plot.getWidth() / (float) keys - 1.0f, 3.0f);
            }
            text (g, "A1", { plot.getX() + 4, plot.getBottom() - 14, 40, 12 }, Pal::lcdDim, 9.0f);
            text (g, "C7", { plot.getRight() - 30, plot.getBottom() - 14, 26, 12 }, Pal::lcdDim, 9.0f, juce::Justification::centredRight);
        }
        else
        {
            text (g, ts.system == tuning::System::custom ? "CENTS FROM EQUAL: DRAG A NOTE'S BAR UP OR DOWN" : "CENTS FROM EQUAL (A KEPT AT THE REFERENCE)",
                  chart.removeFromTop (16), Pal::lcdDim, 9.5f);
            auto names = chart.removeFromBottom (16);
            const auto d = tuning::centsFromEqual (ts);
            // custom: the full +-50 cents you can drag through; a temperament: scaled to its own largest offset
            double range = 50.0;
            if (ts.system != tuning::System::custom)
            {
                range = 15.0;
                for (auto c : d) range = std::max (range, std::abs (c) * 1.15);
            }
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
                text (g, noteNames[i], { (int) x + 4, names.getY(), (int) cw, 16 }, Pal::lcdDim, 9.5f);
            }
        }
    }

    if (! chartArea.isEmpty() && tab == 2)
    {
        auto chart = chartArea;
        text (g, juce::CharPointer_UTF8 ("THE 64 REEDS \xc2\xb7 HOW LONG EACH RINGS (BARS) \xc2\xb7 HOW FAR OUT OF TUNE (DOTS)"), chart.removeFromTop (16), Pal::lcdDim, 9.5f);
        auto labels = chart.removeFromBottom (16);
        auto dots = chart.removeFromBottom (18);
        const auto reedsNow = piano.currentReeds();
        double longest = 0.0;
        for (int k = ep::lowestKey; k <= ep::highestKey; ++k) longest = std::max (longest, reedsNow[(size_t) k].ringSeconds);
        const float cw = (float) chart.getWidth() / 64.0f;
        juce::String shortList;
        for (int i = 0; i < 64; ++i)
        {
            const auto& r = reedsNow[(size_t) (ep::lowestKey + i)];
            const float x = (float) chart.getX() + cw * (float) i;
            const float h = (float) (r.ringSeconds / std::max (0.01, longest)) * (float) chart.getHeight();
            g.setColour (r.shortRinger ? Pal::lamp : Pal::lcdText);
            g.fillRect (juce::Rectangle<float> (x, (float) chart.getBottom() - h, cw - 2.0f, h));
            const float dy = (float) juce::jlimit (-8.0, 8.0, r.driftCents * 0.5);
            g.setColour (Pal::lcdDim);
            g.fillEllipse (x + cw * 0.5f - 2.0f, (float) dots.getCentreY() - dy - 2.0f, 3.0f, 3.0f);
            if (r.shortRinger && shortList.length() < 40) shortList << (shortList.isEmpty() ? "" : ", ") << noteName (ep::lowestKey + i);
        }
        g.setColour (Pal::lcdRule);
        g.drawHorizontalLine (dots.getCentreY(), (float) chart.getX(), (float) chart.getRight());
        text (g, "A1", labels, Pal::lcdDim, 9.0f);
        text (g, "C7", labels, Pal::lcdDim, 9.0f, juce::Justification::centredRight);
        if (shortList.isNotEmpty())
            text (g, "SHORT: " + shortList, labels.reduced (60, 0), Pal::lamp, 9.0f, juce::Justification::centred);
    }

    if (! diagramArea.isEmpty())
    {
        // middle C's reed between its two fingers, end on, to scale
        auto r = diagramArea;
        g.setColour (Pal::lcdRule);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 1.0f);
        text (g, juce::CharPointer_UTF8 ("END VIEW \xc2\xb7 MIDDLE C BETWEEN ITS FINGERS"), r.reduced (8, 4).removeFromTop (14), Pal::lcdDim, 9.0f);
        const auto geo = ep::pickupGeometry (s, 60);
        const auto inner = r.reduced (8, 4).withTrimmedTop (16).withTrimmedBottom (14);
        const float swingMm = (float) ep::fullSwingMm (261.63);
        const float spanMm = 2.0f * swingMm + (float) geo.reedThickness + 0.2f;
        const float scale = juce::jmin (40.0f, (float) inner.getHeight() / spanMm);   // pixels per mm
        const float cx = (float) inner.getCentreX(), mid = (float) inner.getCentreY();
        const float reedW = 3.07f * scale, gapPx = juce::jmax (2.0f, (float) geo.sideGap * scale * 3.0f);   // the gap drawn 3x
        const float reedH = (float) geo.reedThickness * scale, fingerH = (float) geo.fingerThickness * scale;
        // the reed's centre at rest is mid; the fingers sit below it (above, in the bass), clear by the rest offset
        const float reedTop = mid - reedH * 0.5f;
        const float fingerTop = geo.below ? reedTop - (float) geo.rest * scale - fingerH : reedTop + reedH + (float) geo.rest * scale;
        const float fingerW = juce::jmin (70.0f, ((float) inner.getWidth() - reedW) * 0.5f - gapPx - 4.0f);
        g.saveState();
        g.reduceClipRegion (inner);
        g.setColour (Pal::lcdDim);
        g.fillRect (juce::Rectangle<float> (cx - reedW * 0.5f - gapPx - fingerW, fingerTop, fingerW, fingerH));
        g.fillRect (juce::Rectangle<float> (cx + reedW * 0.5f + gapPx, fingerTop, fingerW, fingerH));
        // its full swing, played hard: the reed's outline at either end
        g.setColour (Pal::lcdDim.withAlpha (0.7f));
        for (float k : { -1.0f, 1.0f })
            g.drawRect (juce::Rectangle<float> (cx - reedW * 0.5f, reedTop + k * swingMm * scale, reedW, reedH), 1.0f);
        g.setColour (Pal::lcdText);
        g.fillRect (juce::Rectangle<float> (cx - reedW * 0.5f, reedTop, reedW, reedH));
        g.restoreState();
        text (g, "REED AT REST; OUTLINES: ITS SWING PLAYED HARD", r.reduced (8, 3).removeFromBottom (12), Pal::lcdDim, 8.5f);
        text (g, "FINGER", { (int) (cx - reedW * 0.5f - gapPx - fingerW), (int) (fingerTop + fingerH + 1.0f), (int) fingerW, 11 }, Pal::lcdDim, 8.0f, juce::Justification::centred);
    }

    if (! traceArea.isEmpty())
    {
        auto r = traceArea;
        g.setColour (Pal::lcdRule);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 1.0f);
        r.reduce (8, 4);
        text (g, "WHAT THE PICKUP SENDS, MIDDLE C (BEFORE THE NETWORK)", r.removeFromTop (14), Pal::lcdDim, 9.0f);
        auto draw = [&] (juce::Rectangle<int> box, double amplitudeMm, juce::Colour c, const juce::String& label)
        {
            const int n = box.getWidth();
            std::vector<double> y ((size_t) n);
            double peak = 1.0e-9, lo = 1.0e9, hi = -1.0e9;
            for (int i = 0; i < n; ++i)
            {
                const double ph = 2.0 * 3.14159265358979 * 2.5 * i / n;
                y[(size_t) i] = ep::pickup (s, amplitudeMm * std::sin (ph), 60);
                lo = std::min (lo, y[(size_t) i]); hi = std::max (hi, y[(size_t) i]);
            }
            peak = std::max (1.0e-9, hi - lo);
            juce::Path p;
            const auto plot = box.withTrimmedBottom (12).toFloat().reduced (0.0f, 2.0f);
            for (int i = 0; i < n; ++i)
            {
                const float yy = plot.getBottom() - (float) ((y[(size_t) i] - lo) / peak) * plot.getHeight();
                if (i == 0) p.startNewSubPath (plot.getX(), yy); else p.lineTo (plot.getX() + (float) i, yy);
            }
            g.setColour (c);
            g.strokePath (p, juce::PathStrokeType (1.6f));
            text (g, label, box.removeFromBottom (12), Pal::lcdDim, 8.5f);
        };
        const double full = ep::fullSwingMm (261.63);
        auto top = r.removeFromTop (r.getHeight() / 3);
        auto midBox = r.removeFromTop (r.getHeight() / 2);
        draw (top, full * 0.15, Pal::lcdText, "SOFT: INTO THE FINGERS AND OUT, A FUNDAMENTAL WITH ITS OCTAVE");
        draw (midBox, full * 0.5, Pal::lcdText, "MEDIUM: THE REED FILLS THE FINGERS AND THE TOP FLATTENS");
        draw (r, full, Pal::lamp, "HARD: RIGHT THROUGH THEM, IT FOLDS OVER: THE BARK");
    }
}
} // namespace tf
