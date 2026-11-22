#include "UiResources.h"

#include <map>

namespace dawg
{

namespace
{
[[nodiscard]] juce::File findUiRoot()
{
    const auto exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    const auto nextToExe = exeDir.getChildFile ("ui");                               // Windows / Linux
    const auto inBundle = exeDir.getParentDirectory().getChildFile ("Resources/ui"); // macOS bundle

    for (const auto& candidate : { nextToExe, inBundle })
        if (candidate.getChildFile ("index.html").existsAsFile())
            return candidate;

    return nextToExe;
}

[[nodiscard]] juce::String mimeTypeFor (const juce::String& extension)
{
    static const std::map<juce::String, const char*> types {
        { "html", "text/html" },    { "js", "text/javascript" },    { "mjs", "text/javascript" },
        { "css", "text/css" },      { "json", "application/json" }, { "map", "application/json" },
        { "svg", "image/svg+xml" }, { "png", "image/png" },         { "jpg", "image/jpeg" },
        { "ico", "image/x-icon" },  { "woff", "font/woff" },        { "woff2", "font/woff2" },
        { "ttf", "font/ttf" },      { "wasm", "application/wasm" }, { "txt", "text/plain" },
    };

    const auto it = types.find (extension.toLowerCase());
    return it != types.end() ? it->second : "application/octet-stream";
}
} // namespace

UiResources::UiResources()
    : m_root (findUiRoot())
{
}

bool UiResources::isAvailable() const
{
    return m_root.getChildFile ("index.html").existsAsFile();
}

std::optional<juce::WebBrowserComponent::Resource> UiResources::get (const juce::String& url) const
{
    auto path = url.upToFirstOccurrenceOf ("?", false, false)
                    .upToFirstOccurrenceOf ("#", false, false)
                    .trimCharactersAtStart ("/");

    if (path.isEmpty())
        path = "index.html";

    const auto file = m_root.getChildFile (juce::URL::removeEscapeChars (path));

    // Refuse anything that escapes the UI folder (e.g. "../").
    if (! file.isAChildOf (m_root) || ! file.existsAsFile())
        return std::nullopt;

    juce::MemoryBlock block;

    if (! file.loadFileAsData (block))
        return std::nullopt;

    const auto* bytes = static_cast<const std::byte*> (block.getData());
    return juce::WebBrowserComponent::Resource { { bytes, bytes + block.getSize() },
                                                 mimeTypeFor (file.getFileExtension().trimCharactersAtStart (".")) };
}

} // namespace dawg
