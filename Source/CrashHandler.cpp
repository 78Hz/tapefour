#include "CrashHandler.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <dbghelp.h>
#endif

namespace tf::CrashHandler
{
namespace
{
    // Prepared up front: a crashed process shouldn't have to work out where to write.
    juce::File crashFolder;
    juce::String versionLine;

    void handler (void* platformInfo)
    {
        auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
        auto base = crashFolder.getChildFile ("crash-" + stamp);

       #if JUCE_WINDOWS
        if (platformInfo != nullptr)
        {
            auto file = CreateFileW (base.withFileExtension ("dmp").getFullPathName().toWideCharPointer(),
                                     GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                MINIDUMP_EXCEPTION_INFORMATION info;
                info.ThreadId = GetCurrentThreadId();
                info.ExceptionPointers = static_cast<EXCEPTION_POINTERS*> (platformInfo);
                info.ClientPointers = FALSE;
                MiniDumpWriteDump (GetCurrentProcess(), GetCurrentProcessId(), file,
                                   (MINIDUMP_TYPE) (MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory),
                                   &info, nullptr, nullptr);
                CloseHandle (file);
            }
        }
       #else
        juce::ignoreUnused (platformInfo);
       #endif

        base.withFileExtension ("txt").replaceWithText (versionLine + "\n\n" + juce::SystemStats::getStackBacktrace());
    }
}

juce::File getFolder()
{
    // Next to the settings file.
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);   // %APPDATA%, ~/Library, ~/.config
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return base.getChildFile ("TapeFour").getChildFile ("Crashes");
}

void install()
{
    crashFolder = getFolder();
    crashFolder.createDirectory();
    versionLine = juce::String ("TapeFour ") + JUCE_APPLICATION_VERSION_STRING + " on " + juce::SystemStats::getOperatingSystemName();
    juce::SystemStats::setApplicationCrashHandler (handler);
}

int takeNewReportCount (juce::PropertiesFile& settings)
{
    auto lastSeen = settings.getValue ("lastCrashSeen").getLargeIntValue();
    juce::int64 newest = lastSeen;
    int count = 0;
    for (const auto& f : juce::RangedDirectoryIterator (getFolder(), false, "crash-*.txt"))
    {
        auto t = f.getModificationTime().toMilliseconds();
        if (t > lastSeen) { ++count; newest = std::max (newest, t); }
    }
    if (newest != lastSeen)
        settings.setValue ("lastCrashSeen", juce::String (newest));
    return count;
}
} // namespace tf::CrashHandler
