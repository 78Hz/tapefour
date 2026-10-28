#pragma once

#include "Common.h"

/*
    Presets for TAPE·FOUR's own instruments: one file each, in Documents/TapeFour/Presets/<instrument>.
    An instrument that has them is a Presettable: it can write its whole state as XML and read it back,
    and remembers which preset it came from and whether it has changed since.
*/
namespace tf::presets
{
/** One kind of instrument's presets: where they live and what their files look like. */
struct Kind
{
    juce::String folder;      // under Documents/TapeFour/Presets
    juce::String extension;   // without the dot
    juce::String fileTag;     // the file's root element
    juce::String noun;        // "PIANO", "ORGAN": for the list's messages
    std::function<juce::String (const juce::XmlElement& state)> summary;   // one line for the list
};

class Presettable
{
public:
    virtual ~Presettable() = default;

    /** The whole instrument (its age too, if it has one and withAge), and back. */
    virtual std::unique_ptr<juce::XmlElement> toXml (bool withAge) const = 0;
    virtual void fromXml (const juce::XmlElement&) = 0;
    virtual const Kind& presetKind() const = 0;

    /** The preset it was loaded from or saved as ("" none), and whether it has changed since. */
    juce::String currentPreset() const { return presetName; }
    void setPreset (const juce::String& name) { presetName = name; presetFingerprint = fingerprint(); }
    bool isModified() const { return presetName.isNotEmpty() && fingerprint() != presetFingerprint; }
    void renamePreset (const juce::String& name) { presetName = name; }   // keeps whether it's modified

protected:
    /** For the instrument's own saving with the deck: the name and the * travel with it. */
    void writePresetAttributes (juce::XmlElement& x) const
    {
        x.setAttribute ("preset", presetName);
        x.setAttribute ("presetFingerprint", presetFingerprint);
    }
    void readPresetAttributes (const juce::XmlElement& x)
    {
        presetName = x.getStringAttribute ("preset");
        presetFingerprint = x.getStringAttribute ("presetFingerprint");
    }

private:
    // the instrument as it sounds, without its age (which changes as you play)
    juce::String fingerprint() const
    {
        return juce::String::toHexString (toXml (false)->toString (juce::XmlElement::TextFormat().singleLine()).hashCode64());
    }
    juce::String presetName, presetFingerprint;
};

struct Entry { juce::File file; juce::String name, summary; juce::Time saved; };

juce::File folder (const Kind&);
juce::File fileFor (const Kind&, const juce::String& name);
std::vector<Entry> list (const Kind&);
bool save (Presettable&, const juce::String& name, juce::String& error);
bool load (Presettable&, const juce::File&, juce::String& error);
bool rename (Presettable&, const juce::String& newName, juce::String& error);
bool remove (Presettable&, juce::String& error);
} // namespace tf::presets
