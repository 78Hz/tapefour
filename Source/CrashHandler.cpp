#include "CrashHandler.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <dbghelp.h>
#else
 #include <execinfo.h>
 #include <fcntl.h>
 #include <unistd.h>
 #include <ctime>
#endif

namespace tf::CrashHandler
{
namespace
{
    /*  The handler runs after the program has crashed: memory may be corrupt and a lock inside the
        allocator may be held, so it mustn't allocate, format strings or touch anything JUCE owns.
        Everything it needs is prepared here, at install time, as plain characters. */
    constexpr int maxPath = 1024;
    constexpr int maxReports = 20;   // older ones are deleted at start-up

   #if JUCE_WINDOWS
    wchar_t folderW[maxPath] {};
    char header[512] {};
   #else
    char folder[maxPath] {};
    char header[512] {};
   #endif

    /** Appends the decimal digits of v to buffer at pos, without the C library. */
    int appendNumber (char* buffer, int pos, int size, unsigned long long v)
    {
        char digits[24];
        int n = 0;
        do { digits[n++] = (char) ('0' + (int) (v % 10)); v /= 10; } while (v != 0 && n < 24);
        while (n > 0 && pos < size - 1) buffer[pos++] = digits[--n];
        buffer[pos] = 0;
        return pos;
    }

    int appendText (char* buffer, int pos, int size, const char* text)
    {
        while (*text != 0 && pos < size - 1) buffer[pos++] = *text++;
        buffer[pos] = 0;
        return pos;
    }

   #if JUCE_WINDOWS
    void handler (void* platformInfo)
    {
        // crash-<seconds>.dmp and .txt, named from the system clock (no allocation)
        FILETIME ft;
        GetSystemTimeAsFileTime (&ft);
        const auto stamp = (((unsigned long long) ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL;
        char name[64] {};
        int n = appendText (name, 0, 64, "\\crash-");
        n = appendNumber (name, n, 64, stamp);

        auto makePath = [&] (wchar_t* out, const char* ext)
        {
            int i = 0;
            for (; folderW[i] != 0 && i < maxPath - 40; ++i) out[i] = folderW[i];
            for (int k = 0; name[k] != 0; ++k) out[i++] = (wchar_t) name[k];
            for (int k = 0; ext[k] != 0; ++k) out[i++] = (wchar_t) ext[k];
            out[i] = 0;
        };

        wchar_t path[maxPath + 64];
        if (platformInfo != nullptr)
        {
            makePath (path, ".dmp");
            auto file = CreateFileW (path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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

        // The text report: the version, and where the stack is (the minidump).
        makePath (path, ".txt");
        auto file = CreateFileW (path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile (file, header, (DWORD) std::strlen (header), &written, nullptr);
            const char note[] = "The stack is in the .dmp file of the same name (open it in Visual Studio or WinDbg).\r\n";
            WriteFile (file, note, (DWORD) sizeof (note) - 1, &written, nullptr);
            CloseHandle (file);
        }
    }
   #else
    void handler (void* platformInfo)
    {
        // Only async-signal-safe calls from here on: time, open, write, close, backtrace_symbols_fd.
        char path[maxPath + 64];
        int n = appendText (path, 0, (int) sizeof (path), folder);
        n = appendText (path, n, (int) sizeof (path), "/crash-");
        n = appendNumber (path, n, (int) sizeof (path), (unsigned long long) ::time (nullptr));
        appendText (path, n, (int) sizeof (path), ".txt");

        const int fd = ::open (path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return;

        char line[64];
        int k = appendText (line, 0, 64, "\nSignal ");
        k = appendNumber (line, k, 64, (unsigned long long) (juce::pointer_sized_int) platformInfo);
        appendText (line, k, 64, "\n\n");
        [[maybe_unused]] auto w1 = ::write (fd, header, std::strlen (header));
        [[maybe_unused]] auto w2 = ::write (fd, line, std::strlen (line));

        void* frames[128];
        const int count = ::backtrace (frames, 128);
        ::backtrace_symbols_fd (frames, count, fd);
        ::close (fd);
    }
   #endif

    void prune (const juce::File& dir)
    {
        // Keep the newest reports (a .txt and, on Windows, its .dmp).
        auto reports = dir.findChildFiles (juce::File::findFiles, false, "crash-*.txt");
        std::sort (reports.begin(), reports.end(),
                   [] (const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
        for (int i = maxReports; i < reports.size(); ++i)
        {
            reports.getReference (i).withFileExtension ("dmp").deleteFile();
            reports.getReference (i).deleteFile();
        }
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
    auto dir = getFolder();
    dir.createDirectory();
    prune (dir);

    auto text = (juce::String ("TapeFour ") + JUCE_APPLICATION_VERSION_STRING + " on " + juce::SystemStats::getOperatingSystemName()
                 + ", " + juce::SystemStats::getCpuModel() + "\n").toStdString();
    std::strncpy (header, text.c_str(), sizeof (header) - 1);
   #if JUCE_WINDOWS
    auto wide = dir.getFullPathName().toWideCharPointer();
    for (int i = 0; i < maxPath - 1 && wide[i] != 0; ++i) folderW[i] = wide[i];
   #else
    std::strncpy (folder, dir.getFullPathName().toRawUTF8(), sizeof (folder) - 1);
   #endif
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
