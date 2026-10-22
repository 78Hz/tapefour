#pragma once

#include "Common.h"

namespace tf
{
/**
    Plug-ins are scanned in a separate copy of TapeFour, one file at a time,
    so a plug-in that crashes while being scanned only takes the helper down.
    TapeFour then marks it as failed and carries on with the rest.
*/
namespace PluginScan
{
    inline constexpr const char* flag = "--scan-plugin";

    /** For the KnownPluginList used by the Instruments page. */
    std::unique_ptr<juce::KnownPluginList::CustomScanner> makeOutOfProcessScanner();

    /** True if this process was started as a scan helper. */
    bool isHelperCommandLine (const juce::String& commandLine);

    /** Does the helper's work: scan one file, write what it found, return the exit code. */
    int runHelper (const juce::String& commandLine);
}
} // namespace tf
