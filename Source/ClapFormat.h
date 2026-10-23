#pragma once

#include "Common.h"

namespace tf
{
/**
    CLAP plug-ins, hosted by TAPE·FOUR itself (JUCE hosts VST3, AU, LV2 and LADSPA, but not CLAP).

    A .clap file (a bundle on macOS) can hold several plug-ins. Each becomes a PluginDescription whose
    fileOrIdentifier is the file and whose uniqueId is a hash of the plug-in's CLAP id, so a saved deck
    finds the same plug-in again. Instruments, audio effects and note effects (MIDI effects) all load;
    parameters, state, latency, note and MIDI in and out, and the plug-in's own window are supported.
*/
class ClapPluginFormat : public juce::AudioPluginFormat
{
public:
    static constexpr const char* formatName = "CLAP";

    juce::String getName() const override { return formatName; }
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String& fileOrIdentifier) override;
    bool fileMightContainThisPluginType (const juce::String& fileOrIdentifier) override;
    juce::String getNameOfPluginFromIdentifier (const juce::String& fileOrIdentifier) override;
    bool pluginNeedsRescanning (const juce::PluginDescription&) override;
    bool doesPluginStillExist (const juce::PluginDescription&) override;
    bool canScanForPlugins() const override { return true; }
    bool isTrivialToScan() const override { return false; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool recursive, bool allowAsync) override;
    juce::FileSearchPath getDefaultLocationsToSearch() override;
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }

protected:
    void createPluginInstance (const juce::PluginDescription&, double sampleRate, int blockSize, PluginCreationCallback) override;
};

/** Every format TAPE·FOUR hosts: JUCE's (VST3, AU, LV2, LADSPA, as built) plus CLAP. */
void addAllPluginFormats (juce::AudioPluginFormatManager&);

/** Formats in order of preference, for listing a plug-in that comes in several once. */
int formatPreference (const juce::String& formatName);
} // namespace tf
