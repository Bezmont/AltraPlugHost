#include "Diagnostics.h"

#include <cstdio>
#include <share.h>
#include <mutex>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace altra::diagnostics
{

namespace
{
    std::mutex& getMutex()
    {
        static std::mutex m;
        return m;
    }

    // Plain C file I/O and Win32 calls only: this also runs while the DLL unloads, when JUCE's
    // own statics may already be gone.
    void writeLine (const char* utf8)
    {
       #if defined (_WIN32)
        wchar_t folder[MAX_PATH] {};

        if (GetEnvironmentVariableW (L"LOCALAPPDATA", folder, MAX_PATH) == 0)
            return;

        std::wstring path (folder);
        path += L"\\AltraPlugHost";
        CreateDirectoryW (path.c_str(), nullptr);
        path += L"\\lifecycle.log";

        const std::lock_guard<std::mutex> lock (getMutex());

        WIN32_FILE_ATTRIBUTE_DATA info {};

        if (GetFileAttributesExW (path.c_str(), GetFileExInfoStandard, &info)
            && (info.nFileSizeHigh > 0 || info.nFileSizeLow > 1024 * 1024))
        {
            auto old = path + L".old";
            MoveFileExW (path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        SYSTEMTIME t {};
        GetLocalTime (&t);

        // Shared, so Live and its plug-in scanner can both write to the log.
        FILE* f = _wfsopen (path.c_str(), L"ab", _SH_DENYNO);

        if (f != nullptr)
        {
            std::fprintf (f, "%04d-%02d-%02d %02d:%02d:%02d.%03d  pid %lu  tid %lu  %s\r\n",
                          t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                          GetCurrentProcessId(), GetCurrentThreadId(), utf8);
            std::fflush (f);
            std::fclose (f);
        }
       #else
        juce::ignoreUnused (utf8);
       #endif
    }

    /** Logs when this module (our DLL) is loaded and unloaded. Unloading happens after Live has
        deleted every AltraPlugHost instance, so a log ending before this line means Live never
        got that far. */
    struct ModuleLifetime
    {
        ModuleLifetime()  { writeLine ("module loaded (" JucePlugin_Name " " JucePlugin_VersionString ")"); }
        ~ModuleLifetime() { writeLine ("module unloading"); }
    };

    ModuleLifetime moduleLifetime;
}

void log (const juce::String& message)
{
    writeLine (message.toRawUTF8());
}

ScopedStep::ScopedStep (juce::String w)
    : what (std::move (w)), start (juce::Time::getMillisecondCounterHiRes())
{
    log (what + "...");
}

ScopedStep::~ScopedStep()
{
    log (what + " done (" + juce::String (juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - start)) + " ms)");
}

} // namespace altra::diagnostics
