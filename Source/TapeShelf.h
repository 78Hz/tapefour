#pragma once

#include "Tape.h"

namespace tf
{
/** Where whole tapes live on disk: Documents/TapeFour/Tape Shelf/<tape id>/ */
struct TapeShelf
{
    struct Entry
    {
        juce::String id, name, created;
        int type = 1, model = defaultTapeModel;
        double usedSeconds = 0;
        int tracksUsed = 0;
        juce::Time modified;
    };

    static juce::File getFolder();
    static juce::File folderFor (const juce::String& id) { return getFolder().getChildFile (id); }
    static std::vector<Entry> list();
    static bool save (Tape& tape);
    static std::unique_ptr<Tape> load (const juce::String& id);
    static std::unique_ptr<Tape> createBlank (int type, int model = defaultTapeModel);

    /** The disk space a full tape of this length takes (all four tracks recorded end to end), with a margin. */
    static juce::int64 bytesForFullTape (const TapeTypeInfo& type);

    /** Free space where tapes are kept; 0 if the system can't say. */
    static juce::int64 freeSpace();

    /** Free space that's known to be short of what's needed. An unknown amount is never short. */
    static bool isShort (juce::int64 needed) { auto free = freeSpace(); return free > 0 && free < needed; }
};
} // namespace tf
