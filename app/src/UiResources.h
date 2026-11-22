#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <optional>

namespace dawg
{

/**
    Serves the built web UI (ui/dist, copied next to the executable) to the WebBrowserComponent.

    @note  get() is called by the browser backend; it only reads files and is safe on any thread.
*/
class UiResources final
{
public:
    /**
        Locates the UI folder next to the executable (or in the macOS bundle's Resources).
    */
    UiResources();

    /**
        Looks up a resource requested by the web view.

        @param url  The requested path, relative to the resource provider root, e.g. "/assets/index.js".
        @return     The file contents and MIME type, or std::nullopt if there is no such file inside the UI
                    folder.
    */
    [[nodiscard]] std::optional<juce::WebBrowserComponent::Resource> get (const juce::String& url) const;

    /**
        Returns true if the UI folder contains an index.html.
    */
    [[nodiscard]] bool isAvailable() const;

    /**
        Returns the folder the UI is served from.
    */
    [[nodiscard]] const juce::File& getRoot() const noexcept { return m_root; }

private:
    juce::File m_root;
};

} // namespace dawg
