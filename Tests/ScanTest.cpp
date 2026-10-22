// A plug-in that crashes while being scanned must not take TapeFour down.
// Usage: ScanTest <folder of VST3s to scan>. This program is also its own scan helper.
#include "../Source/PluginScan.h"
#include <cstdio>

using namespace tf;

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add (juce::String (argv[i]).quoted());
    auto commandLine = args.joinIntoString (" ");

    if (PluginScan::isHelperCommandLine (commandLine))
        return PluginScan::runHelper (commandLine);

    if (argc < 2) { std::printf ("usage: ScanTest <folder>\n"); return 2; }

    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager (formats);
    juce::AudioPluginFormat* vst3 = nullptr;
    for (auto* f : formats.getFormats()) if (f->getName() == "VST3") vst3 = f;
    if (vst3 == nullptr) { std::printf ("FAIL  no VST3 format\n"); return 1; }

    juce::KnownPluginList list;
    list.setCustomScanner (PluginScan::makeOutOfProcessScanner());
    auto pedal = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tf_scan_pedal");
    pedal.deleteFile();
    juce::PluginDirectoryScanner scanner (list, *vst3, juce::FileSearchPath (juce::String (argv[1])), true, pedal);

    juce::String name;
    while (scanner.scanNextFile (true, name)) {}

    // Still running, and the plug-in that crashed its helper is set aside rather than retried.
    auto blocked = list.getBlacklistedFiles();
    bool ok = blocked.size() == 1 && blocked[0].contains ("Crash");
    std::printf ("%s  crashing plug-in scanned out of process and set aside: %s\n",
                 ok ? "PASS" : "FAIL", blocked.joinIntoString (", ").toRawUTF8());
    std::printf ("\n%s\n", ok ? "ALL PASSED (0 failures)" : "SOME FAILED (1 failure)");
    return ok ? 0 : 1;
}
