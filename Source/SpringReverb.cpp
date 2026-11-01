#include "SpringReverb.h"

namespace tf::spring
{
namespace
{
    constexpr double pi = 3.14159265358979323846, twoPi = 2.0 * pi;

    // Accutronics' published data (part number, springs, decay class, the drive coil's impedance at 1 kHz
    // and its DC resistance); each path's length relative to the fitted spring is inferred (see the docs).
    const TankInfo tankTable[numTanks] {
        { "4AB3C1B", "2 SPRING LONG", "TYPE 4, 17 IN \xc2\xb7 2 \xc3\x97 2 COUNTER-WOUND SPRINGS \xc2\xb7 LONG DECAY \xc2\xb7 FENDER",
          2, { 1.0, 0.86, 0.0 }, 2.0, "LONG (2.75-4 S)", 8.0, 0.81, 16.75 },
        { "9EB2C1B", "3 SPRING", "TYPE 9, 17 IN \xc2\xb7 3 \xc3\x97 2 COUNTER-WOUND SPRINGS \xc2\xb7 MEDIUM DECAY",
          3, { 1.0, 0.89, 0.79 }, 3.0, "MEDIUM (1.75-3 S)", 800.0, 58.0, 16.75 },
        { "8EB2C1B", "3 SPRING SHORT", "TYPE 8, 9 IN \xc2\xb7 3 SPRINGS \xc2\xb7 MEDIUM DECAY",
          3, { 0.55, 0.49, 0.44 }, 3.0, "MEDIUM (1.75-3 S)", 800.0, 58.0, 9.25 },
    };

    // The magnets, as van Walstijn shapes the modes with them (his eqs. 70-74 and Table 1): their main
    // resonance, very low, makes a low-pass; another often lies above the transition; and they slow the
    // lowest frequencies down.
    double magnetLowPass (double hz) { constexpr double fco = 100.0, p = 1.8; return std::pow (fco, p) / (std::pow (fco, p) + std::pow (hz, p)); }
    double magnetPeak (double hz)
    {
        constexpr double hc = 16.0, fc = 6300.0, fb = 300.0;
        return 1.0 + (hc - 1.0) * fb * fb / (fb * fb + (hz - fc) * (hz - fc));
    }
    double magnetSlowing (double hz) { constexpr double r0 = 1.2, fd = 600.0, u = 3.0; return 1.0 + (r0 - 1.0) * std::pow (fd / (hz + fd), u); }

    /** The 12AT7 driver's plate current against its grid (bias units: cutoff at -2.5, grid current
        from 0, biased at -1), the curve's shape a 3/2 power law (set by ear: no 12AT7 data fitted). */
    float triode (float v) noexcept
    {
        v -= 1.0f;
        if (v > 0.0f) v = 0.35f * std::tanh (v / 0.35f);   // past 0 the grid draws current and stops following
        const float u = std::max (0.0f, (v + 2.5f) / 2.5f);
        return u * std::sqrt (u);
    }
    const float triodeRest = triode (0.0f);
    const float triodeSlope = 1.5f * std::sqrt (1.5f / 2.5f) / 2.5f;

    void lowPass (Biquad& b, double f, double q, double sr)
    {
        const double w = twoPi * std::min (f, sr * 0.45) / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q);
        b.set ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void highPass (Biquad& b, double f, double q, double sr)
    {
        const double w = twoPi * std::min (f, sr * 0.45) / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q);
        b.set ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
}

//==============================================================================
Physics Physics::lengthened (double s) const
{
    // In van Walstijn's scaling, q is L/R, kappa goes as 1/L^2 and gamma as 1/L.
    Physics p = *this;
    p.q *= s;
    p.kappa /= s * s;
    p.gamma /= s;
    return p;
}

double Physics::lowBranch (double beta) const
{
    const double d = beta * beta - q * q;
    const double b = kappa * kappa * d * d + gamma * gamma * (beta * beta + q * q);
    const double c = gamma * gamma * beta * beta * kappa * kappa * d * d;
    return std::sqrt (std::max (0.0, (b - std::sqrt (std::max (0.0, b * b - 4.0 * c))) / 2.0));
}

double Physics::transitionHz() const
{
    double best = 0.0;
    for (int n = 1; n * pi < q; ++n) best = std::max (best, lowBranch (n * pi));
    return best / twoPi;
}

double Physics::roundTripSeconds() const
{
    // The low frequencies are mostly carried by waves about a coil turn long (wavenumber near q), where the
    // lower branch dips to zero; their group velocity there sets the echoes' spacing. (The magnets slow the
    // very lowest further: by up to 1.2 times, below about 600 Hz.)
    const double v = (lowBranch (q + 2.0 * pi) - lowBranch (q + pi)) / pi;
    return 2.0 / std::max (1.0e-9, v);
}

double Physics::t60 (double hz) const
{
    const double om = twoPi * hz;
    return 6.9078 / (sigma + 0.5 * phi * om * om);
}

const TankInfo& tankInfo (Tank t) { return tankTable[std::clamp ((int) t, 0, numTanks - 1)]; }

Physics tankPhysics (Tank t, int path)
{
    const auto& info = tankInfo (t);
    Physics p = Physics().lengthened (info.lengths[std::clamp (path, 0, 2)]);
    p.sigma = info.sigma;
    return p;
}

//==============================================================================
void ModalPath::build (const Physics& p, double sr, double maxHz)
{
    // The modes of a spring fixed at both ends, taken as sin(n pi x) (the pinned-end approximation: van
    // Walstijn computes them numerically for clamped ends), at the lower branch of the dispersion relation.
    std::vector<float> A1, A2, Gin, Gout;
    maxHz = std::min (maxHz, sr * 0.45);
    for (int n = 1; n < 100000; ++n)
    {
        const double beta = n * pi;
        const double w0 = p.lowBranch (beta);
        if (beta > p.q && w0 / twoPi > maxHz) break;
        const double hz = w0 / twoPi / magnetSlowing (w0 / twoPi);
        const double w = twoPi * hz;
        if (hz > maxHz || hz < 20.0) continue;
        // the mode's shape: transverse U sin, longitudinal V cos (from the two equations without loss)
        const double U = 1.0;
        const double V = -p.gamma * p.gamma * beta * U / (p.gamma * p.gamma * beta * beta - w0 * w0);
        const double mass = 0.5 * (U * U + p.q * p.q * V * V);   // the system's own weighting: q^2 on the longitudinal part
        // how much the transducer (a raised cosine over w at the end) moves this mode, and hears it at the other end
        double reach = 0.0;
        constexpr int steps = 48;
        for (int i = 0; i < steps; ++i)
        {
            const double x = (i + 0.5) * p.w / steps;
            reach += (1.0 / p.w) * (1.0 + std::cos (pi * x / p.w)) * std::sin (beta * x) * (p.w / steps);
        }
        const double alpha = p.sigma + 0.5 * p.phi * w * w;   // Kelvin-Voigt: the decay rises with frequency
        const double r = std::exp (-alpha / sr);
        A1.push_back ((float) (2.0 * r * std::cos (w / sr)));
        A2.push_back ((float) (r * r));
        Gin.push_back ((float) (p.q * U * reach / mass / (sr * sr)));
        const double sign = (n % 2 == 1) ? 1.0 : -1.0;   // sin(n pi (1 - x)) = (-1)^(n+1) sin(n pi x)
        // what the spring pushes on the pickup end with (the restoring force: the mode's stiffness, w^2), through the magnets
        Gout.push_back ((float) (sign * U * reach * w * w * magnetLowPass (hz) * magnetPeak (hz)));
    }
    count = (int) A1.size();
    padded = (count + 7) / 8 * 8;
    auto pad = [this] (std::vector<float>& v) { v.resize ((size_t) padded, 0.0f); };
    pad (A1); pad (A2); pad (Gin); pad (Gout);
    a1 = std::move (A1); a2 = std::move (A2); gin = std::move (Gin); gout = std::move (Gout);
    y1.assign ((size_t) padded, 0.0f);
    y2.assign ((size_t) padded, 0.0f);
}

double ModalPath::clickEnergy() const
{
    double e = 0.0;
    for (int k = 0; k < count; ++k)
    {
        const double r = std::sqrt ((double) a2[(size_t) k]);
        const double c = std::clamp ((double) a1[(size_t) k] / (2.0 * r), -1.0, 1.0);   // cos w
        const double sinSq = std::max (1.0e-12, 1.0 - c * c);
        const double g = (double) gin[(size_t) k] * gout[(size_t) k];
        e += g * g / (sinSq * 2.0 * std::max (1.0e-12, 1.0 - r * r));
    }
    return e;
}

void ModalPath::reset()
{
    std::fill (y1.begin(), y1.end(), 0.0f);
    std::fill (y2.begin(), y2.end(), 0.0f);
}

float ModalPath::process (float force) noexcept
{
    float acc[8] {};
    float* __restrict Y1 = y1.data();
    float* __restrict Y2 = y2.data();
    const float* __restrict A1 = a1.data();
    const float* __restrict A2 = a2.data();
    const float* __restrict GI = gin.data();
    const float* __restrict GO = gout.data();
    for (int i = 0; i < padded; i += 8)
        for (int j = 0; j < 8; ++j)
        {
            const int k = i + j;
            const float y = A1[k] * Y1[k] - A2[k] * Y2[k] + GI[k] * force;
            acc[j] += GO[k] * y;
            Y2[k] = Y1[k];
            Y1[k] = y;
        }
    return ((acc[0] + acc[1]) + (acc[2] + acc[3])) + ((acc[4] + acc[5]) + (acc[6] + acc[7]));
}

//==============================================================================
void WaveguidePath::build (const Physics& p, double sr)
{
    // Valimaki, Parker & Abel's structure; the values the Leem spring was fitted with (Gamper et al.,
    // DAFx-11, Table 1: a 0.62, 100 sections; high chirps a -0.6, 200 sections), the delay and transition
    // from the same spring as the modal model, so the two match.
    const double fc = p.transitionHz(), td = p.roundTripSeconds(), t60 = p.t60 (500.0);
    const double K = sr / (2.0 * fc);
    k1 = std::max (1, (int) std::lround (K) - 1);
    const double d = K - k1;
    aFd = (float) ((1.0 - d) / (1.0 + d));
    lines.assign ((size_t) (lowSections * k1), 0.0f);
    fdState.assign ((size_t) lowSections, 0.0f);
    highState.assign ((size_t) highSections, 0.0f);
    const double gdLow = lowSections * K * (1.0 - aLow) / (1.0 + aLow);    // the cascades' delay at low frequencies
    const double gdHigh = highSections * (1.0 - aHigh) / (1.0 + aHigh) * 0.25;   // and the high one's, well above fc (set by ear)
    lowDelay = std::max (1, (int) std::lround (td * sr - gdLow));
    highDelay = std::max (1, (int) std::lround (td * sr - gdHigh));
    tap = std::max (1, (int) std::lround (0.5 * td * sr - gdLow));
    highTap = std::max (1, (int) std::lround (0.5 * td * sr - gdHigh));
    lowLoop.assign ((size_t) lowDelay + 2, 0.0f);
    highLoop.assign ((size_t) highDelay + 2, 0.0f);
    lowPos = highPos = 0;
    gLow = (float) -std::pow (10.0, -3.0 * td / t60);                 // a round trip's loss; the reflection inverts
    gHigh = (float) -std::pow (10.0, -3.0 * td / (t60 / 1.3));         // the high chirps die 1.3 times as fast (Gamper et al.)
    dcA = (float) std::exp (-twoPi * 40.0 / sr);
    lowPass (lowOut1, fc, 0.54, sr); lowPass (lowOut2, fc, 1.31, sr);
    highPass (highOut1, fc, 0.54, sr); highPass (highOut2, fc, 1.31, sr);
    reset();
}

void WaveguidePath::reset()
{
    std::fill (lines.begin(), lines.end(), 0.0f);
    std::fill (fdState.begin(), fdState.end(), 0.0f);
    linePos = 0;
    std::fill (highState.begin(), highState.end(), 0.0f);
    std::fill (lowLoop.begin(), lowLoop.end(), 0.0f);
    std::fill (highLoop.begin(), highLoop.end(), 0.0f);
    dcX = dcY = 0.0f;
    lowOut1.reset(); lowOut2.reset(); highOut1.reset(); highOut2.reset();
}

float WaveguidePath::process (float x) noexcept
{
    const float y0 = x - dcX + dcA * dcY;   // DC blocker (40 Hz)
    dcX = x; dcY = y0;
    auto at = [] (const std::vector<float>& line, int pos, int back)
    {
        int i = pos - back;
        if (i < 0) i += (int) line.size();
        return line[(size_t) i];
    };
    // low chirps: the loop through the dispersive cascade
    float v = y0 + gLow * at (lowLoop, lowPos, lowDelay);
    {
        // each section: D(z) = Afd(z) z^-K1 on its line; v' = v - a D, out = a v' + D
        float* line = lines.data() + linePos;
        float* fd = fdState.data();
        for (int k = 0; k < lowSections; ++k, line += k1)
        {
            const float delayed = *line;
            const float d = aFd * delayed + fd[k];
            fd[k] = delayed - aFd * d;
            const float vin = v - aLow * d;
            *line = vin;
            v = aLow * vin + d;
        }
        linePos = (linePos + 1) % k1;
    }
    lowLoop[(size_t) lowPos] = v;
    const float lowOut = at (lowLoop, lowPos, tap);
    lowPos = (lowPos + 1) % (int) lowLoop.size();
    // high chirps: first-order allpasses
    float h = y0 + gHigh * at (highLoop, highPos, highDelay);
    for (auto& z : highState)
    {
        const float o = aHigh * h + z;
        z = h - aHigh * o;
        h = o;
    }
    highLoop[(size_t) highPos] = h;
    const float highOut = at (highLoop, highPos, highTap);
    highPos = (highPos + 1) % (int) highLoop.size();
    return lowOut2.process (lowOut1.process (lowOut)) + highMix * highOut2.process (highOut1.process (highOut));
}

//==============================================================================
void Circuit::prepare (double sampleRate, const TankInfo& t)
{
    sr = sampleRate;
    juce::ignoreUnused (t);   // (the coils' own response is in the magnets' low-pass, fitted to a measured tank)
    xfA = (float) std::exp (-twoPi * 30.0 / sr);   // the reverb transformer's low end (set by ear)
    reset();
}

void Circuit::reset() { xf = xfIn = toneY = dcIn = dcOut = 0.0f; }

float Circuit::drive (float x, float dwell) noexcept
{
    const float grid = x * juce::Decibels::decibelsToGain (-24.0f + 4.2f * dwell);
    const float plate = (triode (grid) - triodeRest) / triodeSlope;
    // the reverb transformer, its low end high-passed
    xf = xfA * (xf + plate - xfIn);
    xfIn = plate;
    return xf;
}

void Circuit::setTone (float tone, double sampleRate)
{
    const double hz = 1500.0 * std::pow (8.0, tone / 10.0);   // 1.5 to 12 kHz across the knob (set by ear)
    toneA = (float) std::exp (-twoPi * std::min (hz, sampleRate * 0.45) / sampleRate);
}

float Circuit::recover (float x) noexcept
{
    // the 12AX7 recovery stage: plenty of gain, a gentle limit when the tank's output is big
    float y = 1.5f * std::tanh (x / 1.5f);
    toneY = toneA * toneY + (1.0f - toneA) * y;
    const float out = toneY - dcIn + 0.9995f * dcOut;
    dcIn = toneY; dcOut = out;
    return out;
}

//==============================================================================
namespace
{
    void buildTank (Tank t, double sr, std::vector<ModalPath>& modal, std::vector<WaveguidePath>& wg, float& modalGain, float& wgGain)
    {
        const auto& info = tankInfo (t);
        modal.assign ((size_t) info.paths, {});
        wg.assign ((size_t) info.paths, {});
        double modalEnergy = 0.0;
        for (int i = 0; i < info.paths; ++i)
        {
            const auto p = tankPhysics (t, i);
            modal[(size_t) i].build (p, sr, 10000.0);
            wg[(size_t) i].build (p, sr);
        }
        // Each model's level: the energy of its response to a click, so the two (and the tanks) come out alike.
        auto energy = [sr] (auto& paths)
        {
            double e = 0.0;
            const int n = (int) (0.3 * sr);
            for (int k = 0; k < n; ++k)
            {
                float s = 0.0f;
                for (auto& p : paths) s += p.process (k == 0 ? 1.0f : 0.0f);
                e += (double) s * s;
            }
            for (auto& p : paths) p.reset();
            return e;
        };
        for (auto& p : modal) modalEnergy += p.clickEnergy();
        const double wgEnergy = energy (wg);
        modalGain = (float) (1.0 / std::sqrt (std::max (1.0e-30, modalEnergy)));
        wgGain = (float) (1.0 / std::sqrt (std::max (1.0e-30, wgEnergy)));
    }

    // the wet level: about the dry level at DWELL 5, each model alike (set by ear, then measured: SpringReverbTest)
    constexpr float outputGain = 1.5f, waveguideTrim = 1.0f;
}

void Processor::Tanks::build (double sampleRate)
{
    for (int t = 0; t < numTanks; ++t)
        buildTank ((Tank) t, sampleRate, modal[(size_t) t], waveguide[(size_t) t], modalGain[(size_t) t], waveguideGain[(size_t) t]);
}

Processor::Processor()
    : juce::AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
{
    tankP = new juce::AudioParameterChoice (juce::ParameterID { "tank", 1 }, "Tank", { "2 spring long", "3 spring", "3 spring short" }, 0);
    addParameter (tankP);
    modelP = new juce::AudioParameterChoice (juce::ParameterID { "model", 1 }, "Model", { "Simulated springs", "Waveguide" }, 0);
    addParameter (modelP);
    dwellP = new juce::AudioParameterFloat (juce::ParameterID { "dwell", 1 }, "Dwell", 0.0f, 10.0f, 5.0f);
    addParameter (dwellP);
    toneP = new juce::AudioParameterFloat (juce::ParameterID { "tone", 1 }, "Tone", 0.0f, 10.0f, 5.0f);
    addParameter (toneP);
}

Processor::~Processor() = default;

const juce::String Processor::getName() const { return "TF SPRING REVERB"; }

juce::RangedAudioParameter* Processor::param (const juce::String& id) const
{
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p); r != nullptr && r->paramID == id)
            return r;
    return nullptr;
}

Tank Processor::tank() const { return (Tank) tankP->getIndex(); }
Model Processor::model() const { return (Model) modelP->getIndex(); }

int Processor::modesInUse() const
{
    if (tanks == nullptr || model() != Model::simulated) return 0;
    int n = 0;
    for (auto& p : tanks->modal[(size_t) tank()]) n += p.modes();
    return n;
}

void Processor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    auto t = std::make_unique<Tanks>();
    t->build (sampleRate);
    tanks = std::move (t);
    lastTank = lastModel = -1;
    lastTone = -1.0f;
}

void Processor::processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    if (tanks == nullptr) { b.clear(); return; }
    const int ti = tankP->getIndex(), mi = modelP->getIndex();
    if (ti != lastTank || mi != lastModel)
    {
        // a different tank: it starts from rest
        circuit.prepare (sr, tankInfo ((Tank) ti));
        for (auto& p : tanks->modal[(size_t) ti]) p.reset();
        for (auto& p : tanks->waveguide[(size_t) ti]) p.reset();
        lastTank = ti; lastModel = mi;
        lastTone = -1.0f;
    }
    if (! juce::exactlyEqual (toneP->get(), lastTone)) { lastTone = toneP->get(); circuit.setTone (lastTone, sr); }
    const float dwell = dwellP->get();
    auto* l = b.getWritePointer (0);
    auto* r = b.getNumChannels() > 1 ? b.getWritePointer (1) : nullptr;
    auto& modal = tanks->modal[(size_t) ti];
    auto& wg = tanks->waveguide[(size_t) ti];
    const float gain = (mi == 0 ? tanks->modalGain[(size_t) ti] : tanks->waveguideGain[(size_t) ti]);
    for (int i = 0; i < n; ++i)
    {
        const float in = r != nullptr ? 0.5f * (l[i] + r[i]) : l[i];
        const float force = circuit.drive (in, dwell);
        float s = 0.0f;
        if (mi == 0) for (auto& p : modal) s += p.process (force);
        else         for (auto& p : wg) s += p.process (force);
        const float y = outputGain * (mi == 0 ? 1.0f : waveguideTrim) * circuit.recover (s * gain);
        l[i] = y;
        if (r != nullptr) r[i] = y;
    }
}

std::vector<float> Processor::click (Tank t, Model m, double sampleRate, double seconds, float dwell)
{
    std::vector<ModalPath> modal;
    std::vector<WaveguidePath> wg;
    float mg = 1.0f, wgg = 1.0f;
    buildTank (t, sampleRate, modal, wg, mg, wgg);
    Circuit c;
    c.prepare (sampleRate, tankInfo (t));
    c.setTone (10.0f, sampleRate);
    std::vector<float> out ((size_t) (seconds * sampleRate));
    for (size_t k = 0; k < out.size(); ++k)
    {
        const float force = c.drive (k == 0 ? 0.25f : 0.0f, dwell);
        float s = 0.0f;
        if (m == Model::simulated) for (auto& p : modal) s += p.process (force);
        else                       for (auto& p : wg) s += p.process (force);
        out[k] = outputGain * (m == Model::simulated ? 1.0f : waveguideTrim) * c.recover (s * (m == Model::simulated ? mg : wgg));
    }
    return out;
}

void Processor::getStateInformation (juce::MemoryBlock& mb)
{
    juce::XmlElement x ("SPRING");
    x.setAttribute ("version", 2);
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            x.setAttribute (rp->paramID, (double) rp->convertFrom0to1 (rp->getValue()));
    copyXmlToBinary (x, mb);
}

void Processor::setStateInformation (const void* data, int size)
{
    auto x = getXmlFromBinary (data, size);
    if (x == nullptr) return;
    // (an earlier TF SPRING REVERB kept DECAY, TONE and DRIP, a different design: its settings don't carry over)
    if (! x->hasTagName ("SPRING")) return;
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p); rp != nullptr && x->hasAttribute (rp->paramID))
            rp->setValueNotifyingHost (rp->convertTo0to1 ((float) x->getDoubleAttribute (rp->paramID)));
}
} // namespace tf::spring
