#include "PluginBrowser.h"

namespace altra
{

//==============================================================================
juce::Image ThumbnailCache::get (const juce::File& file)
{
    const auto key = file.getFullPathName();

    if (const auto it = images.find (key); it != images.end())
        return it->second;

    if (file != juce::File() && requested.insert (key).second)
    {
        queue.push_back (file);

        if (! isTimerRunning())
            startTimer (15);
    }

    return {};
}

void ThumbnailCache::timerCallback()
{
    constexpr double budgetMs = 8.0;
    const auto start = juce::Time::getMillisecondCounterHiRes();
    bool decodedAny = false;

    while (! queue.empty() && juce::Time::getMillisecondCounterHiRes() - start < budgetMs)
    {
        const auto file = queue.back();
        queue.pop_back();

        // A missing or broken file is stored as an invalid image, so it isn't retried.
        images[file.getFullPathName()] = juce::ImageFileFormat::loadFrom (file);
        decodedAny = true;
    }

    if (decodedAny)
        sendChangeMessage();

    if (queue.empty())
        stopTimer();
}

//==============================================================================
namespace
{
    constexpr int cardWidth = 180, imageHeight = 104, textHeight = 38, gap = 10;
    constexpr int cardHeight = imageHeight + textHeight;

    // Tags that duplicate the type filter or describe processing capabilities, not what the plug-in
    // is for. PlugBrowser leaves the same ones out of its category list.
    const juce::StringArray hiddenTags { "Fx", "Instrument", "OnlyRT", "NoOfflineProcess", "OnlyOfflineProcess",
                                         "Mono", "Stereo", "Surround", "Ambisonics" };

    const auto cardColour     = juce::Colour (0xff2b2e33);
    const auto selectedColour = juce::Colour (0xff3d7eff);
    const auto mutedText      = juce::Colour (0xff9aa0a6);
    const auto warningColour  = juce::Colour (0xffffa63d);

    juce::String describeAge (juce::Time generated)
    {
        if (generated.toMilliseconds() == 0)
            return {};

        const auto days = (int) (juce::Time::getCurrentTime() - generated).inDays();

        if (days <= 0)  return "updated today";
        if (days == 1)  return "updated yesterday";
        return "updated " + juce::String (days) + " days ago";
    }
}

//==============================================================================
class PluginBrowser::Grid final : public juce::Component
{
public:
    explicit Grid (PluginBrowser& b) : browser (b) {}

    int getColumns() const          { return std::max (1, (getWidth() - gap) / (cardWidth + gap)); }

    juce::Rectangle<int> getCardBounds (int visibleIndex) const
    {
        const auto columns = getColumns();
        // Spread spare width evenly so the grid doesn't hug the left edge.
        const auto spare = getWidth() - gap - columns * (cardWidth + gap);
        const auto x = gap + spare / 2 + (visibleIndex % columns) * (cardWidth + gap);
        const auto y = gap + (visibleIndex / columns) * (cardHeight + gap);
        return { x, y, cardWidth, cardHeight };
    }

    void updateHeight (int viewportWidth)
    {
        setSize (viewportWidth, 1);
        const auto rows = ((int) browser.visible.size() + getColumns() - 1) / getColumns();
        setSize (viewportWidth, std::max (1, gap + rows * (cardHeight + gap)));
    }

    int getIndexAt (juce::Point<int> p) const
    {
        for (int i = 0; i < (int) browser.visible.size(); ++i)
            if (getCardBounds (i).contains (p))
                return i;

        return -1;
    }

    void paint (juce::Graphics& g) override
    {
        const auto clip = g.getClipBounds();
        const auto& entries = browser.catalog.getEntries();

        for (int i = 0; i < (int) browser.visible.size(); ++i)
        {
            const auto bounds = getCardBounds (i);

            if (! bounds.intersects (clip))
                continue;

            const auto entryIndex = browser.visible[(size_t) i];
            const auto& entry = entries[(size_t) entryIndex];
            const auto isSelected = entryIndex == browser.selected;
            const auto isHovered = i == hovered;

            g.setColour (isSelected ? selectedColour.withAlpha (0.25f) : cardColour.brighter (isHovered ? 0.08f : 0.0f));
            g.fillRoundedRectangle (bounds.toFloat(), 6.0f);

            if (isSelected)
            {
                g.setColour (selectedColour);
                g.drawRoundedRectangle (bounds.toFloat().reduced (1.0f), 6.0f, 2.0f);
            }

            auto imageArea = bounds.withHeight (imageHeight).reduced (6);
            const auto image = browser.thumbnails->get (entry.getCardImage());

            if (image.isValid())
            {
                g.drawImage (image, imageArea.toFloat(), juce::RectanglePlacement::centred);
            }
            else
            {
                g.setColour (juce::Colours::black.withAlpha (0.25f));
                g.fillRoundedRectangle (imageArea.toFloat(), 4.0f);
                g.setColour (mutedText);
                g.setFont (juce::FontOptions (12.0f));
                g.drawText (entry.getCardImage() == juce::File() ? "No screenshot" : "", imageArea,
                            juce::Justification::centred);
            }

            auto text = bounds.withTrimmedTop (imageHeight).reduced (8, 2);
            g.setColour (juce::Colours::white);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (entry.name, text.removeFromTop (17), juce::Justification::centredLeft, true);
            g.setColour (mutedText);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (entry.vendor, text.removeFromTop (15), juce::Justification::centredLeft, true);

            if (entry.hadScanProblem())
            {
                auto badge = juce::Rectangle<int> (bounds.getRight() - 26, bounds.getY() + 8, 18, 18);
                g.setColour (warningColour);
                g.fillEllipse (badge.toFloat());
                g.setColour (juce::Colours::black);
                g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
                g.drawText ("!", badge, juce::Justification::centred);
            }
        }
    }

    void mouseMove (const juce::MouseEvent& e) override     { setHovered (getIndexAt (e.getPosition())); }
    void mouseExit (const juce::MouseEvent&) override       { setHovered (-1); }
    void mouseDown (const juce::MouseEvent& e) override     { browser.select (getIndexAt (e.getPosition())); }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (getIndexAt (e.getPosition()) >= 0)
            browser.loadSelected();
    }

private:
    void setHovered (int index)
    {
        if (index == hovered)
            return;

        if (hovered >= 0)  repaint (getCardBounds (hovered));
        hovered = index;
        if (hovered >= 0)  repaint (getCardBounds (hovered));
    }

    PluginBrowser& browser;
    int hovered = -1;
};

//==============================================================================
PluginBrowser::PluginBrowser (PluginCatalog c, bool preferInstruments)
    : catalog (std::move (c))
{
    grid = std::make_unique<Grid> (*this);
    viewport.setViewedComponent (grid.get(), false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);

    search.setTextToShowWhenEmpty ("Search name, vendor or category", mutedText);
    search.onTextChange = [this] { updateFilter(); };
    search.onEscapeKey = [this] { search.clear(); updateFilter(); };

    typeBox.addItem ("All types", 1);
    typeBox.addItem ("Instruments", 2);
    typeBox.addItem ("Effects", 3);
    typeBox.setSelectedId (preferInstruments ? 2 : 3, juce::dontSendNotification);

    for (const auto& entry : catalog.getEntries())
    {
        if (entry.vendor.isNotEmpty())
            vendors.addIfNotAlreadyThere (entry.vendor, true);

        for (const auto& tag : entry.tags)
            if (! hiddenTags.contains (tag, true))
                categories.addIfNotAlreadyThere (tag, true);
    }

    vendors.sortNatural();
    categories.sortNatural();

    vendorBox.addItem ("All vendors", 1);
    for (int i = 0; i < vendors.size(); ++i)
        vendorBox.addItem (vendors[i], i + 2);
    vendorBox.setSelectedId (1, juce::dontSendNotification);

    categoryBox.addItem ("All categories", 1);
    for (int i = 0; i < categories.size(); ++i)
        categoryBox.addItem (categories[i], i + 2);
    categoryBox.setSelectedId (1, juce::dontSendNotification);

    for (auto* box : { &typeBox, &vendorBox, &categoryBox })
        box->onChange = [this] { updateFilter(); };

    countLabel.setJustificationType (juce::Justification::centredRight);
    countLabel.setColour (juce::Label::textColourId, mutedText);
    detailLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    footerLabel.setColour (juce::Label::textColourId, mutedText);
    footerLabel.setFont (juce::FontOptions (11.0f));

    auto source = "Catalog from " + (catalog.getGenerator().isNotEmpty() ? catalog.getGenerator()
                                                                          : juce::String ("PlugBrowser"));
    if (auto age = describeAge (catalog.getGeneratedTime()); age.isNotEmpty())
        source << ", " << age;

    footerLabel.setText (source + ". Missing a plugin? Rescan in PlugBrowser, or browse for the file.",
                         juce::dontSendNotification);

    loadButton.onClick  = [this] { loadSelected(); };
    closeButton.onClick = [this] { juce::NullCheckedInvocation::invoke (onClose); };
    filesButton.onClick = [this] { juce::NullCheckedInvocation::invoke (onBrowseFiles); };

    for (auto* comp : std::initializer_list<juce::Component*> { &search, &typeBox, &vendorBox, &categoryBox,
                                                                &countLabel, &viewport, &detailLabel,
                                                                &footerLabel, &loadButton, &closeButton,
                                                                &filesButton })
        addAndMakeVisible (comp);

    thumbnails->addChangeListener (this);
    updateFilter();
}

PluginBrowser::~PluginBrowser()
{
    thumbnails->removeChangeListener (this);
}

void PluginBrowser::changeListenerCallback (juce::ChangeBroadcaster*)
{
    grid->repaint();
}

void PluginBrowser::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff202225));
    g.setColour (juce::Colours::black.withAlpha (0.3f));
    g.fillRect (viewport.getBounds());
}

void PluginBrowser::resized()
{
    auto area = getLocalBounds();

    auto top = area.removeFromTop (32).reduced (0, 2);
    search.setBounds (top.removeFromLeft (std::max (160, top.getWidth() / 3)));
    top.removeFromLeft (8);
    typeBox.setBounds (top.removeFromLeft (120));
    top.removeFromLeft (8);
    vendorBox.setBounds (top.removeFromLeft (160));
    top.removeFromLeft (8);
    categoryBox.setBounds (top.removeFromLeft (150));
    countLabel.setBounds (top);

    area.removeFromTop (6);

    auto bottom = area.removeFromBottom (54);
    auto buttons = bottom.removeFromRight (330).withTrimmedTop (4).removeFromTop (28);
    loadButton.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    closeButton.setBounds (buttons.removeFromRight (80));
    buttons.removeFromRight (8);
    filesButton.setBounds (buttons.removeFromRight (130));
    detailLabel.setBounds (bottom.removeFromTop (34));
    footerLabel.setBounds (bottom);

    area.removeFromBottom (4);
    viewport.setBounds (area);
    grid->updateHeight (viewport.getMaximumVisibleWidth());
}

void PluginBrowser::updateFilter()
{
    const auto words = juce::StringArray::fromTokens (search.getText(), true);
    const auto type = typeBox.getSelectedId();
    const auto vendor = vendorBox.getSelectedId() > 1 ? vendors[vendorBox.getSelectedId() - 2] : juce::String();
    const auto category = categoryBox.getSelectedId() > 1 ? categories[categoryBox.getSelectedId() - 2] : juce::String();

    visible.clear();
    const auto& entries = catalog.getEntries();

    for (int i = 0; i < (int) entries.size(); ++i)
    {
        const auto& e = entries[(size_t) i];

        // A plug-in of unknown type shows under both, since we can't tell which it is.
        if (e.kindKnown && ((type == 2 && ! e.isInstrument) || (type == 3 && e.isInstrument)))
            continue;

        if (vendor.isNotEmpty() && ! e.vendor.equalsIgnoreCase (vendor))
            continue;

        if (category.isNotEmpty() && ! e.tags.contains (category, true))
            continue;

        const auto haystack = e.name + " " + e.vendor + " " + e.category;
        const auto matchesAllWords = std::all_of (words.begin(), words.end(), [&] (const auto& word)
        {
            return haystack.containsIgnoreCase (word);
        });

        if (matchesAllWords)
            visible.push_back (i);
    }

    countLabel.setText (juce::String ((int) visible.size()) + " of " + juce::String ((int) entries.size()) + " plugins",
                        juce::dontSendNotification);

    grid->updateHeight (viewport.getMaximumVisibleWidth());
    viewport.setViewPosition (0, 0);
    grid->repaint();
    updateDetails();
}

void PluginBrowser::select (int visibleIndex)
{
    if (! juce::isPositiveAndBelow (visibleIndex, (int) visible.size()))
        return;

    selected = visible[(size_t) visibleIndex];
    grid->repaint();
    updateDetails();
}

const CatalogEntry* PluginBrowser::getSelected() const
{
    if (std::find (visible.begin(), visible.end(), selected) == visible.end())
        return nullptr;

    return &catalog.getEntries()[(size_t) selected];
}

void PluginBrowser::updateDetails()
{
    const auto* entry = getSelected();
    loadButton.setEnabled (entry != nullptr);

    if (entry == nullptr)
    {
        detailLabel.setText ("Pick a plugin, then Load (or double-click it).", juce::dontSendNotification);
        detailLabel.setColour (juce::Label::textColourId, mutedText);
        return;
    }

    juce::String text = entry->name;

    if (entry->vendor.isNotEmpty())
        text << "  -  " << entry->vendor;

    if (entry->category.isNotEmpty())
        text << "  -  " << entry->category.replace ("|", ", ");

    if (entry->hadScanProblem())
    {
        text << "\nPlugBrowser couldn't load this when it scanned"
             << (entry->stateDetail.isNotEmpty() ? " (" + entry->stateDetail + ")" : juce::String())
             << ". It may still load.";
        detailLabel.setColour (juce::Label::textColourId, warningColour);
    }
    else
    {
        detailLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    }

    detailLabel.setText (text, juce::dontSendNotification);
}

void PluginBrowser::loadSelected()
{
    if (const auto* entry = getSelected())
        juce::NullCheckedInvocation::invoke (onLoad, *entry);
}

} // namespace altra
