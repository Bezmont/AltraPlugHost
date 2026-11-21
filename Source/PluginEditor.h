#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginBrowser.h"
#include "NativeWindowHelpers.h"

namespace altra
{

class WrapperProcessor;
enum class SnapMode;

/** AltraPlugHost's window in Live: a toolbar, and below it the hosted plug-in's editor (docked),
    the plug-in browser, or a status message. Collapsed, it is only a small bar with the plug-in
    and track names and a Restore button.

    Live creates and destroys this freely. The hosted editor is never destroyed with it: it is
    moved back into the processor's hidden window first (see WrapperProcessor). */
class WrapperEditor final : public juce::AudioProcessorEditor
{
public:
    explicit WrapperEditor (WrapperProcessor&);
    ~WrapperEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    /** Where the hosted editor is docked. Tells us when the plug-in resizes its editor. */
    struct DockSlot final : public juce::Component
    {
        std::function<void()> onChildResized;
        void childBoundsChanged (juce::Component*) override  { juce::NullCheckedInvocation::invoke (onChildResized); }
    };

    /** An invisible child whose only job is to hear JUCE's teardown early.

        Before JUCE destroys a native window (Component::removeFromDesktop), and when a component
        is hidden or removed from its parent, it calls CachedComponentImage::releaseResources() on
        every component inside. That is our earliest warning that the docked editor is about to
        stop being shown, and comes *before* the native window is destroyed. Parking any later,
        even on WM_DESTROY, leaves many plug-ins drawing nothing once docked again (verified with
        tools/DockTest). */
    struct TeardownSentinel final : public juce::Component
    {
        explicit TeardownSentinel (std::function<void()> onTeardown);

    private:
        struct Hook;
    };

    void parkBeforeTeardown();
    void refresh();
    void fitToHostedEditor();
    void runBusy (const juce::String& message, std::function<void()> work);
    void showMessage (const juce::String& text, const juce::String& buttonText, std::function<void()> action);

    void startLoading();
    void openBrowser (PluginCatalog);
    void closeBrowser();
    void showLoadMenu();
    void browseForPlugin();
    void loadFromFile (const juce::File&, const juce::String& preferredName = {});
    bool load (const juce::PluginDescription&);
    void collapse (SnapMode);
    void restore();
    void showOptionsMenu();
    int getCollapsedWidth() const;
    juce::Rectangle<int> getSwatchBounds() const;
    juce::String getTrackLabel() const;
    std::wstring makeWindowTitle (const std::wstring& hostTitle);
    void showError (const juce::String& message);

    WrapperProcessor& processor;

    juce::Label nameLabel;
    juce::TextButton browseButton { "Browse" }, restoreButton { "Restore" };

    // Icon buttons: collapse to the top row, the bottom row, or a floating bar; the options menu;
    // and the slim bar's close.
    juce::ShapeButton collapseTopButton    { "Collapse to top",    juce::Colours::lightgrey, juce::Colours::white, juce::Colours::grey },
                      collapseBottomButton { "Collapse to bottom", juce::Colours::lightgrey, juce::Colours::white, juce::Colours::grey },
                      collapseFloatButton  { "Collapse floating",  juce::Colours::lightgrey, juce::Colours::white, juce::Colours::grey },
                      menuButton           { "Options",            juce::Colours::lightgrey, juce::Colours::white, juce::Colours::grey },
                      closeBarButton       { "Close",              juce::Colours::grey,      juce::Colours::white, juce::Colours::lightgrey };

    DockSlot slot;
    juce::Label messageLabel;
    juce::TextButton messageButton;
    std::unique_ptr<PluginBrowser> browser;

    // JUCE's VST3 wrapper destroys our native window *before* deleting this editor, and Windows
    // destroys child windows with their parent, so the destructor is too late to park. The
    // sentinel parks first; the watcher (WM_DESTROY) is a last resort that at least keeps the
    // plug-in's window from being destroyed.
    TeardownSentinel sentinel { [this] { parkBeforeTeardown(); } };
    std::unique_ptr<native::DestroyWatcher> windowWatcher;

    // Shows "<hosted plug-in>/<track>" as Live's window title instead of "AltraPlugHost/<track>".
    std::unique_ptr<native::TitleWatcher> titleWatcher;
    juce::String trackFromTitle;  // the track part of Live's own title, if the host doesn't tell us
    bool moveToExpandedPosition = false; // opened after closing collapsed
    bool onScreenCheckPending = false;

    // While collapsed with Slim on: Live's window without its title bar.
    std::unique_ptr<native::FramelessWindow> frameless;
    bool isSlimBar() const;
    void* getHostWindow() const;

    // Shows the tooltips of the toolbar's buttons and label (JUCE shows none without one). A
    // window of its own on the desktop: drawn inside ours, the hosted plug-in's native window
    // would cover it.
    juce::TooltipWindow tooltipWindow { nullptr, 600 };

    juce::String busyMessage;     // non-empty while slow work is about to run
    bool fitting = false;

    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::ScopedMessageBox messageBox;
    juce::StringArray menuPaths;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WrapperEditor)
};

} // namespace altra
