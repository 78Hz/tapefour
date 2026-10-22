#include "TapeShelf.h"

namespace tf
{
juce::File TapeShelf::getFolder()
{
    auto f = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                 .getChildFile ("TapeFour").getChildFile ("Tape Shelf");
    f.createDirectory();
    return f;
}

std::vector<TapeShelf::Entry> TapeShelf::list()
{
    std::vector<Entry> out;

    for (const auto& d : juce::RangedDirectoryIterator (getFolder(), false, "*", juce::File::findDirectories))
    {
        auto meta = d.getFile().getChildFile ("tape.xml");
        if (! meta.existsAsFile())
            continue;

        // Only the opening tag's attributes are needed, so don't parse the whole file.
        juce::XmlDocument doc (meta);
        if (auto xml = doc.getDocumentElementIfTagMatches ("TAPEFOUR_TAPE"))
        {
            Entry e;
            e.id          = xml->getStringAttribute ("id", d.getFile().getFileName());
            e.name        = xml->getStringAttribute ("name");
            e.created     = xml->getStringAttribute ("created");
            e.type        = xml->getIntAttribute ("type", 1);
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

std::unique_ptr<Tape> TapeShelf::createBlank (int type)
{
    auto tape = std::make_unique<Tape> (type);
    tape->name = "tape " + juce::String ((int) list().size() + 1);
    return tape;
}
} // namespace tf
