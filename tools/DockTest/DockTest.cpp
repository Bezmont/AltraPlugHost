// DockTest: a minimal host for testing AltraPlugHost's editor docking outside Live.
//
// Loads AltraPlugHost.vst3, has it load a target plug-in (through its saved-state format), then
// opens and closes AltraPlugHost's editor several times the way a host does: editor deleted
// (IPlugView::removed()) before the host window is destroyed. After each open it saves a
// screenshot (PrintWindow, which also captures GPU-drawn content) and logs the native window
// tree, so a blank docked editor can be diagnosed without Live.
//
// Usage: DockTest <AltraPlugHost.vst3> <target.vst3> <output folder> [cycles] [first wait ms]
//        DockTest --raw <target.vst3> <output folder> <variant>
//
// --raw hosts the target directly (no AltraPlugHost) and moves its editor between windows in one
// of several ways, to find which kind of move a plug-in survives:
//   park-hidden   via a hidden parking window, first window destroyed (what AltraPlugHost does)
//   park-visible  via a visible parking window, first window destroyed
//   direct        straight into a second window, through JUCE
//   setparent     straight into a second window with a raw SetParent, bypassing JUCE
//   park-on-destroy  parked while the first window handles WM_DESTROY (AltraPlugHost's current fix)

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <psapi.h>
#pragma comment (lib, "comctl32.lib")

namespace
{
    juce::String describeWindow (HWND hwnd)
    {
        wchar_t className[128] {};
        GetClassNameW (hwnd, className, 128);
        RECT r {};
        GetWindowRect (hwnd, &r);
        DWORD pid = 0;
        GetWindowThreadProcessId (hwnd, &pid);

        return juce::String::toHexString ((juce::pointer_sized_int) hwnd)
             + "  class=" + juce::String (className)
             + "  visible=" + juce::String (IsWindowVisible (hwnd) ? "yes" : "no")
             + "  rect=" + juce::String (r.left) + "," + juce::String (r.top) + " "
                         + juce::String (r.right - r.left) + "x" + juce::String (r.bottom - r.top)
             + (pid != GetCurrentProcessId() ? "  OTHER PROCESS" : "");
    }

    void dumpTree (HWND hwnd, int depth, juce::String& out)
    {
        out << juce::String::repeatedString ("  ", depth) << describeWindow (hwnd) << "\n";

        for (auto child = GetWindow (hwnd, GW_CHILD); child != nullptr; child = GetWindow (child, GW_HWNDNEXT))
            dumpTree (child, depth + 1, out);
    }

    juce::String dumpAllWindows()
    {
        juce::String out;

        EnumThreadWindows (GetCurrentThreadId(), [] (HWND hwnd, LPARAM param) -> BOOL
        {
            dumpTree (hwnd, 0, *reinterpret_cast<juce::String*> (param));
            return TRUE;
        }, reinterpret_cast<LPARAM> (&out));

        return out;
    }

    bool capture (HWND hwnd, const juce::File& file)
    {
        RECT r {};
        GetWindowRect (hwnd, &r);
        const int w = r.right - r.left, h = r.bottom - r.top;

        if (w <= 0 || h <= 0)
            return false;

        auto screen = GetDC (nullptr);
        auto dc = CreateCompatibleDC (screen);
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        auto bitmap = CreateDIBSection (dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        auto old = SelectObject (dc, bitmap);

        PrintWindow (hwnd, dc, 2 /* PW_RENDERFULLCONTENT */);

        juce::Image image (juce::Image::RGB, w, h, false, juce::SoftwareImageType());
        {
            juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);

            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    auto* p = static_cast<const uint8_t*> (bits) + (y * w + x) * 4;
                    data.setPixelColour (x, y, juce::Colour (p[2], p[1], p[0]));
                }
        }

        SelectObject (dc, old);
        DeleteObject (bitmap);
        DeleteDC (dc);
        ReleaseDC (nullptr, screen);

        file.deleteFile();
        juce::FileOutputStream stream (file);
        return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
    }
}

//==============================================================================
class DockTestApp final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return "DockTest"; }
    const juce::String getApplicationVersion() override    { return "1.0"; }

    void initialise (const juce::String&) override
    {
        auto args = getCommandLineParameterArray();

        if (args.size() >= 4 && args[0] == "--raw")
        {
            targetFile = juce::File (args[1]);
            outDir = juce::File (args[2]);
            variant = args[3];
            outDir.createDirectory();
            juce::addDefaultFormatsToManager (formats);
            juce::Timer::callAfterDelay (100, [this] { startRaw(); });
            return;
        }

        if (args.size() >= 6 && args[0] == "--timing")
        {
            juce::addDefaultFormatsToManager (formats);
            juce::Timer::callAfterDelay (100, [this, args]
            {
                measureTiming (juce::File (args[1]), { juce::File (args[2]), juce::File (args[3]) },
                               args[4].getIntValue(), juce::File (args[5]));
            });
            return;
        }

        if (args.size() >= 2 && args[0] == "--count")
        {
            juce::addDefaultFormatsToManager (formats);
            juce::Timer::callAfterDelay (100, [this, file = juce::File (args[1])] { countParameters (file); });
            return;
        }

        if (args.size() < 3)
        {
            juce::Logger::writeToLog ("Usage: DockTest <AltraPlugHost.vst3> <target.vst3> <output folder> [cycles]");
            setApplicationReturnValue (2);
            quit();
            return;
        }

        wrapperFile = juce::File (args[0]);
        targetFile = juce::File (args[1]);
        outDir = juce::File (args[2]);
        cycles = args.size() > 3 ? args[3].getIntValue() : 3;
        firstWaitMs = args.size() > 4 ? args[4].getIntValue() : 8000;
        outDir.createDirectory();

        juce::addDefaultFormatsToManager (formats);
        juce::Timer::callAfterDelay (100, [this] { start(); });
    }

    void shutdown() override
    {
        for (auto* w : { &windowA, &windowB, &windowC })
            if (*w != nullptr)
                (*w)->clearContentComponent();

        rawEditor.reset();
        windowA.reset(); windowB.reset(); windowC.reset();
        target.reset();
        window.reset();
        wrapper.reset();
        log.saveTo();
    }

    void systemRequestedQuit() override                    { quit(); }

private:
    struct Log
    {
        juce::File file;
        juce::String text;

        void operator() (const juce::String& line)
        {
            text << juce::Time::getCurrentTime().toString (false, true, true, true) << "  " << line << "\n";
            saveTo();
        }

        void saveTo() const     { if (file != juce::File()) file.replaceWithText (text); }
    };

    void start()
    {
        log.file = outDir.getChildFile ("docktest.log");
        log ("Wrapper: " + wrapperFile.getFullPathName());
        log ("Target:  " + targetFile.getFullPathName());

        juce::OwnedArray<juce::PluginDescription> types;

        for (auto* format : formats.getFormats())
            format->findAllTypesForFile (types, wrapperFile.getFullPathName());

        juce::String error;

        if (types.isEmpty() || (wrapper = formats.createPluginInstance (*types[0], 48000.0, 512, error)) == nullptr)
            return fail ("Couldn't load the wrapper: " + error);

        juce::OwnedArray<juce::PluginDescription> targetTypes;

        for (auto* format : formats.getFormats())
            format->findAllTypesForFile (targetTypes, targetFile.getFullPathName());

        if (targetTypes.isEmpty())
            return fail ("No plug-in found in the target file");

        // Hand the wrapper its own saved-state format, naming the target plug-in.
        juce::XmlElement state ("AltraPlugHost");
        state.setAttribute ("version", 1);
        state.addChildElement (targetTypes[0]->createXml().release());

        juce::MemoryBlock pluginState;
        juce::AudioProcessor::copyXmlToBinary (state, pluginState);

        // JUCE's VST3 host expects its own envelope: the plug-in's component state, base64 encoded.
        juce::XmlElement envelope ("VST3PluginState");
        envelope.createNewChildElement ("IComponent")->addTextElement (pluginState.toBase64Encoding());

        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (envelope, block);
        wrapper->setStateInformation (block.getData(), (int) block.getSize());
        log ("Wrapper loaded " + targetTypes[0]->name);
        logParameters();

        juce::Timer::callAfterDelay (500, [this] { openCycle(); });
    }

    std::unique_ptr<juce::PluginDescription> describe (const juce::File& file)
    {
        juce::OwnedArray<juce::PluginDescription> types;

        for (auto* format : formats.getFormats())
            format->findAllTypesForFile (types, file.getFullPathName());

        return types.isEmpty() ? nullptr : std::make_unique<juce::PluginDescription> (*types[0]);
    }

    /** The wrapper's saved state naming this plug-in, in JUCE's VST3 host envelope. */
    static juce::MemoryBlock wrapperStateFor (const juce::PluginDescription& target)
    {
        juce::XmlElement state ("AltraPlugHost");
        state.setAttribute ("version", 1);
        state.addChildElement (target.createXml().release());

        juce::MemoryBlock pluginState;
        juce::AudioProcessor::copyXmlToBinary (state, pluginState);

        juce::XmlElement envelope ("VST3PluginState");
        envelope.createNewChildElement ("IComponent")->addTextElement (pluginState.toBase64Encoding());

        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (envelope, block);
        return block;
    }

    /** --timing: what the wrapper's parameter slots cost a host. Times creating the wrapper (a
        device insert), switching between two hosted plug-ins (a load), saving its state and
        deleting it, and loading the two plug-ins directly as a baseline. Writes to `report`. */
    void measureTiming (const juce::File& wrapperPath, std::array<juce::File, 2> targets, int rounds,
                        const juce::File& report)
    {
        juce::StringArray lines;
        auto wrapperDesc = describe (wrapperPath);
        std::array<std::unique_ptr<juce::PluginDescription>, 2> descs { describe (targets[0]), describe (targets[1]) };

        if (wrapperDesc == nullptr || descs[0] == nullptr || descs[1] == nullptr)
        {
            report.replaceWithText ("couldn't find a plug-in\n");
            setApplicationReturnValue (1);
            quit();
            return;
        }

        const auto ms = [] (double start) { return juce::Time::getMillisecondCounterHiRes() - start; };
        const auto fmt = [] (double v) { return juce::String (v, 1); };
        juce::String error;

        // Baseline: the hosted plug-ins on their own.
        for (auto& d : descs)
        {
            double total = 0.0;

            for (int i = 0; i < rounds; ++i)
            {
                const auto t = juce::Time::getMillisecondCounterHiRes();
                auto p = formats.createPluginInstance (*d, 48000.0, 512, error);
                p.reset();
                total += ms (t);
            }

            lines.add ("direct load+delete " + d->name + ": " + fmt (total / rounds) + " ms");
        }

        double create = 0.0, firstLoad = 0.0, switchLoad = 0.0, save = 0.0, destroy = 0.0;
        int numParams = 0;
        const std::array<juce::MemoryBlock, 2> states { wrapperStateFor (*descs[0]), wrapperStateFor (*descs[1]) };

        for (int i = 0; i < rounds; ++i)
        {
            auto t = juce::Time::getMillisecondCounterHiRes();
            auto w = formats.createPluginInstance (*wrapperDesc, 48000.0, 512, error);
            create += ms (t);

            if (w == nullptr)
            {
                lines.add ("couldn't create the wrapper: " + error);
                break;
            }

            numParams = w->getParameters().size();

            t = juce::Time::getMillisecondCounterHiRes();
            w->setStateInformation (states[0].getData(), (int) states[0].getSize());
            firstLoad += ms (t);

            t = juce::Time::getMillisecondCounterHiRes();
            w->setStateInformation (states[1].getData(), (int) states[1].getSize());
            switchLoad += ms (t);

            juce::MemoryBlock saved;
            t = juce::Time::getMillisecondCounterHiRes();
            w->getStateInformation (saved);
            save += ms (t);

            t = juce::Time::getMillisecondCounterHiRes();
            w.reset();
            destroy += ms (t);
        }

        lines.add ("wrapper reports " + juce::String (numParams) + " parameters");
        lines.add ("create wrapper (insert): " + fmt (create / rounds) + " ms");
        lines.add ("load " + descs[0]->name + " into empty wrapper: " + fmt (firstLoad / rounds) + " ms");
        lines.add ("switch to " + descs[1]->name + ": " + fmt (switchLoad / rounds) + " ms");
        lines.add ("save state: " + fmt (save / rounds) + " ms");
        lines.add ("delete wrapper: " + fmt (destroy / rounds) + " ms");

        report.replaceWithText (lines.joinIntoString ("\n") + "\n");
        quit();
    }

    /** --count: how many parameters a plug-in has, and how many of them are automatable. */
    void countParameters (const juce::File& file)
    {
        juce::OwnedArray<juce::PluginDescription> types;

        for (auto* format : formats.getFormats())
            format->findAllTypesForFile (types, file.getFullPathName());

        juce::String error;
        std::unique_ptr<juce::AudioPluginInstance> plugin;

        if (! types.isEmpty())
            plugin = formats.createPluginInstance (*types[0], 48000.0, 512, error);

        if (plugin == nullptr)
        {
            std::printf ("%s: couldn't load (%s)\n", file.getFileName().toRawUTF8(), error.toRawUTF8());
            setApplicationReturnValue (1);
        }
        else
        {
            int automatable = 0;

            for (auto* p : plugin->getParameters())
                if (p->isAutomatable() && p != plugin->getBypassParameter())
                    ++automatable;

            std::printf ("%s: %d parameters, %d automatable\n", file.getFileName().toRawUTF8(),
                         plugin->getParameters().size(), automatable);
        }

        std::fflush (stdout);
        plugin.reset();

        // Which plug-in files are still loaded once the plug-in is deleted? A plug-in that stays
        // loaded runs its cleanup only when the process exits, after Windows has stopped its threads.
        juce::Timer::callAfterDelay (500, [this]
        {
            HMODULE modules[2048] {};
            DWORD needed = 0;

            if (EnumProcessModules (GetCurrentProcess(), modules, sizeof (modules), &needed))
            {
                for (DWORD i = 0; i < needed / sizeof (HMODULE); ++i)
                {
                    wchar_t name[MAX_PATH] {};
                    GetModuleFileNameW (modules[i], name, MAX_PATH);
                    const juce::String path (name);

                    if (! path.containsIgnoreCase ("\\Windows\\") && ! path.containsIgnoreCase ("DockTest"))
                    {
                        std::printf ("  still loaded: %s\n", path.toRawUTF8());

                        // Experiment: is it pinned, or does something hold extra references?
                        if (path.endsWithIgnoreCase (".vst3") && std::getenv ("DOCKTEST_FREE") != nullptr)
                        {
                            int frees = 0;

                            while (frees < 50 && GetModuleHandleW (name) != nullptr && FreeLibrary (modules[i]))
                                ++frees;

                            std::printf ("    after %d FreeLibrary calls: %s\n", frees,
                                         GetModuleHandleW (name) != nullptr ? "still loaded (pinned)" : "unloaded");
                        }
                    }
                }
            }

            std::fflush (stdout);
            quit();
        });
    }

    /** The wrapper's parameters as the host sees them: the slots should carry the hosted plug-in's
        names and value texts. */
    void logParameters()
    {
        const auto& params = wrapper->getParameters();
        int mapped = 0, unused = 0;
        juce::StringArray sample;

        for (auto* p : params)
        {
            const auto name = p->getName (128);

            if (name.startsWith ("Unused "))
                ++unused;
            else if (name.startsWith ("MIDI CC ") || name.isEmpty())
                continue; // JUCE's hidden MIDI CC parameters
            else if (++mapped <= 10)
                sample.add ("\"" + name + "\" = " + p->getText (p->getValue(), 32) + " " + p->getLabel());
        }

        log ("Wrapper reports " + juce::String (params.size()) + " parameters: " + juce::String (mapped)
             + " mapped, " + juce::String (unused) + " unused slots");

        for (auto& s : sample)
            log ("  " + s);
    }

    void openCycle()
    {
        ++cycle;
        log ("--- cycle " + juce::String (cycle) + ": open");

        auto* editor = wrapper->createEditorAndMakeActive();

        if (editor == nullptr)
            return fail ("Wrapper has no editor");

        window = std::make_unique<juce::DocumentWindow> ("DockTest host window " + juce::String (cycle),
                                                         juce::Colours::darkgrey, juce::DocumentWindow::closeButton);
        window->setUsingNativeTitleBar (true);
        window->setContentOwned (editor, true);
        window->setTopLeftPosition (80, 80);
        window->setVisible (true);

        // The first open builds the hosted editor, which can take a while.
        juce::Timer::callAfterDelay (cycle == 1 ? firstWaitMs : 2500, [this] { inspectAndClose(); });
    }

    void inspectAndClose()
    {
        auto hwnd = (HWND) window->getPeer()->getNativeHandle();
        const auto shot = outDir.getChildFile ("cycle-" + juce::String (cycle) + ".png");
        log ("capture " + shot.getFileName() + (capture (hwnd, shot) ? " ok" : " FAILED"));
        log ("window tree:\n" + dumpAllWindows());
        logWindowTree (hwnd, 0);

        log ("--- cycle " + juce::String (cycle) + ": close");
        window.reset(); // deletes the editor first (IPlugView::removed()), then the window

        if (cycle >= cycles)
        {
            juce::Timer::callAfterDelay (500, [this]
            {
                log ("after final close:\n" + dumpAllWindows());
                log ("done");
                quit();
            });
            return;
        }

        juce::Timer::callAfterDelay (800, [this] { openCycle(); });
    }

    //==============================================================================
    std::unique_ptr<juce::DocumentWindow> makeWindow (const juce::String& name, int x, int y, bool visible)
    {
        auto w = std::make_unique<juce::DocumentWindow> (name, juce::Colours::darkgrey, juce::DocumentWindow::closeButton);
        w->setUsingNativeTitleBar (true);
        w->setTopLeftPosition (x, y);
        w->setVisible (visible);
        return w;
    }

    void snap (juce::DocumentWindow& w, const juce::String& name)
    {
        const auto shot = outDir.getChildFile (variant + "-" + name + ".png");
        log ("capture " + shot.getFileName() + (capture ((HWND) w.getPeer()->getNativeHandle(), shot) ? " ok" : " FAILED"));
        log ("window tree:\n" + dumpAllWindows());
    }

    void logWindowTree (HWND hwnd, int depth)
    {
        RECT r {};
        GetWindowRect (hwnd, &r);
        wchar_t cls[128] {};
        GetClassNameW (hwnd, cls, 128);
        log (juce::String::repeatedString ("  ", depth) + juce::String (cls) + "  "
             + juce::String (r.right - r.left) + "x" + juce::String (r.bottom - r.top)
             + " at " + juce::String (r.left) + "," + juce::String (r.top)
             + (IsWindowVisible (hwnd) ? "" : "  (hidden)"));

        for (auto child = GetWindow (hwnd, GW_CHILD); child != nullptr; child = GetWindow (child, GW_HWNDNEXT))
            logWindowTree (child, depth + 1);
    }

    void startRaw()
    {
        log.file = outDir.getChildFile (variant + ".log");
        log ("Raw test, variant " + variant + ", target " + targetFile.getFullPathName());

        juce::OwnedArray<juce::PluginDescription> types;

        for (auto* format : formats.getFormats())
            format->findAllTypesForFile (types, targetFile.getFullPathName());

        juce::String error;

        if (types.isEmpty() || (target = formats.createPluginInstance (*types[0], 48000.0, 512, error)) == nullptr)
            return fail ("Couldn't load the target: " + error);

        rawEditor.reset (target->createEditorAndMakeActive());

        if (rawEditor == nullptr)
            return fail ("Target has no editor");

        if (variant == "probe-park")
        {
            // Like AltraPlugHost: the new editor goes into a hidden window first, then is moved
            // into the visible one.
            windowB = makeWindow ("B (hidden parking)", 1100, 80, false);
            windowB->setContentNonOwned (rawEditor.get(), true);
            log ("parked: " + rawEditor->getBounds().toString());

            juce::Timer::callAfterDelay (300, [this]
            {
                windowA = makeWindow ("A (docked)", 80, 80, true);
                windowB->clearContentComponent();
                windowA->setContentNonOwned (rawEditor.get(), true);
                log ("docked: " + rawEditor->getBounds().toString());

                juce::Timer::callAfterDelay (3000, [this]
                {
                    log ("3 s later: " + rawEditor->getBounds().toString());
                    logWindowTree ((HWND) windowA->getPeer()->getNativeHandle(), 0);
                    quit();
                });
            });
            return;
        }

        windowA = makeWindow ("A (first host window)", 80, 80, true);
        windowA->setContentNonOwned (rawEditor.get(), true);

        juce::Timer::callAfterDelay (6000, [this]
        {
            snap (*windowA, "1-before");

            if (variant == "probe")
            {
                // How big the plug-in's own windows really are, versus the size it reports.
                log ("editor component: " + rawEditor->getBounds().toString()
                     + (rawEditor->isResizable() ? " (resizable)" : " (not resizable)"));
                logWindowTree ((HWND) windowA->getPeer()->getNativeHandle(), 0);

                // Re-read the size (IPlugView::getSize).
                rawEditor->setScaleFactor (1.0f);
                log ("after re-reading the size: " + rawEditor->getBounds().toString());

                // Move the editor to a new window: detaches and re-attaches the plug-in's view.
                windowC = makeWindow ("C (reattached)", 80, 80, true);
                windowA->clearContentComponent();
                windowC->setContentNonOwned (rawEditor.get(), true);
                windowA.reset();

                juce::Timer::callAfterDelay (2000, [this]
                {
                    log ("after re-attaching: " + rawEditor->getBounds().toString());
                    rawEditor->setScaleFactor (1.0f);
                    log ("after re-attaching and re-reading: " + rawEditor->getBounds().toString());
                    logWindowTree ((HWND) windowC->getPeer()->getNativeHandle(), 0);
                    quit();
                });
                return;
            }

            if (variant == "park-hidden" || variant == "park-visible")
            {
                windowB = makeWindow ("B (parking)", 1100, 80, variant == "park-visible");
                windowA->clearContentComponent();
                windowB->setContentNonOwned (rawEditor.get(), true);
                windowA.reset();

                juce::Timer::callAfterDelay (1500, [this]
                {
                    if (variant == "park-visible")
                        snap (*windowB, "2-parked");

                    windowC = makeWindow ("C (reopened host window)", 80, 80, true);
                    windowB->clearContentComponent();
                    windowC->setContentNonOwned (rawEditor.get(), true);
                    juce::Timer::callAfterDelay (2500, [this] { snap (*windowC, "3-after"); quit(); });
                });
            }
            else if (variant == "park-on-destroy")
            {
                windowB = makeWindow ("B (parking)", 1100, 80, false);
                destroyCallback = [this]
                {
                    log ("WM_DESTROY on window A: parking now");
                    windowA->clearContentComponent();
                    windowB->setContentNonOwned (rawEditor.get(), true);
                };

                SetWindowSubclass ((HWND) windowA->getPeer()->getNativeHandle(), [] (HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) -> LRESULT
                {
                    if (m == WM_DESTROY)
                        if (auto& cb = reinterpret_cast<DockTestApp*> (ref)->destroyCallback)
                            std::exchange (cb, nullptr)();

                    return DefSubclassProc (h, m, w, l);
                }, 1, reinterpret_cast<DWORD_PTR> (this));

                // Like JUCE's VST3 wrapper: the window is destroyed while the editor is still in it.
                windowA->removeFromDesktop();
                windowA.reset();

                juce::Timer::callAfterDelay (1500, [this]
                {
                    windowC = makeWindow ("C (reopened host window)", 80, 80, true);
                    windowB->clearContentComponent();
                    windowC->setContentNonOwned (rawEditor.get(), true);
                    juce::Timer::callAfterDelay (2500, [this] { snap (*windowC, "3-after"); quit(); });
                });
            }
            else if (variant == "direct")
            {
                windowC = makeWindow ("C (second host window)", 80, 80, true);
                windowA->clearContentComponent();
                windowC->setContentNonOwned (rawEditor.get(), true);
                juce::Timer::callAfterDelay (2500, [this] { snap (*windowC, "3-after"); quit(); });
            }
            else if (variant == "setparent")
            {
                windowC = makeWindow ("C (second host window)", 80, 80, true);
                windowC->setSize (rawEditor->getWidth() + 16, rawEditor->getHeight() + 40);

                // The plug-in's container is the first native child of window A's content.
                auto container = GetWindow ((HWND) windowA->getPeer()->getNativeHandle(), GW_CHILD);
                log ("moving " + describeWindow (container));
                SetParent (container, (HWND) windowC->getPeer()->getNativeHandle());
                SetWindowPos (container, nullptr, 8, 31, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                juce::Timer::callAfterDelay (2500, [this] { snap (*windowC, "3-after"); quit(); });
            }
            else
            {
                fail ("Unknown variant " + variant);
            }
        });
    }

    void fail (const juce::String& message)
    {
        log ("FAILED: " + message);
        setApplicationReturnValue (1);
        quit();
    }

    juce::AudioPluginFormatManager formats;
    std::unique_ptr<juce::AudioPluginInstance> wrapper;
    std::unique_ptr<juce::DocumentWindow> window;
    std::unique_ptr<juce::AudioPluginInstance> target;
    std::unique_ptr<juce::AudioProcessorEditor> rawEditor;
    std::unique_ptr<juce::DocumentWindow> windowA, windowB, windowC;
    juce::String variant;
    std::function<void()> destroyCallback;
    juce::File wrapperFile, targetFile, outDir;
    int cycles = 3, cycle = 0, firstWaitMs = 8000;
    Log log;
};

START_JUCE_APPLICATION (DockTestApp)
