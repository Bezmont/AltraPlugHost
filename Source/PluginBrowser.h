#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginCatalog.h"

namespace altra
{

/** Decodes card images on demand, a few per timer tick on the message thread.

    JUCE 9 images may be GPU-backed on Windows, so decoding stays on the message thread, in small
    slices to keep Live responsive. Shared through SharedResourcePointer, so it is freed when the
    last browser closes and costs nothing while no browser is open. */
class ThumbnailCache final : public juce::ChangeBroadcaster,
                             private juce::Timer
{
public:
    /** The decoded image, or an invalid one while it is still queued (or can't be read). */
    juce::Image get (const juce::File&);

private:
    void timerCallback() override;

    std::map<juce::String, juce::Image> images;
    std::set<juce::String> requested;
    std::vector<juce::File> queue; // processed newest first, so what is on screen now comes first
};

/** Screenshot grid for picking a plug-in, with search and basic filters. */
class PluginBrowser final : public juce::Component,
                            private juce::ChangeListener
{
public:
    PluginBrowser (PluginCatalog, bool preferInstruments);
    ~PluginBrowser() override;

    std::function<void (const CatalogEntry&)> onLoad;
    std::function<void()> onClose, onBrowseFiles;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Grid;
    friend class Grid;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void updateFilter();
    void select (int visibleIndex);
    void loadSelected();
    void updateDetails();
    const CatalogEntry* getSelected() const;

    PluginCatalog catalog;
    std::vector<int> visible; // indices into catalog.getEntries() that pass the filters
    int selected = -1;        // index into catalog.getEntries()

    juce::SharedResourcePointer<ThumbnailCache> thumbnails;

    juce::TextEditor search;
    juce::ComboBox typeBox, vendorBox, categoryBox;
    juce::StringArray vendors, categories;
    juce::Label countLabel, detailLabel, footerLabel;
    juce::TextButton loadButton { "Load" }, closeButton { "Close" }, filesButton { "Browse files..." };
    juce::Viewport viewport;
    std::unique_ptr<Grid> grid;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginBrowser)
};

} // namespace altra
