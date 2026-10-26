#include "TapeShelf.h"

namespace tf
{
static juce::File& shelfOverride()
{
    static juce::File f;
    return f;
}

void TapeShelf::setFolderForTests (const juce::File& folder)
{
    shelfOverride() = folder;
}

juce::File TapeShelf::getFolder()
{
    auto f = shelfOverride() != juce::File() ? shelfOverride()
                                             : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                   .getChildFile ("TapeFour").getChildFile ("Tape Shelf");
    f.createDirectory();
    return f;
}

std::vector<TapeShelf::Entry> TapeShelf::list()
{
    std::vector<Entry> out;

    for (const auto& d : juce::RangedDirectoryIterator (getFolder(), false, "*", juce::File::findDirectories))
    {
        auto folder = d.getFile();
        if (folder.getFileName().startsWithChar ('.')) continue;   // an import or replacement in progress
        auto manifest = folder.getChildFile (tfmt::CassFiles::manifest);
        if (manifest.existsAsFile())
        {
            auto dir = tfmt::pathFromUtf8 (folder.getFullPathName().toStdString());
            tfmt::CassetteInfo info;
            tfmt::CassetteState state;
            std::string err;
            if (! tfmt::readManifest (dir, info, err)) continue;
            tfmt::readState (dir, state, err);
            Entry e;
            e.id          = juce::String::fromUTF8 (info.id.c_str());
            e.name        = juce::String::fromUTF8 (info.label.c_str());
            e.type        = 1;
            for (int i = 0; i < numTapeTypes; ++i) if (tapeTypes[i].minutes == info.minutesPerSide) e.type = i;
            e.model       = defaultTapeModel;
            for (int k = 0; k < numTapeModels; ++k) if (info.formulation.name == tapeModels[k].name) e.model = k;
            e.minutes     = info.minutesPerSide;
            e.formulation = juce::String::fromUTF8 (info.formulation.name.c_str());
            e.grade       = juce::String::fromUTF8 (info.condition.grade.c_str());
            e.usedSeconds = state.usedSeconds;
            e.tracksUsed  = state.tracksUsed;
            auto stateFile = folder.getChildFile (tfmt::CassFiles::state);
            e.modified    = stateFile.existsAsFile() ? stateFile.getLastModificationTime() : manifest.getLastModificationTime();
            {
                auto iso = juce::String::fromUTF8 (info.created.c_str());
                static const char* months[] { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
                auto m = iso.substring (5, 7).getIntValue();
                e.created = (m >= 1 && m <= 12) ? juce::String (iso.substring (8, 10).getIntValue()).paddedLeft ('0', 2) + " " + months[m - 1] + " " + iso.substring (0, 4) : iso;
            }
            out.push_back (e);
            continue;
        }

        // A tape from before the cassette format; it is converted when it is first loaded.
        auto meta = folder.getChildFile ("tape.xml");
        if (! meta.existsAsFile())
            continue;

        // Only the opening tag's attributes are needed, so don't parse the whole file.
        juce::XmlDocument doc (meta);
        if (auto xml = doc.getDocumentElementIfTagMatches ("TAPEFOUR_TAPE"))
        {
            Entry e;
            e.id          = xml->getStringAttribute ("id", folder.getFileName());
            e.name        = xml->getStringAttribute ("name");
            e.created     = xml->getStringAttribute ("created");
            e.type        = juce::jlimit (0, numTapeTypes - 1, xml->getIntAttribute ("type", 1));
            e.model       = juce::jlimit (0, numTapeModels - 1, xml->getIntAttribute ("model", defaultTapeModel));
            e.minutes     = tapeTypes[e.type].minutes;
            e.formulation = tapeModels[e.model].name;
            e.usedSeconds = xml->getDoubleAttribute ("usedSeconds");
            e.tracksUsed  = xml->getIntAttribute ("tracksUsed");
            e.modified    = meta.getLastModificationTime();
            out.push_back (e);
        }
    }

    std::sort (out.begin(), out.end(), [] (const Entry& a, const Entry& b) { return a.modified > b.modified; });
    return out;
}

bool TapeShelf::save (Tape& tape)
{
    return tape.save (folderFor (tape.id));
}

std::unique_ptr<Tape> TapeShelf::load (const juce::String& id)
{
    return Tape::load (folderFor (id));
}

std::unique_ptr<Tape> TapeShelf::createBlank (int type, int model, const juce::String& grade)
{
    auto tape = std::make_unique<Tape> (type, model, tfmt::Condition::forGrade (grade.toStdString()));
    tape->name = "tape " + juce::String ((int) list().size() + 1);
    return tape;
}

//==============================================================================
static std::filesystem::path pathOf (const juce::File& f) { return tfmt::pathFromUtf8 (f.getFullPathName().toStdString()); }

bool TapeShelf::exportCassette (const juce::String& id, const juce::File& target, juce::String& error)
{
    std::string err;
    if (tfmt::packCassette (pathOf (folderFor (id)), pathOf (target), err))
        return true;
    error = juce::String::fromUTF8 (err.c_str());
    return false;
}

bool TapeShelf::peek (const juce::File& cassFile, tfmt::CassetteInfo& info, juce::String& error)
{
    std::string err;
    if (tfmt::readPackedManifest (pathOf (cassFile), info, err))
        return true;
    error = juce::String::fromUTF8 (err.c_str());
    return false;
}

bool TapeShelf::importCassette (const juce::File& cassFile, juce::String& id, juce::String& error)
{
    tfmt::CassetteInfo info;
    if (! peek (cassFile, info, error)) return false;
    id = juce::String::fromUTF8 (info.id.c_str());
    if (! juce::File::isAbsolutePath (folderFor (id).getFullPathName()) || id.containsAnyOf ("/\\:") || id == "." || id == "..")
    {
        error = "a cassette id that can't be a folder name";
        return false;
    }

    // Unpacked beside the shelf first, and tried: only a cassette this deck can play goes on it.
    auto temp = getFolder().getChildFile (".importing-" + juce::Uuid().toString());
    std::string err;
    if (! tfmt::unpackCassette (pathOf (cassFile), pathOf (temp), err))
    {
        temp.deleteRecursively();
        error = juce::String::fromUTF8 (err.c_str());
        return false;
    }
    if (Tape::load (temp) == nullptr)
    {
        temp.deleteRecursively();
        error = "this deck can't play that cassette";
        return false;
    }

    auto target = folderFor (id);
    if (target.isDirectory())
    {
        // A copy is already on the shelf: the file's cassette replaces it; the session you had with it stays.
        auto session = target.getChildFile ("session.xml");
        if (session.existsAsFile()) session.copyFileTo (temp.getChildFile ("session.xml"));
        auto old = getFolder().getChildFile (".replaced-" + juce::Uuid().toString());
        if (! target.moveFileTo (old)) { temp.deleteRecursively(); error = "the copy on the shelf is in use"; return false; }
        old.deleteRecursively();
    }
    if (! temp.moveFileTo (target))
    {
        temp.deleteRecursively();
        error = "could not put it on the shelf";
        return false;
    }
    return true;
}

juce::int64 TapeShelf::bytesForFullTape (const TapeTypeInfo& type)
{
    const auto cells = (juce::int64) type.minutes * 60 * cellRate;
    return numTracks * cells * (juce::int64) sizeof (int16_t) + 16 * 1024 * 1024;   // + the deck state and a margin
}

juce::int64 TapeShelf::freeSpace()
{
    return getFolder().getBytesFreeOnVolume();
}
} // namespace tf
