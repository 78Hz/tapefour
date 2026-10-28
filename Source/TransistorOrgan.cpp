#include "TransistorOrgan.h"

namespace tf::organ
{
namespace
{
    constexpr double pi = 3.14159265358979323846, twoPi = 2.0 * pi;

    // splitmix64: a fixed random number for (seed, key, salt), 0..1
    double uniform (uint32_t seed, int key, int salt)
    {
        uint64_t z = ((uint64_t) seed << 24) ^ ((uint64_t) (key + 1) << 12) ^ (uint64_t) (uint32_t) salt;
        z += 0x9e3779b97f4a7c15ull;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        z ^= z >> 31;
        return (double) (z >> 11) / 9007199254740992.0;
    }

    int fold (int n)
    {
        while (n < lowestTone) n += 12;
        while (n > highestTone) n -= 12;
        return n;
    }

    // The vibrato's square wave through its RC filter, at phase p (0..1) of a cycle: the filter's time
    // constant is 0.12 of the period (set by ear: the wobble's rounded corners). Normalised to -1..1.
    double vibratoAt (double p)
    {
        constexpr double r = 0.12;
        static const double k = std::exp (-0.5 / r), peak = (1.0 - k) / (1.0 + k);
        const double x = p < 0.5 ? 1.0 - (1.0 + peak) * std::exp (-p / r) : -1.0 + (1.0 + peak) * std::exp (-(p - 0.5) / r);
        return x / peak;
    }

    constexpr double chatterSlot = 0.00015;    // a bouncing contact makes or breaks every 0.15 ms at most
}

//==============================================================================
double drawbarGain (double position)
{
    if (position <= 0.01) return 0.0;
    return std::pow (10.0, -3.0 * (8.0 - std::min (8.0, position)) / 20.0);
}

KeyTones keyTones (int key)
{
    KeyTones k;
    k.foot16 = fold (key - 12);
    k.foot8 = fold (key);
    k.foot4 = fold (key + 12);
    k.mixture = { fold (key + 19), fold (key + 24), fold (key + 28), fold (key + 36) };   // 2 2/3', 2', 1 3/5', 1'
    return k;
}

double contactDelay (const Settings& s, uint32_t seed, int key, int contact, bool opening)
{
    return uniform (seed, key, contact + (opening ? 10 : 0)) * std::max (0.0f, s.spreadMs) * 0.001;
}

bool contactClosed (const Settings& s, uint32_t seed, int key, int contact, double t, double u)
{
    const double bounce = std::max (0.0f, s.bounceMs) * 0.001;
    // while it bounces, the contact is made or broken slot by slot, more often made as it settles
    auto chatter = [&] (double since, bool closing)
    {
        const int slot = (int) (since / chatterSlot);
        const double settled = since / bounce;
        const double pMade = closing ? 0.35 + 0.65 * settled : 0.65 * (1.0 - settled);
        return uniform (seed, key, 1000 + contact * 100003 + slot * 4 + (closing ? 0 : 2)) < pMade;
    };
    auto down = [&] (double since)
    {
        const double d = contactDelay (s, seed, key, contact, false);
        if (since < d) return false;
        if (since < d + bounce) return chatter (since - d, true);
        return true;
    };
    if (u < 0.0) return down (t);
    const double d = contactDelay (s, seed, key, contact, true);
    if (u < d) return down (t);
    if (u < d + bounce) return down (t - u + d) && chatter (u - d, false);
    return false;
}

double vibratoShape (double t, double hz)
{
    const double c = t * hz;
    return vibratoAt (c - std::floor (c));
}

const juce::StringArray& systemChoices()
{
    static const juce::StringArray c { "EQUAL", "JUST", "PYTHAGOREAN", "MEANTONE", "WERCKMEISTER III", "KIRNBERGER III",
                                       "VALLOTTI", "PER NOTE", "SCALA FILE" };
    return c;
}

tuning::System systemFor (int choice)
{
    return choice >= 8 ? tuning::System::scala : (tuning::System) std::clamp (choice, 0, 7);
}

//==============================================================================
struct Processor::Impl
{
    Processor& owner;
    explicit Impl (Processor& p) : owner (p) {}

    juce::AudioParameterFloat *d16 {}, *d8 {}, *d4 {}, *dIV {}, *flute {}, *reed {};
    juce::AudioParameterBool *vibrato {};
    juce::AudioParameterFloat *vibHz {}, *vibDepth {}, *driftP {}, *click {}, *bounce {}, *spread {}, *a4 {};
    juce::AudioParameterChoice *system {}, *keyP {};
    std::array<juce::AudioParameterFloat*, 12> cents {};

    juce::SpinLock scaleLock;
    tuning::Scale scale;
    std::string sclText;
    std::atomic<int> scaleVersion { 0 };

    Settings read() const
    {
        Settings s;
        s.drawbar16 = d16->get(); s.drawbar8 = d8->get(); s.drawbar4 = d4->get(); s.drawbarIV = dIV->get();
        s.flute = flute->get(); s.reed = reed->get(); s.vibrato = vibrato->get();
        s.vibratoHz = vibHz->get(); s.vibratoCents = vibDepth->get(); s.driftCents = driftP->get();
        s.click = click->get(); s.bounceMs = bounce->get(); s.spreadMs = spread->get();
        return s;
    }

    tuning::Settings readTuning (bool withScale)
    {
        tuning::Settings t;
        t.system = systemFor (system->getIndex());
        t.key = keyP->getIndex();
        t.a4Hz = a4->get();
        for (int i = 0; i < 12; ++i) t.customCents[(size_t) i] = cents[(size_t) i]->get();
        if (withScale)
        {
            const juce::SpinLock::ScopedLockType sl (scaleLock);
            t.scale = scale;
        }
        return t;
    }

    // audio thread ---------------------------------------------------------------
    double sr = 48000.0;
    Settings live;
    std::array<double, 12> tuned {};          // the oscillators, C7..B7
    tuning::Settings lastTuning;
    int lastScaleVersion = -1;
    bool haveTuning = false;

    // the generators: each oscillator counts its half-cycles; bit k of the count is the k-th divider's output
    std::array<uint32_t, 12> count {};
    std::array<double, 12> frac {};
    std::array<double, 12> drift {};          // cents
    std::array<std::atomic<float>, 12> driftShown {};
    std::array<double, numTones> naive {}, pending {}, correction {}, rc {}, rcA {};
    double vibPhase = 0.0;
    double fl1 = 0.0, fl2 = 0.0, flA = 0.0;
    juce::Random rng;

    struct Key
    {
        bool active = false;
        int64_t downAt = 0, upAt = -1;
        std::array<float, 4> gain {};
        std::array<float, 4> target {};
        KeyTones tones;
    };
    std::array<Key, 128> keys {};
    int64_t now = 0;
    int tick = 0;

    void prepare (double rate)
    {
        sr = rate;
        haveTuning = false;
        flA = 1.0 - std::exp (-twoPi * fluteCornerHz / sr);
        for (int t = 0; t < numTones; ++t)
        {
            naive[(size_t) t] = -1.0; pending[(size_t) t] = -1.0; correction[(size_t) t] = 0.0; rc[(size_t) t] = 0.0;
        }
        for (int pc = 0; pc < 12; ++pc)
        {
            count[(size_t) pc] = (uint32_t) rng.nextInt (128);   // the dividers start wherever they start
            frac[(size_t) pc] = rng.nextDouble();
            for (int k = 0; k < 7; ++k)
            {
                const int t = 96 + pc - 12 * k - lowestTone;
                naive[(size_t) t] = pending[(size_t) t] = ((count[(size_t) pc] >> k) & 1u) ? 1.0 : -1.0;
            }
        }
        update (0);
    }

    void update (int n)
    {
        live = read();
        auto t = readTuning (false);
        const int sv = scaleVersion.load();
        const bool same = haveTuning && t.system == lastTuning.system && t.key == lastTuning.key && juce::exactlyEqual (t.a4Hz, lastTuning.a4Hz)
                       && t.customCents == lastTuning.customCents && sv == lastScaleVersion;
        if (! same)
        {
            bool got = true;
            if (t.system == tuning::System::scala)
            {
                const juce::SpinLock::ScopedTryLockType sl (scaleLock);
                got = sl.isLocked();
                if (got) t.scale = scale;
            }
            if (got)
            {
                const auto f = tuning::frequencies (t);
                for (int pc = 0; pc < 12; ++pc) tuned[(size_t) pc] = f[(size_t) (96 + pc)] > 0.0 ? f[(size_t) (96 + pc)] : 440.0 * std::pow (2.0, (96 + pc - 69) / 12.0);
                // each tone's RC corner follows its frequency
                for (int tone = 0; tone < numTones; ++tone)
                {
                    const int midi = tone + lowestTone, pc = midi % 12, k = (96 + pc - midi) / 12;
                    const double hz = tuned[(size_t) pc] / (double) (1 << k);
                    rcA[(size_t) tone] = 1.0 - std::exp (-twoPi * std::min (reedCornerRatio * hz, sr * 0.45) / sr);
                }
                lastTuning = t;
                lastScaleVersion = sv;
                haveTuning = true;
            }
        }
        // the oscillators wander: each a slow random walk about its tuning (90 s to forget where it was)
        if (owner.tuneRequest.exchange (false)) drift.fill (0.0);
        const double dt = n / sr, tau = 90.0, sd = live.driftCents;
        for (int pc = 0; pc < 12; ++pc)
        {
            auto& d = drift[(size_t) pc];
            const double g = std::sqrt (-2.0 * std::log (std::max (1.0e-12, (double) rng.nextFloat()))) * std::cos (twoPi * rng.nextFloat());
            d += -d * dt / tau + sd * std::sqrt (2.0 * dt / tau) * g;
            driftShown[(size_t) pc] = (float) d;
        }
    }

    void keyDown (int k)
    {
        auto& key = keys[(size_t) k];
        key.active = true;
        key.downAt = now;
        key.upAt = -1;
        key.tones = keyTones (k);
    }

    void keyUp (int k)
    {
        auto& key = keys[(size_t) k];
        if (key.active && key.upAt < 0) key.upAt = now;
    }

    void render (float* out, int n)
    {
        const auto& s = live;
        const double gains[4] { drawbarGain (s.drawbar16), drawbarGain (s.drawbar8), drawbarGain (s.drawbar4), drawbarGain (s.drawbarIV) };
        const double gReed = drawbarGain (s.reed), gFlute = drawbarGain (s.flute);
        const double vibInc = (double) s.vibratoHz / sr;
        const int rampSamples = std::max (1, (int) ((1.0 - std::clamp ((double) s.click, 0.0, 1.0)) * 0.005 * sr));
        const float rampStep = 1.0f / (float) rampSamples;
        const double release = (s.spreadMs + s.bounceMs) * 0.001 + 0.002;
        for (int i = 0; i < n; ++i)
        {
            // the vibrato and the twelve oscillators
            const double vib = s.vibrato ? std::pow (2.0, s.vibratoCents * vibratoAt (vibPhase) / 1200.0) : 1.0;
            vibPhase += vibInc;
            if (vibPhase >= 1.0) vibPhase -= 1.0;
            for (int pc = 0; pc < 12; ++pc)
            {
                const double inc = 2.0 * tuned[(size_t) pc] * std::pow (2.0, drift[(size_t) pc] / 1200.0) * vib / sr;   // half-cycles per sample
                auto& f = frac[(size_t) pc];
                f += inc;
                if (f >= 1.0)
                {
                    f -= 1.0;
                    const double since = f / inc;   // samples since the edge, 0..1
                    const uint32_t before = count[(size_t) pc]++;
                    const uint32_t changed = before ^ count[(size_t) pc];
                    for (int k = 0; k < 7; ++k)
                    {
                        if (((changed >> k) & 1u) == 0) continue;
                        const size_t t = (size_t) (96 + pc - 12 * k - lowestTone);
                        const double v = ((count[(size_t) pc] >> k) & 1u) ? 1.0 : -1.0;
                        const double half = v;   // half the step (the step is 2v)
                        // band-limit the edge (polyBLEP over the samples either side of it)
                        pending[t] += half * since * since;
                        correction[t] -= half * (1.0 - since) * (1.0 - since);
                        naive[t] = v;
                    }
                }
            }
            // each tone: out by a sample, through its RC filter
            for (size_t t = 0; t < (size_t) numTones; ++t)
            {
                const double y = pending[t];
                pending[t] = naive[t] + correction[t];
                correction[t] = 0.0;
                rc[t] += rcA[t] * (y - rc[t]);
            }

            // the keys: contacts looked at every 8 samples, gains ramped every sample
            const bool look = (tick++ & 7) == 0;
            double bus[4] {};
            for (int k = 0; k < 128; ++k)
            {
                auto& key = keys[(size_t) k];
                if (! key.active) continue;
                if (look)
                {
                    const double t = (double) (now - key.downAt) / sr;
                    const double u = key.upAt >= 0 ? (double) (now - key.upAt) / sr : -1.0;
                    for (int c = 0; c < 4; ++c)
                        key.target[(size_t) c] = contactClosed (s, owner.contactSeed, k, c, t, u) ? 1.0f : 0.0f;
                    auto silent = [] (const std::array<float, 4>& a) { return std::all_of (a.begin(), a.end(), [] (float v) { return v <= 0.0f; }); };
                    if (u > release && silent (key.gain) && silent (key.target)) { key.active = false; continue; }
                }
                for (int c = 0; c < 4; ++c)
                {
                    auto& g = key.gain[(size_t) c];
                    const float tg = key.target[(size_t) c];
                    g = g < tg ? std::min (tg, g + rampStep) : std::max (tg, g - rampStep);
                }
                const auto& kt = key.tones;
                bus[0] += key.gain[0] * rc[(size_t) (kt.foot16 - lowestTone)];
                bus[1] += key.gain[1] * rc[(size_t) (kt.foot8 - lowestTone)];
                bus[2] += key.gain[2] * rc[(size_t) (kt.foot4 - lowestTone)];
                if (key.gain[3] > 0.0f)
                {
                    double m = 0.0;
                    for (int tone : kt.mixture) m += rc[(size_t) (tone - lowestTone)];
                    bus[3] += key.gain[3] * m * 0.5;   // four ranks, each voiced down
                }
            }
            const double mix = bus[0] * gains[0] + bus[1] * gains[1] + bus[2] * gains[2] + bus[3] * gains[3];
            // the flute tone: the mix through two more RC filters; the reed tone: the mix as it is
            fl1 += flA * (mix - fl1);
            fl2 += flA * (fl1 - fl2);
            double y = (mix * gReed + fl2 * gFlute * fluteMakeup) * 0.06;
            y = std::tanh (y * 1.5) / 1.5;   // the preamp's two transistor stages, near their limit only on big chords
            out[i] = (float) y;
            ++now;
        }
    }
};

//==============================================================================
Processor::Processor()
    : juce::AudioProcessor (BusesProperties().withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
      impl (std::make_unique<Impl> (*this))
{
    auto& i = *impl;
    auto fl = [this] (const char* id, const char* name, float lo, float hi, float def, const char* unit = "")
    {
        auto* p = new juce::AudioParameterFloat (juce::ParameterID { id, 1 }, name, juce::NormalisableRange<float> (lo, hi), def,
                                                 juce::AudioParameterFloatAttributes().withLabel (unit));
        addParameter (p);
        return p;
    };
    const Settings d;
    i.d16 = fl ("d16", "16'", 0.0f, 8.0f, d.drawbar16);
    i.d8 = fl ("d8", "8'", 0.0f, 8.0f, d.drawbar8);
    i.d4 = fl ("d4", "4'", 0.0f, 8.0f, d.drawbar4);
    i.dIV = fl ("dIV", "IV", 0.0f, 8.0f, d.drawbarIV);
    i.flute = fl ("flute", "Flute", 0.0f, 8.0f, d.flute);
    i.reed = fl ("reed", "Reed", 0.0f, 8.0f, d.reed);
    i.vibrato = new juce::AudioParameterBool (juce::ParameterID { "vibrato", 1 }, "Vibrato", d.vibrato);
    addParameter (i.vibrato);
    i.vibHz = fl ("vibHz", "Vibrato speed", 3.0f, 9.0f, d.vibratoHz, "Hz");
    i.vibDepth = fl ("vibDepth", "Vibrato depth", 0.0f, 60.0f, d.vibratoCents, "cents");
    i.driftP = fl ("drift", "Drift", 0.0f, 15.0f, d.driftCents, "cents");
    i.click = fl ("click", "Click", 0.0f, 1.0f, d.click);
    i.bounce = fl ("bounce", "Bounce", 0.0f, 5.0f, d.bounceMs, "ms");
    i.spread = fl ("spread", "Contact spread", 0.0f, 10.0f, d.spreadMs, "ms");
    i.system = new juce::AudioParameterChoice (juce::ParameterID { "system", 1 }, "Tuning system", systemChoices(), 0);
    addParameter (i.system);
    i.keyP = new juce::AudioParameterChoice (juce::ParameterID { "key", 1 }, "Key", { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" }, 0);
    addParameter (i.keyP);
    i.a4 = fl ("a4", "A4", 415.0f, 466.0f, 440.0f, "Hz");
    static const char* names[] { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    for (int k = 0; k < 12; ++k)
        i.cents[(size_t) k] = fl (("cents" + juce::String (k)).toRawUTF8(), (juce::String ("Cents ") + names[k]).toRawUTF8(), -50.0f, 50.0f, 0.0f, "cents");
    contactSeed = (uint32_t) juce::Random::getSystemRandom().nextInt();
    // the organ has been switched on a while: each oscillator somewhere in its wander
    for (int pc = 0; pc < 12; ++pc)
    {
        i.drift[(size_t) pc] = d.driftCents * (i.rng.nextFloat() * 2.0 - 1.0) * 0.8;
        i.driftShown[(size_t) pc] = (float) i.drift[(size_t) pc];
    }
}

Processor::~Processor() = default;

const juce::String Processor::getName() const { return "TF TRANSISTOR"; }

Settings Processor::settings() const { return impl->read(); }

void Processor::setSettings (const Settings& s)
{
    auto& i = *impl;
    auto setF = [] (juce::AudioParameterFloat* p, float v) { p->setValueNotifyingHost (p->convertTo0to1 (v)); };
    setF (i.d16, s.drawbar16); setF (i.d8, s.drawbar8); setF (i.d4, s.drawbar4); setF (i.dIV, s.drawbarIV);
    setF (i.flute, s.flute); setF (i.reed, s.reed);
    i.vibrato->setValueNotifyingHost (s.vibrato ? 1.0f : 0.0f);
    setF (i.vibHz, s.vibratoHz); setF (i.vibDepth, s.vibratoCents); setF (i.driftP, s.driftCents);
    setF (i.click, s.click); setF (i.bounce, s.bounceMs); setF (i.spread, s.spreadMs);
}

tuning::Settings Processor::tuningSettings() const { return impl->readTuning (true); }

bool Processor::setScale (const tuning::Scale& sc, const std::string& text, std::string& error)
{
    if (sc.cents.size() != 12 || std::abs (sc.cents.back() - 1200.0) > 0.01)
    {
        error = "A DIVIDE-DOWN ORGAN NEEDS 12 NOTES AND A 2/1 OCTAVE";
        return false;
    }
    { const juce::SpinLock::ScopedLockType sl (impl->scaleLock); impl->scale = sc; impl->sclText = text; }
    ++impl->scaleVersion;
    updateHostDisplay();
    return true;
}

juce::String Processor::scaleDescription() const
{
    const juce::SpinLock::ScopedLockType sl (impl->scaleLock);
    if (! impl->scale.valid()) return {};
    return juce::String (impl->scale.description);
}

std::array<double, 12> Processor::oscillatorHz() const
{
    const auto f = tuning::frequencies (tuningSettings());
    std::array<double, 12> out {};
    for (int pc = 0; pc < 12; ++pc) out[(size_t) pc] = f[(size_t) (96 + pc)];
    return out;
}

std::array<double, 12> Processor::driftNow() const
{
    std::array<double, 12> out {};
    for (int pc = 0; pc < 12; ++pc) out[(size_t) pc] = impl->driftShown[(size_t) pc].load();
    return out;
}

juce::RangedAudioParameter* Processor::param (const juce::String& id) const
{
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p); r != nullptr && r->paramID == id)
            return r;
    return nullptr;
}

void Processor::prepareToPlay (double sampleRate, int)
{
    impl->prepare (sampleRate);
}

void Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    auto& i = *impl;
    const int n = buffer.getNumSamples();
    i.update (n);
    buffer.clear();
    auto* out = buffer.getWritePointer (0);
    int pos = 0;
    for (const auto m : midi)
    {
        const int at = std::clamp (m.samplePosition, pos, n);
        if (at > pos) { i.render (out + pos, at - pos); pos = at; }
        const auto msg = m.getMessage();
        if (msg.isNoteOn()) i.keyDown (msg.getNoteNumber());
        else if (msg.isNoteOff()) i.keyUp (msg.getNoteNumber());
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            for (int k = 0; k < 128; ++k) i.keyUp (k);
    }
    if (pos < n) i.render (out + pos, n - pos);
    for (int c = 1; c < buffer.getNumChannels(); ++c) buffer.copyFrom (c, 0, buffer, 0, 0, n);
    midi.clear();
}

//==============================================================================
std::unique_ptr<juce::XmlElement> Processor::toXml (bool) const
{
    auto x = std::make_unique<juce::XmlElement> ("ORGAN");
    x->setAttribute ("version", 1);
    x->setAttribute ("seed", juce::String::toHexString ((juce::int64) contactSeed));
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
            x->setAttribute (r->paramID, (double) r->convertFrom0to1 (r->getValue()));
    const juce::SpinLock::ScopedLockType sl (impl->scaleLock);
    if (! impl->sclText.empty()) x->createNewChildElement ("SCL")->addTextElement (juce::String (impl->sclText));
    return x;
}

void Processor::fromXml (const juce::XmlElement& x)
{
    if (! x.hasTagName ("ORGAN")) return;
    if (x.hasAttribute ("seed")) contactSeed = (uint32_t) x.getStringAttribute ("seed").getHexValue64();
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p); r != nullptr && x.hasAttribute (r->paramID))
            r->setValueNotifyingHost (r->convertTo0to1 ((float) x.getDoubleAttribute (r->paramID)));
    tuning::Scale sc;
    std::string err, text;
    if (auto* e = x.getChildByName ("SCL")) { text = e->getAllSubText().toStdString(); if (! tuning::parseScala (text, sc, err)) text.clear(); }
    {
        const juce::SpinLock::ScopedLockType sl (impl->scaleLock);
        impl->scale = text.empty() ? tuning::Scale() : sc;
        impl->sclText = text;
    }
    ++impl->scaleVersion;
}

void Processor::getStateInformation (juce::MemoryBlock& mb)
{
    auto x = toXml (true);
    writePresetAttributes (*x);
    copyXmlToBinary (*x, mb);
}

void Processor::setStateInformation (const void* data, int size)
{
    auto x = getXmlFromBinary (data, size);
    if (x == nullptr || ! x->hasTagName ("ORGAN")) return;
    fromXml (*x);
    readPresetAttributes (*x);
}

//==============================================================================
const presets::Kind& presetKind()
{
    static const presets::Kind kind { "Transistor", "tfto", "TFTO", "ORGAN", [] (const juce::XmlElement& o)
    {
        auto bar = [&o] (const char* id, double def) { return juce::String (juce::roundToInt (o.getDoubleAttribute (id, def))); };
        const int sys = o.getIntAttribute ("system");
        static const char* keys[] { "C", "C#", "D", "EB", "E", "F", "F#", "G", "AB", "A", "BB", "B" };
        juce::String tun = systemChoices()[std::clamp (sys, 0, 8)];
        if (sys > 0 && sys < 7) tun << " IN " << keys[std::clamp (o.getIntAttribute ("key"), 0, 11)];
        const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
        return bar ("d16", 6) + bar ("d8", 8) + bar ("d4", 5) + bar ("dIV", 3) + dot + "FLUTE " + bar ("flute", 4) + " REED " + bar ("reed", 8)
             + dot + (o.getDoubleAttribute ("vibrato", 1.0) > 0.5 ? "VIBRATO" : "NO VIBRATO") + dot + tun;
    } };
    return kind;
}

const presets::Kind& Processor::presetKind() const { return organ::presetKind(); }
} // namespace tf::organ
