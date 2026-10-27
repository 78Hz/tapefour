#include "Library.h"
#include "../Source/CassetteComponent.h"

namespace tf::pocket
{
namespace
{
    constexpr const char* key = "cassettes";

    std::unique_ptr<juce::XmlElement> toXml (const CassetteEntry& e)
    {
        auto x = std::make_unique<juce::XmlElement> ("CASSETTE");
        x->setAttribute ("file", e.file.getFullPathName());
        x->setAttribute ("id", e.id);
        x->setAttribute ("label", e.label);
        x->setAttribute ("product", e.product);
        x->setAttribute ("grade", e.grade);
        x->setAttribute ("model", e.model);
        x->setAttribute ("stereo", e.stereo);
        x->setAttribute ("side", e.side);
        x->setAttribute ("position", e.position);
        x->setAttribute ("length", e.lengthCells);
        x->setAttribute ("lastPlayed", juce::String (e.lastPlayed));
        return x;
    }

    CassetteEntry fromXml (const juce::XmlElement& x)
    {
        CassetteEntry e;
        e.file = juce::File (x.getStringAttribute ("file"));
        e.id = x.getStringAttribute ("id");
        e.label = x.getStringAttribute ("label");
        e.product = x.getStringAttribute ("product");
        e.grade = x.getStringAttribute ("grade");
        e.model = juce::jlimit (0, numTapeModels - 1, x.getIntAttribute ("model", defaultTapeModel));
        e.stereo = x.getBoolAttribute ("stereo", true);
        e.side = x.getIntAttribute ("side") == 1 ? 1 : 0;
        e.position = x.getDoubleAttribute ("position");
        e.lengthCells = x.getDoubleAttribute ("length");
        e.lastPlayed = x.getStringAttribute ("lastPlayed").getLargeIntValue();
        return e;
    }
}

Library::Library (juce::PropertiesFile& settings) : props (settings) {}

std::vector<CassetteEntry> Library::recent() const
{
    std::vector<CassetteEntry> list;
    if (auto x = props.getXmlValue (key))
        for (auto* c : x->getChildWithTagNameIterator ("CASSETTE"))
            list.push_back (fromXml (*c));
    std::stable_sort (list.begin(), list.end(), [] (const auto& a, const auto& b) { return a.lastPlayed > b.lastPlayed; });
    return list;
}

std::optional<CassetteEntry> Library::find (const juce::String& id) const
{
    for (auto& e : recent())
        if (e.id == id) return e;
    return std::nullopt;
}

void Library::remember (const CassetteEntry& entry)
{
    auto list = recent();
    // One entry per cassette: copies share an id, and the same file can be opened twice.
    list.erase (std::remove_if (list.begin(), list.end(), [&] (const auto& e) { return e.id == entry.id || e.file == entry.file; }), list.end());
    list.insert (list.begin(), entry);
    if (list.size() > (size_t) maxRemembered) list.resize ((size_t) maxRemembered);
    juce::XmlElement x ("CASSETTES");
    for (auto& e : list) x.addChildElement (toXml (e).release());
    props.setValue (key, &x);
    props.saveIfNeeded();
}

void Library::forget (const juce::File& file)
{
    auto list = recent();
    list.erase (std::remove_if (list.begin(), list.end(), [&] (const auto& e) { return e.file == file; }), list.end());
    juce::XmlElement x ("CASSETTES");
    for (auto& e : list) x.addChildElement (toXml (e).release());
    props.setValue (key, &x);
    props.saveIfNeeded();
}

CassetteArtwork Library::artworkOf (const juce::File& cassFile, const Tape* tape)
{
    tfmt::PackedCassette p;
    p.file = tfmt::pathFromUtf8 (cassFile.getFullPathName().toStdString());
    auto art = CassetteArtwork::fromPacked (p);
    // (The TF-4 0.2 on Windows packed its own looks with empty images: those are drawn again here.)
    if (art.isValid() && ! (art.look.design.rfind ("tapefour.", 0) == 0 && art.isBlank())) return art;

    // A cassette from before looks were saved, or from a writer that doesn't save one:
    // the built-in look of its kind, drawn as the TF-4 draws it.
    std::unique_ptr<Tape> opened;
    if (tape == nullptr)
    {
        juce::String err;
        opened = Tape::openPacked (cassFile, err);
        tape = opened.get();
    }
    if (tape == nullptr) return {};
    auto made = CassetteComponent::renderArt (CassetteComponent::ArtSpec::of (*tape));
    return CassetteArtwork::fromParts (made.look, [&made] (const std::string& name)
    {
        if (name == made.look.back)   return made.back;
        if (name == made.look.frontA) return made.frontA;
        if (name == made.look.frontB) return made.frontB;
        if (name == made.look.hub)    return made.hub;
        return juce::MemoryBlock();
    });
}

OpenedCassette Library::open (const juce::File& cassFile) const
{
    OpenedCassette o;
    o.tape = Tape::openPacked (cassFile, o.error);
    if (o.tape == nullptr)
    {
        if (o.error.isEmpty()) o.error = "not a cassette";
        return o;
    }
    auto& t = *o.tape;
    o.art = artworkOf (cassFile, &t);

    auto& e = o.entry;
    e.file = cassFile;
    e.id = t.id;
    e.label = t.name;
    e.product = t.productName();
    e.grade = juce::String (t.getCondition().grade);
    e.model = t.getModel();
    e.stereo = t.getLayout() == tfmt::Layout::stereoTwoSides;
    e.lengthCells = (double) t.getLengthCells();
    if (auto known = find (t.id))
    {
        e.side = known->side;
        e.position = known->position;
    }
    else
    {
        // First time in this player: wherever the cassette itself says it was left, on side A.
        e.side = 0;
        e.position = t.position.load();
    }
    e.lastPlayed = juce::Time::currentTimeMillis();
    return o;
}
juce::File Library::cassettesFolder()
{
   #if JUCE_ANDROID
    // The player's own storage: always there and writable, whatever the Android version
    // (the phone's shared Documents folder needs permissions an app may not get).
    auto folder = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Cassettes");
    folder.createDirectory();
    return folder;
   #else
    // iOS: the player's Documents folder, which the Files app shows (On My iPhone > TF-P1 Pocket).
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
   #endif
}

juce::Array<juce::File> Library::takeInNewArrivals()
{
    auto folder = cassettesFolder();
    // "Open in" and the share sheet leave a copy in Documents/Inbox: keep it with the others.
    auto inbox = folder.getChildFile ("Inbox");
    for (auto& f : inbox.findChildFiles (juce::File::findFiles, false, "*.cass"))
        f.moveFileTo (folder.getNonexistentChildFile (f.getFileNameWithoutExtension(), ".cass", false));

    auto known = recent();
    juce::Array<juce::File> added;
    for (auto& f : folder.findChildFiles (juce::File::findFiles, false, "*.cass"))
    {
        bool isKnown = false;
        for (auto& e : known) isKnown = isKnown || e.file == f;
        if (isKnown) continue;
        auto opened = open (f);
        if (opened.tape == nullptr) continue;
        opened.entry.lastPlayed = f.getLastModificationTime().toMilliseconds() - 1;   // after the ones played, newest first
        remember (opened.entry);
        known = recent();
        added.add (f);
    }
    std::sort (added.begin(), added.end(), [] (const juce::File& a, const juce::File& b)
               { return a.getLastModificationTime() > b.getLastModificationTime(); });
    return added;
}
} // namespace tf::pocket
