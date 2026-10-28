#include "ElectricPiano.h"

namespace tf::ep
{
namespace
{
    constexpr double pi = 3.14159265358979323846, twoPi = 2.0 * pi;

    // splitmix64: a fixed random number for (seed, key, salt)
    uint64_t mix (uint64_t z)
    {
        z += 0x9e3779b97f4a7c15ull;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    double uniform (uint32_t seed, int key, int salt)
    {
        return (double) (mix (((uint64_t) seed << 20) ^ ((uint64_t) (key + 1) << 4) ^ (uint64_t) salt) >> 11) / 9007199254740992.0;
    }
    double gaussian (uint32_t seed, int key, int salt)
    {
        const double u1 = std::max (1.0e-12, uniform (seed, key, salt)), u2 = uniform (seed, key, salt + 7);
        return std::sqrt (-2.0 * std::log (u1)) * std::cos (twoPi * u2);
    }

    // How far a reed's tip swings at full velocity, in the pickup's units: long low reeds swing further.
    double swing (double hz) { return std::clamp (1.35 * std::pow (261.63 / std::max (20.0, hz), 0.35), 0.5, 2.6); }
    // The pickup's geometry in its own units: the comb's reach, and where the reed rests.
    double reach (const Settings& s)  { return 0.45 + (s.gapMm - 0.3) * 1.0; }
    double height (const Settings& s) { return 0.1 + 1.1 * s.reedHeight; }

    double capacitance (double u, double w) { const double r = u / w; return 1.0 / (1.0 + r * r); }

    // The spectrum of a half-sine force pulse of length tc (the felt's contact), at f: 1 at 0 Hz.
    double pulse (double f, double tc)
    {
        const double a = 2.0 * f * tc;
        if (std::abs (1.0 - a) < 1.0e-4) return pi / 4.0;
        return std::abs (std::cos (pi * f * tc) / (1.0 - a * a));
    }

    double dbToGain (double db) { return db <= -119.0 ? 0.0 : std::pow (10.0, db / 20.0); }

    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double process (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
        void set (double nb0, double nb1, double nb2, double a0, double na1, double na2)
        {
            b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = na1 / a0; a2 = na2 / a0;
        }
        void lowPass (double f, double q, double sr)
        {
            const double w = twoPi * std::min (f, sr * 0.45) / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q);
            set ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
        }
        void highPass (double f, double q, double sr)
        {
            const double w = twoPi * f / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q);
            set ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
        }
        void peak (double f, double q, double db, double sr)
        {
            const double A = std::pow (10.0, db / 40.0), w = twoPi * f / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q);
            set (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
        }
    };
}

//==============================================================================
const char* conditionName (int c)
{
    static const char* n[] { "NEW", "SERVICED", "WORN", "NEGLECTED" };
    return n[std::clamp (c, 0, 3)];
}

void applyCondition (Settings& s, int c)
{
    struct C { float spread, ring; int ringers; float felt, strike, height, gap, volts, damping, damperDb, actionDb, pedalDb, humDb; };
    static const C table[] {
        { 0.5f, 1.00f, 0, 0.35f,  0.00f, 0.50f, 0.50f, 168.0f, 0.80f, -64.0f, -58.0f, -66.0f, -92.0f },   // NEW
        { 3.0f, 0.95f, 1, 0.50f,  0.00f, 0.52f, 0.50f, 165.0f, 0.70f, -56.0f, -50.0f, -58.0f, -78.0f },   // SERVICED
        { 9.0f, 0.85f, 3, 0.70f,  0.15f, 0.62f, 0.62f, 158.0f, 0.50f, -48.0f, -44.0f, -52.0f, -68.0f },   // WORN
        { 20.0f, 0.70f, 7, 0.86f, -0.20f, 0.76f, 0.78f, 145.0f, 0.30f, -42.0f, -38.0f, -46.0f, -58.0f },  // NEGLECTED
    };
    const auto& t = table[std::clamp (c, 0, 3)];
    s.tuningSpreadCents = t.spread; s.ringTime = t.ring; s.shortRingers = t.ringers;
    s.felt = t.felt; s.strikePoint = t.strike; s.blowInches = 1.078f; s.letOffInches = 0.125f; s.velocityCurve = 0.0f;
    s.reedHeight = t.height; s.gapMm = t.gap; s.volts = t.volts;
    s.damping = t.damping; s.damperNoiseDb = t.damperDb; s.actionNoiseDb = t.actionDb; s.pedalNoiseDb = t.pedalDb;
    s.humDb = t.humDb;
}

Ageing ageing (double h)
{
    Ageing a;
    h = std::max (0.0, h);
    a.spreadCents = 2.0 * std::sqrt (h / 100.0);
    a.felt = 0.25 * (1.0 - std::exp (-h / 2000.0));
    a.damping = 0.2 * (1.0 - std::exp (-h / 3000.0));
    a.shortRingers = std::min (12, (int) (h / 400.0));
    return a;
}

std::array<Reed, 128> reeds (const Settings& s, const std::array<double, 128>& tuned, uint32_t seed, double hours)
{
    std::array<Reed, 128> out {};
    const auto a = ageing (hours);
    const int shortCount = std::clamp (s.shortRingers + a.shortRingers, 0, 64);
    // Which reeds ring short: the first shortCount of the 64, in an order fixed by the piano's seed.
    std::array<std::pair<double, int>, 64> order {};
    for (int i = 0; i < 64; ++i) order[(size_t) i] = { uniform (seed, lowestKey + i, 91), lowestKey + i };
    std::sort (order.begin(), order.end());
    std::array<bool, 128> isShort {};
    for (int i = 0; i < shortCount; ++i) isShort[(size_t) order[(size_t) i].second] = true;

    for (int k = 0; k < 128; ++k)
    {
        auto& r = out[(size_t) k];
        r.onKeyboard = k >= lowestKey && k <= highestKey;
        if (tuned[(size_t) k] <= 0.0) continue;
        // Each reed's own drift: its share of the set spread, plus what playing has added since.
        r.driftCents = gaussian (seed, k, 3) * s.tuningSpreadCents + gaussian (seed, k, 17) * a.spreadCents;
        r.hz = tuned[(size_t) k] * std::pow (2.0, r.driftCents / 1200.0);
        r.shortRinger = isShort[(size_t) k];
        r.ringSeconds = std::clamp (s.ringTime * 1.25 * std::pow (261.63 / tuned[(size_t) k], 0.62), 0.08, 12.0) * (r.shortRinger ? 0.22 : 1.0);
    }
    return out;
}

double ringToMinus40 (const Reed& r) { return r.ringSeconds * std::log (100.0); }

double pickup (const Settings& s, double x, double reedHz)
{
    const double w = reach (s), h = height (s);
    const double u = x * swing (reedHz) + h;
    return (capacitance (u, w) - capacitance (h, w)) * (s.volts / 165.0) / std::pow (s.gapMm / 0.5, 0.7);
}

//==============================================================================
namespace
{
    const juce::StringArray systemChoices { "EQUAL", "JUST", "PYTHAGOREAN", "MEANTONE", "WERCKMEISTER III", "KIRNBERGER III",
                                            "VALLOTTI", "PER NOTE", "EDO", "SCALA FILE" };
    const juce::StringArray keyChoices { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    const juce::StringArray pressureChoices { "OFF", "VOLUME", "VIBRATO DEPTH", "PREAMP DRIVE", "DAMPER" };
    const juce::StringArray slideChoices { "OFF", "HAMMER FELT", "REED HEIGHT" };
}

struct Processor::Impl
{
    Processor& owner;
    explicit Impl (Processor& p) : owner (p) {}

    // parameters
    juce::AudioParameterChoice *model {}, *system {}, *key {}, *output {}, *mains {}, *pressure {}, *slide {};
    juce::AudioParameterBool *ages {}, *mpe {}, *bend {};
    juce::AudioParameterInt *edo {}, *ringers {};
    juce::AudioParameterFloat *a4 {}, *stretch {}, *spread {}, *ring {}, *felt {}, *strike {}, *blow {}, *letoff {}, *curve {},
                              *heightP {}, *gap {}, *volts {}, *damping {}, *damperDb {}, *actionDb {}, *pedalDb {},
                              *volume {}, *vibrato {}, *drive {}, *humDb {}, *pressureAmt {};
    std::array<juce::AudioParameterFloat*, 12> cents {};

    // the Scala scale and map (message thread writes, audio thread copies under the lock)
    juce::SpinLock scaleLock;
    tuning::Scale scale;
    tuning::KeyboardMap kbm;
    bool hasKbm = false;
    std::string sclText, kbmText;
    std::atomic<int> scaleVersion { 0 };

    // audio thread
    double sr = 48000.0;
    juce::MPESynthesiser synth;
    Settings live;
    std::array<double, 128> tuned {};
    std::array<Reed, 128> reedTable {};
    int lastScaleVersion = -1;
    tuning::Settings lastTuning;
    bool haveTuning = false;
    Settings lastReedSettings;
    double lastReedHours = -1.0;
    juce::AudioBuffer<float> mono;
    double hpState = 0, hpPrevIn = 0, hpCoef = 0.998;
    double vibPhase = 0, humPhase = 0;
    Biquad spkHp, spkPeak, spkLp;
    int spkModel = -1;
    juce::Random rng { 0x5eed };
    float pedalNoiseEnv = 0.0f, pedalLp = 0.0f;
    bool pedalDown = false;
    float maxPressure = 0.0f;
    std::atomic<int> activeVoices { 0 };

    Settings read() const
    {
        Settings s;
        s.model = model->getIndex(); s.ages = ages->get();
        s.tuningSpreadCents = spread->get(); s.ringTime = ring->get(); s.shortRingers = ringers->get();
        s.felt = felt->get(); s.strikePoint = strike->get(); s.blowInches = blow->get(); s.letOffInches = letoff->get(); s.velocityCurve = curve->get();
        s.reedHeight = heightP->get(); s.gapMm = gap->get(); s.volts = volts->get();
        s.damping = damping->get(); s.damperNoiseDb = damperDb->get(); s.actionNoiseDb = actionDb->get(); s.pedalNoiseDb = pedalDb->get();
        s.volume = volume->get(); s.vibrato = vibrato->get(); s.drive = drive->get(); s.humDb = humDb->get();
        s.rearJack = output->getIndex() == 1; s.mains60 = mains->getIndex() == 1;
        s.mpe = mpe->get(); s.bend = bend->get(); s.pressureTarget = pressure->getIndex(); s.pressureAmount = pressureAmt->get();
        s.slideTarget = slide->getIndex();
        return s;
    }

    tuning::Settings readTuning (bool withScale)
    {
        tuning::Settings t;
        t.system = (tuning::System) system->getIndex();
        t.key = key->getIndex();
        t.a4Hz = a4->get();
        t.stretchCentsPerOctave = stretch->get();
        for (int i = 0; i < 12; ++i) t.customCents[(size_t) i] = cents[(size_t) i]->get();
        t.edoSteps = edo->get();
        if (withScale)
        {
            const juce::SpinLock::ScopedLockType sl (scaleLock);
            t.scale = scale;
            if (hasKbm) t.keyboardMap = kbm;
        }
        return t;
    }

    static bool sameTuning (const tuning::Settings& a, const tuning::Settings& b)
    {
        return a.system == b.system && a.key == b.key && juce::exactlyEqual (a.a4Hz, b.a4Hz)
            && juce::exactlyEqual (a.stretchCentsPerOctave, b.stretchCentsPerOctave) && a.customCents == b.customCents && a.edoSteps == b.edoSteps;
    }

    static bool sameReedInputs (const Settings& a, const Settings& b)
    {
        return juce::exactlyEqual (a.tuningSpreadCents, b.tuningSpreadCents) && juce::exactlyEqual (a.ringTime, b.ringTime)
            && a.shortRingers == b.shortRingers;
    }

    void update (int numSamples)
    {
        live = read();
        // the tuning table: when a control or the scale changes (the scale is copied only if its lock is free)
        auto t = readTuning (false);
        const int sv = scaleVersion.load();
        if (! haveTuning || ! sameTuning (t, lastTuning) || sv != lastScaleVersion)
        {
            bool got = true;
            if (t.system == tuning::System::scala)
            {
                const juce::SpinLock::ScopedTryLockType sl (scaleLock);
                got = sl.isLocked();
                if (got) { t.scale = scale; if (hasKbm) t.keyboardMap = kbm; }
            }
            if (got)
            {
                tuned = tuning::frequencies (t);
                lastTuning = t;
                lastScaleVersion = sv;
                haveTuning = true;
                lastReedHours = -1.0;   // the reeds follow
            }
        }
        const double h = owner.hours.load();
        if (lastReedHours < 0.0 || ! sameReedInputs (live, lastReedSettings) || std::abs (h - lastReedHours) > 0.01)
        {
            reedTable = reeds (live, tuned, owner.reedSeed, h);
            lastReedSettings = live;
            lastReedHours = h;
        }
        // the speakers' voicing follows the model
        if (spkModel != live.model)
        {
            spkModel = live.model;
            spkHp.highPass (110.0, 1.0, sr);
            if (live.model == 0) { spkPeak.peak (2200.0, 1.2, 1.0, sr); spkLp.lowPass (5200.0, 0.7, sr); }   // 200: Alnico, smoother
            else                 { spkPeak.peak (2800.0, 1.4, 3.0, sr); spkLp.lowPass (7000.0, 0.8, sr); }   // 200A: ceramic, brighter
        }
        if (live.ages && activeVoices.load() > 0)
            owner.hours.store (h + numSamples / sr / 3600.0);
    }

    void amp (juce::AudioBuffer<float>& out, int n)
    {
        const auto& s = live;
        const double press = maxPressure;
        double vib = s.vibrato, drv = s.drive;
        if (s.mpe && s.pressureTarget == 2) vib = std::min (1.0, vib + press * s.pressureAmount);
        if (s.mpe && s.pressureTarget == 3) drv = std::min (1.0, drv + press * s.pressureAmount);
        const double g = 1.0 + 7.0 * drv;
        const double bias = s.model == 0 ? 0.2 : 0.12;     // the 200's preamp is the less clean
        const double norm = 1.0 / (std::pow (g, 0.6) * (1.0 - std::tanh (bias) * std::tanh (bias)));
        const double vol = std::pow (s.volume / 10.0, 2.0) * 1.6;
        const double vibInc = twoPi * 5.75 / sr;
        const double humHz = s.mains60 ? 60.0 : 50.0, humInc = twoPi * humHz / sr;
        const double hum = dbToGain (s.humDb + (s.model == 0 ? 6.0 : 0.0));
        const double hiss = dbToGain (s.model == 0 ? -84.0 : -92.0);
        const double pedalGain = dbToGain (s.pedalNoiseDb);
        auto* m = mono.getWritePointer (0);
        auto* l = out.getWritePointer (0);
        auto* r = out.getNumChannels() > 1 ? out.getWritePointer (1) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            double x = m[i];
            // the pedal: a felt thump through the case
            if (pedalNoiseEnv > 1.0e-5f)
            {
                pedalLp += (rng.nextFloat() * 2.0f - 1.0f - pedalLp) * 0.02f;
                x += pedalLp * pedalNoiseEnv * pedalGain * 6.0;
                pedalNoiseEnv *= 0.9985f;
            }
            // input coupling into the preamp
            const double hp = hpCoef * (hpState + x - hpPrevIn);
            hpPrevIn = x; hpState = hp;
            // preamp: a transistor stage, a little asymmetric
            double y = (std::tanh (g * hp + bias) - std::tanh (bias)) * norm;
            // vibrato: the preamp's feedback switched at 5.75 Hz, between a sine and a square
            vibPhase += vibInc; if (vibPhase > twoPi) vibPhase -= twoPi;
            const double sq = std::tanh (1.6 * std::sin (vibPhase)) / std::tanh (1.6);
            y *= 1.0 - vib * 0.45 * (1.0 + sq);
            y *= vol;
            if (! s.rearJack)
            {
                y = std::tanh (1.2 * y) / 1.2;          // the power amp, pushed
                y = spkLp.process (spkPeak.process (spkHp.process (y)));
            }
            humPhase += humInc; if (humPhase > twoPi) humPhase -= twoPi;
            y += hum * (std::sin (humPhase) + 0.5 * std::sin (2.0 * humPhase) + 0.3 * std::sin (3.0 * humPhase));
            y += hiss * (rng.nextFloat() * 2.0f - 1.0f);
            l[i] = (float) y;
            if (r != nullptr) r[i] = (float) y;
        }
    }
};

//==============================================================================
namespace
{
class Voice : public juce::MPESynthesiserVoice
{
public:
    explicit Voice (Processor::Impl& i) : impl (i) {}

    void noteStarted() override
    {
        const auto note = getCurrentlyPlayingNote();
        const int keyNo = std::clamp ((int) note.initialNote, 0, 127);
        const auto& reed = impl.reedTable[(size_t) keyNo];
        const auto& s = impl.live;
        sr = currentSampleRate;
        if (reed.hz <= 0.0) { clearCurrentNote(); return; }

        // the hammer: how hard it hits (the velocity, the curve, the blow), and whether it reaches at all
        double v = note.noteOnVelocity.asUnsignedFloat();
        v = std::pow (std::max (0.0, v), std::pow (2.0, -1.2 * s.velocityCurve));
        const double threshold = 0.01 + (s.letOffInches - 0.0625) * 0.5;
        if (v < threshold) { clearCurrentNote(); return; }
        const double speed = v * std::sqrt (s.blowInches / 1.078);

        double feltNow = std::clamp (s.felt + ageing (impl.owner.hoursPlayed()).felt, 0.0, 1.0);
        if (s.mpe && s.slideTarget == 1) feltNow = std::clamp (feltNow + (note.timbre.asUnsignedFloat() - 0.5) * 0.8, 0.0, 1.0);
        const double tc = 0.0016 * (1.0 - 0.75 * feltNow) * (1.0 - 0.35 * std::min (1.0, speed));

        baseHz = reed.hz;
        // the reed's bending modes, above the fundamental: inharmonic, set by ear (docs/electric-piano.md)
        const double reg = std::clamp ((keyNo - lowestKey) / 63.0, 0.0, 1.0);
        ratio[0] = 1.0;
        ratio[1] = 7.0 - 0.4 * reg;
        ratio[2] = 19.0 - 2.0 * reg;
        const double sp = s.strikePoint;
        const double shape[3] { 1.0, 0.55 + 0.35 * sp, 0.35 - 0.25 * sp };
        const double upper = s.model == 1 ? 1.15 : 1.0;   // the 200A's thicker middle and upper reeds keep their overtones longer
        tau[0] = reed.ringSeconds;
        tau[1] = std::max (0.01, reed.ringSeconds * 0.18 * upper);
        tau[2] = std::max (0.005, reed.ringSeconds * 0.06 * upper);
        const double gains[3] { 1.0, 1.8, 2.5 };
        for (int k = 0; k < 3; ++k)
        {
            const double f = baseHz * ratio[k];
            amp[k] = f < sr * 0.45 ? speed * shape[k] * pulse (f, tc) * (1.0 / ratio[k]) * gains[k] : 0.0;
            re[k] = amp[k]; im[k] = 0.0;
        }
        swingScale = swing (reed.hz);
        damped = false;
        setRates();
        // the action: a short knock of wood and felt as the hammer lands
        actionEnv = (float) (dbToGain (s.actionNoiseDb) * (0.4 + 0.6 * v));
        damperEnv = 0.0f;
        quiet = 0;
        impl.activeVoices.fetch_add (1);
        counted = true;
    }

    void noteStopped (bool allowTailOff) override
    {
        if (! allowTailOff) { finish(); return; }
        // the damper comes down on the reed
        damped = true;
        setRates();
        damperEnv = (float) dbToGain (impl.live.damperNoiseDb);
    }

    void notePitchbendChanged() override { setRates(); }
    void notePressureChanged() override {}
    void noteTimbreChanged() override {}
    void noteKeyStateChanged() override {}

    using juce::MPESynthesiserVoice::renderNextBlock;
    void renderNextBlock (juce::AudioBuffer<float>& out, int start, int num) override
    {
        if (! isActive()) return;
        const auto& s = impl.live;
        const auto note = getCurrentlyPlayingNote();
        const double press = s.mpe ? note.pressure.asUnsignedFloat() : 0.0;
        if (press > impl.maxPressure) impl.maxPressure = (float) press;
        double gain = 1.0;
        if (s.mpe && s.pressureTarget == 1) gain = 0.6 + 0.8 * press * s.pressureAmount;
        if (s.mpe && s.pressureTarget == 4 && press > 0.05) { pressDamp = 1.0 - press * s.pressureAmount * 0.0015; } else pressDamp = 1.0;
        double hNow = height (s);
        if (s.mpe && s.slideTarget == 2) hNow += (note.timbre.asUnsignedFloat() - 0.5) * 0.6;
        const double w = reach (s);
        const double c0 = capacitance (hNow, w);
        const double level = 0.32 * (s.volts / 165.0) / std::pow (s.gapMm / 0.5, 0.7) * gain;
        auto* o = out.getWritePointer (0);
        for (int i = 0; i < num; ++i)
        {
            double x = 0.0;
            for (int k = 0; k < 3; ++k)
            {
                const double nr = (re[k] * cs[k] - im[k] * sn[k]) * dec[k] * pressDamp;
                const double ni = (re[k] * sn[k] + im[k] * cs[k]) * dec[k] * pressDamp;
                re[k] = nr; im[k] = ni;
                x += ni;
            }
            double y = (capacitance (x * swingScale + hNow, w) - c0) * level;
            if (actionEnv > 1.0e-6f)
            {
                noise1 += ((impl.rng.nextFloat() * 2.0f - 1.0f) - noise1) * 0.35f;
                y += noise1 * actionEnv * 0.5;
                actionEnv *= 0.9965f;
            }
            if (damperEnv > 1.0e-6f)
            {
                noise2 += ((impl.rng.nextFloat() * 2.0f - 1.0f) - noise2) * 0.08f;
                y += noise2 * damperEnv * 1.5;
                damperEnv *= 0.9975f;
            }
            o[start + i] += (float) y;
        }
        // finished once the reed has stopped and the noises have died
        const double a = std::abs (re[0]) + std::abs (im[0]) + std::abs (re[1]) + std::abs (im[1]);
        if (a < 2.0e-5 && actionEnv < 1.0e-6f && damperEnv < 1.0e-6f && ++quiet > 2) finish();
    }

private:
    void setRates()
    {
        const auto note = getCurrentlyPlayingNote();
        const auto& s = impl.live;
        double bendRatio = 1.0;
        if (s.mpe && s.bend) bendRatio = std::pow (2.0, note.totalPitchbendInSemitones / 12.0);
        const double tauDamper = 0.02 + (1.0 - std::clamp (s.damping - ageing (impl.owner.hoursPlayed()).damping, 0.0, 1.0)) * 0.35;
        for (int k = 0; k < 3; ++k)
        {
            const double w = twoPi * baseHz * ratio[k] * bendRatio / sr;
            cs[k] = std::cos (w); sn[k] = std::sin (w);
            const double t = damped ? std::min (tau[k], tauDamper / (1.0 + k)) : tau[k];
            dec[k] = std::exp (-1.0 / (t * sr));
        }
    }

    void finish()
    {
        if (counted) { impl.activeVoices.fetch_sub (1); counted = false; }
        clearCurrentNote();
    }

    Processor::Impl& impl;
    double sr = 48000.0, baseHz = 440.0, swingScale = 1.0, pressDamp = 1.0;
    double ratio[3] {}, tau[3] {}, amp[3] {}, re[3] {}, im[3] {}, cs[3] {}, sn[3] {}, dec[3] {};
    bool damped = false, counted = false;
    float actionEnv = 0.0f, damperEnv = 0.0f, noise1 = 0.0f, noise2 = 0.0f;
    int quiet = 0;
};
}

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
    auto ch = [this] (const char* id, const char* name, const juce::StringArray& c, int def)
    {
        auto* p = new juce::AudioParameterChoice (juce::ParameterID { id, 1 }, name, c, def);
        addParameter (p);
        return p;
    };
    auto bo = [this] (const char* id, const char* name, bool def)
    {
        auto* p = new juce::AudioParameterBool (juce::ParameterID { id, 1 }, name, def);
        addParameter (p);
        return p;
    };
    auto in = [this] (const char* id, const char* name, int lo, int hi, int def)
    {
        auto* p = new juce::AudioParameterInt (juce::ParameterID { id, 1 }, name, lo, hi, def);
        addParameter (p);
        return p;
    };

    Settings d;
    applyCondition (d, 1);
    i.model = ch ("model", "Model", { "200", "200A" }, d.model);
    i.ages = bo ("ages", "Ages as you play", d.ages);
    i.system = ch ("system", "Tuning system", systemChoices, 0);
    i.key = ch ("key", "Key", keyChoices, 0);
    i.a4 = fl ("a4", "A4", 415.0f, 466.0f, 440.0f, "Hz");
    i.stretch = fl ("stretch", "Octave stretch", 0.0f, 10.0f, 0.0f, "cents");
    i.edo = in ("edo", "EDO steps", 5, 72, 19);
    for (int k = 0; k < 12; ++k)
    {
        const auto nm = "Cents " + keyChoices[k];
        auto* p = new juce::AudioParameterFloat (juce::ParameterID { "cents" + juce::String (k), 1 }, nm, juce::NormalisableRange<float> (-50.0f, 50.0f), 0.0f,
                                                 juce::AudioParameterFloatAttributes().withLabel ("cents"));
        addParameter (p);
        i.cents[(size_t) k] = p;
    }
    i.spread = fl ("spread", "Tuning spread", 0.0f, 30.0f, d.tuningSpreadCents, "cents");
    i.ring = fl ("ring", "Ring time", 0.5f, 2.0f, d.ringTime);
    i.ringers = in ("ringers", "Short ringers", 0, 12, d.shortRingers);
    i.felt = fl ("felt", "Hammer felt", 0.0f, 1.0f, d.felt);
    i.strike = fl ("strike", "Strike point", -1.0f, 1.0f, d.strikePoint);
    i.blow = fl ("blow", "Blow distance", 0.9f, 1.4f, d.blowInches, "in");
    i.letoff = fl ("letoff", "Let-off", 0.0625f, 0.25f, d.letOffInches, "in");
    i.curve = fl ("curve", "Velocity curve", -1.0f, 1.0f, d.velocityCurve);
    i.heightP = fl ("height", "Reed height", 0.0f, 1.0f, d.reedHeight);
    i.gap = fl ("gap", "Pickup gap", 0.3f, 1.2f, d.gapMm, "mm");
    i.volts = fl ("volts", "Polarising voltage", 120.0f, 170.0f, d.volts, "V");
    i.damping = fl ("damping", "Damping", 0.0f, 1.0f, d.damping);
    i.damperDb = fl ("damperNoise", "Damper noise", -90.0f, -30.0f, d.damperNoiseDb, "dB");
    i.actionDb = fl ("actionNoise", "Action noise", -90.0f, -30.0f, d.actionNoiseDb, "dB");
    i.pedalDb = fl ("pedalNoise", "Pedal noise", -90.0f, -30.0f, d.pedalNoiseDb, "dB");
    i.volume = fl ("volume", "Volume", 0.0f, 10.0f, d.volume);
    i.vibrato = fl ("vibrato", "Vibrato", 0.0f, 1.0f, d.vibrato);
    i.drive = fl ("drive", "Preamp drive", 0.0f, 1.0f, d.drive);
    i.output = ch ("output", "Output", { "SPEAKERS", "REAR JACK" }, d.rearJack ? 1 : 0);
    i.humDb = fl ("hum", "Mains hum", -100.0f, -40.0f, d.humDb, "dB");
    i.mains = ch ("mains", "Mains", { "50 HZ", "60 HZ" }, 0);
    i.mpe = bo ("mpe", "MPE", false);
    i.bend = bo ("bend", "Per-note bend", true);
    i.pressure = ch ("pressure", "Pressure", pressureChoices, 0);
    i.pressureAmt = fl ("pressureAmount", "Pressure amount", 0.0f, 1.0f, 0.6f);
    i.slide = ch ("slide", "Slide", slideChoices, 0);

    reedSeed = (uint32_t) juce::Random::getSystemRandom().nextInt();
    for (int v = 0; v < 15; ++v) i.synth.addVoice (new Voice (i));
    i.synth.enableLegacyMode (2);
    i.synth.setVoiceStealingEnabled (true);
    i.mono.setSize (1, 512);
}

Processor::~Processor() = default;

const juce::String Processor::getName() const { return "TF ELECTRIC PIANO"; }

Settings Processor::settings() const { return impl->read(); }

void Processor::setSettings (const Settings& s)
{
    auto& i = *impl;
    auto setF = [] (juce::AudioParameterFloat* p, float v) { p->setValueNotifyingHost (p->convertTo0to1 (v)); };
    auto setB = [] (juce::AudioParameterBool* p, bool v) { p->setValueNotifyingHost (v ? 1.0f : 0.0f); };
    auto setC = [] (juce::AudioParameterChoice* p, int v) { p->setValueNotifyingHost (p->convertTo0to1 ((float) v)); };
    auto setI = [] (juce::AudioParameterInt* p, int v) { p->setValueNotifyingHost (p->convertTo0to1 ((float) v)); };
    setC (i.model, s.model); setB (i.ages, s.ages);
    setF (i.spread, s.tuningSpreadCents); setF (i.ring, s.ringTime); setI (i.ringers, s.shortRingers);
    setF (i.felt, s.felt); setF (i.strike, s.strikePoint); setF (i.blow, s.blowInches); setF (i.letoff, s.letOffInches); setF (i.curve, s.velocityCurve);
    setF (i.heightP, s.reedHeight); setF (i.gap, s.gapMm); setF (i.volts, s.volts);
    setF (i.damping, s.damping); setF (i.damperDb, s.damperNoiseDb); setF (i.actionDb, s.actionNoiseDb); setF (i.pedalDb, s.pedalNoiseDb);
    setF (i.volume, s.volume); setF (i.vibrato, s.vibrato); setF (i.drive, s.drive); setF (i.humDb, s.humDb);
    setC (i.output, s.rearJack ? 1 : 0); setC (i.mains, s.mains60 ? 1 : 0);
    setB (i.mpe, s.mpe); setB (i.bend, s.bend); setC (i.pressure, s.pressureTarget); setF (i.pressureAmt, s.pressureAmount); setC (i.slide, s.slideTarget);
}

void Processor::chooseCondition (int c)
{
    auto s = settings();
    applyCondition (s, c);
    setSettings (s);
    condition = std::clamp (c, 0, 3);
    hours = 0.0;
}

tuning::Settings Processor::tuningSettings() const { return impl->readTuning (true); }

void Processor::setScale (const tuning::Scale& sc, const std::string& text)
{
    { const juce::SpinLock::ScopedLockType sl (impl->scaleLock); impl->scale = sc; impl->sclText = text; }
    ++impl->scaleVersion;
    updateHostDisplay();
}

void Processor::setKeyboardMap (const tuning::KeyboardMap& m, const std::string& text)
{
    { const juce::SpinLock::ScopedLockType sl (impl->scaleLock); impl->kbm = m; impl->hasKbm = true; impl->kbmText = text; }
    ++impl->scaleVersion;
    updateHostDisplay();
}

void Processor::clearKeyboardMap()
{
    { const juce::SpinLock::ScopedLockType sl (impl->scaleLock); impl->kbm = {}; impl->hasKbm = false; impl->kbmText.clear(); }
    ++impl->scaleVersion;
}

juce::String Processor::scaleDescription() const
{
    const juce::SpinLock::ScopedLockType sl (impl->scaleLock);
    if (! impl->scale.valid()) return {};
    return juce::String (impl->scale.description) + " (" + juce::String ((int) impl->scale.cents.size()) + " NOTES)";
}

bool Processor::hasKeyboardMap() const
{
    const juce::SpinLock::ScopedLockType sl (impl->scaleLock);
    return impl->hasKbm;
}

std::array<Reed, 128> Processor::currentReeds() const
{
    return reeds (settings(), tuning::frequencies (tuningSettings()), reedSeed, hours.load());
}

juce::RangedAudioParameter* Processor::param (const juce::String& id) const
{
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p); r != nullptr && r->paramID == id)
            return r;
    return nullptr;
}

void Processor::applyMpe (bool on, int bendRange) noexcept
{
    if (on == zone.load() && bendRange == zoneBend.load()) return;
    zone = on;
    zoneBend = bendRange;
    if (on)
    {
        juce::MPEZoneLayout layout;
        layout.setLowerZone (15, bendRange, 2);
        impl->synth.setZoneLayout (layout);
    }
    else
        impl->synth.enableLegacyMode (2);
}

void Processor::prepareToPlay (double sampleRate, int maxBlock)
{
    auto& i = *impl;
    i.sr = sampleRate;
    i.synth.setCurrentPlaybackSampleRate (sampleRate);
    i.mono.setSize (1, std::max (maxBlock, 512));
    i.hpCoef = std::exp (-twoPi * 15.0 / sampleRate);
    i.spkModel = -1;
    i.haveTuning = false;
    i.update (0);
}

void Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    auto& i = *impl;
    const int n = buffer.getNumSamples();
    if (i.mono.getNumSamples() < n) { buffer.clear(); return; }   // (never: blocks are no longer than promised)
    i.update (n);

    // the sustain pedal's own noise
    for (const auto m : midi)
    {
        const auto msg = m.getMessage();
        if (msg.isController() && msg.getControllerNumber() == 64)
        {
            const bool down = msg.getControllerValue() >= 64;
            if (down != i.pedalDown) { i.pedalDown = down; i.pedalNoiseEnv = 1.0f; }
        }
    }

    i.maxPressure = 0.0f;
    i.mono.clear (0, 0, n);
    juce::AudioBuffer<float> view (i.mono.getArrayOfWritePointers(), 1, n);
    i.synth.renderNextBlock (view, midi, 0, n);
    buffer.clear();
    i.amp (buffer, n);
    midi.clear();
}

std::unique_ptr<juce::XmlElement> Processor::toXml (bool withAge) const
{
    auto x = std::make_unique<juce::XmlElement> ("EPIANO");
    x->setAttribute ("version", 1);
    x->setAttribute ("seed", juce::String::toHexString ((juce::int64) reedSeed));
    if (withAge) x->setAttribute ("hours", hours.load());
    x->setAttribute ("condition", condition.load());
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
            x->setAttribute (r->paramID, (double) r->convertFrom0to1 (r->getValue()));
    const juce::SpinLock::ScopedLockType sl (impl->scaleLock);
    if (! impl->sclText.empty()) x->createNewChildElement ("SCL")->addTextElement (juce::String (impl->sclText));
    if (! impl->kbmText.empty()) x->createNewChildElement ("KBM")->addTextElement (juce::String (impl->kbmText));
    return x;
}

void Processor::fromXml (const juce::XmlElement& x)
{
    if (! x.hasTagName ("EPIANO")) return;
    if (x.hasAttribute ("seed")) reedSeed = (uint32_t) x.getStringAttribute ("seed").getHexValue64();
    hours = std::max (0.0, x.getDoubleAttribute ("hours", hours.load()));
    condition = std::clamp (x.getIntAttribute ("condition", 1), 0, 3);
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p); r != nullptr && x.hasAttribute (r->paramID))
            r->setValueNotifyingHost (r->convertTo0to1 ((float) x.getDoubleAttribute (r->paramID)));
    // the tuning files: what the piano had, or none
    tuning::Scale sc;
    tuning::KeyboardMap m;
    std::string err, sclText, kbmText;
    if (auto* e = x.getChildByName ("SCL")) { sclText = e->getAllSubText().toStdString(); if (! tuning::parseScala (sclText, sc, err)) sclText.clear(); }
    if (auto* e = x.getChildByName ("KBM")) { kbmText = e->getAllSubText().toStdString(); if (! tuning::parseKeyboardMap (kbmText, m, err)) kbmText.clear(); }
    {
        const juce::SpinLock::ScopedLockType sl (impl->scaleLock);
        impl->scale = sclText.empty() ? tuning::Scale() : sc;
        impl->sclText = sclText;
        impl->kbm = kbmText.empty() ? tuning::KeyboardMap() : m;
        impl->hasKbm = ! kbmText.empty();
        impl->kbmText = kbmText;
    }
    ++impl->scaleVersion;
}

juce::String Processor::fingerprint() const
{
    // the piano as it sounds, without its playing time (which changes as you play)
    return juce::String::toHexString (toXml (false)->toString (juce::XmlElement::TextFormat().singleLine()).hashCode64());
}

void Processor::setPreset (const juce::String& name)
{
    presetName = name;
    presetFingerprint = fingerprint();
}

bool Processor::isModified() const
{
    return presetName.isNotEmpty() && fingerprint() != presetFingerprint;
}

void Processor::getStateInformation (juce::MemoryBlock& mb)
{
    auto x = toXml (true);
    x->setAttribute ("preset", presetName);
    x->setAttribute ("presetFingerprint", presetFingerprint);
    copyXmlToBinary (*x, mb);
}

void Processor::setStateInformation (const void* data, int size)
{
    auto x = getXmlFromBinary (data, size);
    if (x == nullptr || ! x->hasTagName ("EPIANO")) return;
    fromXml (*x);
    presetName = x->getStringAttribute ("preset");
    presetFingerprint = x->getStringAttribute ("presetFingerprint");
}

//==============================================================================
namespace presets
{
juce::File folder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("TapeFour").getChildFile ("Presets").getChildFile ("Electric Piano");
}

juce::File fileFor (const juce::String& name)
{
    return folder().getChildFile (juce::File::createLegalFileName (name.trim()) + ".tfep");
}

std::vector<Entry> list()
{
    std::vector<Entry> out;
    for (auto& f : folder().findChildFiles (juce::File::findFiles, false, "*.tfep"))
    {
        auto x = juce::XmlDocument::parse (f);
        if (x == nullptr || ! x->hasTagName ("TFEP")) continue;
        auto* p = x->getChildByName ("EPIANO");
        if (p == nullptr) continue;
        Entry e;
        e.file = f;
        e.name = x->getStringAttribute ("name", f.getFileNameWithoutExtension());
        e.saved = f.getLastModificationTime();
        const int sys = p->getIntAttribute ("system");
        static const char* keys[] { "C", "C#", "D", "EB", "E", "F", "F#", "G", "AB", "A", "BB", "B" };
        juce::String tun = tuning::systemName ((tuning::System) std::clamp (sys, 0, (int) tuning::System::numSystems - 1));
        if (sys == (int) tuning::System::edo) tun = juce::String (p->getIntAttribute ("edo")) + "-EDO";
        else if (sys > 0 && sys < (int) tuning::System::custom) tun << " IN " << keys[std::clamp (p->getIntAttribute ("key"), 0, 11)];
        const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
        e.summary = juce::String (p->getIntAttribute ("model", 1) == 0 ? "200" : "200A") + dot + conditionName (p->getIntAttribute ("condition", 1))
                  + dot + tun + dot + (p->getIntAttribute ("output", 1) == 1 ? "REAR JACK" : "SPEAKERS");
        out.push_back (e);
    }
    std::sort (out.begin(), out.end(), [] (const Entry& a, const Entry& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return out;
}

bool save (Processor& piano, const juce::String& name, juce::String& error)
{
    if (name.trim().isEmpty()) { error = "GIVE IT A NAME"; return false; }
    const auto f = fileFor (name);
    if (! f.getParentDirectory().createDirectory()) { error = "CAN'T MAKE THE PRESETS FOLDER"; return false; }
    juce::XmlElement x ("TFEP");
    x.setAttribute ("version", 1);
    x.setAttribute ("name", name.trim());
    x.addChildElement (piano.toXml (true).release());
    if (! x.writeTo (f)) { error = "CAN'T WRITE " + f.getFileName().toUpperCase(); return false; }
    piano.setPreset (name.trim());
    return true;
}

bool load (Processor& piano, const juce::File& f, juce::String& error)
{
    auto x = juce::XmlDocument::parse (f);
    auto* p = x != nullptr && x->hasTagName ("TFEP") ? x->getChildByName ("EPIANO") : nullptr;
    if (p == nullptr) { error = "NOT AN ELECTRIC PIANO PRESET"; return false; }
    piano.fromXml (*p);
    piano.setPreset (x->getStringAttribute ("name", f.getFileNameWithoutExtension()));
    return true;
}

bool rename (Processor& piano, const juce::String& newName, juce::String& error)
{
    const auto old = fileFor (piano.currentPreset());
    const auto to = fileFor (newName);
    if (newName.trim().isEmpty()) { error = "GIVE IT A NAME"; return false; }
    if (to.existsAsFile() && to != old) { error = "THERE'S ALREADY ONE CALLED THAT"; return false; }
    // the file keeps what was saved in it: only its name changes
    auto x = juce::XmlDocument::parse (old);
    if (x == nullptr || ! x->hasTagName ("TFEP")) return save (piano, newName, error);
    x->setAttribute ("name", newName.trim());
    if (! x->writeTo (to)) { error = "CAN'T WRITE IT"; return false; }
    if (to != old) old.deleteFile();
    piano.renamePreset (newName.trim());
    return true;
}

bool remove (Processor& piano, juce::String& error)
{
    const auto f = fileFor (piano.currentPreset());
    if (! f.existsAsFile()) { error = "NOTHING TO DELETE"; return false; }
    if (! f.deleteFile()) { error = "CAN'T DELETE IT"; return false; }
    piano.setPreset ({});
    return true;
}
} // namespace presets
} // namespace tf::ep
