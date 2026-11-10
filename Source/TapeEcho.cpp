#include "TapeEcho.h"

namespace tf::echo
{
namespace
{
    // The loop cassette's formulation, as the deck's tapes: SD (normal), XR (chrome), MX (metal).
    const TapeModel& formulation (int t) { return tapeModels[1 + std::clamp (t, 0, 2)]; }
    // Where the treble goes at 1 7/8 ips (it scales with the speed): the head's gap and the tape.
    constexpr double trebleHz[] { 9000.0, 11000.0, 12500.0 };
}

Processor::Processor()
    : juce::AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
{
    pRate = addFloat ("rate", "Rate", 0.25f, 2.0f, 1.0f, 1.0f);
    pIntensity = addFloat ("intensity", "Intensity", 0.0f, 1.2f, 0.45f);
    pBass = addFloat ("bass", "Bass", -10.0f, 10.0f, 0.0f);
    pTreble = addFloat ("treble", "Treble", -10.0f, 10.0f, 0.0f);
    pSpread = addFloat ("spread", "Spread", 0.0f, 1.0f, 0.6f);
    pAge = addFloat ("age", "Tape age", 0.0f, 1.0f, 0.3f);
    pWow = addFloat ("wow", "Wow and flutter", 0.0f, 1.0f, 0.3f);
    pDrive = addFloat ("drive", "Drive", -12.0f, 12.0f, 0.0f);
    const float defaults[] { 0.21f, 0.42f, 0.63f };
    for (int h = 0; h < numHeads; ++h)
    {
        pHead[h] = addFloat ("head" + juce::String (h + 1), "Head " + juce::String (h + 1), 0.0f, (float) loopSeconds[numLoops - 1], defaults[h], 1.0f);
        pOn[h] = addBool ("on" + juce::String (h + 1), "Head " + juce::String (h + 1) + " on", h != 1);
    }
    pErase = addBool ("erase", "Erase", true);
    pSplice = addBool ("splice", "Splice tick", true);
    pHold = addBool ("hold", "Hold", false);
    pTape = addChoice ("tape", "Tape", { "NORMAL", "CHROME", "METAL" }, 0);
    pLoop = addChoice ("loop", "Loop length", { "10 S", "20 S", "30 S", "60 S" }, 1);
    juce::StringArray notes;
    for (auto& v : noteValues) notes.add (v.label);
    pNote = addChoice ("note", "Head 1 is", notes, 2);

    tape.assign ((size_t) (loopSeconds[numLoops - 1] * cellRate) + 16, 0.0f);
    dropout.assign (tape.size() / dropoutCells + 2, 1.0f);
    for (auto& b : bins) b = 0.0f;
}

Processor::~Processor() = default;

juce::AudioParameterFloat* Processor::addFloat (const juce::String& id, const juce::String& name, float lo, float hi, float def, float skewCentre)
{
    juce::NormalisableRange<float> range (lo, hi);
    if (skewCentre > lo && skewCentre < hi) range.setSkewForCentre (skewCentre);
    auto* p = new juce::AudioParameterFloat (juce::ParameterID { id, 1 }, name, range, def);
    addParameter (p);
    params[id] = p;
    return p;
}

juce::AudioParameterBool* Processor::addBool (const juce::String& id, const juce::String& name, bool def)
{
    auto* p = new juce::AudioParameterBool (juce::ParameterID { id, 1 }, name, def);
    addParameter (p);
    params[id] = p;
    return p;
}

juce::AudioParameterChoice* Processor::addChoice (const juce::String& id, const juce::String& name, const juce::StringArray& choices, int def)
{
    auto* p = new juce::AudioParameterChoice (juce::ParameterID { id, 1 }, name, choices, def);
    addParameter (p);
    params[id] = p;
    return p;
}

juce::RangedAudioParameter* Processor::param (const juce::String& id) const
{
    auto it = params.find (id);
    return it != params.end() ? it->second : nullptr;
}

float Processor::get (const juce::String& id) const
{
    auto* p = param (id);
    return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f;
}

void Processor::set (const juce::String& id, float value)
{
    if (auto* p = param (id))
        p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (p->getNormalisableRange().start, p->getNormalisableRange().end, value)));
}

double Processor::loopNominal() const { return loopSeconds[std::clamp (pLoop->getIndex(), 0, numLoops - 1)]; }
double Processor::rate() const { return pRate->get(); }

double Processor::headPos (int h) const
{
    // On a shorter loop a head can't be further than the loop goes.
    return std::min ((double) pHead[std::clamp (h, 0, numHeads - 1)]->get(), loopNominal() - 0.001);
}

void Processor::setHeadPos (int h, double s)       { set ("head" + juce::String (h + 1), (float) juce::jlimit (0.001, loopNominal() - 0.001, s)); }
bool Processor::headOn (int h) const               { return pOn[std::clamp (h, 0, numHeads - 1)]->get(); }
void Processor::setHeadOn (int h, bool on)         { set ("on" + juce::String (h + 1), on ? 1.0f : 0.0f); }
int Processor::noteValue() const                   { return std::clamp (pNote->getIndex(), 0, numNoteValues - 1); }
void Processor::setNoteValue (int v)               { set ("note", (float) std::clamp (v, 0, numNoteValues - 1)); }

double Processor::bpm() const
{
    const double t = toSeconds (headPos (0));
    return t > 0.0 ? 60.0 * noteValues[noteValue()].quarters / t : 0.0;
}

void Processor::setBpm (double b)
{
    if (b <= 0.0) return;
    set ("rate", (float) (b * headPos (0) / (60.0 * noteValues[noteValue()].quarters)));
}

void Processor::place (int k)
{
    const auto& p = placements[std::clamp (k, 0, numPlacements - 1)];
    const double h1 = headPos (0);
    setHeadPos (1, h1 * p.h2);
    setHeadPos (2, h1 * p.h3);
}

bool Processor::tooClose (int h) const
{
    const double near = tooCloseMm / mmPerNominalSecond, len = loopNominal(), at = headPos (h);
    auto apart = [len] (double a, double b) { const double d = std::abs (a - b); return std::min (d, len - d); };
    if (apart (at, 0.0) < near) return true;   // on the record head
    for (int k = 0; k < numHeads; ++k)
        if (k != h && apart (at, headPos (k)) < near) return true;
    return false;
}

float Processor::takeRecordPeakDb()
{
    const float p = recPeak.exchange (0.0f);
    return p > 1.0e-6f ? 20.0f * std::log10 (p / tapeRefPeak) : -60.0f;
}

void Processor::prepareToPlay (double rate, int)
{
    sr = rate;
    speed = pRate->get();
    lastLpHz = -1.0f;
    lastBass = lastTreble = 99.0f;
    for (auto* b : { &recLp1, &recLp2, &fbBass, &fbTreble, &outBassL, &outTrebleL, &outBassR, &outTrebleR }) b->reset();
}

//==============================================================================
void Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const int n = buffer.getNumSamples();
    auto* l = buffer.getWritePointer (0);
    auto* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : l;

    const int len = std::max (1000, (int) (loopNominal() * cellRate));
    if (writePos >= len) writePos = std::fmod (writePos, (double) len);
    const auto& f = formulation (pTape->getIndex());
    const float age = pAge->get();

    // Dropouts are places on the tape: the same ones come round every turn, more as it ages.
    if (std::abs (age - dropoutFor) > 0.01f)
    {
        dropoutFor = age;
        juce::Random places (0x10e7);
        const float chance = 0.002f + 0.03f * age * age;
        for (auto& d : dropout)
        {
            const float roll = places.nextFloat(), depth = places.nextFloat();
            d = roll < chance ? 1.0f - (0.3f + 0.6f * depth) * std::min (1.0f, age * 1.5f + 0.2f) : 1.0f;
        }
    }

    // The treble the tape keeps at this speed, and the tone of the repeats.
    const float lpHz = (float) juce::jlimit (400.0, sr * 0.45, trebleHz[std::clamp (pTape->getIndex(), 0, 2)] * speed * (1.0 - 0.45 * age));
    if (std::abs (lpHz - lastLpHz) > lastLpHz * 0.01f)
    {
        lastLpHz = lpHz;
        recLp1.setLowPass (sr, lpHz, 0.6);
        recLp2.setLowPass (sr, lpHz * 1.4, 0.7);
    }
    if (! juce::exactlyEqual (pBass->get(), lastBass) || ! juce::exactlyEqual (pTreble->get(), lastTreble))
    {
        lastBass = pBass->get();
        lastTreble = pTreble->get();
        for (auto* b : { &fbBass, &outBassL, &outBassR }) b->setLowShelf (sr, 200.0, lastBass);
        for (auto* b : { &fbTreble, &outTrebleL, &outTrebleR }) b->setHighShelf (sr, 3000.0, lastTreble);
    }

    const float drive = dbToGain (pDrive->get()), intensity = pIntensity->get();
    const float sat = tapeRefPeak * dbToGain (f.mol400 + 2.0f);
    const float hiss = tapeRefPeak * dbToGain (-f.snr + 10.0f * age) * 1.7f;
    const bool hold = pHold->get(), erase = pErase->get() && ! hold, splice = pSplice->get();
    const double wowD = pWow->get() * 0.006 + age * 0.001, flD = pWow->get() * 0.0025;
    const double ease = 1.0 - std::exp (-1.0 / (0.25 * sr));   // the motor takes a moment to get there
    const double target = pRate->get();
    const double eraseCells = eraseGapSeconds * cellRate;
    const double inc[4] { 0.55 / sr, 1.3 / sr, 6.3 / sr, 11.7 / sr };
    const auto tp = juce::MathConstants<double>::twoPi;

    double cells[numHeads];
    bool on[numHeads];
    float gl[numHeads], gr[numHeads];
    const float spread = pSpread->get();
    for (int h = 0; h < numHeads; ++h)
    {
        cells[h] = headPos (h) * cellRate;
        on[h] = pOn[h]->get();
        const float pan = spread * (float) (h - 1);                   // heads 1-3, left to right
        const float a = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        gl[h] = std::cos (a) * juce::MathConstants<float>::sqrt2;
        gr[h] = std::sin (a) * juce::MathConstants<float>::sqrt2;
    }

    auto at = [&] (double idx)
    {
        while (idx < 0.0) idx += len;
        while (idx >= len) idx -= len;
        const int i0 = (int) idx;
        const float fr = (float) (idx - i0);
        const float a = tape[(size_t) i0], b = tape[(size_t) ((i0 + 1) % len)];
        const int d0 = i0 / dropoutCells;
        const float dg = dropout[(size_t) d0] + (dropout[(size_t) d0 + 1] - dropout[(size_t) d0]) * (float) ((idx - d0 * dropoutCells) / dropoutCells);
        return std::make_pair ((a + (b - a) * fr) * dg, idx);
    };

    float peak = recPeak.load();
    for (int i = 0; i < n; ++i)
    {
        for (int k = 0; k < 4; ++k) { ph[k] += inc[k]; if (ph[k] >= 1.0) ph[k] -= 1.0; }
        const double mod = 1.0 + wowD * (0.7 * std::sin (tp * ph[0]) + 0.3 * std::sin (tp * ph[1]))
                               + flD * (0.6 * std::sin (tp * ph[2]) + 0.4 * std::sin (tp * ph[3]));
        speed += (target - speed) * ease;
        const double v = speed * mod * cellRate / sr;   // cells past the heads this sample

        // ---- playback: each head reads the tape where it sits ----
        float mono = 0.0f, outL = 0.0f, outR = 0.0f;
        for (int h = 0; h < numHeads; ++h)
        {
            if (! on[h]) continue;
            auto [y, idx] = at (writePos - cells[h]);
            // the splice passing this head: a tick and a moment's gap
            if (splice && readPrev[h] - idx > len * 0.5) clickEnv[h] = 1.0f;
            readPrev[h] = idx;
            y += (rng.nextFloat() * 2.0f - 1.0f) * hiss;
            if (clickEnv[h] > 1.0e-4f)
            {
                y = y * (1.0f - 0.85f * clickEnv[h]) + clickEnv[h] * 0.12f * tapeRefPeak * ((i & 1) ? 1.0f : -0.6f);
                clickEnv[h] *= 0.985f;
            }
            mono += y;
            outL += y * gl[h];
            outR += y * gr[h];
        }

        // ---- record: the input and what goes round again, into the tape ----
        const float fb = fbTreble.process (fbBass.process (mono));
        const float x = 0.5f * (l[i] + r[i]) * drive + intensity * fb;
        peak = std::max (peak, std::abs (x));
        float y = sat * std::tanh (x / sat);
        y = recLp2.process (recLp1.process (y));
        const double next = writePos + v;
        if (! hold)
        {
            for (auto c = (long long) std::floor (writePos) + 1; c <= (long long) std::floor (next); ++c)
            {
                const float fr = (float) (((double) c - writePos) / v);
                const float val = (float) lastRecValue + (y - (float) lastRecValue) * fr;
                const auto ci = (size_t) (c % len);
                if (erase)
                {
                    tape[(size_t) ((c + (long long) eraseCells) % len)] = 0.0f;   // the erase head, just ahead
                    tape[ci] = val;
                }
                else
                    tape[ci] = tape[ci] * 0.7f + val;   // recording over what's there takes a little of it
                const int b = (int) ((long long) ci * overviewBins / len);
                if (b != lastBin) { lastBin = b; bins[(size_t) b] = 0.0f; }
                if (std::abs (tape[ci]) > bins[(size_t) b].load()) bins[(size_t) b] = std::abs (tape[ci]);
            }
        }
        writePos = next >= len ? next - len : next;
        lastRecValue = y;

        l[i] = outTrebleL.process (outBassL.process (outL));
        r[i] = outTrebleR.process (outBassR.process (outR));
    }
    recPeak = peak;
    recFraction = writePos / len;
    for (int c = 2; c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);
}

//==============================================================================
std::unique_ptr<juce::XmlElement> Processor::toXml (bool) const
{
    auto x = std::make_unique<juce::XmlElement> ("TFTAPEECHO");
    x->setAttribute ("version", 1);
    for (auto& [id, p] : params)
        x->setAttribute (id, (double) p->convertFrom0to1 (p->getValue()));
    return x;
}

void Processor::fromXml (const juce::XmlElement& x)
{
    if (x.hasTagName ("FX")) { fromOldEcho (x); return; }
    if (! x.hasTagName ("TFTAPEECHO")) return;
    for (auto& [id, p] : params)
        if (x.hasAttribute (id))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) x.getDoubleAttribute (id)));
}

void Processor::fromOldEcho (const juce::XmlElement& x)
{
    // Decks from before the cassette loop: one head at its TIME, FEEDBACK, TONE (as treble) and WOW.
    set ("rate", 1.0f);
    setHeadPos (0, x.getDoubleAttribute ("time", 340.0) * 0.001);
    setHeadOn (0, true);
    setHeadOn (1, false);
    setHeadOn (2, false);
    set ("intensity", (float) x.getDoubleAttribute ("feedback", 0.45));
    set ("treble", (float) juce::jmap (x.getDoubleAttribute ("tone", 3200.0), 1000.0, 8000.0, -8.0, 4.0));
    set ("wow", (float) x.getDoubleAttribute ("wow", 0.3));
}

void Processor::getStateInformation (juce::MemoryBlock& mb)
{
    auto x = toXml (true);
    writePresetAttributes (*x);
    copyXmlToBinary (*x, mb);
}

void Processor::setStateInformation (const void* data, int size)
{
    if (auto x = getXmlFromBinary (data, size))
    {
        fromXml (*x);
        readPresetAttributes (*x);
    }
}

const presets::Kind& presetKind()
{
    static const presets::Kind kind { "Tape Echo", "tfte", "TFTAPEECHO", "ECHO", [] (const juce::XmlElement& s)
    {
        int heads = 0;
        for (int h = 1; h <= numHeads; ++h) heads += s.getDoubleAttribute ("on" + juce::String (h)) > 0.5 ? 1 : 0;
        const int loop = std::clamp ((int) s.getDoubleAttribute ("loop", 1), 0, numLoops - 1);
        return juce::String (heads) + (heads == 1 ? " HEAD" : " HEADS") + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 LOOP "))
               + juce::String ((int) loopSeconds[loop]) + " S" + (s.getDoubleAttribute ("erase", 1) < 0.5 ? juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 NO ERASE")) : juce::String());
    } };
    return kind;
}

const presets::Kind& Processor::presetKind() const { return echo::presetKind(); }
} // namespace tf::echo
