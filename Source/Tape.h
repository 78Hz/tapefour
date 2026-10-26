#pragma once

#include "Common.h"
#include "Cassette.h"

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

    // ---- audio thread ----
    inline float readCell (int track, int64_t c) const noexcept
    {
        if (c < 0 || c >= lengthCells) return 0.0f;
        return (float) data[track][c].load (std::memory_order_relaxed) * readScale;
    }

    float readInterpolated (int track, double pos) const noexcept;
    void writeCell (int track, int64_t c, float v, bool nrEncoded, int nrType, float speed, int position) noexcept;

    /** The cassette's condition as the deck hears it (its starting condition; wear and age add to it). */
    struct Character
    {
        std::atomic<float> hissDb { 0 }, dropoutsPerMinute { 0 }, trebleLossDb { 0 }, modNoiseDb { 0 }, shellFlutterPct { 0 };
    };
    const Character& getCharacter() const noexcept { return character; }

    // ---- metadata (safe to read from any thread) ----
    bool isRecorded (int track, int block) const noexcept   { return (flagsAt (track, block).load (std::memory_order_relaxed) & 1) != 0; }
    bool isNrEncoded (int track, int block) const noexcept  { return (flagsAt (track, block).load (std::memory_order_relaxed) & 2) != 0; }
    /** The tape switch position this stretch was recorded at (0 normal, 1 chrome, 2 metal), or the tape's own if not known. */
    int recordedPosition (int track, int block) const noexcept
    {
        auto f = (flagsAt (track, block).load (std::memory_order_relaxed) >> 2) & 3;
        return f == 0 ? naturalPosition() : f - 1;
    }
    /** The NR system this stretch was encoded with (0 2:1, 1 B, 2 C), or -1 if not known. */
    int recordedNrType (int track, int block) const noexcept
    {
        return ((flagsAt (track, block).load (std::memory_order_relaxed) >> 4) & 7) - 1;
    }
    int naturalPosition() const noexcept        { return positionForIec (getModelInfo().iecType); }
    float recordedSpeed (int track, int block) const noexcept { return speedMilliAt (track, block).load (std::memory_order_relaxed) / 1000.0f; }
    double usedSeconds() const;
    int tracksUsed() const;

    std::atomic<double> position { 0.0 };   // in cells
    std::atomic<double> counterZero { 0.0 };   // the deck's counter memory (kept in the session)
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
    bool needsSaving() const noexcept;
    bool hasUnsavedAudio() const noexcept       { return dataDirty.load(); }
    void markMetaChanged() noexcept { metaDirty = true; }
    void markMetaSaved() noexcept   { metaDirty = false; }

private:
    void allocate();
    static std::unique_ptr<Tape> loadLegacy (const juce::File& folder);

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
    std::atomic<uint8_t>&  dirtyAt (int t, int s) const noexcept      { return dirty[t][(size_t) s]; }

    Cell* data[numTracks] {};
    std::unique_ptr<std::atomic<uint8_t>[]>  flags[numTracks], recorder[numTracks];
    std::unique_ptr<std::atomic<uint16_t>[]> speedMilli[numTracks];
    std::unique_ptr<std::atomic<uint8_t>[]>  dirty[numTracks];   // per second of tape
    std::atomic<bool> dataDirty { false }, metaDirty { true };

    JUCE_DECLARE_NON_COPYABLE (Tape)
};

/** A built-in formulation as the cassette format writes it. */
tfmt::Formulation formulationOf (const TapeModel&);
} // namespace tf
