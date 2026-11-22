#pragma once

#include <juce_core/juce_core.h>

namespace altra
{

/** One plug-in a user can pick, read from PlugBrowser's catalog export. */
struct CatalogEntry
{
    juce::File file;              // the .vst3 bundle or file to load
    juce::String name, vendor, category, cid;
    juce::StringArray tags;
    bool isInstrument = false;
    bool kindKnown = false;
    juce::String state, stateDetail; // PlugBrowser's scan result, e.g. "probed", "failed"
    juce::File image, thumbnail;

    /** The plug-in failed or hung when PlugBrowser loaded it. Not always permanent, so we warn. */
    bool hadScanProblem() const     { return state == "failed" || state == "timedOut"; }

    /** Best image for a grid card. */
    juce::File getCardImage() const { return thumbnail != juce::File() ? thumbnail : image; }
};

/** PlugBrowser's catalog export: the stable JSON file PlugBrowser writes after each scan.
    The format is specified in PlugBrowser's docs/catalog-export.md. */
class PluginCatalog
{
public:
    static constexpr int supportedVersion = 1;

    /** %LOCALAPPDATA%\PlugBrowser\export\plugins.json */
    static juce::File getDefaultFile();

    /** Reads the export. Returns nullopt if it's missing, unreadable or a version we don't
        support; we then fall back to listing the VST3 folders ourselves. */
    static std::optional<PluginCatalog> load (const juce::File& file = getDefaultFile());

    const std::vector<CatalogEntry>& getEntries() const noexcept   { return entries; }
    juce::Time getGeneratedTime() const noexcept                   { return generated; }
    juce::String getGenerator() const                              { return generator; }

private:
    std::vector<CatalogEntry> entries;
    juce::Time generated;
    juce::String generator;
};

} // namespace altra
