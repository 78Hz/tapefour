#include "Tape.h"
#include "Zip.h"

#include <cstdlib>

namespace tf
{
namespace
{
    constexpr const char* sessionFile = "session.xml";
    constexpr const char* inlayLayout = "tf4-inlay-1";

    juce::String madeByThisProgram()
    {
       #ifdef JUCE_APPLICATION_VERSION_STRING
        return juce::String (juce::CharPointer_UTF8 ("TAPE\xc2\xb7" "FOUR ")) + JUCE_APPLICATION_VERSION_STRING;
       #else
        return juce::String (juce::CharPointer_UTF8 ("TAPE\xc2\xb7" "FOUR"));
       #endif
    }

    std::string str (const juce::String& s)  { return s.toStdString(); }
    juce::String str (const std::string& s)  { return juce::String::fromUTF8 (s.c_str()); }
    std::filesystem::path pathOf (const juce::File& f) { return tfmt::pathFromUtf8 (f.getFullPathName().toStdString()); }

    uint64_t fnv1a (const juce::String& s)
    {
        uint64_t h = 1469598103934665603ull;
        for (auto c : s.toStdString()) { h ^= (uint8_t) c; h *= 1099511628211ull; }
        return h;
    }

    juce::String accentText (juce::uint32 argb)
    {
        return "#" + juce::String::toHexString ((int) (argb & 0xffffff)).paddedLeft ('0', 6);
    }
}

tfmt::Formulation formulationOf (const TapeModel& m)
{
    tfmt::Formulation f;
    f.name = m.name;
    f.line = m.line;
    f.position = m.position;
    f.basedOn = m.basedOn;
    f.accent = str (accentText (m.accent));
    f.iecType = m.iecType;
    f.eqMicroseconds = m.eqMicroseconds;
    // Measured to a tenth of a dB: written as such, not as the nearest float (3.8, not 3.7999999523).
    auto tenth = [] (float v) { return std::round ((double) v * 10.0) / 10.0; };
    f.mol100 = tenth (m.mol100);
    f.mol400 = tenth (m.mol400);
    f.sol10k = tenth (m.sol10k);
    f.snrDbA = tenth (m.snr);
    f.modNoiseDb = tenth (m.modNoise);
    f.sensitivityDb = tenth (m.sensitivity);
    return f;
}

//==============================================================================
Tape::Tape (int typeIndex, int modelIndex, const tfmt::Condition& c)
{
    typeInfo = tapeTypes[juce::jlimit (0, numTapeTypes - 1, typeIndex)];
    model = juce::jlimit (0, numTapeModels - 1, modelIndex);
    modelInfo = tapeModels[model];
    formulation = formulationOf (modelInfo);
    lengthCells = (int64_t) typeInfo.minutes * 60 * cellRate;
    allocate();

    id = juce::Uuid().toString();
    createdIso = str (tfmt::isoNow());
    madeBy = str (madeByThisProgram());
    for (int i = 0; i < Inlay::numFields; ++i)
        trackSheet.add ({});
    trackSheet.set (Inlay::date, juce::Time::getCurrentTime().formatted ("%d.%m.%y"));
    recorders = { tf4DeckId };
    currentRecorder = 1;
    seed = fnv1a (id);
    madeDay = tfmt::dayNumberOf (str (createdIso));
    setCondition (c);
}

Tape::Tape (const tfmt::CassetteInfo& info)
{
    // The length: one of the standard ones, or what it says.
    typeInfo = { nullptr, info.minutesPerSide, 0.0f };
    for (auto& tt : tapeTypes)
        if (tt.minutes == info.minutesPerSide) typeInfo = tt;
    if (typeInfo.name == nullptr)
    {
        lengthName = "C-" + std::to_string (info.minutesPerSide * 2);
        typeInfo.name = lengthName.c_str();
        typeInfo.fullPackMm = juce::jlimit (34.0f, 56.0f, 40.0f + (float) (info.minutesPerSide - 23) * (8.0f / 22.0f));
    }

    // The formulation: one of ours, or the numbers it brings with it.
    formulation = info.formulation;
    model = -1;
    for (int k = 0; k < numTapeModels; ++k)
        if (formulationOf (tapeModels[k]) == info.formulation) model = k;
    if (model >= 0)
    {
        modelInfo = tapeModels[model];
    }
    else
    {
        formulationStrings[0] = info.formulation.name;
        formulationStrings[1] = info.formulation.line;
        formulationStrings[2] = info.formulation.position;
        formulationStrings[3] = info.formulation.basedOn;
        modelInfo.name = formulationStrings[0].c_str();
        modelInfo.line = formulationStrings[1].c_str();
        modelInfo.position = formulationStrings[2].c_str();
        modelInfo.basedOn = formulationStrings[3].c_str();
        modelInfo.iecType = info.formulation.iecType >= 4 ? 4 : info.formulation.iecType >= 2 ? 2 : 1;
        modelInfo.eqMicroseconds = info.formulation.eqMicroseconds == 70 ? 70 : 120;
        modelInfo.mol100 = (float) info.formulation.mol100;
        modelInfo.mol400 = (float) info.formulation.mol400;
        modelInfo.sol10k = (float) info.formulation.sol10k;
        modelInfo.snr = (float) info.formulation.snrDbA;
        modelInfo.modNoise = (float) info.formulation.modNoiseDb;
        modelInfo.sensitivity = (float) info.formulation.sensitivityDb;
        auto accent = juce::String (info.formulation.accent).trimCharactersAtStart ("#");
        modelInfo.accent = accent.length() == 6 ? (0xff000000u | (juce::uint32) accent.getHexValue32()) : 0xffe8702au;
        // Its artwork: the built-in tape of the same type.
        model = defaultTapeModel;
        for (int k = numTapeModels - 1; k >= 0; --k)
            if (tapeModels[k].iecType == modelInfo.iecType) model = k;
    }

    lengthCells = info.cellsPerTrack;
    layout = info.layout;
    allocate();
    if (! info.tracks.empty()) setFluxScale ((float) info.tracks.front().fluxScale);

    id = str (info.id);
    createdIso = str (info.created);
    madeBy = info.madeBy;
    name = str (info.label);
    for (int i = 0; i < Inlay::numFields; ++i)
        trackSheet.add (info.inlayLayout == inlayLayout && (size_t) i < info.inlay.size() ? str (info.inlay[(size_t) i]) : juce::String());
    if (info.inlayLayout != inlayLayout)
        trackSheet.set (Inlay::title, name);
    recorders = info.recorders;
    unknownManifest = info.unknown;
    seed = fnv1a (id);
    madeDay = tfmt::dayNumberOf (info.created);
    setCondition (info.condition);
}

void Tape::allocate()
{
    numBlocks  = (int) ((lengthCells + blockCells - 1) / blockCells);
    numSeconds = (int) ((lengthCells + cellRate - 1) / cellRate);

    for (int t = 0; t < numTracks; ++t)
    {
        // calloc'd zeroes are valid int16 atomics, and pages stay untouched until recorded on.
        data[t] = static_cast<Cell*> (std::calloc ((size_t) lengthCells, sizeof (Cell)));
        if (data[t] == nullptr)
        {
            for (auto*& d : data) { std::free (d); d = nullptr; }
            throw std::bad_alloc();
        }

        flags[t].reset (new std::atomic<uint8_t>[(size_t) numBlocks]);
        recorder[t].reset (new std::atomic<uint8_t>[(size_t) numBlocks]);
        speedMilli[t].reset (new std::atomic<uint16_t>[(size_t) numBlocks]);
        day[t].reset (new std::atomic<uint16_t>[(size_t) numBlocks]);
        dirty[t].reset (new std::atomic<uint8_t>[(size_t) numSeconds]);

        for (int b = 0; b < numBlocks; ++b) { flagsAt (t, b) = 0; recorderAt (t, b) = 0; speedMilliAt (t, b) = 0; dayAt (t, b) = 0; }
        for (int s = 0; s < numSeconds; ++s) dirtyAt (t, s) = 0;
    }
    passes.reset (new std::atomic<uint16_t>[(size_t) numSeconds]);
    for (int s = 0; s < numSeconds; ++s) passesAt (s) = 0;
    today = tfmt::dayNumberToday();
}

Tape::~Tape()
{
    for (auto* d : data)
        std::free (d);
}

juce::String Tape::created() const
{
    // "2026-10-27T09:30:00Z" -> "27 OCT 2026"
    static const char* months[] { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    auto m = createdIso.substring (5, 7).getIntValue();
    if (createdIso.length() < 10 || m < 1 || m > 12) return createdIso;
    return juce::String (createdIso.substring (8, 10).getIntValue()).paddedLeft ('0', 2) + " " + months[m - 1] + " " + createdIso.substring (0, 4);
}

void Tape::setCondition (const tfmt::Condition& c)
{
    condition = c;
    character.hissDb = (float) c.hissDb;
    character.dropoutsPerMinute = (float) c.dropoutsPerMinute;
    character.trebleLossDb = (float) c.trebleLossDb;
    character.modNoiseDb = (float) c.modNoiseDb;
    character.shellFlutterPct = (float) c.shellFlutterPct;
    metaDirty = true;
}

void Tape::useRecorder (const std::string& deckId)
{
    auto it = std::find (recorders.begin(), recorders.end(), deckId);
    if (it == recorders.end())
    {
        if (recorders.size() >= 255) return;   // the map has a byte for it
        recorders.push_back (deckId);
        metaDirty = true;
        it = recorders.end() - 1;
    }
    currentRecorder = (uint8_t) (std::distance (recorders.begin(), it) + 1);
}

float Tape::readInterpolated (int track, double pos) const noexcept
{
    auto i = (int64_t) std::floor (pos);
    auto f = (float) (pos - (double) i);
    auto y0 = readCell (track, i - 1), y1 = readCell (track, i), y2 = readCell (track, i + 1), y3 = readCell (track, i + 2);
    auto c1 = 0.5f * (y2 - y0);
    auto c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    auto c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

void Tape::writeCell (int track, int64_t c, float v, bool nrEncoded, int nrType, float speed, int pos) noexcept
{
    if (c < 0 || c >= lengthCells)
        return;

    data[track][c].store ((int16_t) juce::roundToInt (juce::jlimit (-1.0f, 1.0f, v * fluxScale) * 32767.0f), std::memory_order_relaxed);

    auto b = (int) (c / blockCells);
    flagsAt (track, b).store ((uint8_t) (1 | (nrEncoded ? 2 : 0) | ((juce::jlimit (0, 2, pos) + 1) << 2)
                                           | (nrEncoded ? (juce::jlimit (0, 2, nrType) + 1) << 4 : 0)),
                              std::memory_order_relaxed);
    recorderAt (track, b).store (currentRecorder.load (std::memory_order_relaxed), std::memory_order_relaxed);
    speedMilliAt (track, b).store ((uint16_t) juce::jlimit (0, 65535, juce::roundToInt (speed * 1000.0f)), std::memory_order_relaxed);
    dayAt (track, b).store (today.load (std::memory_order_relaxed), std::memory_order_relaxed);
    // Release: a saver that sees the flag also sees the cell.
    dirtyAt (track, (int) (c / cellRate)).store (1, std::memory_order_release);
    dataDirty.store (true, std::memory_order_release);
}

double Tape::usedSeconds() const
{
    int last = -1;
    for (int t = 0; t < numTracks; ++t)
        for (int b = numBlocks - 1; b > last; --b)
            if (isRecorded (t, b)) { last = b; break; }
    return (last + 1) * (double) blockCells / cellRate;
}

int Tape::tracksUsed() const
{
    int n = 0;
    for (int t = 0; t < numTracks; ++t)
        for (int b = 0; b < numBlocks; ++b)
            if (isRecorded (t, b)) { ++n; break; }
    return n;
}

bool Tape::needsSaving() const noexcept
{
    return dataDirty.load() || metaDirty.load() || wearDirty.load();
}

//==============================================================================
static juce::File pcmFile (const juce::File& folder, int t)  { return folder.getChildFile (tfmt::CassFiles::pcm (t)); }
static juce::File mapFile (const juce::File& folder, int t)  { return folder.getChildFile (tfmt::CassFiles::map (t)); }

bool Tape::writeAudio (const juce::File& folder)
{
    if (! folder.createDirectory())
        return false;
    if (! dataDirty.exchange (false, std::memory_order_acq_rel))
        return true;

    bool ok = true;
    std::vector<int16_t> chunk ((size_t) cellRate);
    const size_t bytesPerSecond = (size_t) cellRate * sizeof (int16_t);

    // Only the seconds that changed, plus anything beyond the current end of the file.
    for (int t = 0; t < numTracks; ++t)
    {
        int lastNeeded = -1;
        for (int s = numSeconds - 1; s >= 0; --s)
            if (dirtyAt (t, s).load (std::memory_order_acquire) != 0) { lastNeeded = s; break; }

        if (lastNeeded < 0)
            continue;

        auto file = pcmFile (folder, t);
        auto existing = file.existsAsFile() ? file.getSize() : 0;
        juce::FileOutputStream out (file);

        if (! out.openedOk())
        {
            ok = false;
            continue;
        }

        const bool reversed = reversedInMemory (t);
        for (int s = 0; s <= lastNeeded; ++s)
        {
            // A track held the other way round is written as it lies on the tape: this second of it
            // is the mirrored second of the file, its cells in reverse.
            auto offset = (int64_t) (reversed ? numSeconds - 1 - s : s) * (int64_t) bytesPerSecond;
            bool beyondEnd = offset >= existing;

            // Clear the flag before copying: if the audio thread writes this second again
            // while it's being copied, the flag goes back up and it is saved again next time.
            if (dirtyAt (t, s).exchange (0, std::memory_order_acq_rel) == 0 && ! beyondEnd)
                continue;

            auto firstCell = (int64_t) s * cellRate;
            auto count = (size_t) std::min<int64_t> (cellRate, lengthCells - firstCell);
            for (size_t i = 0; i < count; ++i)
                chunk[i] = data[t][firstCell + (int64_t) (reversed ? count - 1 - i : i)].load (std::memory_order_relaxed);

            out.setPosition (offset);
            if (! out.write (chunk.data(), count * sizeof (int16_t)))
            {
                ok = false;
                dirtyAt (t, s).store (1, std::memory_order_release);
            }
        }
        out.flush();
        if (out.getStatus().failed()) ok = false;
    }

    if (! ok)
        dataDirty = true;   // the seconds that didn't make it are still marked: the next save retries them
    return ok;
}

std::shared_ptr<Tape::Snapshot> Tape::snapshot() const
{
    auto s = std::make_shared<Snapshot>();
    auto& info = s->info;
    info.id = str (id);
    info.created = str (createdIso);
    info.madeBy = madeBy;
    info.label = str (name);
    info.formulation = formulation;
    info.minutesPerSide = typeInfo.minutes;
    info.cellRate = cellRate;
    info.cellsPerTrack = lengthCells;
    info.blockCells = blockCells;
    info.layout = layout;
    for (int t = 0; t < numTracks; ++t)
        info.tracks.push_back ({ tfmt::CassFiles::pcm (t), tfmt::CassFiles::map (t), "pcm-s16le", (double) fluxScale });
    info.recorders = recorders;
    info.condition = condition;
    info.inlayLayout = inlayLayout;
    for (auto& line : trackSheet) info.inlay.push_back (str (line));
    info.unknown = unknownManifest;

    auto& st = s->state;
    st.protectA = protectA.load();
    st.protectB = protectB.load();
    st.position = position.load();
    st.lastUsed = tfmt::isoNow();
    st.usedSeconds = usedSeconds();
    st.tracksUsed = tracksUsed();
    st.unknown = unknownState;

    s->maps.resize (numTracks);
    for (int t = 0; t < numTracks; ++t)
    {
        auto& m = s->maps[(size_t) t];
        m.resize ((size_t) numBlocks);
        for (int b = 0; b < numBlocks; ++b)
        {
            const auto k = (size_t) (reversedInMemory (t) ? numBlocks - 1 - b : b);   // as it lies on the tape
            m.flags[k] = flagsAt (t, b).load (std::memory_order_relaxed);
            m.recorder[k] = recorderAt (t, b).load (std::memory_order_relaxed);
            m.speedMilli[k] = speedMilliAt (t, b).load (std::memory_order_relaxed);
            m.day[k] = dayAt (t, b).load (std::memory_order_relaxed);
        }
    }
    if (wearDirty.exchange (false))
    {
        s->wear.resize ((size_t) numSeconds);
        for (int sec = 0; sec < numSeconds; ++sec) s->wear[(size_t) sec] = passesAt (sec).load (std::memory_order_relaxed);
    }
    return s;
}

bool Tape::writeSnapshot (const Snapshot& s, const juce::File& folder)
{
    if (! folder.createDirectory()) return false;
    auto dir = pathOf (folder);
    std::string err;
    // The maps first: the description never claims more than is on disk.
    bool ok = true;
    for (size_t t = 0; t < s.maps.size(); ++t)
    {
        const auto& m = s.maps[t];
        auto file = mapFile (folder, (int) t);
        bool any = std::any_of (m.flags.begin(), m.flags.end(), [] (uint8_t f) { return f != 0; });
        if (any || file.existsAsFile())   // a track never recorded on has no map
            ok = tfmt::writeMap (pathOf (file), m) && ok;
    }
    if (! s.wear.empty())
        ok = tfmt::writeWear (pathOf (folder.getChildFile (tfmt::CassFiles::wear)), s.wear) && ok;
    ok = tfmt::writeState (dir, s.state, err) && ok;
    ok = tfmt::writeManifest (dir, s.info, err) && ok;
    return ok;
}

bool Tape::writeSession (const juce::XmlElement& sessionXml, const juce::File& folder)
{
    if (! folder.createDirectory()) return false;
    auto target = folder.getChildFile (sessionFile);
    auto tmp = target.getSiblingFile (target.getFileName() + ".tmp");
    return sessionXml.writeTo (tmp) && tmp.moveFileTo (target);
}

bool Tape::save (const juce::File& folder)
{
    // Audio first: the description never claims a recording whose audio isn't on disk yet.
    bool ok = writeAudio (folder);
    if (session != nullptr && sessionNeedsWriting)
    {
        if (writeSession (*session, folder)) sessionNeedsWriting = false;
        else ok = false;
    }
    if (writeSnapshot (*snapshot(), folder))
        metaDirty = false;
    else
        ok = false;
    return ok;
}

//==============================================================================
static void readSession (Tape& tape, const juce::File& folder)
{
    if (auto x = juce::XmlDocument::parse (folder.getChildFile (sessionFile)); x != nullptr && x->hasTagName ("DECK"))
    {
        if (auto* td = x->getChildByName ("TAPE_DECK"))
            tape.counterZero = juce::jlimit (0.0, (double) tape.getLengthCells(), td->getDoubleAttribute ("counterZero"));
        tape.session = std::move (x);
    }
}

std::unique_ptr<Tape> Tape::load (const juce::File& folder)
{
    if (! folder.getChildFile (tfmt::CassFiles::manifest).existsAsFile())
        return loadLegacy (folder);
    return loadCassette (folder, false);
}

std::unique_ptr<Tape> Tape::loadForStereoDeck (const juce::File& folder)
{
    auto t = loadCassette (folder, true);
    return t != nullptr && t->layout == tfmt::Layout::stereoTwoSides ? std::move (t) : nullptr;
}

std::unique_ptr<Tape> Tape::makeStereo (int typeIndex, int modelIndex, const tfmt::Condition& c)
{
    auto t = std::make_unique<Tape> (typeIndex, modelIndex, c);
    t->layout = tfmt::Layout::stereoTwoSides;
    t->sidesInPlayingOrder = true;
    return t;
}

std::unique_ptr<Tape> Tape::loadCassette (const juce::File& folder, bool playingOrder)
{
    tfmt::CassetteInfo info;
    std::string err;
    if (! tfmt::readManifest (pathOf (folder), info, err))
        return {};
    // What this deck can hold: four tracks at its own cell rate.
    if (info.cellRate != cellRate || info.blockCells != blockCells || info.tracks.size() != (size_t) numTracks
        || info.cellsPerTrack <= 0 || info.cellsPerTrack > (int64_t) cellRate * 60 * 120)
        return {};

    std::unique_ptr<Tape> tape;
    try { tape = std::make_unique<Tape> (info); }
    catch (...) { return {}; }
    tape->sidesInPlayingOrder = playingOrder;

    tfmt::CassetteState st;
    if (tfmt::readState (pathOf (folder), st, err))
    {
        tape->position = juce::jlimit (0.0, (double) tape->lengthCells, st.position);
        tape->protectA = st.protectA;
        tape->protectB = st.protectB;
        tape->unknownState = st.unknown;
    }

    for (int t = 0; t < numTracks; ++t)
    {
        tfmt::TrackMap m;
        tfmt::readMap (pathOf (folder.getChildFile (str (info.tracks[(size_t) t].map))), (size_t) tape->numBlocks, m);
        const bool reversed = tape->reversedInMemory (t);
        for (int b = 0; b < tape->numBlocks; ++b)
        {
            const auto k = (size_t) (reversed ? tape->numBlocks - 1 - b : b);
            tape->flagsAt (t, b) = m.flags[k];
            tape->recorderAt (t, b) = m.recorder[k];
            tape->speedMilliAt (t, b) = m.speedMilli[k];
            tape->dayAt (t, b) = m.day[k];
        }

        auto file = folder.getChildFile (str (info.tracks[(size_t) t].pcm));
        if (file.existsAsFile())
        {
            juce::FileInputStream in (file);
            if (in.openedOk())
            {
                auto cells = std::min<int64_t> (in.getTotalLength() / (int64_t) sizeof (int16_t), tape->lengthCells);
                // Nothing else can see this tape yet, so read straight into it.
                in.read (static_cast<void*> (tape->data[t]), (int) std::min<int64_t> (cells * (int64_t) sizeof (int16_t), (int64_t) std::numeric_limits<int>::max()));
                if (reversed)
                {
                    // The file is the tape as it lies; this deck holds side B the way it plays.
                    auto* d = tape->data[t];
                    const auto len = tape->lengthCells;
                    for (int64_t c = 0; c < len / 2; ++c)
                    {
                        auto a = d[c].load (std::memory_order_relaxed), z = d[len - 1 - c].load (std::memory_order_relaxed);
                        d[c].store (z, std::memory_order_relaxed);
                        d[len - 1 - c].store (a, std::memory_order_relaxed);
                    }
                }
            }
        }
    }

    std::vector<uint16_t> wear;
    tfmt::readWear (pathOf (folder.getChildFile (tfmt::CassFiles::wear)), (size_t) tape->numSeconds, wear);
    for (int sec = 0; sec < tape->numSeconds; ++sec) tape->passesAt (sec) = wear[(size_t) sec];

    readSession (*tape, folder);
    tape->dataDirty = false;
    tape->metaDirty = false;
    tape->wearDirty = false;
    return tape;
}

/** A tape from before the cassette format (tape.xml + deck.xml), converted in place. */
std::unique_ptr<Tape> Tape::loadLegacy (const juce::File& folder)
{
    auto xml = juce::XmlDocument::parse (folder.getChildFile ("tape.xml"));
    if (xml == nullptr || ! xml->hasTagName ("TAPEFOUR_TAPE"))
        return {};

    // The old deck file held the tape's character with everything else; hiss and dropouts were
    // the cassette's, and become its condition.
    auto deckXml = juce::XmlDocument::parse (folder.getChildFile ("deck.xml"));
    if (deckXml == nullptr || ! deckXml->hasTagName ("DECK"))
        if (auto* inside = xml->getChildByName ("DECK"))
            deckXml = std::make_unique<juce::XmlElement> (*inside);
    tfmt::Condition cond;
    {
        auto* td = deckXml != nullptr ? deckXml->getChildByName ("TAPE_DECK") : nullptr;
        auto hiss = td != nullptr ? td->getDoubleAttribute ("hiss", 0.55) : 0.55;
        auto drop = td != nullptr ? td->getDoubleAttribute ("drop", 0.10) : 0.10;
        cond.hissDb = std::round (juce::jlimit (-12.0, 12.0, (hiss - 0.5) * 24.0) * 100.0) / 100.0;
        cond.dropoutsPerMinute = std::round (drop * drop * 2.0 * 60.0 * 1000.0) / 1000.0;
        cond.grade = cond.dropoutsPerMinute < 0.1 ? "new" : cond.dropoutsPerMinute < 0.5 ? "used" : cond.dropoutsPerMinute < 2.5 ? "worn" : "chewed";
    }

    std::unique_ptr<Tape> tape;
    try { tape = std::make_unique<Tape> (xml->getIntAttribute ("type", 1), xml->getIntAttribute ("model", defaultTapeModel), cond); }
    catch (...) { return {}; }
    tape->setFluxScale ((float) juce::jlimit (0.1, 1.0, xml->getDoubleAttribute ("fluxScale", 1.0)));

    tape->id          = xml->getStringAttribute ("id", tape->id);
    tape->seed        = fnv1a (tape->id);
    tape->name        = xml->getStringAttribute ("name");
    tape->position    = juce::jlimit (0.0, (double) tape->lengthCells, xml->getDoubleAttribute ("position"));
    tape->counterZero = juce::jlimit (0.0, (double) tape->lengthCells, xml->getDoubleAttribute ("counterZero"));
    tape->madeBy      = "TAPE-FOUR (before the cassette format)";
    {
        // "27 OCT 2026" -> ISO; the time of day wasn't kept.
        auto parts = juce::StringArray::fromTokens (xml->getStringAttribute ("created"), " ", {});
        static const char* months[] { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
        int m = 0;
        for (int i = 0; i < 12; ++i) if (parts.size() == 3 && parts[1].equalsIgnoreCase (months[i])) m = i + 1;
        if (m > 0)
            tape->createdIso = parts[2] + "-" + juce::String (m).paddedLeft ('0', 2) + "-" + parts[0].paddedLeft ('0', 2) + "T00:00:00Z";
        tape->madeDay = tfmt::dayNumberOf (str (tape->createdIso));
    }

    if (auto* sheet = xml->getChildByName ("TRACK_SHEET"))
    {
        juce::StringArray lines;
        for (auto* line : sheet->getChildIterator())
            lines.add (line->getStringAttribute ("text"));

        if (sheet->getStringAttribute ("layout") == "inlay1")
        {
            for (int i = 0; i < std::min (lines.size(), (int) Inlay::numFields); ++i)
                tape->trackSheet.set (i, lines[i]);
        }
        else
        {
            // The old eight free lines: title, then the four tracks, then notes as songs.
            tape->trackSheet.set (Inlay::title, lines[0]);
            for (int t = 0; t < numTracks; ++t)
                tape->trackSheet.set (Inlay::trackNote (t), lines[1 + t]);
            for (int i = 0; i < 3; ++i)
                tape->trackSheet.set (Inlay::songName (i), lines[5 + i]);
        }
    }

    for (auto* tr : xml->getChildWithTagNameIterator ("TRACK"))
    {
        auto t = tr->getIntAttribute ("index", -1);
        if (t < 0 || t >= numTracks) continue;

        juce::MemoryBlock fb, sb;
        fb.fromBase64Encoding (tr->getStringAttribute ("flags"));
        sb.fromBase64Encoding (tr->getStringAttribute ("speeds"));
        auto nb = std::min ((int) fb.getSize(), tape->numBlocks);
        auto* fp = static_cast<const uint8_t*> (fb.getData());
        auto* sp = static_cast<const uint8_t*> (sb.getData());
        for (int b = 0; b < nb; ++b)
        {
            tape->flagsAt (t, b) = fp[b];
            if ((fp[b] & 1) != 0)
            {
                tape->recorderAt (t, b) = 1;   // this deck recorded everything on it...
                tape->dayAt (t, b) = (uint16_t) std::max (1, (int) tfmt::dayNumberOf (str (tape->createdIso)));   // ...some time after it was made
            }
            if ((size_t) b * 2 + 1 < sb.getSize())
                tape->speedMilliAt (t, b) = (uint16_t) (sp[b * 2] | (sp[b * 2 + 1] << 8));
        }

        auto file = pcmFile (folder, t);
        if (file.existsAsFile())
        {
            juce::FileInputStream in (file);
            if (in.openedOk())
            {
                auto cells = std::min<int64_t> (in.getTotalLength() / (int64_t) sizeof (int16_t), tape->lengthCells);
                in.read (static_cast<void*> (tape->data[t]), (int) std::min<int64_t> (cells * (int64_t) sizeof (int16_t), (int64_t) std::numeric_limits<int>::max()));
            }
        }
    }

    // The session: the old deck file, with the counter's zero now kept in it.
    if (deckXml != nullptr)
    {
        auto* td = deckXml->getChildByName ("TAPE_DECK");
        if (td == nullptr) td = deckXml->createNewChildElement ("TAPE_DECK");
        td->setAttribute ("counterZero", tape->counterZero.load());
        tape->session = std::move (deckXml);
    }

    // Convert: the cassette's files, then the old ones kept aside (the track audio is the same).
    tape->dataDirty = false;
    bool converted = writeSnapshot (*tape->snapshot(), folder)
                     && (tape->session == nullptr || writeSession (*tape->session, folder));
    if (converted)
    {
        folder.getChildFile ("tape.xml").moveFileTo (folder.getChildFile ("tape-v2.xml.bak"));
        folder.getChildFile ("deck.xml").moveFileTo (folder.getChildFile ("deck-v2.xml.bak"));
    }
    tape->metaDirty = ! converted;
    tape->sessionNeedsWriting = ! converted && tape->session != nullptr;
    return tape;
}
} // namespace tf
