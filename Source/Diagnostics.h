#pragma once

#include <juce_core/juce_core.h>

/** A small lifecycle log, for diagnosing problems that only show up in the host, such as Live
    sometimes not exiting.

    Lines go to %LOCALAPPDATA%\AltraPlugHost\lifecycle.log with the time, process ID and thread.
    Each line is written and flushed immediately, so the log is complete up to the moment a hang
    starts. The file is started again when it passes 1 MB. Safe to call from any thread, and while
    the module is unloading. */
namespace altra::diagnostics
{
    void log (const juce::String& message);

    /** Logs "<what>..." now and "<what> done (N ms)" when it goes out of scope, so a step that
        never finishes shows up as a line without its "done". */
    class ScopedStep
    {
    public:
        explicit ScopedStep (juce::String what);
        ~ScopedStep();

    private:
        juce::String what;
        double start;
    };
}
