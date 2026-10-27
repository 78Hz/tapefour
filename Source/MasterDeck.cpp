#include "MasterDeck.h"
#include "TapeShelf.h"

namespace tf
{
//==============================================================================
std::unique_ptr<juce::XmlElement> MasterSettings::toXml() const
{
    auto x = std::make_unique<juce::XmlElement> ("MASTER_DECK");
    x->setAttribute ("model", model);
    x->setAttribute ("length", length);
    x->setAttribute ("grade", grade);
    x->setAttribute ("nr", nr);
    x->setAttribute ("recLevelDb", (double) recLevelDb);
    x->setAttribute ("masterId", masterId);
    x->setAttribute ("side", side);
    return x;
}

MasterSettings MasterSettings::fromXml (const juce::XmlElement* x)
{
    MasterSettings s;
    if (x == nullptr) return s;
    s.model = juce::jlimit (-1, numTapeModels - 1, x->getIntAttribute ("model", defaultTapeModel));
    s.length = juce::jlimit (0, numTapeTypes - 1, x->getIntAttribute ("length", 1));
    s.grade = x->getStringAttribute ("grade", "new");
    s.nr = x->getBoolAttribute ("nr", true);
    s.recLevelDb = (float) juce::jlimit (-12.0, 6.0, x->getDoubleAttribute ("recLevelDb"));
    s.masterId = x->getStringAttribute ("masterId");
    s.side = juce::jlimit (0, 1, x->getIntAttribute ("side"));
    return s;
}

tfmt::Json MixTake::toJson() const
{
    auto j = tfmt::Json::object();
    j.set ("number", number);
    j.set ("side", side);
    j.set ("start", start);
    j.set ("length", length);
    j.set ("created", created.toStdString());
    j.set ("onTape", onTape.toStdString());
    j.set ("formats", formats.toStdString());
    auto& f = j.set ("files", tfmt::Json::array());
    for (auto& file : files) f.push (file.toStdString());
    return j;
}

MixTake MixTake::fromJson (const tfmt::Json& j)
{
    MixTake t;
    t.number = (int) j["number"].integer();
    t.side = (int) j["side"].integer();
    t.start = j["start"].number();
    t.length = j["length"].number();
    t.created = juce::String::fromUTF8 (j["created"].string().c_str());
    t.onTape = juce::String::fromUTF8 (j["onTape"].string().c_str());
    t.formats = juce::String::fromUTF8 (j["formats"].string().c_str());
    for (auto& f : j["files"].elements()) t.files.add (juce::String::fromUTF8 (f.string().c_str()));
    return t;
}

//==============================================================================
/** Drains the FIFO into the files, on its own thread. */
class MasterDeck::Writer : public juce::Thread
{
public:
    explicit Writer (MasterDeck& m) : juce::Thread ("TapeFour mixdown writer"), owner (m) { startThread (juce::Thread::Priority::high); }
    ~Writer() override { stopThread (5000); }

    void run() override
    {
        std::vector<float> l (16384), r (16384);
        while (! threadShouldExit())
        {
            const auto st = (State) owner.state.load (std::memory_order_acquire);
            const bool taking = st == State::recording || st == State::tailing || st == State::finishing;
            if (taking && files == nullptr && ! failed)
            {
                // The take has started: its files, as arm() set them out.
                files = std::make_unique<AudioFileSet> (owner.pending.base, owner.pending.formats, owner.sr, 2);
                if (! files->ok()) { problem = files->getError(); failed = true; files.reset(); }
            }

            bool did = false;
            while (taking && owner.fifo.getNumReady() > 0)
            {
                int s1, n1, s2, n2;
                owner.fifo.prepareToRead (std::min (owner.fifo.getNumReady(), (int) l.size()), s1, n1, s2, n2);
                std::copy_n (owner.fifoL.data() + s1, n1, l.data());
                std::copy_n (owner.fifoR.data() + s1, n1, r.data());
                std::copy_n (owner.fifoL.data() + s2, n2, l.data() + n1);
                std::copy_n (owner.fifoR.data() + s2, n2, r.data() + n1);
                owner.fifo.finishedRead (n1 + n2);
                if (files != nullptr)
                {
                    const float* ch[] { l.data(), r.data() };
                    if (! files->write (ch, n1 + n2) && problem.isEmpty()) problem = files->getError();
                }
                did = true;
            }

            if (st == State::finishing && owner.fifo.getNumReady() == 0)
            {
                auto take = std::make_unique<MixTake>();
                take->number = owner.pending.number;
                take->side = owner.directTake ? -1 : owner.takeSide;
                take->start = owner.directTake ? 0.0 : owner.takeStart;
                take->length = (double) owner.takeSamples.load() / owner.sr * cellRate;
                take->created = juce::String (tfmt::isoNow());
                take->onTape = owner.pending.onTape;
                take->formats = owner.pending.formats.describe();
                if (files != nullptr)
                {
                    if (! files->finish() && problem.isEmpty()) problem = files->getError();
                    for (auto& f : files->getFiles()) take->files.add (f.getFullPathName());
                }
                if (owner.dropped.load() > 0 && problem.isEmpty())
                    problem = "the disk couldn't keep up: " + juce::String (owner.dropped.load()) + " samples missing";
                files.reset();
                failed = false;
                {
                    const juce::ScopedLock sl (owner.finishedLock);
                    owner.finished = std::move (take);
                    owner.finishedProblem = std::exchange (problem, {});
                }
                owner.state.store ((int) State::done, std::memory_order_release);
                did = true;
            }

            if (! did) wait (5);
        }
    }

private:
    MasterDeck& owner;
    std::unique_ptr<AudioFileSet> files;
    juce::String problem;
    bool failed = false;
};

//==============================================================================
MasterDeck::MasterDeck()
{
    machine = factoryMachine();
    writer = std::make_unique<Writer> (*this);
}

MasterDeck::~MasterDeck()
{
    writer.reset();
}

tfmt::DeckDescription MasterDeck::factoryMachine()
{
    tfmt::DeckDescription d;
    d.id = "tapefour.tf-m";
    d.name = juce::String (juce::CharPointer_UTF8 ("TAPE\xc2\xb7" "FOUR TF-M")).toStdString();
    d.maker = "78Hz";
    d.kind = "hifi";
    d.layouts = { "stereo-two-sides" };
    d.records = true;
    d.headCount = 3;   // it plays back what it has just recorded
    d.speedsIps = { 1.875 };
    d.nrSystems = { "compander-2to1", "sliding-b", "sliding-c" };
    d.tapeSwitch = "auto";
    d.condition = "new";
    // A good stereo deck, well looked after: steadier than the multitracker.
    d.transport.wowPct = 0.05;
    d.transport.flutterPct = 0.035;
    d.transport.driftPct = 0.05;
    d.heads.cutoffHz = 18000.0;
    d.heads.crosstalkDb = -120.0;
    d.transport.windSecondsC60 = 90.0;   // a stereo deck's stronger reel motors
    d.counter.kind = "realtime";
    d.electronics.channelNoiseDb = -140.0;   // no mixer of its own
    d.electronics.busNoiseDb = -85.0;
    return d;
}

void MasterDeck::stopForDeviceChange()
{
    // The audio has stopped: a take that was running ends here, and is saved before anything moves.
    const auto st = getState();
    if (st == State::recording || st == State::tailing)
        state.store ((int) State::finishing, std::memory_order_release);
    for (int i = 0; i < 3000 && getState() == State::finishing; ++i)
        juce::Thread::sleep (1);
    listening = false;
}

void MasterDeck::prepare (double sampleRate, int block)
{
    sr = sampleRate;
    maxBlock = std::max (block, 4096);
    inL.assign ((size_t) maxBlock, 0.0f);
    inR.assign ((size_t) maxBlock, 0.0f);
    for (int t = 0; t < numTracks; ++t)
    {
        outs[t].assign ((size_t) maxBlock, 0.0f);
        outPtrs[t] = outs[t].data();
    }
    // Eight seconds of slack for the disk.
    const auto size = (int) (sr * 8.0);
    fifo.setTotalSize (size);
    fifoL.assign ((size_t) size, 0.0f);
    fifoR.assign ((size_t) size, 0.0f);
    rec.prepare (sr, maxBlock);
    play.prepare (sr, maxBlock);
    play.setReadOnly (true);
    gapSamples = (int64_t) (headGapSeconds * sr);
}

void MasterDeck::setMachine (const tfmt::DeckDescription& d)
{
    machine = d;
}

void MasterDeck::configureEngines (int nrType)
{
    for (auto* e : { &rec, &play })
    {
        e->params.speedSel = 1;
        e->params.pitch = 0.0f;
        e->params.nrOn = settings.nr;
        e->params.nrType = juce::jlimit (0, 2, nrType);
        e->params.sat = 0.5f;
        e->machine.wowPct = (float) machine.transport.wowPct;
        e->machine.flutterPct = (float) machine.transport.flutterPct;
        e->machine.driftPct = (float) machine.transport.driftPct;
        e->machine.speedErrorPct = (float) machine.transport.speedErrorPct;
        e->machine.cutoffHz = (float) machine.heads.cutoffHz;
        e->machine.crosstalkDb = (float) machine.heads.crosstalkDb;
        e->machine.honoursTapeCondition = machine.honoursTapeCondition;
        e->machine.windSecondsC60 = (float) machine.transport.windSecondsC60;
        e->machine.cueLevelDb = (float) machine.cue.levelDb;
        e->machine.cueSpacingUm = (float) machine.cue.spacingUm;
        e->machine.counterRealtime = machine.counter.kind == "realtime";
        e->machine.countsPerTurn = (float) machine.counter.countsPerTurn;
        if (tape != nullptr)
        {
            e->setTape (tape.get(), false);
            e->params.tapeSwitch = tape->naturalPosition();   // it reads the cassette's sensing holes
        }
    }
}

void MasterDeck::applySettings (int nrType)
{
    const auto st = getState();
    if (st != State::off && st != State::armed) return;
    recGain = dbToGain (settings.recLevelDb);
    configureEngines (nrType);
}

std::unique_ptr<Tape> MasterDeck::swapTape (std::unique_ptr<Tape> t)
{
    jassert (getState() == State::off && ! isListening());
    auto old = std::move (tape);
    tape = std::move (t);
    takes.clear();
    if (tape != nullptr)
    {
        tape->useRecorder (machine.id);
        loadTakes();
    }
    configureEngines (0);
    return old;
}

juce::File MasterDeck::tapeFolder() const
{
    return tape != nullptr ? TapeShelf::folderFor (tape->id) : juce::File();
}

void MasterDeck::loadTakes()
{
    takes.clear();
    auto file = tapeFolder().getChildFile ("takes.json");
    if (! file.existsAsFile()) return;
    if (auto j = tfmt::Json::parse (file.loadFileAsString().toStdString()))
        for (auto& t : (*j)["takes"].elements())
            takes.push_back (MixTake::fromJson (t));
}

void MasterDeck::saveTakes() const
{
    if (tape == nullptr) return;
    auto j = tfmt::Json::object();
    auto& list = j.set ("takes", tfmt::Json::array());
    for (auto& t : takes) list.push (t.toJson());
    tapeFolder().createDirectory();
    tfmt::writeTextAtomically (tfmt::pathFromUtf8 (tapeFolder().getChildFile ("takes.json").getFullPathName().toStdString()), j.dump());
}

double MasterDeck::sideLengthSeconds() const
{
    return tape != nullptr ? tape->getLengthSeconds() : 0.0;
}

double MasterDeck::sideUsedSeconds (int side) const
{
    double end = 0;
    for (auto& t : takes)
        if (t.side == side) end = std::max (end, t.start + t.length);
    return end / cellRate;
}

double MasterDeck::nextStart() const
{
    if (tape == nullptr) return -1;
    const auto used = sideUsedSeconds (settings.side);
    const auto start = (used > 0 ? used + gapSeconds : leadInSeconds) * cellRate;
    return start < (double) tape->getLengthCells() - 5.0 * cellRate ? start : -1.0;
}

juce::String MasterDeck::describeTape() const
{
    if (settings.direct()) return "DIRECT";
    if (tape == nullptr) return "NO TAPE";
    return tape->productName() + (settings.nr ? juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 NR")) : juce::String());
}

bool MasterDeck::arm (const ExportFormats& formats, const juce::File& folder, const juce::String& baseName, int nrType, juce::String& whyNot)
{
    const auto st = getState();
    if (st != State::off && st != State::armed) { whyNot = "THE LAST TAKE IS STILL BEING SAVED"; return false; }
    if (isListening()) stopListening();
    if (! formats.any()) { whyNot = "CHOOSE A FILE FORMAT"; return false; }

    const bool direct = settings.direct();
    double start = 0;
    if (! direct)
    {
        if (tape == nullptr) { whyNot = "NO MASTER TAPE"; return false; }
        if (settings.side == 0 ? tape->protectA.load() : tape->protectB.load()) { whyNot = "THE MASTER TAPE IS WRITE-PROTECTED"; return false; }
        start = nextStart();
        if (start < 0) { whyNot = juce::String ("SIDE ") + (settings.side == 0 ? "A" : "B") + " IS FULL - TURN IT OVER"; return false; }
    }
    if (! folder.createDirectory()) { whyNot = "COULD NOT MAKE THE MIXDOWN FOLDER"; return false; }

    int number = 1;
    for (auto& t : takes) number = std::max (number, t.number + 1);
    while (folder.getChildFile (juce::File::createLegalFileName (baseName + " - mix " + juce::String (number)) + ".wav").exists()
           || folder.getChildFile (juce::File::createLegalFileName (baseName + " - mix " + juce::String (number)) + ".mp3").exists()
           || folder.getChildFile (juce::File::createLegalFileName (baseName + " - mix " + juce::String (number)) + ".flac").exists())
        ++number;   // never over an earlier take's files

    pending.number = number;
    pending.base = folder.getChildFile (juce::File::createLegalFileName (baseName + " - mix " + juce::String (number)));
    pending.formats = formats;
    pending.onTape = describeTape();
    directTake = direct;
    takeSide = settings.side;
    takeStart = start;
    recGain = dbToGain (settings.recLevelDb);
    if (! direct)
    {
        configureEngines (nrType);
        play.setReadOnly (true);   // monitoring: the record head does the wearing
        tape->setSideB (takeSide == 1);
        rec.seek (start);
        // The playback head trails the record head: it starts a head-gap behind and reads blank leader.
        play.seek (std::max (0.0, start - headGapSeconds * cellRate));
    }
    stopRequested = false;
    wantArmed = true;
    state.store ((int) State::armed, std::memory_order_release);
    return true;
}

void MasterDeck::disarm()
{
    wantArmed = false;
    const auto st = getState();
    if (st == State::recording) stopRequested = true;
    else if (st == State::armed) state.store ((int) State::off, std::memory_order_release);
}

bool MasterDeck::collectFinished (MixTake& take, juce::String& problem)
{
    if (getState() != State::done) return false;
    {
        const juce::ScopedLock sl (finishedLock);
        if (finished == nullptr) return false;
        take = *finished;
        problem = finishedProblem;
        finished.reset();
    }
    if (take.side >= 0 && tape != nullptr)
    {
        // The tape is left where the take ended, along the tape itself.
        auto end = take.start + take.length;
        tape->position = take.side == 0 ? end : (double) tape->getLengthCells() - end;
        tape->setSideB (false);
    }
    if (! take.files.isEmpty() || take.side >= 0)
    {
        takes.push_back (take);
        saveTakes();
    }
    state.store ((int) State::off, std::memory_order_release);
    return true;
}

bool MasterDeck::listen (const MixTake& take)
{
    const auto st = getState();
    if ((st != State::off && st != State::armed) || tape == nullptr || take.side < 0) return false;
    listening = false;
    play.setReadOnly (false);   // a real play: it wears the tape, a little
    play.setTape (tape.get(), false);
    play.params.tapeSwitch = tape->naturalPosition();
    tape->setSideB (take.side == 1);
    play.seek (take.start);
    play.command (TapeEngine::Command::play);
    takeSide = take.side;
    listenEnd = take.start + take.length;
    listening = true;
    return true;
}

//==============================================================================
void MasterDeck::push (const float* l, const float* r, int n) noexcept
{
    if (n <= 0) return;
    for (int i = 0; i < n; ++i)
        if (clipAt.load (std::memory_order_relaxed) < 0 && (std::abs (l[i]) >= 0.999f || std::abs (r[i]) >= 0.999f))
            clipAt.store (takeSamples.load (std::memory_order_relaxed) + i, std::memory_order_relaxed);
    const auto room = fifo.getFreeSpace();
    if (n > room) { dropped += n - room; n = room; }
    int s1, n1, s2, n2;
    fifo.prepareToWrite (n, s1, n1, s2, n2);
    std::copy_n (l, n1, fifoL.data() + s1);
    std::copy_n (r, n1, fifoR.data() + s1);
    std::copy_n (l + n1, n2, fifoL.data() + s2);
    std::copy_n (r + n1, n2, fifoR.data() + s2);
    fifo.finishedWrite (n1 + n2);
    takeSamples += n1 + n2;
}

void MasterDeck::startTake() noexcept
{
    takeSamples = 0;
    clipAt = -1;
    dropped = 0;
    if (! directTake)
    {
        rec.command (TapeEngine::Command::rec);
        play.command (TapeEngine::Command::play);
        skipSamples = gapSamples;
    }
    state.store ((int) State::recording, std::memory_order_release);
}

void MasterDeck::process (const float* l, const float* r, int n, bool deckPlaying) noexcept
{
    if (outs[0].empty()) return;   // not prepared: no audio device yet
    n = std::min (n, maxBlock);
    auto st = (State) state.load (std::memory_order_acquire);
    if (st == State::armed && deckPlaying && wantArmed.load())
    {
        startTake();
        st = State::recording;
    }

    const int sideL = takeSide == 0 ? 0 : 2, sideR = sideL + 1;
    auto playbackHead = [&] (int count, int64_t& skip, int64_t limit)
    {
        const bool none[numTracks] {};
        play.beginBlock (count, none);
        play.readAll (outPtrs, count);
        play.endBlock();
        auto from = (int) std::min<int64_t> (skip, count);
        skip -= from;
        auto amount = (int) std::min<int64_t> (count - from, limit);
        push (outs[sideL].data() + from, outs[sideR].data() + from, amount);
        return amount;
    };

    if (st == State::recording)
    {
        bool stopNow = ! deckPlaying || stopRequested.load();
        if (directTake)
        {
            if (! stopNow) push (l, r, n);
            else state.store ((int) State::finishing, std::memory_order_release);
            return;
        }

        if (! stopNow)
        {
            // The record head: the mix, at the master deck's record level, onto this side's two tracks.
            const auto g = recGain.load();
            for (int i = 0; i < n; ++i) { inL[(size_t) i] = l[i] * g; inR[(size_t) i] = r[i] * g; }
            bool armedTracks[numTracks] {};
            armedTracks[sideL] = armedTracks[sideR] = true;
            rec.beginBlock (n, armedTracks);
            rec.writeTrack (sideL, inL.data(), n, 0.0);
            rec.writeTrack (sideR, inR.data(), n, 0.0);
            rec.endBlock();
            if (rec.getMode() != Mode::rec) stopNow = true;   // the end of the side
        }

        // The playback head, a head-gap behind: what the files get.
        playbackHead (n, skipSamples, n);

        if (stopNow)
        {
            rec.command (TapeEngine::Command::stop);
            tailSamples = gapSamples;   // the playback head still has the last of the take to read
            state.store ((int) State::tailing, std::memory_order_release);
        }
        return;
    }

    if (st == State::tailing)
    {
        int64_t noSkip = 0;
        tailSamples -= playbackHead (n, noSkip, tailSamples);
        if (tailSamples <= 0)
        {
            play.command (TapeEngine::Command::stop);
            state.store ((int) State::finishing, std::memory_order_release);
        }
    }
}

bool MasterDeck::listenInto (float* l, float* r, int n) noexcept
{
    if (! listening.load() || outs[0].empty()) return false;
    n = std::min (n, maxBlock);
    const bool none[numTracks] {};
    play.beginBlock (n, none);
    play.readAll (outPtrs, n);
    play.endBlock();
    const int sideL = takeSide == 0 ? 0 : 2;
    std::copy_n (outs[sideL].data(), n, l);
    std::copy_n (outs[sideL + 1].data(), n, r);
    if (play.getPosition() >= listenEnd + headGapSeconds * cellRate || play.getMode() != Mode::play)
    {
        play.command (TapeEngine::Command::stop);
        listening = false;
    }
    return true;
}

//==============================================================================
bool SideExport::run (Tape& masterTape, const tfmt::DeckDescription& machine, bool nr, int nrType)
{
    TapeEngine eng;
    constexpr int block = 4096;
    eng.prepare (sampleRate, block);
    eng.setReadOnly (true);
    eng.params.speedSel = 1;
    eng.params.nrOn = nr;
    eng.params.nrType = juce::jlimit (0, 2, nrType);
    eng.machine.wowPct = (float) machine.transport.wowPct;
    eng.machine.flutterPct = (float) machine.transport.flutterPct;
    eng.machine.driftPct = (float) machine.transport.driftPct;
    eng.machine.cutoffHz = (float) machine.heads.cutoffHz;
    eng.machine.crosstalkDb = (float) machine.heads.crosstalkDb;
    eng.machine.honoursTapeCondition = machine.honoursTapeCondition;
    eng.setTape (&masterTape, false);
    eng.params.tapeSwitch = masterTape.naturalPosition();
    masterTape.setSideB (side == 1);
    eng.seek (0.0);

    const int sideL = side == 0 ? 0 : 2;
    int lastBlock = -1;
    const auto blocks = masterTape.blockAt ((double) masterTape.getLengthCells());
    for (int b = 0; b <= blocks; ++b)
        if (masterTape.isRecorded (sideL, b) || masterTape.isRecorded (sideL + 1, b)) lastBlock = b;
    if (lastBlock < 0) { error = "nothing recorded on that side"; masterTape.setSideB (false); return false; }
    const double endCells = std::min ((double) masterTape.getLengthCells(), (lastBlock + 1.0) * blockCells + 0.5 * cellRate);

    AudioFileSet files (base, formats, sampleRate, 2);
    if (! files.ok()) { error = files.getError(); masterTape.setSideB (false); return false; }
    std::vector<std::vector<float>> outs (numTracks, std::vector<float> ((size_t) block));
    float* ptrs[numTracks] { outs[0].data(), outs[1].data(), outs[2].data(), outs[3].data() };
    const bool none[numTracks] {};
    eng.command (TapeEngine::Command::play);
    while (eng.getPosition() < endCells)
    {
        eng.beginBlock (block, none);
        eng.readAll (ptrs, block);
        eng.endBlock();
        const float* ch[] { outs[(size_t) sideL].data(), outs[(size_t) sideL + 1].data() };
        if (! files.write (ch, block)) { error = files.getError(); break; }
        if (eng.getMode() != Mode::play) break;
    }
    masterTape.setSideB (false);
    if (! files.finish() && error.isEmpty()) error = files.getError();
    written = files.getFiles();
    return error.isEmpty();
}
} // namespace tf
