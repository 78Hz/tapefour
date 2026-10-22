// Drives the whole audio path (inputs -> strips -> tape -> mix) with a fake audio device.
#include "../Source/AudioEngine.h"
#include <cstdio>

using namespace tf;

namespace
{
constexpr double sr = 48000.0;
constexpr int bs = 256;
int failures = 0;

void check (bool ok, const char* what, double got, double want)
{
    std::printf ("%s  %-60s got %10.4f  want %10.4f\n", ok ? "PASS" : "FAIL", what, got, want);
    if (! ok) ++failures;
}

class FakeDevice : public juce::AudioIODevice
{
public:
    FakeDevice() : juce::AudioIODevice ("Fake", "Fake") {}
    juce::StringArray getOutputChannelNames() override { return { "Out 1", "Out 2" }; }
    juce::StringArray getInputChannelNames() override  { return { "In 1", "In 2" }; }
    juce::Array<double> getAvailableSampleRates() override { return { sr }; }
    juce::Array<int> getAvailableBufferSizes() override { return { bs }; }
    int getDefaultBufferSize() override { return bs; }
    juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start (juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return true; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return bs; }
    double getCurrentSampleRate() override { return sr; }
    int getCurrentBitDepth() override { return 24; }
    juce::BigInteger getActiveOutputChannels() const override { juce::BigInteger b; b.setRange (0, 2, true); return b; }
    juce::BigInteger getActiveInputChannels() const override  { juce::BigInteger b; b.setRange (0, 2, true); return b; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }
};

struct Rig
{
    juce::File propsFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tf_mixer_test.settings");
    std::unique_ptr<juce::PropertiesFile> props;
    std::unique_ptr<AudioEngine> engine;
    FakeDevice device;
    double ph[2] {};
    double f[2] { 0, 0 };
    float amp[2] { 0, 0 };

    Rig()
    {
        propsFile.deleteFile();
        props = std::make_unique<juce::PropertiesFile> (propsFile, juce::PropertiesFile::Options());
        engine = std::make_unique<AudioEngine> (*props);
        engine->deviceManager.removeAudioCallback (engine.get());   // we drive it ourselves
        engine->swapTape (std::make_unique<Tape> (1));
        engine->audioDeviceAboutToStart (&device);
        auto& p = engine->tapeEngine.params;
        p.hiss = 0; p.wow = 0; p.flutter = 0; p.sat = 0; p.hf = 0; p.xtalk = 0; p.drop = 0; p.drift = 0; p.nrOn = false;
        for (auto& t : engine->track) { t.fader = 0.75f; t.gain = 1.0f / 3.0f; t.pan = 0; t.armed = false; }
    }

    ~Rig() { engine.reset(); props.reset(); propsFile.deleteFile(); }

    std::vector<float> run (double seconds)
    {
        std::vector<float> in0 ((size_t) bs), in1 ((size_t) bs), out0 ((size_t) bs), out1 ((size_t) bs), captured;
        const float* ins[2] { in0.data(), in1.data() };
        float* outs[2] { out0.data(), out1.data() };
        for (int b = 0; b < (int) (seconds * sr / bs); ++b)
        {
            for (int i = 0; i < bs; ++i)
                for (int c = 0; c < 2; ++c)
                {
                    (c == 0 ? in0 : in1)[(size_t) i] = amp[c] * (float) std::sin (ph[c]);
                    ph[c] += juce::MathConstants<double>::twoPi * f[c] / sr;
                }
            engine->audioDeviceIOCallbackWithContext (ins, 2, outs, 2, bs, {});
            captured.insert (captured.end(), out0.begin(), out0.end());
        }
        return captured;
    }

    void cmd (TapeEngine::Command c) { engine->tapeEngine.command (c); }
};

// amplitude of one frequency in [from, to) seconds
double tone (const std::vector<float>& v, double hz, double from, double to)
{
    auto a = (size_t) (from * sr), b = std::min (v.size(), (size_t) (to * sr));
    double re = 0, im = 0;
    for (auto i = a; i < b; ++i)
    {
        auto w = juce::MathConstants<double>::twoPi * hz * (double) i / sr;
        re += v[i] * std::cos (w); im += v[i] * std::sin (w);
    }
    return 2.0 * std::sqrt (re * re + im * im) / (double) (b - a);
}

double rms (const std::vector<float>& v, double from, double to)
{
    auto a = (size_t) (from * sr), b = std::min (v.size(), (size_t) (to * sr));
    double s = 0; for (auto i = a; i < b; ++i) s += (double) v[i] * v[i];
    return std::sqrt (s / std::max<double> (1, (double) (b - a)));
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const double pan = std::cos (juce::MathConstants<double>::pi / 4.0);

    Rig r;
    auto& e = *r.engine;
    e.setAudioSource (0, 0);
    e.setAudioSource (1, 1);
    e.setInstrument (3, BuiltIn::create (BuiltIn::poly));
    r.f[0] = 500;  r.amp[0] = 0.3f;
    r.f[1] = 1500; r.amp[1] = 0.3f;

    // Monitoring: an armed track hears its input while stopped; an unarmed one doesn't.
    e.track[0].armed = true;
    auto mon = r.run (0.5);
    check (std::abs (tone (mon, 500, 0.1, 0.5) - 0.3 * pan) < 0.01, "armed track monitors its input while stopped", tone (mon, 500, 0.1, 0.5), 0.3 * pan);
    check (tone (mon, 1500, 0.1, 0.5) < 0.001, "unarmed track is silent while stopped", tone (mon, 1500, 0.1, 0.5), 0.0);

    // Record tracks 1 and 2 together.
    e.track[1].armed = true;
    r.cmd (TapeEngine::Command::rec);
    r.run (2.0);
    r.cmd (TapeEngine::Command::stop);
    r.run (0.05);
    check (e.getTape()->isRecorded (0, 10) && e.getTape()->isRecorded (1, 10), "tracks 1+2 recorded", e.getTape()->tracksUsed(), 2);

    // Return to zero.
    r.cmd (TapeEngine::Command::rtz);
    r.run (1.5);
    check (e.tapeEngine.getMode() == Mode::stop && e.getTape()->position.load() < 1.0, "RTZ rewinds to 000 and stops", e.getTape()->position.load(), 0.0);

    // Bounce 1+2 onto 3: silence the inputs so only the tape is heard.
    r.amp[0] = r.amp[1] = 0;
    e.track[0].armed = false;
    e.track[1].armed = false;
    e.toggleBounceSource (2, 0);
    e.toggleBounceSource (2, 1);
    e.track[2].armed = true;
    r.cmd (TapeEngine::Command::rec);
    auto during = r.run (2.0);
    r.cmd (TapeEngine::Command::stop);
    r.run (0.05);
    check (std::abs (tone (during, 500, 0.3, 1.8) - 0.3 * pan) < 0.01, "while bouncing you hear the sources once (not doubled)", tone (during, 500, 0.3, 1.8), 0.3 * pan);

    r.cmd (TapeEngine::Command::rtz);
    r.run (1.5);
    e.track[2].armed = false;
    e.track[0].fader = 0.0f;   // pull the originals down: what's left is the bounced track
    e.track[1].fader = 0.0f;
    r.cmd (TapeEngine::Command::play);
    auto bounced = r.run (2.0);
    r.cmd (TapeEngine::Command::stop);
    r.run (0.05);
    check (std::abs (tone (bounced, 500, 0.3, 1.8) - 0.3 * pan) < 0.015, "bounced track 3 holds track 1 (500 Hz)", tone (bounced, 500, 0.3, 1.8), 0.3 * pan);
    check (std::abs (tone (bounced, 1500, 0.3, 1.8) - 0.3 * pan) < 0.015, "bounced track 3 holds track 2 (1500 Hz)", tone (bounced, 1500, 0.3, 1.8), 0.3 * pan);

    // Fader and pan act on the mix.
    e.track[2].pan = -1.0f;
    r.cmd (TapeEngine::Command::rtz);
    r.run (1.5);
    r.cmd (TapeEngine::Command::play);
    auto hardLeft = r.run (1.0);
    r.cmd (TapeEngine::Command::stop);
    r.run (0.05);
    check (std::abs (tone (hardLeft, 500, 0.3, 0.9) - 0.3) < 0.015, "pan hard left puts the full level in the left output", tone (hardLeft, 500, 0.3, 0.9), 0.3);

    // Play the built-in synth on track 4 from MIDI and record it.
    r.cmd (TapeEngine::Command::rtz);
    r.run (1.5);
    e.track[3].armed = true;
    r.cmd (TapeEngine::Command::rec);
    r.run (0.2);
    e.injectMidi (juce::MidiMessage::noteOn (1, 57, (juce::uint8) 110));
    r.run (1.0);
    e.injectMidi (juce::MidiMessage::noteOff (1, 57));
    r.run (0.8);
    r.cmd (TapeEngine::Command::stop);
    r.run (0.05);
    check (e.getTape()->isRecorded (3, 5), "track 4 recorded from the instrument", 1, 1);

    e.track[3].armed = false;
    e.track[2].fader = 0.0f;
    r.cmd (TapeEngine::Command::rtz);
    r.run (2.5);
    r.cmd (TapeEngine::Command::play);
    auto synth = r.run (2.0);
    check (rms (synth, 0.5, 1.1) > 0.01, "synth note is on tape and plays back", rms (synth, 0.5, 1.1), 0.01);
    check (rms (synth, 0.0, 0.15) < 0.002, "silence before the note", rms (synth, 0.0, 0.15), 0.0);

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "SOME FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
