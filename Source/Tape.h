#pragma once

#include "Common.h"
#include "Geometry.h"
#include "../Formats/Cassette.h"

namespace tf
{
/** Where each field of the cassette inlay lives in Tape::trackSheet. */
namespace Inlay
{
    enum : int { title = 0, songsStart = 1, numSongs = 8, tracksStart = 17, nr = 21, speed = 22, date = 23, numFields = 24 };
    inline int songCounter (int i) { return songsStart + i * 2; }
    inline int songName (int i)    { return songsStart + i * 2 + 1; }
    inline int trackNote (int t)   { return tracksStart + t; }
}

/** The id TAPE-FOUR's own machine writes into the cassettes it records (docs/formats/deck-v1.md). */
inline constexpr const char* tf4DeckId = "tapefour.tf-4";

/**
    One cassette, in memory: the tape itself (its formulation, length and condition) and
    what is recorded on it, four mono tracks plus what was recorded where.
    Saved in the cassette format (docs/formats/cass-v1.md); the session that goes with it
    (mixer, chains...) is kept beside it but is not part of it.

    Memory is allocated zeroed and only becomes real as it is written to,
    so a blank C-90 costs almost nothing until you record on it.
*/
class Tape
{
public:
    /** A new blank cassette of one of the standard lengths and formulations. */
    explicit Tape (int typeIndex, int modelIndex = defaultTapeModel, const tfmt::Condition& condition = tfmt::Condition::forGrade ("new"));
    /** A cassette as its description says (any length, any formulation). */
    explicit Tape (const tfmt::CassetteInfo& info);
    ~Tape();

    const TapeTypeInfo& getTypeInfo() const     { return typeInfo; }
    /** The built-in formulation this tape is, or the nearest one (for its artwork). */
    int getModel() const noexcept               { return model; }
    const TapeModel& getModelInfo() const       { return modelInfo; }
    /** What's printed on the shell: "SD60", "MX46"... */
    juce::String productName() const            { return juce::String (getModelInfo().name) + juce::String (getTypeInfo().minutes * 2); }
    int64_t getLengthCells() const noexcept     { return lengthCells; }
    double getLengthSeconds() const noexcept    { return (double) lengthCells / cellRate; }
    int blockAt (double pos) const noexcept     { return juce::jlimit (0, numBlocks - 1, (int) (pos / blockCells)); }
    /** A number from the cassette's id, the same every time: places its dropouts. */
    uint64_t getSeed() const noexcept           { return seed; }
    tfmt::Layout getLayout() const noexcept     { return layout; }

    // ---- turning the cassette over ----
    /** Side B up: the deck sees the tape from the other side, as a real one does when you turn the
        cassette over. Everything on it plays backwards, and the tracks come in mirrored: what was
        recorded on track 1 is under the track 4 head, track 2 under track 3. Tracks, cells, blocks
        and positions given to the Tape are the deck's; they're turned into places on the tape here.
        Message thread, transport stopped; the deck's position is turned round with it. */
    void turnOver (bool sideBUp) noexcept;
    bool isTurnedOver() const noexcept          { return turnedOver.load (std::memory_order_relaxed); }
    /** Where on the tape the deck's track and position are. A 4-track tape turned over comes in
        mirrored (track 1 under the track 4 head), as it lies; a stereo cassette's side B is its
        tracks 3 and 4, left and right (cass-v1.md, layout). */
    int physicalTrack (int track) const noexcept
    {
        if (! isTurnedOver()) return track;
        return layout == tfmt::Layout::stereoTwoSides ? (track + 2) % numTracks : numTracks - 1 - track;
    }
    double physicalPos (double pos) const noexcept { return isTurnedOver() ? (double) lengthCells - pos : pos; }

    /** The clear leaders at the two ends of the tape itself (cells of travel): end 0 is before
        side A's start, end 1 after its end. A few seconds each, different for every cassette
        (from its id, Geometry::leaderSeconds), and the same wherever it's played. Nothing can be
        recorded on a leader, and it carries no hiss: it has no oxide. */
    double leaderCells (int end) const noexcept  { return leader[end & 1]; }
    /** The leader before and after the recordable tape as the deck sees it (turned over swaps them).
        The deck's position runs from -leadBefore() (fully rewound) to length + leadAfter(). */
    double leadBefore() const noexcept { return isTurnedOver() ? leader[1] : leader[0]; }
    double leadAfter() const noexcept  { return isTurnedOver() ? leader[0] : leader[1]; }
    double startOfTape() const noexcept { return -leadBefore(); }
    double endOfTape() const noexcept   { return (double) lengthCells + leadAfter(); }
    /** How much of the whole tape, leaders included, is on the right-hand spool at this position (0..1). */
    double rightFraction (double pos) const noexcept
    {
        const auto total = (double) lengthCells + leader[0] + leader[1];
        return total > 0.0 ? std::clamp ((pos + leadBefore()) / total, 0.0, 1.0) : 0.0;
    }
    /** The pack's diameter (mm) with all of this cassette's tape on one hub (Geometry.h). */
    float fullPackMm() const noexcept { return fullPack; }
    /** The tab of the side facing up is broken out (a stereo cassette in the TF-4: either side's). */
    bool isWriteProtected() const noexcept
    {
        if (layout == tfmt::Layout::stereoTwoSides) return protectA.load() || protectB.load();
        return isTurnedOver() ? protectB.load() : protectA.load();
    }

    // ---- audio thread ----
    inline float readCell (int track, int64_t c) const noexcept
    {
        if (c < 0 || c >= lengthCells) return 0.0f;
        if (isTurnedOver()) { track = physicalTrack (track); c = lengthCells - 1 - c; }
        if (c >= readable[track]) return 0.0f;   // past the end of what a packed cassette holds
        return (float) data[track][c].load (std::memory_order_relaxed) * readScale;
    }

    float readInterpolated (int track, double pos) const noexcept;
    void writeCell (int track, int64_t c, float v, bool nrEncoded, int nrType, float speed, int position) noexcept;

    // ---- wear and age (docs/formats/cass-v1.md; amounts in Wear.h) ----
    /** One more pass over the heads at this second of tape (audio thread). */
    void addPass (int64_t second) noexcept
    {
        if (second < 0 || second >= numSeconds) return;
        if (secondsReversed()) second = numSeconds - 1 - second;   // passes are counted along the tape itself
        auto& p = passesAt ((int) second);
        auto v = p.load (std::memory_order_relaxed);
        if (v < 65535) p.store ((uint16_t) (v + 1), std::memory_order_relaxed);
        wearDirty.store (true, std::memory_order_release);
    }
    int passesAt (double pos) const noexcept
    {
        auto s = juce::jlimit (0, numSeconds - 1, (int) (pos / cellRate));
        return passesAt (secondsReversed() ? numSeconds - 1 - s : s).load (std::memory_order_relaxed);
    }

    // ---- two-sided cassettes ----
    /** A stereo cassette held side by side: tracks 3 and 4 (side B) in the order they play, so a
        stereo deck runs both sides forwards. A deck that only plays one way (the TF-4) holds them as
        they lie on the tape, so side B plays backwards in it, as on a real multitracker. */
    bool holdsSidesInPlayingOrder() const noexcept { return sidesInPlayingOrder; }
    /** Which side is in front of the heads (for counting passes along the tape). */
    void setSideB (bool b) noexcept             { sideB = b; }
    /** The day a stretch was recorded (days since 2000-01-01), or 0 if unknown or blank. */
    int recordedDay (int track, int block) const noexcept { return dayAt (deckTrack (track), deckBlock (block)).load (std::memory_order_relaxed); }
    /** Today, as the cassette counts it: stamps new recordings, and ages old ones. The deck keeps it current. */
    void setToday (uint16_t dayNumber) noexcept { today = dayNumber; }
    int getToday() const noexcept               { return today.load (std::memory_order_relaxed); }
    /** Years since the cassette was made. */
    float ageYears() const noexcept             { return madeDay > 0 ? std::max (0.0f, (float) (getToday() - madeDay) / 365.25f) : 0.0f; }
    /** Test hook: every second of the tape as if it had passed the heads this often. */
    void setPassesForTests (uint16_t n) noexcept { for (int s = 0; s < numSeconds; ++s) passesAt (s) = n; }

    /** The cassette's condition as the deck hears it (its starting condition; wear and age add to it). */
    struct Character
    {
        std::atomic<float> hissDb { 0 }, dropoutsPerMinute { 0 }, trebleLossDb { 0 }, modNoiseDb { 0 }, shellFlutterPct { 0 };
    };
    const Character& getCharacter() const noexcept { return character; }

    // ---- metadata (safe to read from any thread) ----
    // (track and block as the deck sees them: see turnOver)
    bool isRecorded (int track, int block) const noexcept   { return (flagsAt (deckTrack (track), deckBlock (block)).load (std::memory_order_relaxed) & 1) != 0; }
    bool isNrEncoded (int track, int block) const noexcept  { return (flagsAt (deckTrack (track), deckBlock (block)).load (std::memory_order_relaxed) & 2) != 0; }
    /** The tape switch position this stretch was recorded at (0 normal, 1 chrome, 2 metal), or the tape's own if not known. */
    int recordedPosition (int track, int block) const noexcept
    {
        auto f = (flagsAt (deckTrack (track), deckBlock (block)).load (std::memory_order_relaxed) >> 2) & 3;
        return f == 0 ? naturalPosition() : f - 1;
    }
    /** The NR system this stretch was encoded with (0 2:1, 1 B, 2 C), or -1 if not known. */
    int recordedNrType (int track, int block) const noexcept
    {
        return ((flagsAt (deckTrack (track), deckBlock (block)).load (std::memory_order_relaxed) >> 4) & 7) - 1;
    }
    int naturalPosition() const noexcept        { return positionForIec (getModelInfo().iecType); }
    float recordedSpeed (int track, int block) const noexcept { return speedMilliAt (deckTrack (track), deckBlock (block)).load (std::memory_order_relaxed) / 1000.0f; }
    double usedSeconds() const;
    int tracksUsed() const;

    std::atomic<double> position { 0.0 };   // in cells, as the deck sees it; -leadBefore() is fully rewound
    std::atomic<bool> protectA { false }, protectB { false };   // write-protect tabs broken out

    // ---- the cassette's description (message thread) ----
    juce::String id, name;
    juce::String createdIso;                     // ISO 8601, UTC
    juce::String created() const;                // as printed: "27 OCT 2026"
    juce::StringArray trackSheet;                // the inlay card: fixed fields, see Inlay
    const tfmt::Condition& getCondition() const noexcept { return condition; }
    void setCondition (const tfmt::Condition& c);
    /** Makes sure a deck is listed among the cassette's recorders; recordings from now on name it. */
    void useRecorder (const std::string& deckId);

    /** The session that goes with this tape (faders, chains, plug-in settings...). */
    std::unique_ptr<juce::XmlElement> session;
    /** The session has changed since it was last written. */
    bool sessionNeedsWriting = false;

    // ---- persistence ----
    // Saving comes in three parts so the slow one can run on a background thread, even while recording:
    //   snapshot() is taken on the message thread;
    //   writeAudio() writes the seconds of tape that changed, safely, from any thread;
    //   writeSnapshot() / writeSession() replace their files atomically.
    struct Snapshot
    {
        tfmt::CassetteInfo info;
        tfmt::CassetteState state;
        std::vector<tfmt::TrackMap> maps;
        std::vector<uint16_t> wear;   // empty: unchanged since the last save
    };
    std::shared_ptr<Snapshot> snapshot() const;
    bool writeAudio (const juce::File& folder);
    static bool writeSnapshot (const Snapshot&, const juce::File& folder);
    static bool writeSession (const juce::XmlElement& session, const juce::File& folder);
    void restoreDirtyAfterFailedSave() noexcept { dataDirty = true; metaDirty = true; }

    /** Everything at once, on the calling thread. */
    bool save (const juce::File& folder);
    /** A cassette from its folder. A folder from before the cassette format is converted. */
    static std::unique_ptr<Tape> load (const juce::File& folder);
    /** A stereo cassette for a stereo deck: side B held in playing order (see holdsSidesInPlayingOrder). */
    static std::unique_ptr<Tape> loadForStereoDeck (const juce::File& folder);
    /** A packed .cass, opened read-only where it lies: its tracks are read from the file, not
        loaded, so a C-90 costs a few MB. For players; nothing can be recorded on it or saved. */
    static std::unique_ptr<Tape> openPacked (const juce::File& cassFile, juce::String& error);
    bool isReadOnly() const noexcept            { return readOnly; }
    /** How many tracks need no memory of their own: read straight from a packed cassette's file, or never recorded. */
    int mappedTracks() const noexcept           { int n = 0; for (auto o : ownsTrack) n += o ? 0 : 1; return n; }
    /** A new blank stereo cassette for a stereo deck. */
    static std::unique_ptr<Tape> makeStereo (int typeIndex, int modelIndex, const tfmt::Condition& condition);
    bool needsSaving() const noexcept;
    bool hasUnsavedAudio() const noexcept       { return dataDirty.load(); }
    void markMetaChanged() noexcept { metaDirty = true; }
    void markMetaSaved() noexcept   { metaDirty = false; }

private:
    Tape (const tfmt::CassetteInfo&, bool withTracks);
    void allocate (bool withTracks);
    static std::unique_ptr<Tape> loadLegacy (const juce::File& folder);
    static std::unique_ptr<Tape> loadCassette (const juce::File& folder, bool sidesInPlayingOrder);
    /** Track t is held the other way round from how it lies on the tape. */
    bool reversedInMemory (int t) const noexcept { return sidesInPlayingOrder && layout == tfmt::Layout::stereoTwoSides && t >= 2; }
    bool sidesInPlayingOrder = false;
    std::atomic<bool> sideB { false }, turnedOver { false };
    bool secondsReversed() const noexcept { return sideB.load (std::memory_order_relaxed) || isTurnedOver(); }
    // The deck's track and block, as places on the tape (see turnOver). With a length in whole
    // blocks (every standard cassette), block b turned over is exactly block numBlocks - 1 - b.
    int deckTrack (int t) const noexcept { return physicalTrack (t); }
    int deckBlock (int b) const noexcept { return isTurnedOver() ? numBlocks - 1 - b : b; }

    TapeTypeInfo typeInfo {};
    TapeModel modelInfo {};
    int model = defaultTapeModel;
    std::string lengthName, formulationStrings[4];   // backing for typeInfo / modelInfo's names when not built in
    tfmt::Formulation formulation;
    tfmt::Condition condition;
    Character character;
    std::vector<std::string> recorders;
    std::atomic<uint8_t> currentRecorder { 0 };
    uint64_t seed = 0;
    double leader[2] { 3.0 * cellRate, 3.0 * cellRate };
    float fullPack = 47.0f;
    void placeLeaders() noexcept;
    tfmt::Layout layout = tfmt::Layout::fourTrackOneWay;
    std::string madeBy;
    tfmt::Json unknownManifest, unknownState;   // members from newer versions, kept on rewrite

    // Cells hold flux at half scale, so a hot metal tape has room above 0 dBFS.
    // Tapes made before that stored it at full scale (fluxScale 1).
    float fluxScale = 0.5f, readScale = 1.0f / (32767.0f * 0.5f);
    void setFluxScale (float s) noexcept { fluxScale = s; readScale = 1.0f / (32767.0f * s); }
    int64_t lengthCells = 0;
    int numBlocks = 0, numSeconds = 0;
    // Written by the audio thread while a background thread saves: relaxed atomics, which cost
    // nothing on any current CPU but make the sharing well-defined.
    using Cell = std::atomic<int16_t>;
    static_assert (sizeof (Cell) == sizeof (int16_t) && Cell::is_always_lock_free);
    // Per track, per block (or per second) of tape; indexed through these so the conversions live in one place.
    std::atomic<uint8_t>&  flagsAt (int t, int b) const noexcept      { return flags[t][(size_t) b]; }
    std::atomic<uint8_t>&  recorderAt (int t, int b) const noexcept   { return recorder[t][(size_t) b]; }
    std::atomic<uint16_t>& speedMilliAt (int t, int b) const noexcept { return speedMilli[t][(size_t) b]; }
    std::atomic<uint16_t>& dayAt (int t, int b) const noexcept        { return day[t][(size_t) b]; }
    std::atomic<uint16_t>& passesAt (int s) const noexcept            { return passes[(size_t) s]; }
    std::atomic<uint8_t>&  dirtyAt (int t, int s) const noexcept      { return dirty[t][(size_t) s]; }

    Cell* data[numTracks] {};
    bool ownsTrack[numTracks] { true, true, true, true };
    int64_t readable[numTracks] {};                       // cells there are to read on each track
    std::unique_ptr<juce::MemoryMappedFile> mapped;       // a packed cassette's file, its tracks read in place
    bool readOnly = false;
    std::unique_ptr<std::atomic<uint8_t>[]>  flags[numTracks], recorder[numTracks];
    std::unique_ptr<std::atomic<uint16_t>[]> speedMilli[numTracks], day[numTracks];
    std::unique_ptr<std::atomic<uint16_t>[]> passes;            // per second of tape
    std::atomic<uint16_t> today { 0 };
    int madeDay = 0;
    std::unique_ptr<std::atomic<uint8_t>[]>  dirty[numTracks];   // per second of tape
    std::atomic<bool> dataDirty { false }, metaDirty { true };
    mutable std::atomic<bool> wearDirty { false };   // taken by snapshot(): the passes are copied out with it

    JUCE_DECLARE_NON_COPYABLE (Tape)
};

/** A built-in formulation as the cassette format writes it. */
tfmt::Formulation formulationOf (const TapeModel&);
} // namespace tf
