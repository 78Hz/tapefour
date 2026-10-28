#include "InstrumentPresets.h"

namespace tf::presets
{
juce::File folder (const Kind& k)
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("TapeFour").getChildFile ("Presets").getChildFile (k.folder);
}

juce::File fileFor (const Kind& k, const juce::String& name)
{
    return folder (k).getChildFile (juce::File::createLegalFileName (name.trim()) + "." + k.extension);
}

std::vector<Entry> list (const Kind& k)
{
    std::vector<Entry> out;
    for (auto& f : folder (k).findChildFiles (juce::File::findFiles, false, "*." + k.extension))
    {
        auto x = juce::XmlDocument::parse (f);
        if (x == nullptr || ! x->hasTagName (k.fileTag)) continue;
        auto* state = x->getFirstChildElement();
        if (state == nullptr) continue;
        Entry e;
        e.file = f;
        e.name = x->getStringAttribute ("name", f.getFileNameWithoutExtension());
        e.saved = f.getLastModificationTime();
        e.summary = k.summary ? k.summary (*state) : juce::String();
        out.push_back (e);
    }
    std::sort (out.begin(), out.end(), [] (const Entry& a, const Entry& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return out;
}

bool save (Presettable& p, const juce::String& name, juce::String& error)
{
    const auto& k = p.presetKind();
    if (name.trim().isEmpty()) { error = "GIVE IT A NAME"; return false; }
    const auto f = fileFor (k, name);
    if (! f.getParentDirectory().createDirectory()) { error = "CAN'T MAKE THE PRESETS FOLDER"; return false; }
    juce::XmlElement x (k.fileTag);
    x.setAttribute ("version", 1);
    x.setAttribute ("name", name.trim());
    x.addChildElement (p.toXml (true).release());
    if (! x.writeTo (f)) { error = "CAN'T WRITE " + f.getFileName().toUpperCase(); return false; }
    p.setPreset (name.trim());
    return true;
}

bool load (Presettable& p, const juce::File& f, juce::String& error)
{
    const auto& k = p.presetKind();
    auto x = juce::XmlDocument::parse (f);
    auto* state = x != nullptr && x->hasTagName (k.fileTag) ? x->getFirstChildElement() : nullptr;
    if (state == nullptr) { error = "NOT AN " + k.noun + " PRESET"; return false; }
    p.fromXml (*state);
    p.setPreset (x->getStringAttribute ("name", f.getFileNameWithoutExtension()));
    return true;
}

bool rename (Presettable& p, const juce::String& newName, juce::String& error)
{
    const auto& k = p.presetKind();
    const auto old = fileFor (k, p.currentPreset());
    const auto to = fileFor (k, newName);
    if (newName.trim().isEmpty()) { error = "GIVE IT A NAME"; return false; }
    if (to.existsAsFile() && to != old) { error = "THERE'S ALREADY ONE CALLED THAT"; return false; }
    // the file keeps what was saved in it: only its name changes
    auto x = juce::XmlDocument::parse (old);
    if (x == nullptr || ! x->hasTagName (k.fileTag)) return save (p, newName, error);
    x->setAttribute ("name", newName.trim());
    if (! x->writeTo (to)) { error = "CAN'T WRITE IT"; return false; }
    if (to != old) old.deleteFile();
    p.renamePreset (newName.trim());
    return true;
}

bool remove (Presettable& p, juce::String& error)
{
    const auto f = fileFor (p.presetKind(), p.currentPreset());
    if (! f.existsAsFile()) { error = "NOTHING TO DELETE"; return false; }
    if (! f.deleteFile()) { error = "CAN'T DELETE IT"; return false; }
    p.setPreset ({});
    return true;
}
} // namespace tf::presets
