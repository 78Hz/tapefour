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
};
} // namespace tf
