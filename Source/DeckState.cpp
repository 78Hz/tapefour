// Everything about the deck that is saved and recalled, in one place:
// the deck state (what you set by hand; saved in the settings and with every tape),
// the hardware state (outputs, latency trim, MIDI devices, deck MIDI mappings; settings only),
// and the settings file.
#include "AudioEngine.h"

namespace tf
{
std::unique_ptr<juce::XmlElement> AudioEngine::createDeckState (bool freshPluginState)
{
    auto deck = std::make_unique<juce::XmlElement> ("DECK");
    deck->setAttribute ("version", 5);   // 5: the machine and the cassette's condition left the session

    for (int t = 0; t < numTracks; ++t)
    {
        auto& p = track[t];
        auto* x = deck->createNewChildElement ("TRACK");
        x->setAttribute ("index", t);
        x->setAttribute ("feed", (Feed) p.feed.load() == Feed::bounce ? "bounce" : "live");
        x->setAttribute ("input", p.inputChannel.load());
        x->setAttribute ("pair", p.inputPair.load());
        x->setAttribute ("bounce", p.bounceMask.load());
        x->setAttribute ("mute", p.mute.load());
        x->setAttribute ("solo", p.solo.load());
        x->setAttribute ("gain", p.gain.load());
        x->setAttribute ("high", p.high.load());
        x->setAttribute ("mid", p.mid.load());
        x->setAttribute ("midFreq", p.midFreq.load());
        x->setAttribute ("low", p.low.load());
        x->setAttribute ("pan", p.pan.load());
        x->setAttribute ("fader", p.fader.load());

        auto port = p.midiPort.load();
        if (port == midiAny)                 x->setAttribute ("midiIn", "any");
        else if (! midi.inputExists (port))  x->setAttribute ("midiIn", "none");
        else { x->setAttribute ("midiIn", "port"); x->setAttribute ("midiInId", midi.inputId (port)); x->setAttribute ("midiInName", midi.inputName (port)); }

        auto* chain = x->createNewChildElement ("CHAIN");
        for (auto& b : chains[t].blocks)
        {
            auto bx = b->saveState (freshPluginState);
            b->writeCommon (*bx);
            if (auto* e = dynamic_cast<ExternalBlock*> (b.get()))
            {
                bx->setAttribute ("outId", midi.outputId (e->outPort.load()));
                bx->setAttribute ("outName", midi.outputName (e->outPort.load()));
            }
            if (dynamic_cast<PendingBlock*> (b.get()) == nullptr)
                mapper.writeBlockMappings (*bx, t, b->id);
            chain->addChildElement (bx.release());
        }
    }

    auto* m = deck->createNewChildElement ("MASTER");
    m->setAttribute ("fader", masterFader.load());
    m->setAttribute ("phones", phones.load());

    auto& tp = tapeEngine.params;
    auto* x = deck->createNewChildElement ("TAPE_DECK");
    x->setAttribute ("speed", tp.speedSel.load());
    x->setAttribute ("pitchPct", (double) tp.pitch.load());
    x->setAttribute ("nr", tp.nrOn.load());
    x->setAttribute ("nrType", tp.nrType.load());
    x->setAttribute ("sat", tp.sat.load());
    if (tape != nullptr)
    {
        x->setAttribute ("counterZero", tape->counterZero.load());
        if (tape->isTurnedOver()) x->setAttribute ("sideB", true);   // left in the deck side B up
    }
    return deck;
}

void AudioEngine::restoreTrackChain (int t, const juce::XmlElement& chainXml)
{
    for (auto* bx : chainXml.getChildWithTagNameIterator ("BLOCK"))
    {
        auto kind = bx->getStringAttribute ("kind");
        if (kind == "plugin")
        {
            juce::PluginDescription desc;
            if (auto* dx = bx->getChildByName ("PLUGIN"); dx != nullptr && desc.loadFromXml (*dx))
                addPlugin (t, -1, desc, bx->getStringAttribute ("state"), 0, nullptr, std::make_unique<juce::XmlElement> (*bx));
        }
        else if (kind == "external")
        {
            auto out = bx->hasAttribute ("outId") ? midi.findOrCreateOutput (bx->getStringAttribute ("outId"), bx->getStringAttribute ("outName")) : -1;
            auto e = std::make_unique<ExternalBlock> (out, bx->getIntAttribute ("channel", 1), bx->getIntAttribute ("return", -1),
                                                      bx->getBoolAttribute ("returnPair"));
            e->readCommon (*bx);
            insertBlock (t, -1, std::move (e));
        }
        else if (auto b = BuiltIn::restore (*bx))
        {
            auto* processor = b->getProcessor();
            auto id = insertBlock (t, -1, std::move (b));
            if (id != 0)
                mapper.restoreBlockMappings (*bx, t, id, processor);
        }
    }
}

/** Decks saved before chains: one source per track (audio in, an instrument, a hardware synth, a bounce). */
void AudioEngine::migrateOldTrack (int t, const juce::XmlElement& x)
{
    enum { none = 0, audio, instrument, bounceSrc, hardware };
    auto& p = track[t];
    switch (x.getIntAttribute ("source"))
    {
        case audio:
            setTrackInput (t, juce::jmax (0, x.getIntAttribute ("input", t)), x.getBoolAttribute ("pair"));
            break;

        case instrument:
            if (auto* ix = x.getChildByName ("INSTRUMENT"))
            {
                if (ix->getStringAttribute ("kind") == "builtin")
                {
                    insertBlock (t, -1, BuiltIn::createSynth (ix->getIntAttribute ("which")));
                }
                else if (auto* dx = ix->getChildByName ("PLUGIN"))
                {
                    juce::PluginDescription desc;
                    if (desc.loadFromXml (*dx))
                        addPlugin (t, -1, desc, ix->getStringAttribute ("state"));
                }
            }
            break;

        case bounceSrc:
            p.feed = (int) Feed::bounce;
            break;

        case hardware:
        {
            auto out = x.hasAttribute ("midiOutId") ? midi.findOrCreateOutput (x.getStringAttribute ("midiOutId"), x.getStringAttribute ("midiOutName")) : -1;
            insertBlock (t, -1, std::make_unique<ExternalBlock> (out, x.getIntAttribute ("midiOutChannel", 1),
                                                                 juce::jmax (0, x.getIntAttribute ("input", t)), x.getBoolAttribute ("pair")));
            break;
        }

        default:
            break;
    }
}

void AudioEngine::applyDeckState (const juce::XmlElement& deck)
{
    for (auto* x : deck.getChildWithTagNameIterator ("TRACK"))
    {
        auto t = x->getIntAttribute ("index", -1);
        if (t < 0 || t >= numTracks) continue;
        auto& p = track[t];
        p.gain  = (float) juce::jlimit (0.0, 1.0, x->getDoubleAttribute ("gain", 1.0 / 3.0));
        p.high  = (float) juce::jlimit (-1.0, 1.0, x->getDoubleAttribute ("high"));
        p.mid   = (float) juce::jlimit (-1.0, 1.0, x->getDoubleAttribute ("mid"));
        p.midFreq = (float) juce::jlimit (0.0, 1.0, x->getDoubleAttribute ("midFreq", defaultMidFreq));
        p.low   = (float) juce::jlimit (-1.0, 1.0, x->getDoubleAttribute ("low"));
        p.pan   = (float) juce::jlimit (-1.0, 1.0, x->getDoubleAttribute ("pan"));
        p.fader = (float) juce::jlimit (0.0, 1.0, x->getDoubleAttribute ("fader", 0.75));
        p.bounceMask = x->getIntAttribute ("bounce") & ((1 << numTracks) - 1);
        p.mute = x->getBoolAttribute ("mute");
        p.solo = x->getBoolAttribute ("solo");

        auto midiIn = x->getStringAttribute ("midiIn", "any");
        if (midiIn == "none") p.midiPort = midiNone;
        else if (midiIn == "port")
        {
            auto slot = midi.findOrCreateInput (x->getStringAttribute ("midiInId"), x->getStringAttribute ("midiInName"));
            p.midiPort = slot >= 0 ? slot : midiNone;
        }
        else p.midiPort = midiAny;

        clearChain (t);
        p.feed = (int) Feed::live;
        setTrackInput (t, -1);

        if (auto* chain = x->getChildByName ("CHAIN"))
        {
            p.feed = (int) (x->getStringAttribute ("feed") == "bounce" ? Feed::bounce : Feed::live);
            setTrackInput (t, x->getIntAttribute ("input", -1), x->getBoolAttribute ("pair"));
            restoreTrackChain (t, *chain);
        }
        else
        {
            migrateOldTrack (t, *x);
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
        // Older decks kept pitch as a step of 2.4%.
        tp.pitch       = (float) juce::jlimit (-12.0, 12.0, x->hasAttribute ("pitchPct") ? x->getDoubleAttribute ("pitchPct")
                                                                                      : x->getIntAttribute ("pitch") * 2.4);
        tp.nrOn        = x->getBoolAttribute ("nr", true);
        tp.nrType      = juce::jlimit (0, 2, x->getIntAttribute ("nrType"));
        tp.sat         = (float) x->getDoubleAttribute ("sat", 0.40);
        // Sessions before version 5 also held the machine's and the cassette's character; the machine
        // took its own from the settings once (restoreMachine), and each cassette from its old deck file.
        if (tape != nullptr && x->hasAttribute ("counterZero"))
            tape->counterZero = juce::jlimit (-(double) tape->getLengthCells(), 2.0 * (double) tape->getLengthCells(), x->getDoubleAttribute ("counterZero"));
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
    hw->addChildElement (mapper.createDeckState().release());
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
        mapper.restoreDeckState (hw->getChildByName ("MIDI_MAPS"));
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

//==============================================================================
tfmt::DeckDescription AudioEngine::factoryMachine()
{
    tfmt::DeckDescription d;
    d.id = tf4DeckId;
    d.name = juce::String (juce::CharPointer_UTF8 ("TAPE\xc2\xb7" "FOUR TF-4")).toStdString();
    d.maker = "78Hz";
    d.kind = "multitracker";
    d.layouts = { "4-track-one-way" };
    d.records = true;
    d.headCount = 2;
    d.speedsIps = { 1.875, 3.75 };
    d.nrSystems = { "compander-2to1", "sliding-b", "sliding-c" };
    d.tapeSwitch = "manual";
    d.applyCondition ("serviced");
    return d;
}

juce::File AudioEngine::getMachineFile() const
{
    return props.getFile().getSiblingFile ("TF-4.deck");
}

tfmt::DeckDescription AudioEngine::getMachine() const
{
    auto d = machineBase;
    const auto& m = tapeEngine.machine;
    // The controls hold floats; the file gets the numbers they mean (0.15, not 0.15000000596).
    auto clean = [] (float v) { return std::round ((double) v * 1.0e5) / 1.0e5; };
    d.transport.wowPct = clean (m.wowPct.load());
    d.transport.flutterPct = clean (m.flutterPct.load());
    d.transport.driftPct = clean (m.driftPct.load());
    d.transport.speedErrorPct = clean (m.speedErrorPct.load());
    d.heads.cutoffHz = std::round ((double) m.cutoffHz.load());
    d.heads.crosstalkDb = clean (m.crosstalkDb.load());
    d.honoursTapeCondition = m.honoursTapeCondition.load();
    return d;
}

void AudioEngine::setMachine (const tfmt::DeckDescription& d)
{
    machineBase = d;
    auto& m = tapeEngine.machine;
    m.wowPct = (float) d.transport.wowPct;
    m.flutterPct = (float) d.transport.flutterPct;
    m.driftPct = (float) d.transport.driftPct;
    m.speedErrorPct = (float) d.transport.speedErrorPct;
    m.cutoffHz = (float) d.heads.cutoffHz;
    m.crosstalkDb = (float) d.heads.crosstalkDb;
    m.honoursTapeCondition = d.honoursTapeCondition;
}

void AudioEngine::restoreMachine (const juce::XmlElement* legacyDeck)
{
    auto file = getMachineFile();
    tfmt::DeckDescription d;
    std::string err;
    if (file.existsAsFile() && tfmt::readDeck (tfmt::pathFromUtf8 (file.getFullPathName().toStdString()), d, err))
    {
        setMachine (d);
        lastSavedMachine = tfmt::toJson (d).dump();
        return;
    }

    d = factoryMachine();
    // Settings from before the deck format kept the machine's character in the deck state.
    if (auto* x = legacyDeck != nullptr ? legacyDeck->getChildByName ("TAPE_DECK") : nullptr; x != nullptr && x->hasAttribute ("wow"))
    {
        auto xt = x->getDoubleAttribute ("xtalk", 0.30);
        d.transport.wowPct = x->getDoubleAttribute ("wow", 0.30) * 0.5;
        d.transport.flutterPct = x->getDoubleAttribute ("flutter", 0.25) * 0.3;
        d.transport.driftPct = x->getDoubleAttribute ("drift", 0.20);
        d.heads.cutoffHz = 20000.0 * std::pow (0.25, x->getDoubleAttribute ("hf", 0.20));
        d.heads.crosstalkDb = xt > 0.0 ? -80.0 + xt * 50.0 : -120.0;
        d.condition = "custom";
    }
    setMachine (d);
}

juce::File AudioEngine::getMasterMachineFile() const
{
    return props.getFile().getSiblingFile ("TF-M.deck");
}

void AudioEngine::saveMachine()
{
    // The master deck's description is written once, so it's there to read (or edit).
    if (auto f = getMasterMachineFile(); ! f.existsAsFile())
    {
        std::string err;
        tfmt::writeDeck (tfmt::pathFromUtf8 (f.getFullPathName().toStdString()), masterDeck.getMachine(), err);
    }
    props.setValue ("masterDeck", masterDeck.settings.toXml().get());

    auto d = getMachine();
    auto text = tfmt::toJson (d).dump();
    if (text == lastSavedMachine) return;
    std::string err;
    if (tfmt::writeDeck (tfmt::pathFromUtf8 (getMachineFile().getFullPathName().toStdString()), d, err))
        lastSavedMachine = text;
}

void AudioEngine::restoreAll()
{
    auto hw = props.getXmlValue ("hardware");
    auto deck = props.getXmlValue ("deck");
    restoreHardware (hw.get(), deck.get());
    restoreMachine (deck.get());
    {
        auto m = props.getXmlValue ("masterDeck");
        masterDeck.settings = MasterSettings::fromXml (m.get());
        tfmt::DeckDescription d;
        std::string err;
        if (tfmt::readDeck (tfmt::pathFromUtf8 (getMasterMachineFile().getFullPathName().toStdString()), d, err))
            masterDeck.setMachine (d);
    }

    if (deck != nullptr)
    {
        applyDeckState (*deck);
        return;
    }

    // Factory setup: two built-in instruments and two audio inputs.
    insertBlock (0, -1, BuiltIn::createSynth (BuiltIn::poly));
    insertBlock (1, -1, BuiltIn::createSynth (BuiltIn::epiano));
    setTrackInput (2, 0);
    setTrackInput (3, 1);
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
    saveMachine();
    props.saveIfNeeded();
}
} // namespace tf
