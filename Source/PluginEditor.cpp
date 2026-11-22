#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Diagnostics.h"

namespace altra
{

namespace
{
    constexpr int toolbarHeight = 34;
    constexpr int minimumWidth = 260;        // the toolbar's buttons need this much
    constexpr int messageWidth = 560, messageHeight = 240;
    constexpr int browserWidth = 960, browserHeight = 660;
    constexpr int swatchSize = 14;       // the track colour, a square at the toolbar's left
    constexpr int restoreWidth = 80;
    constexpr int browseWidth = 72;
    constexpr int iconWidth = 28;
    constexpr int collapsedWidth = 300;  // fixed, so the bars line up in the bottom row
    constexpr int closeBarWidth = 22;

    // An arrow pointing up, turned by `angle` (radians, clockwise).
    juce::Path makeArrow (float angle)
    {
        juce::Path arrow;
        arrow.addArrow ({ 6.0f, 13.0f, 6.0f, 0.0f }, 2.0f, 10.0f, 6.0f);
        arrow.applyTransform (juce::AffineTransform::rotation (angle, 6.0f, 6.5f));
        return arrow;
    }

    void setIcon (juce::ShapeButton& button, const juce::Path& icon, const juce::String& tooltip)
    {
        button.setShape (icon, false, true, false);
        button.setBorderSize (juce::BorderSize<int> (6));
        button.setTooltip (tooltip);
    }

    const auto background = juce::Colour (0xff202225);
    const auto toolbarColour = juce::Colour (0xff2b2e33);
}

//==============================================================================
struct WrapperEditor::TeardownSentinel::Hook final : public juce::CachedComponentImage
{
    explicit Hook (std::function<void()> cb) : callback (std::move (cb)) {}

    void paint (juce::Graphics&) override                       {}
    bool invalidateAll() override                               { return true; }
    bool invalidate (const juce::Rectangle<int>&) override      { return true; }
    void releaseResources() override                            { juce::NullCheckedInvocation::invoke (callback); }

    std::function<void()> callback;
};

WrapperEditor::TeardownSentinel::TeardownSentinel (std::function<void()> onTeardown)
{
    setInterceptsMouseClicks (false, false);
    setCachedComponentImage (new Hook (std::move (onTeardown)));
}

void WrapperEditor::parkBeforeTeardown()
{
    // Closing while collapsed: remember where the bar was, for the next Collapse.
    if (processor.isCollapsed() && processor.collapseMode == SnapMode::off)
    {
        int x = 0, y = 0;

        if (titleWatcher != nullptr && native::getWindowPosition (titleWatcher->getWindow(), x, y))
            processor.collapsedPosition = juce::Point<int> (x, y);
    }

    if (! processor.isHostedEditorDocked())
        return;

    processor.parkHostedEditor();

    // If this was only a hide and the window lives on, dock again once things settle. During a
    // real teardown this editor is deleted first, so this does nothing.
    juce::MessageManager::callAsync ([safeThis = SafePointer<WrapperEditor> (this)]
    {
        if (safeThis != nullptr && safeThis->isShowing())
            safeThis->refresh();
    });
}

//==============================================================================
WrapperEditor::WrapperEditor (WrapperProcessor& p)
    : AudioProcessorEditor (p), processor (p)
{
    diagnostics::log ("AltraPlugHost window opening");

    // Opening always shows the plug-in. If the window was closed collapsed, Live reopens it where
    // the bar was, so it goes back to the expanded position once it's on screen.
    if (processor.isCollapsed())
    {
        processor.setCollapsed (false);
        moveToExpandedPosition = true;
    }

    // First child, so JUCE's teardown reaches it before the slot holding the hosted editor.
    addChildComponent (sentinel);

    nameLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    nameLabel.setMinimumHorizontalScale (0.7f);

    messageLabel.setJustificationType (juce::Justification::centred);
    messageLabel.setFont (juce::FontOptions (16.0f));

    for (auto* c : std::initializer_list<juce::Component*> { &nameLabel, &browseButton, &collapseTopButton,
                                                             &collapseBottomButton, &collapseFloatButton,
                                                             &menuButton, &slot })
        addAndMakeVisible (c);

    addChildComponent (restoreButton);

    slot.addChildComponent (messageLabel);
    slot.addChildComponent (messageButton);
    slot.onChildResized = [this] { fitToHostedEditor(); };

    constexpr auto pi = juce::MathConstants<float>::pi;

    browseButton.onClick  = [this] { startLoading(); };
    browseButton.setTooltip ("Pick a plugin to load");
    restoreButton.onClick = [this] { restore(); };
    restoreButton.setTooltip ("Show the plugin again");

    setIcon (collapseTopButton, makeArrow (0.0f), "Collapse to a bar along the top of the screen");
    setIcon (collapseBottomButton, makeArrow (pi), "Collapse to a bar along the bottom of the screen");
    setIcon (collapseFloatButton, makeArrow (-pi / 2.0f), "Collapse to a floating bar, back where you last left it");
    collapseTopButton.onClick    = [this] { collapse (SnapMode::top); };
    collapseBottomButton.onClick = [this] { collapse (SnapMode::bottom); };
    collapseFloatButton.onClick  = [this] { collapse (SnapMode::off); };

    juce::Path burger;
    for (const auto y : { 1.0f, 6.0f, 11.0f })
        burger.addRectangle (0.0f, y, 14.0f, 2.0f);

    setIcon (menuButton, burger, "Options");
    menuButton.onClick = [this] { showOptionsMenu(); };

    juce::Path cross;
    cross.addLineSegment ({ 0.0f, 0.0f, 10.0f, 10.0f }, 1.6f);
    cross.addLineSegment ({ 10.0f, 0.0f, 0.0f, 10.0f }, 1.6f);
    closeBarButton.setShape (cross, true, true, false);
    closeBarButton.setBorderSize (juce::BorderSize<int> (7));
    closeBarButton.setTooltip ("Close the window (the plugin keeps running)");
    closeBarButton.onClick = [this] { native::closeWindow (getHostWindow()); };
    addChildComponent (closeBarButton);

    processor.onStatusChanged = [this] { refresh(); };

    setSize (messageWidth, messageHeight);

    // Nothing loaded yet: go straight to picking one, if PlugBrowser's catalog is there.
    if (! processor.isPluginLoaded() && processor.getMissingPluginName().isEmpty())
        if (auto catalog = PluginCatalog::load())
        {
            openBrowser (std::move (*catalog));
            return;
        }

    refresh();
}

WrapperEditor::~WrapperEditor()
{
    diagnostics::log ("AltraPlugHost window closing");
    processor.onStatusChanged = nullptr;
    frameless.reset();        // gives Live's window its frame back, if it still exists
    titleWatcher.reset();     // gives Live's window its own title back
    windowWatcher.reset();

    // Usually already parked by windowWatcher; this covers any other way of closing.
    processor.parkHostedEditor();
    browser.reset();
}

void WrapperEditor::parentHierarchyChanged()
{
    AudioProcessorEditor::parentHierarchyChanged();

    auto* peer = getPeer();
    auto* window = peer != nullptr ? peer->getNativeHandle() : nullptr;

    if (window == nullptr || (windowWatcher != nullptr && windowWatcher->getWindow() == window))
        return;

    windowWatcher = std::make_unique<native::DestroyWatcher> (window, [safeThis = SafePointer<WrapperEditor> (this)]
    {
        // Our window is being destroyed and its child windows are next, the hosted plug-in's
        // included. Move the hosted editor into the processor's hidden window first.
        if (safeThis != nullptr)
            safeThis->processor.parkHostedEditor();
    });

    auto* topLevel = native::getTopLevelWindow (window);

    if (topLevel != nullptr && (titleWatcher == nullptr || titleWatcher->getWindow() != topLevel))
    {
        titleWatcher.reset();
        titleWatcher = std::make_unique<native::TitleWatcher> (topLevel, [this] (const std::wstring& hostTitle)
        {
            return makeWindowTitle (hostTitle);
        });
    }

    if (moveToExpandedPosition && titleWatcher != nullptr)
    {
        // Live places its window after attaching us, so move once that's done.
        juce::MessageManager::callAsync ([safeThis = SafePointer<WrapperEditor> (this)]
        {
            if (safeThis == nullptr || ! safeThis->moveToExpandedPosition || safeThis->titleWatcher == nullptr)
                return;

            safeThis->moveToExpandedPosition = false;

            if (const auto target = safeThis->processor.expandedPosition)
            {
                native::moveWindowIfOnScreen (safeThis->titleWatcher->getWindow(), target->x, target->y);
                native::keepWindowOnScreen (safeThis->titleWatcher->getWindow());
            }
        });
    }
}

std::wstring WrapperEditor::makeWindowTitle (const std::wstring& hostTitle)
{
    // Live's title is "<device>/<track>". Keep the track part, and show the hosted plug-in as the
    // device. This runs inside Live's own title change, so anything more waits.
    const auto title = juce::String (hostTitle.c_str());
    const auto prefix = juce::String (JucePlugin_Name) + "/";

    if (title.startsWith (prefix))
    {
        const auto track = title.substring (prefix.length());

        if (track != trackFromTitle)
        {
            trackFromTitle = track;
            juce::MessageManager::callAsync ([safeThis = SafePointer<WrapperEditor> (this)]
            {
                if (safeThis != nullptr)
                    safeThis->refresh();
            });
        }
    }

    if (! processor.isPluginLoaded() || ! title.startsWith (prefix))
        return hostTitle;

    return (processor.getHostedPluginName() + "/" + trackFromTitle).toWideCharPointer();
}

juce::String WrapperEditor::getTrackLabel() const
{
    const auto fromHost = processor.getTrackName();
    return fromHost.isNotEmpty() ? fromHost : trackFromTitle;
}

//==============================================================================
void WrapperEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);
    auto toolbar = getLocalBounds().removeFromTop (toolbarHeight);
    g.setColour (toolbarColour);
    g.fillRect (toolbar);

    if (const auto colour = processor.getTrackColour())
    {
        g.setColour (*colour);
        g.fillRect (getSwatchBounds());
    }

    // Without Live's frame, the bar needs an edge of its own.
    if (isSlimBar())
    {
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRect (getLocalBounds());
    }
}

juce::Rectangle<int> WrapperEditor::getSwatchBounds() const
{
    return { 8, (toolbarHeight - swatchSize) / 2, swatchSize, swatchSize };
}

void WrapperEditor::resized()
{
    // Live grows its window to the right and down from the top-left corner, so a bigger plug-in
    // can end up partly off screen. Once Live has resized it, pull it back in.
    if (! processor.isCollapsed() && ! onScreenCheckPending && titleWatcher != nullptr)
    {
        onScreenCheckPending = true;
        juce::MessageManager::callAsync ([safeThis = SafePointer<WrapperEditor> (this)]
        {
            if (safeThis == nullptr)
                return;

            safeThis->onScreenCheckPending = false;

            if (! safeThis->processor.isCollapsed())
                native::keepWindowOnScreen (safeThis->getHostWindow());
        });
    }

    auto area = getLocalBounds();
    auto toolbar = area.removeFromTop (toolbarHeight);

    if (processor.getTrackColour().has_value())
        toolbar.removeFromLeft (getSwatchBounds().getRight());

    toolbar = toolbar.reduced (6, 4);

    if (closeBarButton.isVisible())
    {
        closeBarButton.setBounds (toolbar.removeFromRight (closeBarWidth));
        toolbar.removeFromRight (4);
    }

    if (restoreButton.isVisible())
    {
        restoreButton.setBounds (toolbar.removeFromRight (restoreWidth));
        toolbar.removeFromRight (6);
    }

    if (menuButton.isVisible())
    {
        menuButton.setBounds (toolbar.removeFromRight (iconWidth));
        toolbar.removeFromRight (6);
    }

    // Left to right: up, down, left.
    if (collapseFloatButton.isVisible())
    {
        collapseFloatButton.setBounds (toolbar.removeFromRight (iconWidth));
        collapseBottomButton.setBounds (toolbar.removeFromRight (iconWidth));
        collapseTopButton.setBounds (toolbar.removeFromRight (iconWidth));
        toolbar.removeFromRight (6);
    }

    if (browseButton.isVisible())
    {
        browseButton.setBounds (toolbar.removeFromRight (browseWidth));
        toolbar.removeFromRight (6);
    }

    nameLabel.setBounds (toolbar);

    slot.setBounds (area);

    if (browser != nullptr)
        browser->setBounds (slot.getLocalBounds().reduced (10));

    auto messageArea = slot.getLocalBounds().withSizeKeepingCentre (std::min (slot.getWidth() - 20, 480), 90);
    messageLabel.setBounds (messageArea.removeFromTop (50));
    messageButton.setBounds (messageArea.withSizeKeepingCentre (160, 30));

    // A docked editor narrower than the toolbar is centred.
    if (processor.isHostedEditorDocked())
        if (auto* editor = processor.getHostedEditor())
        {
            const juce::ScopedValueSetter<bool> svs (fitting, true);
            editor->setTopLeftPosition (std::max (0, (slot.getWidth() - editor->getWidth()) / 2), 0);
        }
}

//==============================================================================
void WrapperEditor::refresh()
{
    const auto loaded = processor.isPluginLoaded();
    const auto name = processor.getHostedPluginName();
    const auto missing = processor.getMissingPluginName();

    const auto collapsed = loaded && processor.isCollapsed();
    const auto track = getTrackLabel();

    // Toolbar. Expanded, Live's title bar already names the plug-in and the track, so the label
    // only shows text in the slim bar. It stays in place (empty) for its tooltip.
    if (loaded)
        nameLabel.setText (! collapsed ? juce::String()
                                       : track.isNotEmpty() ? name + "  -  " + track : name,
                           juce::dontSendNotification);
    else if (missing.isNotEmpty())
        nameLabel.setText ("Missing: " + missing, juce::dontSendNotification);
    else
        nameLabel.setText (JucePlugin_Name, juce::dontSendNotification);

    juce::String tooltip;

    if (loaded)
    {
        tooltip << "Latency: " << processor.getHostedLatencySamples() << " samples\n"
                << "Automatable parameters: " << processor.getNumAutomatableParameters();

        if (const auto notMapped = processor.getNumParametersNotMapped(); notMapped > 0)
            tooltip << " (" << notMapped << " more don't fit in AltraPlugHost's "
                    << ParameterBank::numSlots << " slots)";
    }

    nameLabel.setTooltip (tooltip);
    browseButton.setEnabled (browser == nullptr && busyMessage.isEmpty());

    for (auto* button : { &collapseTopButton, &collapseBottomButton, &collapseFloatButton })
    {
        button->setVisible (! collapsed && loaded);
        button->setEnabled (busyMessage.isEmpty());
    }
    if (collapsed)
        processor.parkHostedEditor(); // before its slot is hidden, so the plug-in never sees that

    // Collapsed, Live's title bar already names the plug-in and the track, unless Slim hides it.
    // In the slim bar the name keeps its size and ends in "..." when it doesn't fit, and
    // clicks go through it to the bar, which drags the window.
    const auto slimBar = collapsed && processor.slimCollapsed;
    nameLabel.setVisible (! collapsed || slimBar);
    nameLabel.setMinimumHorizontalScale (slimBar ? 1.0f : 0.7f);
    nameLabel.setFont (juce::FontOptions (slimBar ? 13.0f : 14.0f, juce::Font::bold));
    nameLabel.setInterceptsMouseClicks (! collapsed, false);
    closeBarButton.setVisible (slimBar);
    restoreButton.setVisible (collapsed);
    menuButton.setVisible (! collapsed);
    browseButton.setVisible (! collapsed);
    slot.setVisible (! collapsed);
    repaint();

    if (titleWatcher != nullptr)
        titleWatcher->reapply();  // the hosted plug-in may have changed

    if (collapsed)
    {
        // The hosted editor waits in the hidden window, exactly as when this window is closed.
        browser.reset();
        processor.parkHostedEditor();
        messageLabel.setVisible (false);
        messageButton.setVisible (false);

        const auto width = getCollapsedWidth();

        if (getWidth() != width || getHeight() != toolbarHeight)
            setSize (width, toolbarHeight);
        else
            resized();

        return;
    }

    if (browser != nullptr)
        return; // the browser fills the window until it's closed

    if (busyMessage.isNotEmpty())
    {
        showMessage (busyMessage, {}, nullptr);
        return;
    }

    if (loaded && processor.didHostedEditorFail())
    {
        processor.parkHostedEditor();
        showMessage (name + " is loaded, but its window couldn't be opened.", {}, nullptr);
        setSize (messageWidth, messageHeight);
        return;
    }

    if (loaded)
    {
        if (! processor.hasHostedEditorBeenBuilt())
        {
            // First time: building the plug-in's editor is slow, so say so before it starts.
            runBusy ("Opening " + name + "...", [this]
            {
                if (! processor.buildHostedEditor (&slot))
                    showError (processor.getHostedPluginName() + " has no editor that can be shown.");
            });
            return;
        }

        processor.dockHostedEditor (slot);
        messageLabel.setVisible (false);
        messageButton.setVisible (false);
        fitToHostedEditor();
        return;
    }

    processor.parkHostedEditor();

    if (missing.isNotEmpty())
        showMessage ("Couldn't load " + missing + ".\nIts settings are kept until you load another plugin.",
                     "Change plugin...", [this] { startLoading(); });
    else
        showMessage ("No plugin loaded.", "Load plugin...", [this] { startLoading(); });

    setSize (messageWidth, messageHeight);
}

void WrapperEditor::showMessage (const juce::String& text, const juce::String& buttonText, std::function<void()> action)
{
    messageLabel.setText (text, juce::dontSendNotification);
    messageLabel.setVisible (true);
    messageButton.setButtonText (buttonText);
    messageButton.onClick = std::move (action);
    messageButton.setVisible (buttonText.isNotEmpty());
    resized();
}

void WrapperEditor::fitToHostedEditor()
{
    if (fitting || ! processor.isHostedEditorDocked())
        return;

    if (auto* editor = processor.getHostedEditor())
    {
        const juce::ScopedValueSetter<bool> svs (fitting, true);
        const auto width = std::max (minimumWidth, editor->getWidth());
        const auto height = toolbarHeight + editor->getHeight();

        if (getWidth() != width || getHeight() != height)
            setSize (width, height);
        else
            resized();
    }
}

void WrapperEditor::runBusy (const juce::String& message, std::function<void()> work)
{
    // Loading a plug-in or building its editor blocks Live's UI thread, so nothing can animate.
    // What we can do is make sure the message is on screen before the freeze starts.
    busyMessage = message;
    processor.parkHostedEditor();
    refresh();
    juce::MouseCursor::showWaitCursor();

    if (auto* peer = getPeer())
        peer->performAnyPendingRepaintsNow();

    juce::Timer::callAfterDelay (50, [safeThis = SafePointer<WrapperEditor> (this), work = std::move (work)]
    {
        if (safeThis == nullptr)
            return;

        work();

        if (safeThis == nullptr)
            return;

        juce::MouseCursor::hideWaitCursor();
        safeThis->busyMessage = {};
        safeThis->refresh();
    });
}

//==============================================================================
void WrapperEditor::startLoading()
{
    // PlugBrowser's catalog gives a screenshot browser; without it, fall back to the folder list.
    if (auto catalog = PluginCatalog::load())
        openBrowser (std::move (*catalog));
    else
        showLoadMenu();
}

void WrapperEditor::openBrowser (PluginCatalog catalog)
{
   #if ALTRA_IS_INSTRUMENT
    constexpr bool preferInstruments = true;
   #else
    constexpr bool preferInstruments = false;
   #endif

    // The browser takes the space; the hosted editor waits in the hidden window meanwhile.
    processor.parkHostedEditor();
    messageLabel.setVisible (false);
    messageButton.setVisible (false);

    browser = std::make_unique<PluginBrowser> (std::move (catalog), preferInstruments);

    // These run inside the browser's own callbacks, so anything that deletes the browser runs
    // asynchronously, never while its code is still on the stack.
    browser->onLoad = [this] (const CatalogEntry& entry)
    {
        juce::MessageManager::callAsync ([safeThis = SafePointer<WrapperEditor> (this), file = entry.file, name = entry.name]
        {
            if (safeThis == nullptr)
                return;

            safeThis->closeBrowser();
            safeThis->runBusy ("Loading " + name + "...", [safeThis, file, name]
            {
                if (safeThis != nullptr)
                    safeThis->loadFromFile (file, name);
            });
        });
    };

    browser->onClose = [this]
    {
        juce::MessageManager::callAsync ([safeThis = SafePointer<WrapperEditor> (this)]
        {
            if (safeThis != nullptr)
                safeThis->closeBrowser();
        });
    };

    browser->onBrowseFiles = [this] { browseForPlugin(); };

    slot.addAndMakeVisible (*browser);
    setSize (browserWidth, browserHeight);
    refresh();
    resized();
}

void WrapperEditor::closeBrowser()
{
    if (browser == nullptr)
        return;

    browser.reset();
    refresh();
}

void WrapperEditor::showLoadMenu()
{
    // Only lists .vst3 files in the standard folders; nothing is loaded until one is picked.
    juce::VST3PluginFormat vst3;
    menuPaths = vst3.searchPathsForPlugins (vst3.getDefaultLocationsToSearch(), true, false);
    menuPaths.sortNatural();

    juce::PopupMenu menu;
    menu.addItem (1, "Browse...");
    menu.addSeparator();

    const auto roots = vst3.getDefaultLocationsToSearch();

    for (int i = 0; i < menuPaths.size(); ++i)
    {
        const juce::File file (menuPaths[i]);
        auto label = file.getFileNameWithoutExtension();

        if (label.startsWithIgnoreCase ("AltraPlugHost"))
            continue; // loading ourselves inside ourselves is never useful

        // Show the vendor folder for plug-ins that live in one.
        for (int r = 0; r < roots.getNumPaths(); ++r)
            if (file.isAChildOf (roots[r]) && file.getParentDirectory() != roots[r])
                label = file.getParentDirectory().getRelativePathFrom (roots[r]) + " / " + label;

        menu.addItem (100 + i, label);
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (browseButton),
                        [safeThis = SafePointer<WrapperEditor> (this)] (int result)
                        {
                            if (safeThis == nullptr || result == 0)
                                return;

                            if (result == 1)
                            {
                                safeThis->browseForPlugin();
                            }
                            else if (juce::isPositiveAndBelow (result - 100, safeThis->menuPaths.size()))
                            {
                                const juce::File file (safeThis->menuPaths[result - 100]);
                                safeThis->runBusy ("Loading " + file.getFileNameWithoutExtension() + "...", [safeThis, file]
                                {
                                    if (safeThis != nullptr)
                                        safeThis->loadFromFile (file);
                                });
                            }
                        });
}

void WrapperEditor::browseForPlugin()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Choose a VST3 plug-in",
                                                       juce::File ("C:\\Program Files\\Common Files\\VST3"),
                                                       "*.vst3");

    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safeThis = SafePointer<WrapperEditor> (this)] (const juce::FileChooser& chooser)
                              {
                                  const auto file = chooser.getResult();

                                  if (safeThis == nullptr || file == juce::File())
                                      return;

                                  safeThis->closeBrowser();
                                  safeThis->runBusy ("Loading " + file.getFileNameWithoutExtension() + "...", [safeThis, file]
                                  {
                                      if (safeThis != nullptr)
                                          safeThis->loadFromFile (file);
                                  });
                              });
}

void WrapperEditor::loadFromFile (const juce::File& file, const juce::String& preferredName)
{
    auto types = processor.findPluginsInFile (file);

    if (types.isEmpty())
    {
        showError ("No VST3 plug-in found in " + file.getFullPathName());
        return;
    }

    // Picked from the catalog: load the plug-in it named, even if the file holds several.
    if (preferredName.isNotEmpty())
        for (auto* type : types)
            if (type->name.equalsIgnoreCase (preferredName))
            {
                load (*type);
                return;
            }

    if (types.size() == 1)
    {
        load (*types[0]);
        return;
    }

    // Some bundles contain several plug-ins (e.g. mono and stereo versions).
    juce::PopupMenu menu;

    for (int i = 0; i < types.size(); ++i)
        menu.addItem (i + 1, types[i]->name + (types[i]->isInstrument ? " (instrument)" : ""));

    auto shared = std::make_shared<juce::OwnedArray<juce::PluginDescription>> (std::move (types));

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (browseButton),
                        [safeThis = SafePointer<WrapperEditor> (this), shared] (int result)
                        {
                            if (safeThis == nullptr || ! juce::isPositiveAndBelow (result - 1, shared->size()))
                                return;

                            const auto description = *(*shared)[result - 1];
                            safeThis->runBusy ("Loading " + description.name + "...", [safeThis, description]
                            {
                                if (safeThis != nullptr)
                                    safeThis->load (description);
                            });
                        });
}

bool WrapperEditor::load (const juce::PluginDescription& description)
{
    const auto error = processor.loadPlugin (description);

    if (error.isNotEmpty())
    {
        showError (error);
        return false;
    }

    // refresh() then builds and docks the new plug-in's editor, with its own "Opening..." message.
    return true;
}

void WrapperEditor::collapse (SnapMode mode)
{
    // Live's window keeps its top-left corner when it resizes. Remember where the expanded window
    // is, shrink to the bar, then put the bar in its row, or (floating) where it was last left.
    if (processor.isCollapsed() || ! processor.isPluginLoaded())
        return;

    auto* window = getHostWindow();
    const auto snapping = mode != SnapMode::off;
    auto* monitor = snapping ? native::getMonitorOfWindow (window) : nullptr;
    int x = 0, y = 0;

    if (native::getWindowPosition (window, x, y))
        processor.expandedPosition = juce::Point<int> (x, y);

    processor.collapseMode = mode;
    processor.setCollapsed (true); // calls refresh(), which resizes us and Live's window

    // The rest once Live's window has its new size: the frame comes off around exactly the bar,
    // and the row's slots are measured in it.
    juce::MessageManager::callAsync ([safeThis = SafePointer<WrapperEditor> (this), monitor, mode, snapping]
    {
        if (safeThis == nullptr || ! safeThis->processor.isCollapsed())
            return;

        auto* hostWindow = safeThis->getHostWindow();

        if (safeThis->processor.slimCollapsed && safeThis->frameless == nullptr)
        {
            safeThis->frameless = std::make_unique<native::FramelessWindow> (hostWindow);
            safeThis->repaint();
        }

        if (snapping)
            native::placeInRow (hostWindow, monitor, mode == SnapMode::top);
        else if (const auto target = safeThis->processor.collapsedPosition)
            native::moveWindowIfOnScreen (hostWindow, target->x, target->y);
    });
}

void WrapperEditor::restore()
{
    if (! processor.isCollapsed())
        return;

    auto* window = getHostWindow();
    int x = 0, y = 0;

    // A floating bar remembers where it was left.
    if (processor.collapseMode == SnapMode::off && native::getWindowPosition (window, x, y))
        processor.collapsedPosition = juce::Point<int> (x, y);

    native::removeFromRow (window);
    frameless.reset(); // the frame first, so Live sizes the expanded window with it
    processor.setCollapsed (false);

    if (const auto target = processor.expandedPosition)
        native::moveWindowIfOnScreen (window, target->x, target->y);
}

void WrapperEditor::showOptionsMenu()
{
    juce::PopupMenu menu;
    menu.addItem ("Slim collapsed bar (no title bar)", true, processor.slimCollapsed,
                  [safeThis = SafePointer<WrapperEditor> (this)]
                  {
                      if (safeThis != nullptr)
                          safeThis->processor.slimCollapsed = ! safeThis->processor.slimCollapsed;
                  });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton));
}

bool WrapperEditor::isSlimBar() const
{
    return processor.isPluginLoaded() && processor.isCollapsed() && processor.slimCollapsed;
}

void* WrapperEditor::getHostWindow() const
{
    return titleWatcher != nullptr ? titleWatcher->getWindow() : nullptr;
}

void WrapperEditor::mouseDown (const juce::MouseEvent& e)
{
    // The slim bar has no title bar, so the bar itself moves the window.
    if (frameless != nullptr && e.mods.isLeftButtonDown())
        native::beginWindowDrag (getHostWindow());
}

int WrapperEditor::getCollapsedWidth() const
{
    // Fixed, so the bars line up in the bottom row.
    return collapsedWidth;
}

void WrapperEditor::showError (const juce::String& message)
{
    messageBox = juce::AlertWindow::showScopedAsync (
        juce::MessageBoxOptions::makeOptionsOk (juce::MessageBoxIconType::WarningIcon, "AltraPlugHost", message),
        nullptr);
}

} // namespace altra
