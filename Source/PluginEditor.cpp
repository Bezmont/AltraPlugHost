#include "PluginEditor.h"
#include "PluginProcessor.h"

namespace altra
{

WrapperEditor::WrapperEditor (WrapperProcessor& p)
    : AudioProcessorEditor (p), processor (p)
{
    titleLabel.setText (JucePlugin_Name, juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    pluginLabel.setFont (juce::FontOptions (15.0f));
    infoLabel.setFont (juce::FontOptions (12.0f));
    infoLabel.setColour (juce::Label::textColourId, juce::Colours::grey);

    for (auto* c : std::initializer_list<juce::Component*> { &titleLabel, &pluginLabel, &infoLabel,
                                                             &loadButton, &unloadButton, &showButton,
                                                             &hideButton, &alwaysOnTopToggle })
        addAndMakeVisible (c);

    loadButton.onClick   = [this] { showLoadMenu(); };
    unloadButton.onClick = [this] { processor.unloadPlugin(); };
    showButton.onClick   = [this] { processor.showHostedEditor (this); };
    hideButton.onClick   = [this] { processor.hideHostedEditor(); };
    alwaysOnTopToggle.onClick = [this] { processor.setHostedWindowAlwaysOnTop (alwaysOnTopToggle.getToggleState()); };

    processor.onStatusChanged = [this] { refresh(); };

    setSize (420, 170);
    refresh();
}

WrapperEditor::~WrapperEditor()
{
    processor.onStatusChanged = nullptr;
}

void WrapperEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff202225));
}

void WrapperEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    titleLabel.setBounds (area.removeFromTop (24));
    pluginLabel.setBounds (area.removeFromTop (24));
    infoLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (8);

    auto row1 = area.removeFromTop (28);
    loadButton.setBounds (row1.removeFromLeft (130));
    row1.removeFromLeft (8);
    unloadButton.setBounds (row1.removeFromLeft (80));

    area.removeFromTop (8);
    auto row2 = area.removeFromTop (28);
    showButton.setBounds (row2.removeFromLeft (110));
    row2.removeFromLeft (8);
    hideButton.setBounds (row2.removeFromLeft (110));
    row2.removeFromLeft (12);
    alwaysOnTopToggle.setBounds (row2);
}

void WrapperEditor::refresh()
{
    const auto loaded = processor.isPluginLoaded();

    if (loaded)
        pluginLabel.setText (processor.getHostedPluginName(), juce::dontSendNotification);
    else if (auto missing = processor.getMissingPluginName(); missing.isNotEmpty())
        pluginLabel.setText ("Missing: " + missing + " (state kept)", juce::dontSendNotification);
    else
        pluginLabel.setText ("No plugin loaded", juce::dontSendNotification);

    juce::String info;

    if (loaded)
    {
        info << "Latency: " << processor.getHostedLatencySamples() << " samples   |   Window: "
             << (! processor.hasHostedEditorBeenBuilt() ? "not built yet"
                                                         : processor.isHostedEditorVisible() ? "shown" : "hidden (alive)");
    }

    infoLabel.setText (info, juce::dontSendNotification);

    unloadButton.setEnabled (loaded);
    showButton.setEnabled (loaded);
    hideButton.setEnabled (loaded && processor.isHostedEditorVisible());
    alwaysOnTopToggle.setToggleState (processor.isHostedWindowAlwaysOnTop(), juce::dontSendNotification);
}

//==============================================================================
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

        // Show the vendor folder for plug-ins that live in one.
        for (int r = 0; r < roots.getNumPaths(); ++r)
            if (file.isAChildOf (roots[r]) && file.getParentDirectory() != roots[r])
                label = file.getParentDirectory().getRelativePathFrom (roots[r]) + " / " + label;

        menu.addItem (100 + i, label);
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (loadButton),
                        [safeThis = SafePointer<WrapperEditor> (this)] (int result)
                        {
                            if (safeThis == nullptr || result == 0)
                                return;

                            if (result == 1)
                                safeThis->browseForPlugin();
                            else if (juce::isPositiveAndBelow (result - 100, safeThis->menuPaths.size()))
                                safeThis->loadFromFile (juce::File (safeThis->menuPaths[result - 100]));
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
                                  if (safeThis != nullptr && chooser.getResult() != juce::File())
                                      safeThis->loadFromFile (chooser.getResult());
                              });
}

void WrapperEditor::loadFromFile (const juce::File& file)
{
    auto types = processor.findPluginsInFile (file);

    if (types.isEmpty())
    {
        showError ("No VST3 plug-in found in " + file.getFullPathName());
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

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (loadButton),
                        [safeThis = SafePointer<WrapperEditor> (this), shared] (int result)
                        {
                            if (safeThis != nullptr && juce::isPositiveAndBelow (result - 1, shared->size()))
                                safeThis->load (*(*shared)[result - 1]);
                        });
}

void WrapperEditor::load (const juce::PluginDescription& description)
{
    const auto error = processor.loadPlugin (description);

    if (error.isNotEmpty())
        showError (error);
}

void WrapperEditor::showError (const juce::String& message)
{
    messageBox = juce::AlertWindow::showScopedAsync (
        juce::MessageBoxOptions::makeOptionsOk (juce::MessageBoxIconType::WarningIcon, "AltraPlugHost", message),
        nullptr);
}

} // namespace altra
