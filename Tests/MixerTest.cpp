// Drives the whole audio path (inputs -> strips -> tape -> mix) with a fake audio device.
#include "../Source/AudioEngine.h"
#include "../Source/TapeWriter.h"
#include <cstdio>

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
    // Each pass through the tape changes the level by the cassette's sensitivity.
    const double gen = std::pow (10.0, tapeModels[defaultTapeModel].sensitivity / 20.0);

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
        me.setInstrument (0, BuiltIn::create (BuiltIn::epiano));
        me.track[0].armed = true;                  // armed + stopped: you hear the instrument live
        m.run (0.1);
        me.injectMidi (juce::MidiMessage::pitchWheel (2, 8192 + 8192 * 12 / 48));
        me.injectMidi (juce::MidiMessage::noteOn (2, 69, (juce::uint8) 110));
        auto bent = m.run (0.6);
        auto at880 = tone (bent, 880, 0.2, 0.5), at440 = tone (bent, 440, 0.2, 0.5);
        check (at880 > 4.0 * at440 && at880 > 0.005, "MPE: per-note bend on channel 2 moves A440 up an octave", at880, 4.0 * at440);

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
        check (again->getInstrument (0) != nullptr && again->getInstrument (0)->getName() == BuiltIn::nameOf (BuiltIn::epiano),
               "settings: track 1 instrument restored after restart", 1, 1);
        again.reset();
    }


    // EQ: the shelves are heard where a portastudio's are (live input, armed, stopped)
    {
        Rig m;
        auto& me = *m.engine;
        me.setAudioSource (0, 0);
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
        me.setAudioSource (0, 0);
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
        me.setInstrument (0, BuiltIn::create (BuiltIn::poly));
        me.setInstrument (1, BuiltIn::create (BuiltIn::poly));
        me.setInstrument (2, BuiltIn::create (BuiltIn::poly));
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
        auto synth = me.addVirtualMidiOutput ("Juno");
        std::vector<juce::MidiMessage> sent;
        juce::SpinLock sentLock;
        me.midiOutTap = [&] (int slot, const juce::MidiBuffer& b)
        {
            if (slot != synth) return;
            const juce::SpinLock::ScopedLockType sl (sentLock);
            for (const auto meta : b) sent.push_back (meta.getMessage());
        };
        me.track[0].armed = false;
        me.track[1].armed = false;
        me.setAudioSource (2, 0);
        me.setHardwareSource (2);
        check (me.getInstrument (2) == nullptr, "hardware synth replaces the track's instrument (never both)", me.getInstrument (2) != nullptr, 0);
        me.setTrackMidiPort (2, keysA);
        me.track[2].midiOutPort = synth;
        me.track[2].midiOutChannel = 5;
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
        me.track[2].midiOutChannel = 7;
        m.run (0.05);
        int offs = 0;
        for (auto& msg : sent) if (msg.isAllNotesOff()) ++offs;
        check (offs == 16, "hardware: re-routing silences the old route (all notes off, 16 channels)", offs, 16);
        me.midiOutTap = nullptr;

        // Deck state: everything savable goes out and comes back.
        me.track[0].fader = 0.31f; me.track[0].pan = 0.4f; me.track[0].high = -0.5f;
        me.masterFader = 0.6f;
        me.tapeEngine.params.pitchStep = 2;
        auto deck = me.createDeckState();
        me.track[0].fader = 0.9f; me.track[0].pan = -0.8f; me.track[0].high = 0.7f;
        me.masterFader = 0.2f;
        me.tapeEngine.params.pitchStep = 0;
        me.unplug (2);
        me.setTrackMidiPort (0, AudioEngine::midiNone);
        me.applyDeckState (*deck);
        check (std::abs (me.track[0].fader.load() - 0.31f) < 1e-4f && std::abs (me.track[0].pan.load() - 0.4f) < 1e-4f
               && std::abs (me.track[0].high.load() + 0.5f) < 1e-4f, "deck recall: fader, pan and EQ come back", me.track[0].fader.load(), 0.31);
        check (std::abs (me.masterFader.load() - 0.6f) < 1e-4f && me.tapeEngine.params.pitchStep.load() == 2,
               "deck recall: master and pitch come back", me.masterFader.load(), 0.6);
        check (me.track[2].source.load() == (int) Source::hardware && me.track[2].midiOutPort.load() == synth
               && me.track[2].midiOutChannel.load() == 7, "deck recall: hardware synth route comes back", me.track[2].midiOutChannel.load(), 7);
        check (me.track[0].midiPort.load() == keysA && me.track[1].midiPort.load() == keysB, "deck recall: each track's controller comes back",
               me.track[0].midiPort.load(), keysA);
        check (me.getInstrument (1) != nullptr && me.getInstrument (1)->getName() == BuiltIn::nameOf (BuiltIn::poly),
               "deck recall: instruments come back", 1, 1);

        // ...and the deck travels with the tape.
        auto& tape = *me.getTape();
        tape.deckState = me.createDeckState();
        tape.deckNeedsWriting = true;
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tf_deck_tape");
        dir.deleteRecursively();
        dir.createDirectory();
        tape.save (dir);
        auto loaded = Tape::load (dir);
        check (loaded != nullptr && loaded->deckState != nullptr && loaded->deckState->isEquivalentTo (tape.deckState.get(), false),
               "deck state is saved with the tape (deck.xml)", loaded != nullptr && loaded->deckState != nullptr, 1);
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
            for (int c = 0; c < 16; ++c) tape.writeCell (t, at + c, 0.4f, false, 1.0f, 0);
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
            me.setAudioSource (1, 0);
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
        check (AudioEngine::recordLatencySamples (Source::instrument, 1200, 900, 48) == 900.0
               && AudioEngine::recordLatencySamples (Source::bounce, 1200, 900, 48) == 0.0
               && AudioEngine::recordLatencySamples (Source::audio, 1200, 900, 48) == 2148.0
               && AudioEngine::recordLatencySamples (Source::hardware, 1200, 900, 48) == 2148.0,
               "latency: instruments wait for the output only; audio and synths for the round trip", 900, 900);
        fakeInputLatency = fakeOutputLatency = 0;
    }

    // ---- Controls glide; arming crossfades ----
    {
        Rig m;
        auto& me = *m.engine;
        me.setAudioSource (0, 0);
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
        me.setAudioSource (0, 0);
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
            if (back->readCell (0, c) != tape.readCell (0, c)) ++diff;
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
        me.loadPlugin (0, ghost, {}, [&] (const juce::String& err) { result = err; });
        me.setInstrument (0, BuiltIn::create (BuiltIn::epiano));   // chosen while the first was loading
        auto until = juce::Time::getMillisecondCounter() + 3000;
        while (result == "pending" && juce::Time::getMillisecondCounter() < until)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        check (result == "superseded" && me.getInstrument (0) != nullptr && me.getInstrument (0)->getName() == BuiltIn::nameOf (BuiltIn::epiano),
               "plug-in load race: the late arrival is discarded", result == "superseded", 1);

        // A plug-in's window is told to close while the instrument still exists.
        bool stillThere = false;
        me.onInstrumentChanging = [&] (int t) { stillThere = me.getInstrument (t) != nullptr; };
        me.setInstrument (0, BuiltIn::create (BuiltIn::poly));
        check (stillThere, "an instrument's window closes before the instrument goes", stillThere, 1);
        me.onInstrumentChanging = nullptr;
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

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "SOME FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
