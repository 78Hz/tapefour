// Everything about the deck that is saved and recalled, in one place:
// the deck state (what you set by hand; saved in the settings and with every tape),
// the hardware state (outputs, latency trim, MIDI devices; settings only), and the settings file.
#include "AudioEngine.h"

namespace tf
{
std::unique_ptr<juce::XmlElement> AudioEngine::createDeckState (bool freshPluginState)
{
    auto deck = std::make_unique<juce::XmlElement> ("DECK");
    deck->setAttribute ("version", 3);

    for (int t = 0; t < numTracks; ++t)
    {
        auto& p = track[t];
        auto* x = deck->createNewChildElement ("TRACK");
        x->setAttribute ("index", t);
        x->setAttribute ("source", p.source.load());
        x->setAttribute ("input", p.inputChannel.load());
        x->setAttribute ("pair", p.inputPair.load());
        x->setAttribute ("gain", p.gain.load());
        x->setAttribute ("high", p.high.load());
        x->setAttribute ("mid", p.mid.load());
        x->setAttribute ("midFreq", p.midFreq.load());
        x->setAttribute ("low", p.low.load());
        x->setAttribute ("pan", p.pan.load());
        x->setAttribute ("fader", p.fader.load());
        x->setAttribute ("bounce", p.bounceMask.load());

        auto port = p.midiPort.load();
        if (port == midiAny)                 x->setAttribute ("midiIn", "any");
        else if (! midi.inputExists (port))  x->setAttribute ("midiIn", "none");
        else { x->setAttribute ("midiIn", "port"); x->setAttribute ("midiInId", midi.inputId (port)); x->setAttribute ("midiInName", midi.inputName (port)); }

        auto outPort = p.midiOutPort.load();
        if (midi.outputExists (outPort))
        {
            x->setAttribute ("midiOutId", midi.outputId (outPort));
            x->setAttribute ("midiOutName", midi.outputName (outPort));
        }
        x->setAttribute ("midiOutChannel", p.midiOutChannel.load());

        if (slots[t].inst != nullptr)
            x->addChildElement (slots[t].inst->saveState (freshPluginState).release());
        else if (pendingInstrument[t] != nullptr)
            x->addChildElement (new juce::XmlElement (*pendingInstrument[t]));
    }

    auto* m = deck->createNewChildElement ("MASTER");
    m->setAttribute ("fader", masterFader.load());
    m->setAttribute ("phones", phones.load());

    auto& tp = tapeEngine.params;
    auto* x = deck->createNewChildElement ("TAPE_DECK");
    x->setAttribute ("speed", tp.speedSel.load());
    x->setAttribute ("pitch", tp.pitchStep.load());
    x->setAttribute ("nr", tp.nrOn.load());
    x->setAttribute ("nrType", tp.nrType.load());
    x->setAttribute ("hiss", tp.hiss.load());
    x->setAttribute ("wow", tp.wow.load());
    x->setAttribute ("flutter", tp.flutter.load());
    x->setAttribute ("sat", tp.sat.load());
    x->setAttribute ("hf", tp.hf.load());
    x->setAttribute ("xtalk", tp.xtalk.load());
    x->setAttribute ("drop", tp.drop.load());
    x->setAttribute ("drift", tp.drift.load());
    return deck;
}

void AudioEngine::applyDeckState (const juce::XmlElement& deck)
{
    for (auto* x : deck.getChildWithTagNameIterator ("TRACK"))
    {
        auto t = x->getIntAttribute ("index", -1);
        if (t < 0 || t >= numTracks) continue;
        auto& p = track[t];
        p.inputChannel = juce::jmax (0, x->getIntAttribute ("input", t));
        p.inputPair = x->getBoolAttribute ("pair");
        p.gain  = (float) juce::jlimit (0.0, 1.0, x->getDoubleAttribute ("gain", 1.0 / 3.0));
        p.high  = (float) juce::jlimit (-1.0, 1.0, x->getDoubleAttribute ("high"));
        p.mid   = (float) juce::jlimit (-1.0, 1.0, x->getDoubleAttribute ("mid"));
        p.midFreq = (float) juce::jlimit (0.0, 1.0, x->getDoubleAttribute ("midFreq", defaultMidFreq));
        p.low   = (float) juce::jlimit (-1.0, 1.0, x->getDoubleAttribute ("low"));
        p.pan   = (float) juce::jlimit (-1.0, 1.0, x->getDoubleAttribute ("pan"));
        p.fader = (float) juce::jlimit (0.0, 1.0, x->getDoubleAttribute ("fader", 0.75));
        p.bounceMask = x->getIntAttribute ("bounce") & ((1 << numTracks) - 1);

        auto midiIn = x->getStringAttribute ("midiIn", "any");
        if (midiIn == "none") p.midiPort = midiNone;
        else if (midiIn == "port")
        {
            auto slot = midi.findOrCreateInput (x->getStringAttribute ("midiInId"), x->getStringAttribute ("midiInName"));
            p.midiPort = slot >= 0 ? slot : midiNone;
        }
        else p.midiPort = midiAny;

        if (x->hasAttribute ("midiOutId"))
            p.midiOutPort = midi.findOrCreateOutput (x->getStringAttribute ("midiOutId"), x->getStringAttribute ("midiOutName"));
        else
            p.midiOutPort = -1;
        p.midiOutChannel = juce::jlimit (0, 16, x->getIntAttribute ("midiOutChannel", 1));

        auto source = juce::jlimit ((int) Source::none, (int) Source::hardware, x->getIntAttribute ("source"));
        if (source == (int) Source::instrument)
        {
            if (auto* ix = x->getChildByName ("INSTRUMENT"))
            {
                if (ix->getStringAttribute ("kind") == "builtin")
                {
                    setInstrument (t, BuiltIn::create (ix->getIntAttribute ("which")));
                }
                else if (auto* dx = ix->getChildByName ("PLUGIN"))
                {
                    juce::PluginDescription desc;
                    if (desc.loadFromXml (*dx))
                    {
                        setInstrument (t, nullptr);
                        p.source = (int) Source::instrument;
                        pendingInstrument[t] = std::make_unique<juce::XmlElement> (*ix);
                        loadPlugin (t, desc, ix->getStringAttribute ("state"), nullptr);
                    }
                }
            }
        }
        else
        {
            setInstrument (t, nullptr);
            p.source = source;
        }
        mpeConfigPending[t] = true;
    }

    if (auto* m = deck.getChildByName ("MASTER"))
    {
        masterFader = (float) juce::jlimit (0.0, 1.0, m->getDoubleAttribute ("fader", 0.75));
        phones      = (float) juce::jlimit (0.0, 1.0, m->getDoubleAttribute ("phones", 0.6));
    }

    if (auto* x = deck.getChildByName ("TAPE_DECK"))
    {
        auto& tp = tapeEngine.params;
        tp.speedSel    = juce::jlimit (1, 2, x->getIntAttribute ("speed", 1));
        tp.pitchStep   = juce::jlimit (-5, 5, x->getIntAttribute ("pitch"));
        tp.nrOn        = x->getBoolAttribute ("nr", true);
        tp.nrType      = juce::jlimit (0, 2, x->getIntAttribute ("nrType"));
        tp.hiss    = (float) x->getDoubleAttribute ("hiss", 0.55);
        tp.wow     = (float) x->getDoubleAttribute ("wow", 0.30);
        tp.flutter = (float) x->getDoubleAttribute ("flutter", 0.25);
        tp.sat     = (float) x->getDoubleAttribute ("sat", 0.40);
        tp.hf      = (float) x->getDoubleAttribute ("hf", 0.20);
        tp.xtalk   = (float) x->getDoubleAttribute ("xtalk", 0.30);
        tp.drop    = (float) x->getDoubleAttribute ("drop", 0.10);
        tp.drift   = (float) x->getDoubleAttribute ("drift", 0.20);
    }

    if (onDeckStateApplied) onDeckStateApplied();
}

//==============================================================================
std::unique_ptr<juce::XmlElement> AudioEngine::createHardwareState() const
{
    auto hw = std::make_unique<juce::XmlElement> ("HARDWARE");
    hw->setAttribute ("mainOut", mainOutPair.load());
    hw->setAttribute ("phonesOut", phonesPair.load());
    hw->setAttribute ("latencyOffsetMs", tapeEngine.params.latencyOffsetMs.load());
    if (auto m = midi.createState())
        for (auto* c : m->getChildIterator())
            hw->addChildElement (new juce::XmlElement (*c));
    return hw;
}

void AudioEngine::restoreHardware (const juce::XmlElement* hw, const juce::XmlElement* legacyDeck)
{
    if (hw != nullptr)
    {
        mainOutPair = juce::jmax (0, hw->getIntAttribute ("mainOut"));
        phonesPair  = hw->getIntAttribute ("phonesOut", -1);
        tapeEngine.params.latencyOffsetMs = juce::jlimit (-50, 50, hw->getIntAttribute ("latencyOffsetMs"));
        midi.restoreState (hw, nullptr);
        return;
    }

    // First run, or settings from before per-track MIDI.
    midi.restoreState (nullptr, legacyDeck != nullptr ? legacyDeck->getChildByName ("MIDI_INPUTS") : nullptr);
    if (legacyDeck != nullptr)
    {
        if (auto* m = legacyDeck->getChildByName ("MASTER"))
        {
            mainOutPair = m->getIntAttribute ("mainOut");
            phonesPair  = m->getIntAttribute ("phonesOut", -1);
        }
        if (auto* x = legacyDeck->getChildByName ("TAPE_DECK"))
            tapeEngine.params.latencyOffsetMs = x->getIntAttribute ("latencyOffsetMs");
    }
}

void AudioEngine::restoreAll()
{
    auto hw = props.getXmlValue ("hardware");
    auto deck = props.getXmlValue ("deck");
    restoreHardware (hw.get(), deck.get());

    if (deck != nullptr)
    {
        applyDeckState (*deck);
        return;
    }

    // Factory setup: two built-in instruments and two audio inputs.
    setInstrument (0, BuiltIn::create (BuiltIn::poly));
    setInstrument (1, BuiltIn::create (BuiltIn::epiano));
    setAudioSource (2, 0);
    setAudioSource (3, 1);
}

void AudioEngine::saveSettings (bool freshPluginState)
{
    if (startingUp) return;
    saveDeviceState();

    // PropertiesFile only marks itself changed when a value differs, so an unchanged deck
    // costs building the XML (plug-in states are cached) and nothing on disk.
    if (auto known = knownPlugins.createXml())
        props.setValue ("knownPlugins", known.get());
    if (auto deck = createDeckState (freshPluginState)) props.setValue ("deck", deck.get());
    if (auto hw = createHardwareState()) props.setValue ("hardware", hw.get());
    props.setValue ("recallDeckWithTape", recallDeckWithTape.load());
    props.saveIfNeeded();
}
} // namespace tf
