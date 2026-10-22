#include "PluginScan.h"

namespace tf
{
namespace PluginScan
{
namespace
{
    class OutOfProcessScanner : public juce::KnownPluginList::CustomScanner
    {
    public:
        bool findPluginTypesFor (juce::AudioPluginFormat& format,
                                 juce::OwnedArray<juce::PluginDescription>& result,
                                 const juce::String& fileOrIdentifier) override
        {
            auto out = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("TapeFour").getChildFile ("scan-" + juce::Uuid().toString() + ".xml");
            out.getParentDirectory().createDirectory();

            juce::ChildProcess child;
            juce::StringArray args { juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName(),
                                     flag, format.getName(), fileOrIdentifier, out.getFullPathName() };
            if (! child.start (args, 0))
            {
                // No helper (shouldn't happen): fall back to scanning here.
                format.findAllTypesForFile (result, fileOrIdentifier);
                return true;
            }

            // Give a slow plug-in two minutes; a hung one is treated like a crash.
            const auto deadline = juce::Time::getMillisecondCounter() + 120000;
            while (child.isRunning())
            {
                if (shouldExit() || juce::Time::getMillisecondCounter() > deadline)
                {
                    child.kill();
                    out.deleteFile();
                    return ! shouldExit() ? false : true;
                }
                child.waitForProcessToFinish (100);
            }

            bool ok = child.getExitCode() == 0 && out.existsAsFile();
            if (ok)
                if (auto xml = juce::XmlDocument::parse (out))
                    for (auto* e : xml->getChildIterator())
                    {
                        auto d = std::make_unique<juce::PluginDescription>();
                        if (d->loadFromXml (*e))
                            result.add (d.release());
                    }
            out.deleteFile();
            return ok;
        }
    };
}

std::unique_ptr<juce::KnownPluginList::CustomScanner> makeOutOfProcessScanner()
{
    return std::make_unique<OutOfProcessScanner>();
}

bool isHelperCommandLine (const juce::String& commandLine)
{
    return commandLine.contains (flag);
}

int runHelper (const juce::String& commandLine)
{
    auto args = juce::StringArray::fromTokens (commandLine, true);
    args.trim();
    for (auto& a : args) a = a.unquoted();
    auto i = args.indexOf (flag);
    if (i < 0 || i + 3 >= args.size())
        return 2;

    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager (formats);
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* f : formats.getFormats())
        if (f->getName() == args[i + 1])
            f->findAllTypesForFile (found, args[i + 2]);

    juce::XmlElement xml ("SCAN");
    for (auto* d : found)
        xml.addChildElement (d->createXml().release());
    return xml.writeTo (juce::File (args[i + 3])) ? 0 : 3;
}
}
} // namespace tf
