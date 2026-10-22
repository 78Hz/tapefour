#pragma once

#include "Common.h"

namespace tf
{
/**
    One cassette: four mono tracks of tape, plus what was recorded where
    (whether noise reduction was encoding, and at what speed).

    Memory is allocated zeroed and only becomes real as it is written to,
    so a blank C-90 costs almost nothing until you record on it.
*/
class Tape
{
public:
    explicit Tape (int typeIndex);
    ~Tape();

    int getType() const noexcept                { return type; }
    const TapeTypeInfo& getTypeInfo() const     { return tapeTypes[type]; }
    int64_t getLengthCells() const noexcept     { return lengthCells; }
    double getLengthSeconds() const noexcept    { return (double) lengthCells / cellRate; }
    int getNumBlocks() const noexcept           { return numBlocks; }
    int blockAt (double pos) const noexcept     { return juce::jlimit (0, numBlocks - 1, (int) (pos / blockCells)); }

    // ---- audio thread ----
    inline float readCell (int track, int64_t c) const noexcept
    {
        if (c < 0 || c >= lengthCells) return 0.0f;
        return (float) data[track][c] * (1.0f / 32767.0f);
    }

    float readInterpolated (int track, double pos) const noexcept;
    void writeCell (int track, int64_t c, float v, bool nrEncoded, float speed) noexcept;

    // ---- metadata (safe to read from any thread) ----
    bool isRecorded (int track, int block) const noexcept   { return (flags[track][block].load (std::memory_order_relaxed) & 1) != 0; }
    bool isNrEncoded (int track, int block) const noexcept  { return (flags[track][block].load (std::memory_order_relaxed) & 2) != 0; }
    float recordedSpeed (int track, int block) const noexcept { return speedMilli[track][block].load (std::memory_order_relaxed) / 1000.0f; }
    double usedSeconds() const;
    int tracksUsed() const;

    std::atomic<double> position { 0.0 };   // in cells
    std::atomic<double> counterZero { 0.0 };

    juce::String id, name, created;
    juce::StringArray trackSheet;

    // ---- persistence (message thread, with the transport stopped) ----
    bool save (const juce::File& folder);
    static std::unique_ptr<Tape> load (const juce::File& folder);
    bool needsSaving() const noexcept;
    void markMetaChanged() noexcept { metaDirty = true; }

private:
    int type;
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
