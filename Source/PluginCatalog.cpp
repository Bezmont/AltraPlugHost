#include "PluginCatalog.h"

namespace altra
{

juce::File PluginCatalog::getDefaultFile()
{
    return juce::File::getSpecialLocation (juce::File::windowsLocalAppData)
               .getChildFile ("PlugBrowser").getChildFile ("export").getChildFile ("plugins.json");
}

namespace
{
    juce::String getString (const juce::var& object, const char* key)
    {
        const auto& value = object[key];
        return value.isString() ? value.toString() : juce::String();
    }

    juce::File getFile (const juce::var& object, const char* key)
    {
        const auto path = getString (object, key);
        return juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
    }

    bool isOurOwnPlugin (const juce::File& file)
    {
        // Loading AltraPlugHost inside itself is never useful.
        return file.getFileNameWithoutExtension().startsWithIgnoreCase ("AltraPlugHost");
    }
}

std::optional<PluginCatalog> PluginCatalog::load (const juce::File& file)
{
    if (! file.existsAsFile())
        return std::nullopt;

    const auto root = juce::JSON::parse (file);

    // A file that is half-written, from someone else, or from a newer incompatible format: ignore it.
    if (getString (root, "format") != "PlugBrowser.CatalogExport"
        || (int) root.getProperty ("version", 0) != supportedVersion)
        return std::nullopt;

    const auto* plugins = root["plugins"].getArray();

    if (plugins == nullptr)
        return std::nullopt;

    PluginCatalog catalog;
    catalog.generator = getString (root, "generator");
    catalog.generated = juce::Time::fromISO8601 (getString (root, "generatedUtc"));

    for (const auto& plugin : *plugins)
    {
        const auto state = getString (plugin, "state");
        const auto architecture = getString (plugin, "architecture");
        const auto pluginFile = getFile (plugin, "path");

        // We host only 64-bit VST3, and the user asked never to load blacklisted plug-ins.
        if (getString (plugin, "format") != "vst3"
            || (architecture != "x64" && architecture != "unknown")
            || state == "blacklisted" || state == "unsupported"
            || pluginFile == juce::File() || isOurOwnPlugin (pluginFile))
            continue;

        CatalogEntry base;
        base.file = pluginFile;
        base.state = state;
        base.stateDetail = getString (plugin, "stateDetail");
        base.name = getString (plugin, "displayName");
        base.vendor = getString (plugin, "displayVendor");

        const auto* classes = plugin["classes"].getArray();

        // Never loaded by PlugBrowser: we know only the file, but it can still be picked.
        if (classes == nullptr || classes->isEmpty())
        {
            if (base.name.isEmpty())
                base.name = pluginFile.getFileNameWithoutExtension();

            catalog.entries.push_back (base);
            continue;
        }

        for (const auto& pluginClass : *classes)
        {
            auto entry = base;
            entry.name = getString (pluginClass, "name");
            entry.cid = getString (pluginClass, "cid");
            entry.category = getString (pluginClass, "category");

            if (auto vendor = getString (pluginClass, "vendor"); vendor.isNotEmpty())
                entry.vendor = vendor;

            if (const auto* tags = pluginClass["tags"].getArray())
                for (const auto& tag : *tags)
                    if (tag.isString())
                        entry.tags.add (tag.toString());

            const auto kind = getString (pluginClass, "kind");
            entry.kindKnown = kind == "instrument" || kind == "effect";
            entry.isInstrument = kind == "instrument";

            if (const auto& image = pluginClass["image"]; image.isObject())
            {
                entry.image = getFile (image, "path");
                entry.thumbnail = getFile (image, "thumbnailPath");
            }

            if (entry.name.isEmpty())
                entry.name = base.name;

            catalog.entries.push_back (std::move (entry));
        }
    }

    std::sort (catalog.entries.begin(), catalog.entries.end(), [] (const auto& a, const auto& b)
    {
        return a.name.compareNatural (b.name) < 0;
    });

    return catalog;
}

} // namespace altra
