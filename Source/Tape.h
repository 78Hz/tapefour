#pragma once

#include "Common.h"

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

/**
    One cassette: four mono tracks of tape, plus what was recorded where
    (whether noise reduction was encoding, and at what speed).

    Memory is allocated zeroed and only becomes real as it is written to,
    so a blank C-90 costs almost nothing until you record on it.
*/
class Tape
{
public:
    explicit Tape (int typeIndex, int modelIndex = defaultTapeModel);
    ~Tape();

    int getType() const noexcept                { return type; }
    const TapeTypeInfo& getTypeInfo() const     { return tapeTypes[type]; }
    int getModel() const noexcept               { return model; }
    const TapeModel& getModelInfo() const       { return tapeModels[model]; }
    /** What's printed on the shell: "SD60", "MX46"... */
    juce::String productName() const            { return juce::String (getModelInfo().name) + juce::String (getTypeInfo().minutes * 2); }
    int64_t getLengthCells() const noexcept     { return lengthCells; }
    double getLengthSeconds() const noexcept    { return (double) lengthCells / cellRate; }
    int getNumBlocks() const noexcept           { return numBlocks; }
    int blockAt (double pos) const noexcept     { return juce::jlimit (0, numBlocks - 1, (int) (pos / blockCells)); }

    // ---- audio thread ----
    inline float readCell (int track, int64_t c) const noexcept
    {
        if (c < 0 || c >= lengthCells) return 0.0f;
        return (float) data[track][c] * readScale;
    }

    float readInterpolated (int track, double pos) const noexcept;
    void writeCell (int track, int64_t c, float v, bool nrEncoded, float speed, int position) noexcept;

    // ---- metadata (safe to read from any thread) ----
    bool isRecorded (int track, int block) const noexcept   { return (flags[track][block].load (std::memory_order_relaxed) & 1) != 0; }
    bool isNrEncoded (int track, int block) const noexcept  { return (flags[track][block].load (std::memory_order_relaxed) & 2) != 0; }
    /** The tape switch position this stretch was recorded at (0 normal, 1 chrome, 2 metal), or the tape's own if not known. */
    int recordedPosition (int track, int block) const noexcept
    {
        auto f = (flags[track][block].load (std::memory_order_relaxed) >> 2) & 3;
        return f == 0 ? naturalPosition() : f - 1;
    }
    int naturalPosition() const noexcept        { return positionForIec (getModelInfo().iecType); }
    float recordedSpeed (int track, int block) const noexcept { return speedMilli[track][block].load (std::memory_order_relaxed) / 1000.0f; }
    double usedSeconds() const;
    int tracksUsed() const;

    std::atomic<double> position { 0.0 };   // in cells
    std::atomic<double> counterZero { 0.0 };

    juce::String id, name, created;

    /** The inlay card: fixed fields, see Inlay below. */
    juce::StringArray trackSheet;

    /** The deck as it was when this tape was last saved (faders, patches, plug-in settings...). */
    std::unique_ptr<juce::XmlElement> deckState;

    // ---- persistence (message thread, with the transport stopped) ----
    bool save (const juce::File& folder);
    static std::unique_ptr<Tape> load (const juce::File& folder);
    bool needsSaving() const noexcept;
    void markMetaChanged() noexcept { metaDirty = true; }

private:
    int type, model;
    // Cells hold flux at half scale, so a hot metal tape has room above 0 dBFS.
    // Tapes made before that stored it at full scale (fluxScale 1).
    float fluxScale = 0.5f, readScale = 1.0f / (32767.0f * 0.5f);
    void setFluxScale (float s) noexcept { fluxScale = s; readScale = 1.0f / (32767.0f * s); }
    int64_t lengthCells;
    int numBlocks, numSeconds;
    int16_t* data[numTracks] {};
    std::unique_ptr<std::atomic<uint8_t>[]>  flags[numTracks];
    std::unique_ptr<std::atomic<uint16_t>[]> speedMilli[numTracks];
    std::unique_ptr<std::atomic<uint8_t>[]>  dirty[numTracks];   // per second of tape
    std::atomic<bool> dataDirty { false }, metaDirty { true };

    JUCE_DECLARE_NON_COPYABLE (Tape)
};
} // namespace tf
