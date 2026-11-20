#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace altra
{

class WrapperProcessor;

/** AltraPlugHost's own small control panel. Live creates and destroys it freely; nothing
    important lives here. */
class WrapperEditor final : public juce::AudioProcessorEditor
{
public:
    explicit WrapperEditor (WrapperProcessor&);
    ~WrapperEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void refresh();
    void showLoadMenu();
    void browseForPlugin();
    void loadFromFile (const juce::File&);
    void load (const juce::PluginDescription&);
    void showError (const juce::String& message);

    WrapperProcessor& processor;

    juce::Label titleLabel, pluginLabel, infoLabel;
    juce::TextButton loadButton { "Load plugin..." }, unloadButton { "Unload" };
    juce::TextButton showButton { "Show window" }, hideButton { "Hide window" };
    juce::ToggleButton alwaysOnTopToggle { "Always on top" };

    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::ScopedMessageBox messageBox;
    juce::StringArray menuPaths;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WrapperEditor)
};

} // namespace altra
