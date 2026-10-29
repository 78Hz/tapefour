// Drives the whole audio path (inputs -> strips -> tape -> mix) with a fake audio device.
#include <cstdlib>
#include "../Source/AudioEngine.h"
#include "../Source/ElectricPiano.h"
#include "../Source/TransistorOrgan.h"
#include "../Source/TapeWriter.h"
#include "../Source/ClapFormat.h"
#include "../Source/TapeShelf.h"
#include "../Source/Exporter.h"
#include <cstdio>
#include <thread>

using namespace tf;

namespace
{
constexpr double sr = 48000.0;
constexpr int bs = 256;
int failures = 0;
int fakeInputLatency = 0, fakeOutputLatency = 0;   // what the fake interface reports

void check (bool ok, const char* what, double got, double want)
{
    std::printf ("%s  %-60s got %10.4f  want %10.4f\n", ok ? "PASS" : "FAIL", what, got, want);
    if (! ok) ++failures;
    // On GitHub, a failure also shows on the run's page.
    if (! ok && std::getenv ("GITHUB_ACTIONS") != nullptr)
        std::printf ("::error::MixerTest: %s (got %.4f, want %.4f)\n", what, got, want);
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
    int getOutputLatencyInSamples() override { return fakeOutputLatency; }
    int getInputLatencyInSamples() override { return fakeInputLatency; }
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
        propsFile.getSiblingFile ("TF-4.deck").deleteFile();
        props = std::make_unique<juce::PropertiesFile> (propsFile, juce::PropertiesFile::Options());
        engine = std::make_unique<AudioEngine> (*props);
        engine->deviceManager.removeAudioCallback (engine.get());   // we drive it ourselves
        auto blank = std::make_unique<Tape> (1, defaultTapeModel, tfmt::Condition());          // a perfect cassette...
        blank->position = 0.0;                                                                  // (wound past its leader)
        engine->swapTape (std::move (blank));
        engine->audioDeviceAboutToStart (&device);
        auto& p = engine->tapeEngine.params;
        p.sat = 0; p.nrOn = false;
        auto& mc = engine->tapeEngine.machine;                                                 // ...on a perfect machine
        mc.wowPct = 0; mc.flutterPct = 0; mc.driftPct = 0; mc.cutoffHz = 20000.0f; mc.crosstalkDb = -120.0f;
        engine->tapeEngine.setHissForTests (false);
        engine->channelNoiseDb = -140.0f;   // and silent electronics: each test hears only what it measures
        engine->busNoiseDb = -140.0f;
        for (auto& t : engine->track) { t.fader = 0.75f; t.gain = 1.0f / 3.0f; t.pan = 0; t.armed = false; }
    }

    ~Rig() { engine.reset(); props.reset(); propsFile.deleteFile(); propsFile.getSiblingFile ("TF-4.deck").deleteFile(); }

    // A cable from output 1 back into input 1, as long as the interface's round trip.
    bool loopback = false;
    std::vector<float> loopLine;
    size_t loopPos = 0;

    std::vector<float> run (double seconds)
    {
        std::vector<float> in0 ((size_t) bs), in1 ((size_t) bs), out0 ((size_t) bs), out1 ((size_t) bs), captured;
        const float* ins[2] { in0.data(), in1.data() };
        float* outs[2] { out0.data(), out1.data() };
        const auto delay = (size_t) (fakeInputLatency + fakeOutputLatency);
        if (loopback && loopLine.size() != delay + 1) { loopLine.assign (delay + 1, 0.0f); loopPos = 0; }
        for (int b = 0; b < (int) (seconds * sr / bs); ++b)
        {
            for (int i = 0; i < bs; ++i)
                for (int c = 0; c < 2; ++c)
                {
                    (c == 0 ? in0 : in1)[(size_t) i] = amp[c] * (float) std::sin (ph[c]);
                    ph[c] += juce::MathConstants<double>::twoPi * f[c] / sr;
                }
            if (loopback)
                for (int i = 0; i < bs; ++i)
                    in0[(size_t) i] = loopLine[(loopPos + (size_t) i) % loopLine.size()];
            engine->audioDeviceIOCallbackWithContext (ins, 2, outs, 2, bs, {});
            if (loopback)
                for (int i = 0; i < bs; ++i)
                {
                    // what leaves output 1 now arrives back, round-trip later
                    loopLine[(loopPos + (size_t) i + delay) % loopLine.size()] = out0[(size_t) i];
                }
            if (loopback) loopPos = (loopPos + (size_t) bs) % loopLine.size();
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

double db (double x) { return 20.0 * std::log10 (std::max (1.0e-9, x)); }

double rms (const std::vector<float>& v, double from, double to)
{
    auto a = (size_t) (from * sr), b = std::min (v.size(), (size_t) (to * sr));
    double s = 0; for (auto i = a; i < b; ++i) s += (double) v[i] * v[i];
    return std::sqrt (s / std::max<double> (1, (double) (b - a)));
}

// Test processors: a MIDI effect that transposes, one that counts what it's given, and a gain.
struct TestProcessor : juce::AudioProcessor
{
    using juce::AudioProcessor::AudioProcessor;
    double getTailLengthSeconds() const override { return 0.0; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
};

struct Transposer : TestProcessor
{
    Transposer() : TestProcessor (BusesProperties()) {}
    const juce::String getName() const override { return "TRANSPOSER"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return true; }
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer& m) override
    {
        juce::MidiBuffer out;
        for (const auto meta : m)
        {
            auto msg = meta.getMessage();
            if (msg.isNoteOnOrOff()) msg.setNoteNumber (msg.getNoteNumber() + 12);
            out.addEvent (msg, meta.samplePosition);
        }
        m.swapWith (out);
    }
};

struct CcCounter : TestProcessor
{
    CcCounter() : TestProcessor (BusesProperties()) {}
    const juce::String getName() const override { return "CC COUNTER"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return true; }
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer& m) override
    {
        for (const auto meta : m) if (meta.getMessage().isController() && meta.getMessage().getControllerNumber() >= 20 && meta.getMessage().getControllerNumber() < 30) ++ccs;
    }
    std::atomic<int> ccs { 0 };
};

struct Gain : TestProcessor
{
    Gain() : TestProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo()).withOutput ("Out", juce::AudioChannelSet::stereo())) {}
    const juce::String getName() const override { return "HALF"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override { b.applyGain (0.5f); }
};

// Counts its blocks: for checking when a plug-in is (and isn't) called.
struct Counting : Gain
{
    std::atomic<int> blocks { 0 };
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override { ++blocks; Gain::processBlock (b, m); }
};

// Counts what the front panel asks it. A VST3's hasEditor() builds and throws away a whole editor
// view, so asking it every frame froze the deck with a big plug-in loaded.
struct Nosy : Gain
{
    static inline std::atomic<int> editorQuestions { 0 }, nameQuestions { 0 };
    bool hasEditor() const override { ++editorQuestions; return true; }
    const juce::String getName() const override { ++nameQuestions; return "NOSY"; }
};

// A plug-in instrument for the gain structure: while any note is held, a 1 kHz tone of a fixed peak,
// the same on both sides (centred) or a quarter-cycle apart (as wide as a stereo patch gets).
struct ToneSynth : TestProcessor
{
    explicit ToneSynth (bool w) : TestProcessor (BusesProperties().withOutput ("Out", juce::AudioChannelSet::stereo())), wide (w) {}
    const juce::String getName() const override { return "TONE"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    void prepareToPlay (double rate, int) override { srate = rate; }
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override
    {
        for (const auto meta : m)
        {
            const auto msg = meta.getMessage();
            if (msg.isNoteOn()) ++held; else if (msg.isNoteOff()) held = std::max (0, held - 1);
        }
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double a = held > 0 ? peak : 0.0;
            b.setSample (0, i, (float) (a * std::sin (phase)));
            b.setSample (1, i, (float) (a * (wide ? std::cos (phase) : std::sin (phase))));
            phase += juce::MathConstants<double>::twoPi * 1000.0 / srate;
        }
        m.clear();
    }
    static constexpr double peak = 0.25;
    bool wide;
    int held = 0;
    double phase = 0.0, srate = 48000.0;
};

// The old one-source-per-track set-ups, as chains.
void audioIn (AudioEngine& e, int t, int ch)
{
    e.clearChain (t);
    e.setTrackFeed (t, AudioEngine::Feed::live);
    e.setTrackInput (t, ch);
    e.setTrackMidiPort (t, AudioEngine::midiNone);
}

void instrument (AudioEngine& e, int t, int which)
{
    e.clearChain (t);
    e.setTrackFeed (t, AudioEngine::Feed::live);
    e.setTrackInput (t, -1);
    e.insertBlock (t, -1, BuiltIn::createSynth (which));
}

juce::String firstName (AudioEngine& e, int t)
{
    auto c = e.getChain (t);
    return c.empty() ? juce::String() : c.front().name;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    // TAPEFOUR_BENCH=1: how long the audio callback takes, for typical and heavy sessions (no tests run).
    if (juce::SystemStats::getEnvironmentVariable ("TAPEFOUR_BENCH", {}).isNotEmpty())
    {
        auto bench = [] (const char* name, int buffer, std::function<void (Rig&)> setup)
        {
            Rig m;
            FakeDevice dev;
            m.engine->audioDeviceAboutToStart (&dev);
            setup (m);
            std::vector<float> in0 ((size_t) buffer), in1 ((size_t) buffer), o0 ((size_t) buffer), o1 ((size_t) buffer);
            for (int i = 0; i < buffer; ++i) { in0[(size_t) i] = 0.2f * (float) std::sin (i * 0.05); in1[(size_t) i] = in0[(size_t) i]; }
            const float* ins[2] { in0.data(), in1.data() };
            float* outs[2] { o0.data(), o1.data() };
            for (int b = 0; b < 200; ++b) m.engine->audioDeviceIOCallbackWithContext (ins, 2, outs, 2, buffer, {});
            std::vector<double> times;
            const int blocks = (int) (20.0 * sr / buffer);
            for (int b = 0; b < blocks; ++b)
            {
                auto t0 = juce::Time::getHighResolutionTicks();
                m.engine->audioDeviceIOCallbackWithContext (ins, 2, outs, 2, buffer, {});
                times.push_back (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1.0e6);
            }
            std::sort (times.begin(), times.end());
            double sum = 0; for (auto t : times) sum += t;
            const double budget = buffer / sr * 1.0e6;
            std::printf ("%-58s avg %7.1f us (%5.1f%%)  p99 %7.1f us  max %7.1f us  budget %6.0f us\n", name,
                         sum / (double) times.size(), 100.0 * sum / (double) times.size() / budget, times[times.size() * 99 / 100], times.back(), budget);
        };
        auto character = [] (Rig& m)
        {
            // A serviced machine and a used cassette: everything the tape does, switched on.
            auto& p = m.engine->tapeEngine.params;
            p.sat = 0.5f; p.nrOn = true; p.nrType = 0;
            m.engine->setMachine (AudioEngine::factoryMachine());
            m.engine->tapeEngine.setHissForTests (true);
            m.engine->getTape()->setCondition (tfmt::Condition::forGrade ("used"));
        };
        auto heavy = [&] (Rig& m)
        {
            character (m);
            for (int t = 0; t < numTracks; ++t)
            {
                auto& e = *m.engine;
                e.clearChain (t);
                e.setTrackInput (t, -1);
                e.insertBlock (t, -1, BuiltIn::createSynth (BuiltIn::transistor));
                e.insertBlock (t, -1, BuiltIn::createFx (BuiltIn::tapeEcho));
                e.insertBlock (t, -1, BuiltIn::createFx (BuiltIn::spring));
                e.insertBlock (t, -1, BuiltIn::createFx (BuiltIn::plate));
                e.track[t].armed = true;
            }
            for (int k : { 48, 52, 55, 59, 62, 64 })   // a six-note chord on every track
                m.engine->injectMidi (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100));
            m.cmd (TapeEngine::Command::rec);
        };
        for (int buffer : { 64, 256 })
        {
            std::printf ("-- %d-sample buffer at 48 kHz --\n", buffer);
            bench ("stopped, nothing armed", buffer, [] (Rig&) {});
            bench ("playing 4 tracks, all tape character, 2:1 NR", buffer, [&] (Rig& m) { character (m); m.cmd (TapeEngine::Command::play); });
            bench ("recording 4 tracks: synth (6 voices) + echo + spring + plate", buffer, heavy);
            bench ("same, but 4 tracks playing back unarmed (chains idle)", buffer, [&] (Rig& m) { heavy (m); for (auto& t : m.engine->track) t.armed = false; m.cmd (TapeEngine::Command::play); });
        }
        return 0;
    }
    const double pan = std::cos (juce::MathConstants<double>::pi / 4.0);
    // Each pass through the tape changes the level by the cassette's sensitivity.
    const double gen = std::pow (10.0, tapeModels[defaultTapeModel].sensitivity / 20.0);

    Rig r;
    auto& e = *r.engine;
    audioIn (e, 0, 0);
    audioIn (e, 1, 1);
    instrument (e, 3, BuiltIn::transistor);
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
    check (e.tapeEngine.getMode() == Mode::stop && std::abs (e.getTape()->position.load()) < cellRate * 0.005 && e.tapeEngine.getCounter() >= 0.0,
           "RTZ rewinds to 000 and stops (within 5 ms of tape, reading 000)", e.getTape()->position.load(), 0.0);

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
    check (std::abs (tone (bounced, 500, 0.3, 1.8) - 0.3 * pan * gen * gen) < 0.015, "bounced track 3 holds track 1 (500 Hz)", tone (bounced, 500, 0.3, 1.8), 0.3 * pan * gen * gen);
    check (std::abs (tone (bounced, 1500, 0.3, 1.8) - 0.3 * pan * gen * gen) < 0.015, "bounced track 3 holds track 2 (1500 Hz)", tone (bounced, 1500, 0.3, 1.8), 0.3 * pan * gen * gen);

    // Fader and pan act on the mix.
    e.track[2].pan = -1.0f;
    r.cmd (TapeEngine::Command::rtz);
    r.run (1.5);
    r.cmd (TapeEngine::Command::play);
    auto hardLeft = r.run (1.0);
    r.cmd (TapeEngine::Command::stop);
    r.run (0.05);
    check (std::abs (tone (hardLeft, 500, 0.3, 0.9) - 0.3 * gen * gen) < 0.015, "pan hard left puts the full level in the left output", tone (hardLeft, 500, 0.3, 0.9), 0.3 * gen * gen);

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

    // MPE: a note on member channel 2, bent +12 semitones on its own channel (48-semitone range).
    {
        Rig m;
        auto& me = *m.engine;
        me.setMidiInputMpe (AudioEngine::computerKeyboardPort, true, 48);
        instrument (me, 0, BuiltIn::epiano);
        // the electric piano hears only velocity until its own MPE is turned on
        if (auto* ep = BuiltIn::electricPiano (me.findBlock (0, me.getChain (0)[0].id)))
        {
            ep->param ("mpe")->setValueNotifyingHost (1.0f);
            auto st = ep->settings();
            st.tuningSpreadCents = 0.0f; st.shortRingers = 0; st.ages = false;   // its reeds in tune, so the octave is on 880 Hz
            ep->setSettings (st);
        }
        me.track[0].armed = true;                  // armed + stopped: you hear the instrument live
        m.run (0.1);
        me.injectMidi (juce::MidiMessage::pitchWheel (2, 8192 + 8192 * 12 / 48));
        me.injectMidi (juce::MidiMessage::noteOn (2, 69, (juce::uint8) 110));
        auto bent = m.run (0.6);
        auto at880 = tone (bent, 880, 0.2, 0.5), at440 = tone (bent, 440, 0.2, 0.5);
        check (at880 > 4.0 * at440 && at880 > 0.001, "MPE: per-note bend on channel 2 moves A440 up an octave", at880, 4.0 * at440);

        // A second note on another member channel is not bent.
        me.injectMidi (juce::MidiMessage::noteOn (3, 64, (juce::uint8) 110));
        auto both = m.run (0.4);
        check (tone (both, juce::MidiMessage::getMidiNoteInHertz (64), 0.15, 0.4) > 0.005, "MPE: unbent note on channel 3 sounds at its own pitch",
               tone (both, juce::MidiMessage::getMidiNoteInHertz (64), 0.15, 0.4), 0.005);
        me.injectMidi (juce::MidiMessage::noteOff (2, 69));
        me.injectMidi (juce::MidiMessage::noteOff (3, 64));

        // Settings survive a restart.
        me.setMidiInputMpe (AudioEngine::computerKeyboardPort, true, 24);
        me.recallDeckWithTape = true;
        me.track[2].fader = 0.42f;
        me.saveSettings();
        m.engine.reset();
        auto again = std::make_unique<AudioEngine> (*m.props);
        bool mpeBack = false; int bendBack = 0;
        for (auto& in : again->getMidiInputs())
            if (in.slot == AudioEngine::computerKeyboardPort) { mpeBack = in.mpe; bendBack = in.bendRange; }
        check (mpeBack && bendBack == 24, "settings: per-controller MPE and bend range restored", bendBack, 24);
        check (again->recallDeckWithTape.load(), "settings: 'recall the deck with each tape' restored", again->recallDeckWithTape.load(), 1);
        check (std::abs (again->track[2].fader.load() - 0.42f) < 1e-4f, "settings: fader position restored after restart", again->track[2].fader.load(), 0.42);
        check (firstName (*again, 0) == BuiltIn::synthName (BuiltIn::epiano),
               "settings: track 1 instrument restored after restart", 1, 1);
        again.reset();
    }


    // EQ: the shelves are heard where a portastudio's are (live input, armed, stopped)
    {
        Rig m;
        auto& me = *m.engine;
        audioIn (me, 0, 0);
        me.track[0].armed = true;
        auto level = [&] (double hz, float hi, float lo)
        {
            me.track[0].high = hi; me.track[0].low = lo;
            m.f[0] = hz; m.amp[0] = 0.05f;
            auto out = m.run (0.4);
            return 20.0 * std::log10 (tone (out, hz, 0.15, 0.4));
        };
        struct Pt { double hz; float hi, lo; double minDb, maxDb; const char* what; };
        const Pt pts[] {
            { 10000, 1, 0,  10.0, 12.5, "EQ: HIGH +12 at 10 kHz (dB)" },
            {  5000, 1, 0,   5.0,  9.0, "EQ: HIGH +12 at 5 kHz (dB)" },
            {   500, 1, 0,  -0.1,  0.6, "EQ: HIGH leaves 500 Hz alone (dB)" },
            {   100, 0, 1,  10.0, 12.5, "EQ: LOW +12 at 100 Hz (dB)" },
            {   250, 0, 1,   4.5,  8.0, "EQ: LOW +12 at 250 Hz (dB)" },
            {  2000, 0, 1,  -0.1,  0.6, "EQ: LOW leaves 2 kHz alone (dB)" },
            { 10000,-1, 0, -12.5,-10.0, "EQ: HIGH -12 at 10 kHz (dB)" } };
        for (auto& p : pts)
        {
            auto flat = level (p.hz, 0, 0), eqd = level (p.hz, p.hi, p.lo);
            auto d = eqd - flat;
            check (d >= p.minDb && d <= p.maxDb, p.what, d, 0.5 * (p.minDb + p.maxDb));
        }
    }

    // Swept mid and MON
    {
        Rig m;
        auto& me = *m.engine;
        audioIn (me, 0, 0);
        me.track[0].armed = true;
        auto boost = [&] (double hz, float freqKnob)
        {
            me.track[0].midFreq = freqKnob;
            m.f[0] = hz; m.amp[0] = 0.05f;
            me.track[0].mid = 0.0f; auto flat = tone (m.run (0.4), hz, 0.15, 0.4);
            me.track[0].mid = 1.0f; auto lifted = tone (m.run (0.4), hz, 0.15, 0.4);
            return 20.0 * std::log10 (lifted / flat);
        };
        auto lo250 = boost (250, 0.0f), lo5k = boost (5000, 0.0f), hi5k = boost (5000, 1.0f), hi250 = boost (250, 1.0f);
        check (lo250 > 11.0 && lo5k < 1.0, "swept mid at 250 Hz lifts 250 Hz, not 5 kHz (dB)", lo250, 12.0);
        check (hi5k > 11.0 && hi250 < 1.0, "swept mid at 5 kHz lifts 5 kHz, not 250 Hz (dB)", hi5k, 12.0);
        check (std::abs (AudioEngine::midFreqHz (AudioEngine::defaultMidFreq) - 1200.0) < 10.0, "swept mid starts at 1.2 kHz",
               AudioEngine::midFreqHz (AudioEngine::defaultMidFreq), 1200.0);
        me.track[0].mid = 0.0f;

        // ARM is the live switch: an armed track hears its input even while the tape plays; unarmed, it doesn't
        m.f[0] = 700; m.amp[0] = 0.2f;
        me.track[0].armed = false;
        auto off = tone (m.run (0.4), 700, 0.15, 0.4);
        me.track[0].armed = true;
        m.cmd (TapeEngine::Command::play);
        auto playing = tone (m.run (0.4), 700, 0.15, 0.4);
        m.cmd (TapeEngine::Command::stop);
        m.run (0.05);
        check (off < 0.001 && playing > 0.1, "ARM: armed track plays its input while the tape plays", playing, 0.14);
        me.track[0].armed = false;
    }

    // Per-track MIDI: two controllers, two instruments, each plays only its own track.
    {
        Rig m;
        auto& me = *m.engine;
        auto keysA = me.addVirtualMidiInput ("Keys A"), keysB = me.addVirtualMidiInput ("Keys B");
        check (keysA > 0 && keysB > 0 && keysA != keysB, "two controllers get their own slots", keysB, keysA + 1);
        instrument (me, 0, BuiltIn::transistor);
        instrument (me, 1, BuiltIn::transistor);
        instrument (me, 2, BuiltIn::transistor);
        me.setTrackMidiPort (0, keysA);
        me.setTrackMidiPort (1, keysB);
        me.setTrackMidiPort (2, AudioEngine::midiAny);
        me.track[0].armed = true;  me.track[0].pan = -1.0f;   // left
        me.track[1].armed = true;  me.track[1].pan =  1.0f;   // right: nothing of it on the left output
        me.track[2].armed = false; me.track[2].pan = -1.0f;   // "any" but not armed: deaf
        m.run (0.1);
        me.injectMidi (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 110), keysA);   // 440
        me.injectMidi (juce::MidiMessage::noteOn (1, 75, (juce::uint8) 110), keysB);   // 622 Hz, not a harmonic of 440
        auto left = m.run (0.5);
        auto a = tone (left, 440, 0.15, 0.45), b = tone (left, juce::MidiMessage::getMidiNoteInHertz (75), 0.15, 0.45);
        check (a > 0.005, "controller A plays track 1", a, 0.005);
        check (b < a * 0.05, "controller B does not reach track 1 (or the unarmed 'any' track)", b, 0.0);
        me.injectMidi (juce::MidiMessage::noteOff (1, 69), keysA);
        me.injectMidi (juce::MidiMessage::noteOff (1, 75), keysB);

        me.track[1].pan = -1.0f;
        me.injectMidi (juce::MidiMessage::noteOn (1, 75, (juce::uint8) 110), keysB);
        auto moved = m.run (0.5);
        auto bTone = tone (moved, juce::MidiMessage::getMidiNoteInHertz (75), 0.15, 0.45);
        check (bTone > 0.005, "controller B plays track 2", bTone, 0.005);
        me.injectMidi (juce::MidiMessage::noteOff (1, 75), keysB);
        m.run (0.3);

        // ...but only while its track is armed: the controller choice doesn't make it live
        me.track[1].armed = false;
        m.run (0.1);
        me.injectMidi (juce::MidiMessage::noteOn (1, 75, (juce::uint8) 110), keysB);
        auto disarmed = tone (m.run (0.5), juce::MidiMessage::getMidiNoteInHertz (75), 0.15, 0.45);
        me.injectMidi (juce::MidiMessage::noteOff (1, 75), keysB);
        check (disarmed < 0.001, "an instrument with its own controller is silent when its track isn't armed", disarmed, 0.0);
        me.track[1].armed = true;
        m.run (0.3);

        // Hardware synth on track 3: controller A goes out on channel 5, audio comes back on IN 1.
        auto juno = me.addVirtualMidiOutput ("Juno");
        std::vector<juce::MidiMessage> sent;
        juce::SpinLock sentLock;
        me.setMidiOutTapForTests ([&] (int slot, const juce::MidiBuffer& sentBuffer)
        {
            if (slot != juno) return;
            const juce::SpinLock::ScopedLockType sl (sentLock);
            for (const auto meta : sentBuffer) sent.push_back (meta.getMessage());
        });
        me.track[0].armed = false;
        me.track[1].armed = false;
        me.clearChain (2);
        me.setTrackInput (2, -1);
        auto extId = me.addExternal (2, -1, juno);
        auto* ext = dynamic_cast<ExternalBlock*> (me.findBlock (2, extId));
        ext->channel = 5;
        ext->returnInput = 0;
        check (me.getChain (2).size() == 1 && me.getChain (2)[0].role == BlockRole::external, "hardware synth is a block in the chain", (double) me.getChain (2).size(), 1);
        me.setTrackMidiPort (2, keysA);
        me.track[2].armed = true;
        m.f[0] = 500; m.amp[0] = 0.3f;
        m.run (0.1);
        sent.clear();
        me.injectMidi (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), keysA);
        me.injectMidi (juce::MidiMessage::noteOn (1, 62, (juce::uint8) 100), keysB);   // B is not routed here
        auto hw = m.run (0.4);
        int onsCh5 = 0, others = 0;
        for (auto& msg : sent)
            if (msg.isNoteOn()) (msg.getChannel() == 5 && msg.getNoteNumber() == 60 ? onsCh5 : others)++;
        check (onsCh5 == 1 && others == 0, "hardware: controller A's note goes out on channel 5, B's does not", onsCh5, 1);
        check (std::abs (tone (hw, 500, 0.1, 0.4) - 0.3) < 0.02, "hardware: the synth's audio comes back on the input", tone (hw, 500, 0.1, 0.4), 0.3);
        check (std::abs (tone (hw, 440, 0.1, 0.4)) < 0.002 && me.describeSource (2).isNotEmpty(), "hardware: no instrument sounding on that track", tone (hw, 440, 0.1, 0.4), 0.0);

        // Changing the route sends all-notes-off to the old one.
        sent.clear();
        ext->channel = 7;
        m.run (0.05);
        int offs = 0;
        for (auto& msg : sent) if (msg.isAllNotesOff()) ++offs;
        check (offs == 16, "hardware: re-routing silences the old route (all notes off, 16 channels)", offs, 16);
        me.setMidiOutTapForTests (nullptr);

        // Deck state: everything savable goes out and comes back.
        me.track[0].fader = 0.31f; me.track[0].pan = 0.4f; me.track[0].high = -0.5f;
        me.masterFader = 0.6f;
        me.tapeEngine.params.pitch = 3.7f;
        auto deck = me.createDeckState();
        me.track[0].fader = 0.9f; me.track[0].pan = -0.8f; me.track[0].high = 0.7f;
        me.masterFader = 0.2f;
        me.tapeEngine.params.pitch = 0.0f;
        me.clearChain (2);
        me.setTrackMidiPort (0, AudioEngine::midiNone);
        me.applyDeckState (*deck);
        check (std::abs (me.track[0].fader.load() - 0.31f) < 1e-4f && std::abs (me.track[0].pan.load() - 0.4f) < 1e-4f
               && std::abs (me.track[0].high.load() + 0.5f) < 1e-4f, "deck recall: fader, pan and EQ come back", me.track[0].fader.load(), 0.31);
        check (std::abs (me.masterFader.load() - 0.6f) < 1e-4f && std::abs (me.tapeEngine.params.pitch.load() - 3.7f) < 1e-4f,
               "deck recall: master and pitch come back", me.masterFader.load(), 0.6);
        auto hwBack = me.getChain (2);
        check (hwBack.size() == 1 && hwBack[0].role == BlockRole::external && hwBack[0].outPort == juno && hwBack[0].channel == 7 && hwBack[0].returnInput == 0,
               "deck recall: hardware synth route comes back", hwBack.empty() ? -1 : hwBack[0].channel, 7);
        check (me.track[0].midiPort.load() == keysA && me.track[1].midiPort.load() == keysB, "deck recall: each track's controller comes back",
               me.track[0].midiPort.load(), keysA);
        check (firstName (me, 1) == BuiltIn::synthName (BuiltIn::transistor),
               "deck recall: instruments come back", 1, 1);

        // ...and the deck travels with the tape.
        auto& tape = *me.getTape();
        tape.session = me.createDeckState();
        tape.sessionNeedsWriting = true;
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tf_deck_tape");
        dir.deleteRecursively();
        dir.createDirectory();
        tape.save (dir);
        auto loaded = Tape::load (dir);
        check (loaded != nullptr && loaded->session != nullptr && loaded->session->isEquivalentTo (tape.session.get(), false),
               "session is saved beside the cassette (session.xml)", loaded != nullptr && loaded->session != nullptr, 1);
        dir.deleteRecursively();
    }

    // ---- Turning the TF-4's cassette over ----
    {
        Rig rt;
        auto& me = *rt.engine;
        auto& tape = *me.getTape();
        tape.position = 20.0 * cellRate;
        me.tapeEngine.setTape (&tape, false);
        me.tapeEngine.setCounter (123.0);   // the machine's counter, wherever it got to
        const bool turned = me.turnTapeOver (true) && tape.isTurnedOver();
        check (turned, "turn over: the deck turns its cassette over when stopped", turned ? 1 : 0, 1);
        check (std::abs (me.tapeEngine.getCounter() - 123.0) < 1e-9
                   && std::abs (me.tapeEngine.getPosition() - ((double) tape.getLengthCells() - 20.0 * cellRate)) < 1e-6,
               "turn over: the machine's counter doesn't move, the transport is at the same place on the tape", me.tapeEngine.getCounter(), 123.0);
        auto deck = me.createDeckState();
        auto* td = deck->getChildByName ("TAPE_DECK");
        check (td != nullptr && td->getBoolAttribute ("sideB"), "turn over: the session remembers side B is up", td != nullptr && td->getBoolAttribute ("sideB"), 1);
        rt.cmd (TapeEngine::Command::play);
        rt.run (0.1);
        check (! me.turnTapeOver (false) && tape.isTurnedOver(), "turn over: not while the tape is running", tape.isTurnedOver() ? 1 : 0, 1);
        rt.cmd (TapeEngine::Command::stop);
        rt.run (0.05);
        const bool turnedBack = me.turnTapeOver (false) && ! tape.isTurnedOver() && ! me.createDeckState()->getChildByName ("TAPE_DECK")->hasAttribute ("sideB");
        check (turnedBack, "turn over: and back again", turnedBack ? 1 : 0, 1);
        juce::File exportDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tf_turned_export");
        tape.writeCell (0, 100, 0.2f, false, 0, 1.0f, 0);
        me.turnTapeOver (true);
        TrackExport ex;
        ex.folder = exportDir;
        ex.baseName = "turned";
        check (! ex.run (tape, me.tapeEngine.params, me.tapeEngine.machine, nullptr), "turn over: tracks export only side A up", 0, 0);
        me.turnTapeOver (false);
    }

    // ---- The machine, the cassette and the session are kept apart ----
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tf_split_test");
        dir.deleteRecursively();
        dir.createDirectory();

        // Settings from before the deck format: the machine's character comes out of the old deck state, once.
        auto settingsFile = dir.getChildFile ("TapeFour.settings");
        {
            juce::PropertiesFile old (settingsFile, juce::PropertiesFile::Options());
            juce::XmlElement deck ("DECK");
            auto* td = deck.createNewChildElement ("TAPE_DECK");
            td->setAttribute ("wow", 0.6); td->setAttribute ("flutter", 0.5); td->setAttribute ("drift", 0.4);
            td->setAttribute ("hf", 0.55); td->setAttribute ("xtalk", 0.5); td->setAttribute ("hiss", 0.75);
            old.setValue ("deck", &deck);
            old.saveIfNeeded();
        }
        {
            juce::PropertiesFile settings (settingsFile, juce::PropertiesFile::Options());
            AudioEngine eng (settings);
            eng.deviceManager.removeAudioCallback (&eng);
            auto mach = eng.getMachine();
            check (std::abs (mach.transport.wowPct - 0.3) < 1e-6 && std::abs (mach.transport.flutterPct - 0.15) < 1e-6
                       && std::abs (mach.transport.driftPct - 0.4) < 1e-6 && std::abs (mach.heads.cutoffHz - 9330.0) < 2.0
                       && std::abs (mach.heads.crosstalkDb + 55.0) < 1e-6 && mach.id == tf4DeckId,
                   "machine: old settings become the TF-4's own (wow, flutter, drift, heads, crosstalk)", mach.transport.wowPct, 0.3);
            eng.saveSettings();
            auto session = eng.createDeckState();
            auto* td = session->getChildByName ("TAPE_DECK");
            check (td != nullptr && ! td->hasAttribute ("wow") && ! td->hasAttribute ("hiss") && ! td->hasAttribute ("drop") && td->hasAttribute ("sat"),
                   "session: holds the operator's controls, not the machine or the cassette", 1, 1);
        }
        tfmt::DeckDescription onDisk;
        std::string err;
        const bool readBack = tfmt::readDeck (tfmt::pathFromUtf8 (settingsFile.getSiblingFile ("TF-4.deck").getFullPathName().toStdString()), onDisk, err);
        check (readBack && std::abs (onDisk.transport.wowPct - 0.3) < 1e-6,
               "machine: saved as TF-4.deck beside the settings", onDisk.transport.wowPct, 0.3);
        {
            juce::PropertiesFile settings (settingsFile, juce::PropertiesFile::Options());
            AudioEngine eng (settings);
            eng.deviceManager.removeAudioCallback (&eng);
            auto d = eng.getMachine();
            d.applyCondition ("new");
            eng.setMachine (d);
            eng.saveSettings();
        }
        {
            juce::PropertiesFile settings (settingsFile, juce::PropertiesFile::Options());
            AudioEngine eng (settings);
            eng.deviceManager.removeAudioCallback (&eng);
            check (eng.getMachine().condition == "new" && std::abs (eng.tapeEngine.machine.wowPct.load() - 0.08f) < 1e-6f,
                   "machine: comes back from TF-4.deck after a restart", eng.tapeEngine.machine.wowPct.load(), 0.08);
        }

        // A cassette goes out as a .cass and comes back as itself; its session stays on the shelf.
        TapeShelf::setFolderForTests (dir.getChildFile ("shelf"));
        auto tape = TapeShelf::createBlank (0, 2, "worn");
        for (int64_t c = 0; c < 32000; ++c) tape->writeCell (1, c, 0.3f * (float) std::sin (0.05 * (double) c), true, 0, 1.0f, 1);
        tape->name = "exported";
        tape->protectA = true;
        tape->session = std::make_unique<juce::XmlElement> ("DECK");
        tape->sessionNeedsWriting = true;
        TapeShelf::save (*tape);
        auto cass = dir.getChildFile ("exported.cass");
        juce::String error;
        check (TapeShelf::exportCassette (tape->id, cass, error), "cass: exported from the shelf", 1, 1);
        juce::ZipFile zip (cass);
        check (zip.getIndexOfFileName (tfmt::CassFiles::manifest) >= 0 && zip.getIndexOfFileName ("session.xml") < 0 && zip.getIndexOfFileName ("track2.pcm") >= 0,
               "cass: the file holds the cassette, not the session (and any zip tool reads it)", zip.getNumEntries(), 4);

        TapeShelf::folderFor (tape->id).getChildFile (tfmt::CassFiles::manifest).deleteFile();   // spoil the shelf copy
        juce::String importedId;
        check (TapeShelf::importCassette (cass, importedId, error) && importedId == tape->id, "cass: imported onto the shelf", 1, 1);
        auto back = TapeShelf::load (importedId);
        bool same = back != nullptr;
        for (int64_t c = 0; same && c < 32000; c += 11) same = std::abs (back->readCell (1, c) - tape->readCell (1, c)) < 1e-9f;
        check (same && back->name == "exported" && back->getCondition().grade == "worn" && back->getModel() == 2
                   && back->protectA.load() && back->isNrEncoded (1, 2) && back->recordedNrType (1, 2) == 0 && back->session != nullptr,
               "cass: comes back as itself: recording, label, formulation, condition, tab, maps; the session kept", same ? 1 : 0, 1);

        auto bytes = cass.loadFileAsString();
        dir.getChildFile ("broken.cass").replaceWithText (bytes.substring (0, 200));
        check (! TapeShelf::importCassette (dir.getChildFile ("broken.cass"), importedId, error) && TapeShelf::load (tape->id) != nullptr,
               "cass: a damaged file is refused and the shelf left as it was", 1, 1);
        TapeShelf::setFolderForTests ({});
        dir.deleteRecursively();
    }

    // ---- Exports: each recorded track as the deck plays it, in every format at once ----
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tf_export_test");
        dir.deleteRecursively();
        dir.createDirectory();
        Tape tape (1, defaultTapeModel, tfmt::Condition());
        for (int64_t c = 0; c < (int64_t) cellRate * 3; ++c)
        {
            auto ph = juce::MathConstants<double>::twoPi * (double) c / cellRate;
            tape.writeCell (0, c, 0.2f * (float) std::sin (1000.0 * ph), false, 0, 1.0f, 0);
            tape.writeCell (2, c, 0.2f * (float) std::sin (300.0 * ph), false, 0, 1.0f, 0);
        }
        tape.position = 12345.0;
        TapeEngine::Params op;
        op.nrOn = false;
        TapeEngine::Machine perfect;
        perfect.wowPct = 0; perfect.flutterPct = 0; perfect.driftPct = 0; perfect.cutoffHz = 20000.0f; perfect.crosstalkDb = -120.0f;
        TrackExport job;
        job.folder = dir;
        job.baseName = "demo";
        job.formats.wav = job.formats.flac = job.formats.mp3 = true;
        job.sampleRate = 48000.0;
        float lastProgress = 0;
        const bool ok = job.run (tape, op, perfect, [&] (float p) { lastProgress = p; });
        check (ok && job.written.size() == 6 && dir.getChildFile ("demo - track 1.wav").existsAsFile() && dir.getChildFile ("demo - track 3.mp3").existsAsFile()
                   && ! dir.getChildFile ("demo - track 2.wav").existsAsFile(),
               "export: one file per recorded track per format (WAV, FLAC, MP3)", job.written.size(), 6);
        check (std::abs (tape.position.load() - 12345.0) < 1e-9 && tape.passesAt (cellRate * 1.5) == 0 && lastProgress > 0.5f,
               "export: the tape in the deck isn't moved or worn", tape.passesAt (cellRate * 1.5), 0);

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        fm.registerFormat (new juce::MP3AudioFormat(), false);
        auto readBack = [&] (const juce::String& name)
        {
            juce::AudioBuffer<float> b;
            if (auto reader = std::unique_ptr<juce::AudioFormatReader> (fm.createReaderFor (dir.getChildFile (name))))
            {
                b.setSize (1, (int) reader->lengthInSamples);
                reader->read (&b, 0, (int) reader->lengthInSamples, 0, true, false);
            }
            return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
        };
        auto wav = readBack ("demo - track 1.wav"), flac = readBack ("demo - track 1.flac"), mp3 = readBack ("demo - track 1.mp3");
        auto mp3Low = readBack ("demo - track 3.mp3");
        double flacDiff = 0;
        for (size_t i = 0; i < std::min (wav.size(), flac.size()); ++i) flacDiff = std::max (flacDiff, (double) std::abs (wav[i] - flac[i]));
        check (std::abs ((double) wav.size() / 48000.0 - 3.5) < 0.1 && flac.size() == wav.size() && flacDiff < 1.0e-5,
               "export: WAV and FLAC hold the same audio (the used tape plus half a second)", (double) wav.size() / 48000.0, 3.5);
        const auto wavLevel = tone (wav, 1000.0, 0.5, 2.5), mp3Level = tone (mp3, 1000.0, 0.6, 2.6), mp3Bass = tone (mp3Low, 300.0, 0.6, 2.6);
        check (wavLevel > 0.05 && std::abs (db (mp3Level / wavLevel)) < 0.5 && mp3Bass > 0.05 && std::abs ((double) mp3.size() / 48000.0 - 3.5) < 0.15,
               "export: the MP3s decode to the same tones, at the right length", db (mp3Level / wavLevel), 0.0);
        dir.deleteRecursively();
    }

    // ---- Mixdown: the master bus, straight to files (DIRECT) or through the master deck's cassette ----
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tf_mixdown_test");
        dir.deleteRecursively();
        dir.createDirectory();
        TapeShelf::setFolderForTests (dir.getChildFile ("shelf"));
        auto mixes = dir.getChildFile ("Mixdowns");
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        auto readStereo = [&] (const juce::String& path)
        {
            juce::AudioBuffer<float> b;
            if (auto reader = std::unique_ptr<juce::AudioFormatReader> (fm.createReaderFor (juce::File (path))))
            {
                b.setSize (2, (int) reader->lengthInSamples);
                reader->read (&b, 0, (int) reader->lengthInSamples, 0, true, true);
            }
            return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
        };

        Rig m;
        auto& me = *m.engine;
        auto& md = me.masterDeck;
        me.track[2].armed = true;   // its input, live, is the mix
        m.f[0] = 500; m.amp[0] = 0.3f;
        ExportFormats wav;
        juce::String why, problem;
        MixTake take;
        auto finishTake = [&]
        {
            m.cmd (TapeEngine::Command::stop);
            for (int i = 0; i < 400; ++i)
            {
                m.run (0.01);
                if (md.collectFinished (take, problem)) return true;
                juce::Thread::sleep (2);
            }
            return false;
        };

        // DIRECT: the file is the master bus, sample for sample.
        md.settings.model = -1;
        check (md.arm (wav, mixes, "demo", 0, why) && md.getState() == MasterDeck::State::armed, "mixdown: arms (DIRECT)", 1, 1);
        m.cmd (TapeEngine::Command::play);
        auto heard = m.run (1.0);
        const bool done = finishTake();
        auto file = take.files.isEmpty() ? std::vector<float>() : readStereo (take.files[0]);
        double worst = file.empty() ? 1.0 : 0.0;
        for (size_t i = 0; i < std::min (file.size(), heard.size()); ++i) worst = std::max (worst, (double) std::abs (file[i] - heard[i]));
        check (done && take.number == 1 && take.side == -1 && std::abs ((double) file.size() / sr - 1.0) < 0.02 && worst < 1.0e-6 && problem.isEmpty(),
               "mixdown DIRECT: the file is what you heard, sample for sample, until STOP", worst, 0.0);

        // Through the master deck: a new SD46, side A.
        md.settings.model = 1;
        md.settings.nr = true;
        md.settings.side = 0;
        md.swapTape (Tape::makeStereo (0, 1, tfmt::Condition()));
        md.applySettings (0);
        check (md.arm (wav, mixes, "demo", 0, why), "mixdown: arms (master tape)", 1, 1);
        m.cmd (TapeEngine::Command::play);
        heard = m.run (2.0);
        finishTake();
        auto first = take;
        file = first.files.isEmpty() ? std::vector<float>() : readStereo (first.files[0]);
        const auto heardTone = tone (heard, 500.0, 0.5, 1.5), fileTone = tone (file, 500.0, 0.5, 1.5);
        check (first.number == 2 && first.side == 0 && std::abs (first.start - MasterDeck::leadInSeconds * cellRate) < 1.0
                   && std::abs (first.length / cellRate - 2.0) < 0.05 && std::abs ((double) file.size() / sr - 2.0) < 0.05,
               "mixdown on tape: after the lead-in on side A, as long as it played", first.start / cellRate, MasterDeck::leadInSeconds);
        check (std::abs (db (fileTone / heardTone)) < 1.5, "mixdown on tape: the files hold it played back off the tape (level, dB)", db (fileTone / heardTone), 0.0);
        auto* master = md.getTape();
        check (master->isRecorded (0, 30) && master->isRecorded (1, 30) && ! master->isRecorded (2, 30) && ! master->isRecorded (0, 5),
               "mixdown on tape: recorded on side A's two tracks, after the leader", 1, 1);

        // The next one goes after it, with a gap; side B starts at its own beginning.
        check (md.arm (wav, mixes, "demo", 0, why), "mixdown: arms again for the next take", 1, 1);
        m.cmd (TapeEngine::Command::play);
        m.run (0.5);
        finishTake();
        check (take.number == 3 && std::abs (take.start - (first.start + first.length + MasterDeck::gapSeconds * cellRate)) < 1.0,
               "mixdown on tape: the next take follows after a gap", (take.start - first.start - first.length) / cellRate, MasterDeck::gapSeconds);
        md.settings.side = 1;
        md.arm (wav, mixes, "demo", 0, why);
        m.cmd (TapeEngine::Command::play);
        m.run (1.0);
        finishTake();
        auto sideB = take;
        check (sideB.side == 1 && std::abs (sideB.start - MasterDeck::leadInSeconds * cellRate) < 1.0 && master->isRecorded (2, 25) && master->isRecorded (3, 25),
               "mixdown on tape: turned over, side B from its own start", sideB.side, 1);

        // Listening to a take: the master tape plays it back in place of the mix.
        me.track[2].armed = false;
        m.amp[0] = 0.0f;
        check (md.listen (first) && md.isListening(), "mixdown: a take can be listened to", 1, 1);
        auto back = m.run (2.3);
        check (std::abs (db (tone (back, 500.0, 0.5, 1.5) / heardTone)) < 1.5 && ! md.isListening(),
               "mixdown: listening plays the take off the master tape, and stops at its end", db (tone (back, 500.0, 0.5, 1.5) / heardTone), 0.0);

        // The master cassette keeps side B the way it lies on the tape: backwards, seen from side A.
        auto folder = dir.getChildFile ("master");
        master->save (folder);
        auto asStereo = Tape::loadForStereoDeck (folder), asFourTrack = Tape::load (folder);
        const auto len = master->getLengthCells();
        const auto c0 = (int64_t) (2.5 * cellRate);
        bool stereoSame = asStereo != nullptr, physical = asFourTrack != nullptr;
        for (int64_t c = c0; c < c0 + 3200 && stereoSame && physical; c += 7)
        {
            stereoSame = std::abs (asStereo->readCell (2, c) - master->readCell (2, c)) < 1e-9f && std::abs (asStereo->readCell (0, c) - master->readCell (0, c)) < 1e-9f;
            physical = std::abs (asFourTrack->readCell (2, len - 1 - c) - master->readCell (2, c)) < 1e-9f;
        }
        check (stereoSame && physical && asStereo->getLayout() == tfmt::Layout::stereoTwoSides && asStereo->isRecorded (2, 25),
               "master cassette: saved as it lies (side B reversed), and a stereo deck reads both sides forwards", stereoSame && physical, 1);

        // A side that's full, a tape that's protected: refused, with why.
        md.settings.side = 0;
        MixTake huge;
        huge.side = 0;
        huge.start = 0;
        huge.length = (double) master->getLengthCells();
        md.takes.push_back (huge);
        check (! md.arm (wav, mixes, "demo", 0, why) && why.contains ("FULL"), "mixdown: a full side is refused (turn it over)", 1, 1);
        md.takes.pop_back();
        master->protectA = true;
        check (! md.arm (wav, mixes, "demo", 0, why) && why.contains ("PROTECTED"), "mixdown: a write-protected master is refused", 1, 1);
        master->protectA = false;

        // A mix that clips says where.
        md.settings.model = -1;
        me.track[2].armed = true;
        m.amp[0] = 1.0f;
        me.track[2].gain = 4.0f;
        md.arm (wav, mixes, "demo", 0, why);
        m.cmd (TapeEngine::Command::play);
        m.run (0.5);
        const auto clipped = md.clippedAtSeconds();

        finishTake();
        check (clipped >= 0.0 && clipped < 0.3, "mixdown: a clipped mix is noticed (seconds in)", clipped, 0.0);
        check (mixes.getChildFile ("demo - mix 1.wav").existsAsFile() && mixes.getChildFile ("demo - mix 2.wav").existsAsFile()
                   && mixes.getChildFile ("demo - mix 5.wav").existsAsFile() && md.takes.size() == 4 && md.takes.front().number == 2,
               "mixdown: every take its own file; the takes made with this master tape listed with it", (int) md.takes.size(), 4);
        TapeShelf::setFolderForTests ({});
        dir.deleteRecursively();
    }

    // ---- Recording lines up with what you heard, on an interface with 25 ms each way ----
    {
        fakeInputLatency = 1200;
        fakeOutputLatency = 1200;
        auto clickAt = [] (Tape& tape, int t)
        {
            int64_t best = 0; float bv = 0;
            for (int64_t c = 0; c < (int64_t) cellRate * 4; ++c)
                if (auto v = std::abs (tape.readCell (t, c)); v > bv) { bv = v; best = c; }
            return best;
        };
        auto putClick = [] (Tape& tape, int t, int64_t at)
        {
            for (int c = 0; c < 16; ++c) tape.writeCell (t, at + c, 0.4f, false, 0, 1.0f, 0);
        };

        // Bounce: straight off the tape, no correction.
        {
            Rig m;
            auto& me = *m.engine;
            auto& tape = *me.getTape();
            putClick (tape, 0, 64000);
            me.toggleBounceSource (1, 0);
            me.track[1].armed = true;
            m.cmd (TapeEngine::Command::rec);
            m.run (3.0);
            m.cmd (TapeEngine::Command::stop);
            m.run (0.05);
            auto offset = (double) (clickAt (tape, 1) - clickAt (tape, 0));
            check (std::abs (offset) < 24.0, "latency: a bounce lands where its sources are (cells)", offset, 0.0);
        }

        // Overdub through the interface: play along to what you hear (a loopback cable stands in for you).
        {
            Rig m;
            auto& me = *m.engine;
            auto& tape = *me.getTape();
            putClick (tape, 0, 64000);
            me.track[0].pan = -1.0f;     // track 1 to the left output, which loops back into IN 1
            audioIn (me, 1, 0);
            me.track[1].pan = 1.0f;      // the overdub's own monitoring goes right, out of the loop
            me.track[1].armed = true;
            m.loopback = true;
            m.cmd (TapeEngine::Command::rec);
            m.run (3.0);
            m.cmd (TapeEngine::Command::stop);
            m.run (0.05);
            m.loopback = false;
            auto offset = (double) (clickAt (tape, 1) - clickAt (tape, 0));
            check (std::abs (offset) < 24.0, "latency: an overdub through the interface lines up (cells)", offset, 0.0);
        }

        // The rule for each source.
        check (juce::exactlyEqual (AudioEngine::recordLatencySamples (false, true, false, 1200, 900, 48, 0), 900.0)
               && juce::exactlyEqual (AudioEngine::recordLatencySamples (false, false, true, 1200, 900, 48, 0), 0.0)
               && juce::exactlyEqual (AudioEngine::recordLatencySamples (true, false, false, 1200, 900, 48, 0), 2148.0)
               && juce::exactlyEqual (AudioEngine::recordLatencySamples (true, true, false, 1200, 900, 48, 64), 2212.0),
               "latency: instruments wait for the output only; audio and synths for the round trip", 900, 900);
        fakeInputLatency = fakeOutputLatency = 0;
    }

    // ---- Controls glide; arming crossfades ----
    {
        Rig m;
        auto& me = *m.engine;
        audioIn (me, 0, 0);
        m.f[0] = 1000; m.amp[0] = 0.3f;
        me.track[0].armed = false;
        m.run (0.2);
        me.track[0].armed = true;   // tape is silent, so this is silence -> 0.3
        auto fade = m.run (0.1);
        float firstMs = 0;
        for (int i = 0; i < 48; ++i) firstMs = std::max (firstMs, std::abs (fade[(size_t) i]));
        check (firstMs < 0.06f && tone (fade, 1000, 0.05, 0.1) > 0.2, "arming fades the input in over ~12 ms (no click)", firstMs, 0.0);

        me.track[0].mid = -1.0f;
        m.run (0.3);
        me.track[0].mid = 1.0f;     // -12 dB to +12 dB in one go
        auto glide = m.run (0.3);
        float maxStep = 0;
        for (size_t i = 1; i < glide.size(); ++i) maxStep = std::max (maxStep, std::abs (glide[i] - glide[i - 1]));
        auto peakNow = 0.3f * 3.98f * (float) (juce::MathConstants<double>::twoPi * 1000.0 / sr);
        check (maxStep < peakNow * 1.3f, "EQ: a sudden knob jump glides (no step)", maxStep, peakNow);
        me.track[0].mid = 0.0f;
        me.track[0].armed = false;
    }

    // ---- The tape is saved while recording, safely ----
    {
        Rig m;
        auto& me = *m.engine;
        audioIn (me, 0, 0);
        me.track[0].armed = true;
        m.f[0] = 440; m.amp[0] = 0.3f;
        auto& tape = *me.getTape();
        tape.id = "tf_writer_test";
        auto dir = TapeShelf::folderFor (tape.id);
        dir.deleteRecursively();

        TapeWriter writer;
        m.cmd (TapeEngine::Command::rec);
        for (int i = 0; i < 6; ++i)
        {
            m.run (0.5);
            writer.saveTape (tape);   // the audio is written on the writer's thread while this one records
        }
        m.cmd (TapeEngine::Command::stop);
        m.run (0.05);
        writer.waitUntilIdle();
        writer.saveTape (tape);
        writer.waitUntilIdle();

        auto back = Tape::load (dir);
        int64_t diff = 0;
        for (int64_t c = 0; back != nullptr && c < (int64_t) cellRate * 3; ++c)
            if (! juce::exactlyEqual (back->readCell (0, c), tape.readCell (0, c))) ++diff;
        check (back != nullptr && diff == 0 && back->isRecorded (0, 20), "saved while recording: the tape on disk matches", (double) diff, 0.0);
        dir.deleteRecursively();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);   // let the writer's callbacks run while the tape exists
    }

    // ---- A plug-in that arrives after something else was chosen is thrown away ----
    {
        Rig m;
        auto& me = *m.engine;
        juce::PluginDescription ghost;
        ghost.pluginFormatName = "VST3";
        ghost.fileOrIdentifier = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("NoSuch.vst3").getFullPathName();
        ghost.name = "Ghost";
        juce::String result = "pending";
        me.clearChain (0);
        auto placeholder = me.addPlugin (0, -1, ghost, {}, 0, [&] (const juce::String& err) { result = err; });
        me.replaceBlock (0, placeholder, BuiltIn::createSynth (BuiltIn::epiano));   // chosen while the first was loading
        auto until = juce::Time::getMillisecondCounter() + 3000;
        while (result == "pending" && juce::Time::getMillisecondCounter() < until)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        check (result == "superseded" && firstName (me, 0) == BuiltIn::synthName (BuiltIn::epiano),
               "plug-in load race: the late arrival is discarded", result == "superseded", 1);

        // A plug-in's window is told to close while the instrument still exists.
        bool stillThere = false;
        me.onBlockRemoving = [&] (int t, int id) { stillThere = me.findBlock (t, id) != nullptr; };
        instrument (me, 0, BuiltIn::transistor);
        check (stillThere, "a block's window closes before the block goes", stillThere, 1);
        me.onBlockRemoving = nullptr;
    }

    // ---- More than 64 MIDI devices over time: old unplugged ones give up their slots ----
    {
        Rig m;
        auto& me = *m.engine;
        auto kept = me.addVirtualMidiInput ("Kept");
        me.setTrackMidiPort (0, kept);
        int failed = 0;
        for (int i = 0; i < 100; ++i)
            if (me.addVirtualMidiInput ("Controller " + juce::String (i)) < 0) ++failed;
        check (failed == 0 && me.midiPortLabel (me.track[0].midiPort.load()) == "KEPT",
               "MIDI: slots are recycled, never from a track that uses one", failed, 0);
    }

    // ---- Settings are only written when something changed ----
    {
        Rig m;
        auto& me = *m.engine;
        me.saveSettings();
        auto file = m.propsFile;
        auto old = juce::Time::getCurrentTime() - juce::RelativeTime::hours (1);
        file.setLastModificationTime (old);
        me.saveSettings();
        auto untouched = std::abs ((file.getLastModificationTime() - old).inSeconds()) < 2.0;
        me.track[0].fader = 0.5f;
        me.saveSettings();
        auto written = file.getLastModificationTime() > old + juce::RelativeTime::minutes (30);
        check (untouched && written, "settings: unchanged saves touch nothing on disk", untouched + 2 * written, 3);
    }

    // ---- The gain structure: inputs, plug-ins and the mono sum ----
    {
        Rig m;
        auto& me = *m.engine;
        me.track[0].armed = true;
        // an input: a 1 kHz sine peaking at 0.25
        audioIn (me, 0, 0);
        m.f[0] = 1000.0; m.amp[0] = 0.25f;
        m.run (0.3);
        const double viaInput = tone (m.run (0.3), 1000.0, 0.05, 0.3);
        m.amp[0] = 0.0f;
        // the same peak from a plug-in instrument: not cut any more, and summed to mono with equal power
        auto viaSynth = [&] (bool wide)
        {
            me.clearChain (0);
            me.setTrackInput (0, -1);
            me.setTrackMidiPort (0, AudioEngine::midiAny);
            me.insertBlock (0, -1, std::make_unique<ProcessorBlock> (std::make_unique<ToneSynth> (wide), ProcessorBlock::Kind::hosted));
            m.run (0.05);
            me.injectMidi (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
            auto out = m.run (0.4);
            me.injectMidi (juce::MidiMessage::noteOff (1, 60));
            m.run (0.1);
            return tone (out, 1000.0, 0.1, 0.4);
        };
        const double wide = viaSynth (true), centred = viaSynth (false);
        check (std::abs (db (wide / viaInput)) < 0.2, "gain: a plug-in as wide as stereo gets comes in at an input's level (dB)", db (wide / viaInput), 0.0);
        check (std::abs (db (centred / viaInput) - 3.01) < 0.2, "gain: a plug-in the same on both sides, 3 dB up (equal-power mono sum)", db (centred / viaInput), 3.01);
        // and its block's meter reads what it sends
        auto* blk = me.findBlock (0, me.getChain (0)[0].id);
        me.injectMidi (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
        blk->sendPeak = 0.0f;
        m.run (0.2);
        const float sent = blk->sendPeak.load();
        me.injectMidi (juce::MidiMessage::noteOff (1, 60));
        check (std::abs (db (sent / (ToneSynth::peak * std::sqrt (2.0)))) < 0.3, "gain: the block's meter reads what it sends into the track (dB)",
               db (sent / (ToneSynth::peak * std::sqrt (2.0))), 0.0);
    }

    // ---- The chain: MIDI and audio streams through blocks ----
    {
        Rig m;
        auto& me = *m.engine;
        auto hostedBlock = [] (std::unique_ptr<juce::AudioProcessor> p) { return std::make_unique<ProcessorBlock> (std::move (p), ProcessorBlock::Kind::hosted); };
        auto playA = [&] (double secs)
        {
            m.run (0.05);
            me.injectMidi (juce::MidiMessage::noteOn (1, 57, (juce::uint8) 110));   // A220
            auto out = m.run (secs);
            me.injectMidi (juce::MidiMessage::noteOff (1, 57));
            m.run (0.6);
            return out;
        };

        // A MIDI effect changes the notes every block after it hears...
        me.clearChain (0);
        me.setTrackInput (0, -1);
        me.insertBlock (0, -1, hostedBlock (std::make_unique<Transposer>()));
        me.insertBlock (0, -1, BuiltIn::createSynth (BuiltIn::epiano));
        me.track[0].armed = true;
        auto up = playA (0.4);
        check (tone (up, 440, 0.1, 0.4) > 4.0 * tone (up, 220, 0.1, 0.4), "chain: a MIDI effect before a synth changes what it plays", tone (up, 440, 0.1, 0.4), 0.0);
        check (me.getChain (0)[0].role == BlockRole::midiFx && me.getChain (0)[1].role == BlockRole::instrument, "chain: blocks know their roles", 1, 1);

        // ...but not the blocks before it.
        me.moveBlock (0, me.getChain (0)[0].id, 1);
        auto same = playA (0.4);
        check (tone (same, 220, 0.1, 0.4) > 4.0 * tone (up, 220, 0.1, 0.4), "chain: a MIDI effect after the synth leaves it alone", tone (same, 220, 0.1, 0.4), 0.0);

        // Two instruments layer: both play, mixed. Two organs, one on 8' and one on 4', so each is heard at its own pitch
        // (a square wave has no 2nd harmonic: 440 Hz is only the second organ's).
        auto organOn = [] (std::unique_ptr<Block> b, bool fourFoot)
        {
            auto* o = BuiltIn::transistorOrgan (b.get());
            auto s = o->settings();
            s.drawbar16 = 0.0f; s.drawbarIV = 0.0f; s.flute = 0.0f; s.reed = 8.0f; s.vibrato = false;
            s.drawbar8 = fourFoot ? 0.0f : 8.0f; s.drawbar4 = fourFoot ? 8.0f : 0.0f;
            o->setSettings (s);
            return b;
        };
        me.clearChain (0);
        me.insertBlock (0, -1, organOn (BuiltIn::createSynth (BuiltIn::transistor), false));
        auto one = playA (0.4);
        me.insertBlock (0, -1, organOn (BuiltIn::createSynth (BuiltIn::transistor), true));
        auto two = playA (0.4);
        check (tone (two, 440, 0.1, 0.4) > 10.0 * tone (one, 440, 0.1, 0.4) && tone (two, 220, 0.1, 0.4) > 0.7 * tone (one, 220, 0.1, 0.4),
               "chain: two instruments in a row are layered (both heard: the second's 440 Hz re the first alone)",
               tone (two, 440, 0.1, 0.4) / std::max (1.0e-9, tone (one, 440, 0.1, 0.4)), 10.0);

        // An audio effect processes what reaches it; MIX blends it with the dry signal; BYPASS takes it out.
        audioIn (me, 1, 0);
        me.track[1].armed = true;
        me.track[0].armed = false;
        m.f[0] = 700; m.amp[0] = 0.2f;
        auto gid = me.insertBlock (1, -1, hostedBlock (std::make_unique<Gain>()));
        auto half = tone (m.run (0.4), 700, 0.15, 0.4);
        me.findBlock (1, gid)->mix = 0.5f;
        auto blend = tone (m.run (0.4), 700, 0.15, 0.4);
        me.setBlockBypass (1, gid, true);
        auto bypassed = tone (m.run (0.4), 700, 0.15, 0.4);
        check (std::abs (half / bypassed - 0.5) < 0.02 && std::abs (blend / bypassed - 0.75) < 0.02,
               "chain: an effect processes the input; MIX 50% blends; bypass passes it", blend / bypassed, 0.75);

        // A plug-in that wipes the MIDI it's given (and makes none) doesn't silence what comes after.
        me.clearChain (0);
        me.track[0].armed = true;
        me.track[1].armed = false;
        m.amp[0] = 0;
        me.insertBlock (0, -1, hostedBlock (std::make_unique<Gain>()));
        me.insertBlock (0, -1, BuiltIn::createSynth (BuiltIn::epiano));
        check (rms (playA (0.4), 0.1, 0.4) > 0.005, "chain: MIDI passes blocks that don't make MIDI", 1, 1);
        me.track[0].armed = false;

        // A plug-in with its window open is asked for its sound at most once a minute by the regular
        // saves, but always at the natural moments (fresh saves: stop, tape save, quit, window closed).
        {
            auto id = me.insertBlock (3, -1, hostedBlock (std::make_unique<Gain>()));
            auto* pb = dynamic_cast<ProcessorBlock*> (me.findBlock (3, id));
            pb->setEditorOpen (true);
            for (int i = 0; i < 12; ++i) me.createDeckState (false);   // a minute of 5-second saves, quickly
            const int regular = pb->stateRequests;
            me.createDeckState (true);
            pb->setEditorOpen (false);
            pb->captureState();
            check (regular == 1 && pb->stateRequests == 3,
                   "plug-in state: an open window isn't asked every save, only once a minute and at natural moments",
                   regular, 1);
            me.clearChain (3);
        }

        // A chain nobody hears (its track unarmed, the fade back to tape over) is left alone, and
        // picks up again when the track is armed.
        {
            audioIn (me, 2, 0);
            auto counting = std::make_unique<Counting>();
            auto* c = counting.get();
            me.insertBlock (2, -1, hostedBlock (std::move (counting)));
            me.track[2].armed = true;
            m.run (0.1);
            me.track[2].armed = false;
            m.run (0.1);                       // the fade back to tape
            const int settled = c->blocks.load();
            m.run (0.5);
            const int whileUnheard = c->blocks.load() - settled;
            me.track[2].armed = true;
            m.run (0.1);
            check (settled > 0 && whileUnheard == 0 && c->blocks.load() > settled,
                   "chains: an unarmed track's chain rests, and runs again when armed", whileUnheard, 0);
            me.track[2].armed = false;
            me.clearChain (2);
        }

        // A plug-in being restarted or reconfigured on another thread holds its callback lock, or is
        // suspended: the audio thread must leave it alone until it's done.
        {
            audioIn (me, 2, 0);
            me.track[2].armed = true;
            auto counting = std::make_unique<Counting>();
            auto* c = counting.get();
            me.insertBlock (2, -1, hostedBlock (std::move (counting)));
            m.run (0.1);
            const int running = c->blocks.load();
            c->suspendProcessing (true);
            m.run (0.1);
            const int whileSuspended = c->blocks.load() - running;
            c->suspendProcessing (false);
            int whileLocked = 0;
            {
                // The lock is held by another thread, as the message thread does during a restart.
                juce::WaitableEvent locked, release;
                std::thread other ([&] { const juce::ScopedLock busy (c->getCallbackLock()); locked.signal(); release.wait (-1); });
                locked.wait (-1);
                const int before = c->blocks.load();
                m.run (0.1);
                whileLocked = c->blocks.load() - before;
                release.signal();
                other.join();
            }
            const int before = c->blocks.load();
            m.run (0.1);
            check (running > 0 && whileSuspended == 0 && whileLocked == 0 && c->blocks.load() > before,
                   "plug-ins: never called while suspended or held by another thread (restart race)", whileSuspended + whileLocked, 0);
            me.track[2].armed = false;
            me.clearChain (2);
        }

        // The track displays rebuild their text only when what they show changes.
        {
            auto v0 = me.viewVersion.load();
            auto id = me.insertBlock (3, -1, BuiltIn::createFx (BuiltIn::plate));
            auto v1 = me.viewVersion.load();
            me.setBlockBypass (3, id, true);
            auto v2 = me.viewVersion.load();
            me.setTrackInput (3, 1);
            auto v3 = me.viewVersion.load();
            m.run (0.1);
            auto v4 = me.viewVersion.load();
            me.removeBlock (3, id);
            check (v1 > v0 && v2 > v1 && v3 > v2 && v4 == v3 && me.viewVersion.load() > v4,
                   "track display: the view version moves with edits, not with playing", (int) (v4 - v3), 0);
        }

        // The front panel reads the chain 30 times a second per strip; the plug-in is asked once.
        me.insertBlock (2, -1, hostedBlock (std::make_unique<Nosy>()));
        Nosy::editorQuestions = 0;
        Nosy::nameQuestions = 0;
        for (int frame = 0; frame < 300; ++frame)
        {
            for (int t = 0; t < numTracks; ++t)
            {
                me.getChain (t);
                me.describeSource (t);
                me.blockAbbreviations (t);
            }
        }
        check (Nosy::editorQuestions.load() == 0 && Nosy::nameQuestions.load() == 0,
               "chain: reading the chain doesn't keep asking the plug-in (hasEditor makes a VST3 view)",
               Nosy::editorQuestions.load() + Nosy::nameQuestions.load(), 0);
        me.clearChain (2);
    }

    // ---- CLAP plug-ins (TAPE·FOUR's own host) ----
    {
        Rig m;
        auto& me = *m.engine;
        juce::OwnedArray<juce::PluginDescription> found;
        ClapPluginFormat clapFormat;
        clapFormat.findAllTypesForFile (found, TAPEFOUR_TEST_CLAP);
        const juce::PluginDescription* half = nullptr;
        const juce::PluginDescription* toneSynth = nullptr;
        for (auto* d : found)
        {
            if (d->name == "TF Test Half") half = d;
            if (d->name == "TF Test Tone") toneSynth = d;
        }
        check (half != nullptr && toneSynth != nullptr && half->pluginFormatName == "CLAP",
               "clap: scanning a .clap file finds both plug-ins in it", found.size(), 2);
        if (half == nullptr || toneSynth == nullptr) return 1;
        check (toneSynth->isInstrument && ! half->isInstrument && half->uniqueId != toneSynth->uniqueId,
               "clap: instrument or effect comes from its features, each with its own id", toneSynth->isInstrument, 1);

        auto load = [&] (int t, const juce::PluginDescription& d, const juce::String& state = {})
        {
            juce::String result = "pending";
            me.addPlugin (t, -1, d, state, 0, [&] (const juce::String& err) { result = err; });
            auto until = juce::Time::getMillisecondCounter() + 5000;
            while (result == "pending" && juce::Time::getMillisecondCounter() < until)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
            return result;
        };
        auto waitForLoads = [&]
        {
            auto until = juce::Time::getMillisecondCounter() + 5000;
            auto pending = [&] { for (int t = 0; t < numTracks; ++t) for (auto& b : me.getChain (t)) if (b.role == BlockRole::pending) return true; return false; };
            while (pending() && juce::Time::getMillisecondCounter() < until)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        };

        // An effect: it processes the input, and its parameter works.
        audioIn (me, 1, 0);
        me.track[1].armed = true;
        m.f[0] = 700; m.amp[0] = 0.2f;
        auto dry = tone (m.run (0.4), 700, 0.15, 0.4);
        auto err = load (1, *half);
        check (err.isEmpty() && firstName (me, 1) == "TF Test Half", "clap: an effect loads into a chain", err.isEmpty(), 1);
        auto halved = tone (m.run (0.4), 700, 0.15, 0.4);
        check (std::abs (halved / dry - 0.5) < 0.02, "clap: the effect processes the track's audio (gain 0.5)", halved / dry, 0.5);

        auto* block = me.findBlock (1, me.getChain (1)[0].id);
        auto* gainParam = block->getProcessor()->getParameters()[0];
        check (gainParam->getName (20) == "Gain" && gainParam->getText (0.25f, 20) == "2/10",
               "clap: parameters show the plug-in's names and text", gainParam->getName (20) == "Gain", 1);
        gainParam->setValueNotifyingHost (0.25f);
        auto quarter = tone (m.run (0.4), 700, 0.15, 0.4);
        check (std::abs (quarter / dry - 0.25) < 0.02, "clap: setting a parameter reaches the plug-in", quarter / dry, 0.25);

        // Its state goes out with the deck and comes back.
        auto deck = me.createDeckState (true);
        gainParam->setValueNotifyingHost (1.0f);
        m.run (0.2);
        me.applyDeckState (*deck);
        waitForLoads();
        auto restored = tone (m.run (0.4), 700, 0.15, 0.4);
        check (std::abs (restored / dry - 0.25) < 0.02, "clap: the plug-in's saved state comes back with the deck", restored / dry, 0.25);
        me.track[1].armed = false;
        m.amp[0] = 0;

        // An instrument that only takes CLAP note events: MIDI notes are translated.
        me.clearChain (0);
        me.setTrackInput (0, -1);
        err = load (0, *toneSynth);
        me.track[0].armed = true;
        m.run (0.05);
        me.injectMidi (juce::MidiMessage::noteOn (1, 57, (juce::uint8) 127));   // A220
        auto played = m.run (0.4);
        me.injectMidi (juce::MidiMessage::noteOff (1, 57));
        auto after = m.run (0.4);
        check (err.isEmpty() && tone (played, 220, 0.1, 0.4) > 0.01 && tone (after, 220, 0.15, 0.4) < 0.001,
               "clap: an instrument plays MIDI notes (as CLAP note events) and stops on note-off", tone (played, 220, 0.1, 0.4), 0.05);
        me.track[0].armed = false;
        me.clearChain (0);
        me.clearChain (1);
    }

    // ---- Real plug-ins, when asked for: TAPEFOUR_SMOKE_DIR=<folder of plug-ins> ----
    // Every format scans the folder; each instrument found is loaded and played, each effect loaded.
    if (auto dir = juce::SystemStats::getEnvironmentVariable ("TAPEFOUR_SMOKE_DIR", {}); dir.isNotEmpty())
    {
        Rig m;
        auto& me = *m.engine;
        for (auto* format : me.formatManager.getFormats())
        {
            for (auto& id : format->searchPathsForPlugins (juce::FileSearchPath (dir), true, false))
            {
                juce::OwnedArray<juce::PluginDescription> types;
                format->findAllTypesForFile (types, id);
                for (auto* d : types)
                {
                    me.clearChain (0);
                    me.setTrackInput (0, -1);
                    juce::String result = "pending";
                    me.addPlugin (0, -1, *d, {}, 0, [&] (const juce::String& err) { result = err; });
                    auto until = juce::Time::getMillisecondCounter() + 30000;
                    while (result == "pending" && juce::Time::getMillisecondCounter() < until)
                        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
                    auto name = format->getName() + " " + d->name;
                    if (result.isNotEmpty()) { check (false, ("smoke: " + name + " loads: " + result).toRawUTF8(), 0, 1); continue; }
                    auto chain = me.getChain (0);
                    if (chain.empty() || chain[0].role != BlockRole::instrument)
                    {
                        check (true, ("smoke: " + name + " loads (" + juce::String (roleName (chain.empty() ? BlockRole::pending : chain[0].role)) + ")").toRawUTF8(), 1, 1);
                        continue;
                    }
                    me.track[0].armed = true;
                    m.run (0.3);
                    me.injectMidi (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110));
                    auto out = m.run (1.0);
                    me.injectMidi (juce::MidiMessage::noteOff (1, 60));
                    m.run (0.3);
                    me.track[0].armed = false;
                    check (rms (out, 0.2, 1.0) > 1.0e-4, ("smoke: " + name + " plays a note").toRawUTF8(), rms (out, 0.2, 1.0), 0.01);
                }
            }
        }
        me.clearChain (0);
    }

    // ---- Mute and solo ----
    {
        Rig m;
        auto& me = *m.engine;
        audioIn (me, 0, 0);
        audioIn (me, 1, 1);
        m.f[0] = 500; m.amp[0] = 0.3f;
        m.f[1] = 900; m.amp[1] = 0.3f;
        me.track[0].armed = me.track[1].armed = true;
        auto heard = [&] (double hz) { return tone (m.run (0.3), hz, 0.12, 0.3); };
        auto a0 = heard (500), b0 = heard (900);
        me.track[0].solo = true;
        auto a1 = heard (500), b1 = heard (900);
        check (a1 > 0.9 * a0 && b1 < 0.001, "solo: only the soloed track is heard", b1, 0.0);
        me.track[0].mute = true;
        auto a2 = heard (500), b2 = heard (900);
        check (a2 < 0.001 && b2 < 0.001 && me.soloedTracksAllMuted(), "solo + mute: mute wins (and the deck knows why it's silent)", a2, 0.0);
        me.track[0].solo = false;
        auto a3 = heard (500), b3 = heard (900);
        check (a3 < 0.001 && b3 > 0.9 * b0, "mute: the track is silent, the others play", a3, 0.0);

        // A muted track still records.
        m.cmd (TapeEngine::Command::rec);
        m.run (1.0);
        m.cmd (TapeEngine::Command::stop);
        m.run (0.05);
        double onTape = 0;
        for (int64_t c = cellRate / 4; c < cellRate * 3 / 4; ++c) onTape = std::max (onTape, (double) std::abs (me.getTape()->readCell (0, c)));
        check (onTape > 0.1, "mute: a muted, armed track still records", onTape, 0.3);

        // Mute takes a source out of a bounce; solo doesn't.
        me.track[0].mute = false;
        me.track[0].armed = me.track[1].armed = false;
        m.amp[0] = m.amp[1] = 0;
        me.toggleBounceSource (2, 0);
        me.toggleBounceSource (2, 1);
        me.track[2].armed = true;
        me.track[2].pan = 1.0f;   // the bounce itself only to the right: what's left is the sources
        me.track[1].solo = true;
        auto solo = m.run (0.05);
        juce::ignoreUnused (solo);
        m.cmd (TapeEngine::Command::rtz);
        m.run (1.0);
        me.track[0].mute = true;
        m.cmd (TapeEngine::Command::rec);
        m.run (0.8);
        m.cmd (TapeEngine::Command::stop);
        m.run (0.05);
        // On tape, track 3 now holds track 2 (soloed or not) and not the muted track 1.
        std::vector<float> cells;
        for (int64_t c = cellRate / 4; c < cellRate / 2; ++c) cells.push_back (me.getTape()->readCell (2, c));
        auto at = [&] (double hz)
        {
            double re = 0, im = 0;
            for (size_t i = 0; i < cells.size(); ++i) { auto w = juce::MathConstants<double>::twoPi * hz * (double) i / cellRate; re += cells[i] * std::cos (w); im += cells[i] * std::sin (w); }
            return 2.0 * std::sqrt (re * re + im * im) / (double) cells.size();
        };
        check (at (900) > 0.05 && at (500) < 0.01, "bounce: a muted source is left out; solo doesn't change a bounce", at (500), 0.0);
        me.track[0].mute = me.track[1].solo = false;
    }

    // ---- MIDI learn ----
    {
        Rig m;
        auto& me = *m.engine;
        auto knobs = me.addVirtualMidiInput ("Knobs");
        me.setTrackMidiPort (0, knobs);
        auto cc = [&] (int number, int value) { me.injectMidi (juce::MidiMessage::controllerEvent (1, number, value), knobs); };
        auto learn = [&] (MidiMapper::Target target, std::function<void()> move)
        {
            me.mapper.beginLearn (target);
            m.run (0.02);
            move();
            m.run (0.05);
            for (int i = 0; i < 40 && me.mapper.isLearning(); ++i) { juce::Thread::sleep (10); m.run (0.01); me.mapper.poll(); }
            return ! me.mapper.isLearning();
        };

        MidiMapper::Target fader1; fader1.control = MidiMapper::Control::fader; fader1.track = 0;
        auto learned = learn (fader1, [&] { for (int v : { 90, 92, 94, 96 }) cc (21, v); });
        auto maps = me.mapper.getMappings();
        check (learned && maps.size() == 1 && maps[0].number == 21 && maps[0].mode == MidiMapper::Mode::absolute,
               "learn: moving a knob maps CC 21 to track 1's fader", maps.empty() ? -1 : maps[0].number, 21);

        // Pickup: the fader is at 0.75; a knob at 20 doesn't grab it, sweeping past 0.75 does.
        me.track[0].fader = 0.75f;
        cc (21, 20); m.run (0.05);
        check (std::abs (me.track[0].fader.load() - 0.75f) < 1e-4f, "pickup: a knob away from the fader doesn't make it jump", me.track[0].fader.load(), 0.75);
        cc (21, 110); m.run (0.05);
        cc (21, 64); m.run (0.05);
        check (std::abs (me.track[0].fader.load() - 64.0f / 127.0f) < 1e-3f, "pickup: once past the fader, the knob moves it", me.track[0].fader.load(), 64.0 / 127.0);

        // A mapped CC doesn't reach the instruments; an unmapped one does.
        auto counter = std::make_unique<CcCounter>();
        auto* counted = counter.get();
        me.insertBlock (0, -1, std::make_unique<ProcessorBlock> (std::move (counter), ProcessorBlock::Kind::hosted));
        me.track[0].armed = true;
        cc (21, 70); cc (22, 70); m.run (0.05);
        check (counted->ccs.load() == 1, "learn: a mapped CC is used by its mapping, not played", counted->ccs.load(), 1);
        me.track[0].armed = false;

        // An endless encoder is recognised and moves the control in steps.
        MidiMapper::Target pan1; pan1.control = MidiMapper::Control::pan; pan1.track = 0;
        learn (pan1, [&] { for (int i = 0; i < 4; ++i) cc (30, 1); });
        bool relative = false;
        for (auto& mp : me.mapper.getMappings()) if (mp.number == 30) relative = mp.mode == MidiMapper::Mode::relTwos;
        me.track[0].pan = 0.0f;
        for (int i = 0; i < 10; ++i) cc (30, 1);
        m.run (0.05);
        check (relative && me.track[0].pan.load() > 0.15f && me.track[0].pan.load() < 0.25f, "learn: an encoder turns the pan in steps", me.track[0].pan.load(), 0.2);

        // A button toggles mute.
        MidiMapper::Target mute2; mute2.control = MidiMapper::Control::mute; mute2.track = 1;
        learn (mute2, [&] { me.injectMidi (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100), knobs); });
        me.injectMidi (juce::MidiMessage::noteOff (1, 36), knobs); m.run (0.05);
        me.track[1].mute = false;
        me.injectMidi (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100), knobs); m.run (0.05);
        me.injectMidi (juce::MidiMessage::noteOff (1, 36), knobs); m.run (0.05);
        check (me.track[1].mute.load(), "learn: a pad toggles track 2's mute", me.track[1].mute.load(), 1);

        // MMC transport buttons need no learning.
        const juce::uint8 play[] { 0xf0, 0x7f, 0x7f, 0x06, 0x02, 0xf7 };
        me.remoteCommand = 0;
        me.injectMidi (juce::MidiMessage (play, 6), knobs); m.run (0.05);
        check (me.remoteCommand.load() == (int) MidiMapper::Control::play + 1, "MMC: a controller's PLAY button plays", me.remoteCommand.load(), (int) MidiMapper::Control::play + 1);

        // Deck mappings survive a restart (kept with the settings).
        me.saveSettings();
        auto count = me.mapper.getMappings().size();
        m.engine.reset();
        m.engine = std::make_unique<AudioEngine> (*m.props);
        check (m.engine->mapper.getMappings().size() == count, "learn: deck mappings come back after a restart", (double) m.engine->mapper.getMappings().size(), (double) count);
    }

    // ---- Plug-in parameter mappings travel with the deck ----
    {
        Rig m;
        auto& me = *m.engine;
        auto knobs = me.addVirtualMidiInput ("Knobs");
        me.clearChain (0);
        auto plate = me.insertBlock (0, -1, BuiltIn::createFx (BuiltIn::plate));
        MidiMapper::Target size; size.plugin = true; size.track = 0; size.blockId = plate; size.param = 0;
        auto* param = me.findBlock (0, plate)->getProcessor()->getParameters()[0];
        me.mapper.beginLearn (size, param, "Size");
        m.run (0.02);
        for (int v : { 10, 12, 14, 16 }) me.injectMidi (juce::MidiMessage::controllerEvent (1, 40, v), knobs);
        m.run (0.05);
        for (int i = 0; i < 40 && me.mapper.isLearning(); ++i) { juce::Thread::sleep (10); m.run (0.01); me.mapper.poll(); }
        auto deck = me.createDeckState();
        me.applyDeckState (*deck);
        auto restored = me.getChain (0);
        bool back = false;
        for (auto& mp : me.mapper.getMappings())
            back = back || (mp.target.plugin && mp.target.blockId == restored[0].id && mp.number == 40 && mp.param != nullptr);
        check (restored.size() == 1 && restored[0].name == BuiltIn::fxName (BuiltIn::plate) && back,
               "learn: a plug-in parameter's mapping is saved with the deck and comes back", back, 1);
    }

    // ---- Decks saved before chains still load ----
    {
        Rig m;
        auto& me = *m.engine;
        auto old = juce::parseXML ("<DECK version=\"3\">"
                                   "<TRACK index=\"0\" source=\"2\" midiIn=\"any\"><INSTRUMENT kind=\"builtin\" which=\"1\"/></TRACK>"
                                   "<TRACK index=\"1\" source=\"1\" input=\"1\" pair=\"1\"/>"
                                   "<TRACK index=\"2\" source=\"4\" input=\"0\" midiOutChannel=\"3\"/>"
                                   "<TRACK index=\"3\" source=\"3\" bounce=\"3\"/></DECK>");
        me.applyDeckState (*old);
        auto c0 = me.getChain (0), c2 = me.getChain (2);
        check (c0.size() == 1 && c0[0].name == BuiltIn::synthName (BuiltIn::epiano)
               && me.track[1].inputChannel.load() == 1 && me.track[1].inputPair.load()
               && c2.size() == 1 && c2[0].role == BlockRole::external && c2[0].channel == 3 && c2[0].returnInput == 0
               && me.track[3].feed.load() == (int) AudioEngine::Feed::bounce && me.track[3].bounceMask.load() == 3,
               "old decks: instrument, audio in, hardware synth and bounce become chains", 1, 1);
    }

    // ---- The machine's electronics: each channel's noise follows its fader and pan; the bus's doesn't ----
    {
        auto noiseDb = [] (int open, float panTo, float channelDb, float busDb)
        {
            Rig nr;
            nr.engine->channelNoiseDb = channelDb;
            nr.engine->busNoiseDb = busDb;
            for (int t = 0; t < numTracks; ++t) { nr.engine->track[t].mute = t >= open; nr.engine->track[t].pan = panTo; }
            auto out = nr.run (1.5);   // the left output
            double p = 0;
            for (size_t i = out.size() / 3; i < out.size(); ++i) p += (double) out[i] * out[i];
            p /= (double) (out.size() - out.size() / 3);
            const double vu = tapeRefPeak / std::sqrt (2.0);
            return 10.0 * std::log10 (p / (vu * vu));
        };
        const auto one = noiseDb (1, 0.0f, -76.0f, -140.0f), four = noiseDb (4, 0.0f, -76.0f, -140.0f);
        check (std::abs (one - (-79.0)) < 1.0, "electronics: one channel at nominal, -76 dB re 0 VU (half in each side)", one, -79.0);
        check (std::abs ((four - one) - 6.0) < 1.0, "electronics: four channels open, 6 dB more (the 424's 76 and 70 dB)", four - one, 6.0);
        const auto right = noiseDb (1, 1.0f, -76.0f, -140.0f);
        check (right < -110.0, "electronics: a channel's noise pans with it (hard right: none on the left)", right, -140.0);
        const auto bus = noiseDb (0, 0.0f, -76.0f, -90.0f);
        check (std::abs (bus - (-90.0)) < 1.0, "electronics: the bus's own, whatever the channels do", bus, -90.0);
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "SOME FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
