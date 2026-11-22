#include "MainComponent.h"
#include "dawg/VarUtils.h"

// Update checks (docs/design/update-checks.md): the app fetches the releases list from GitHub on a background thread
// and opens a release page in the browser; the UI decides when to check and what to offer.

namespace dawg
{

namespace
{
constexpr auto releasesUrl = "https://api.github.com/repos/OliverLeenders/DAWg/releases?per_page=30";
constexpr auto releasePages = "https://github.com/OliverLeenders/DAWg/";
constexpr int connectionTimeoutMs = 10000;
} // namespace

void MainComponent::registerUpdateHandlers()
{
    // Starts a fetch; the answer arrives as dawg_updates { status, body, error }. False while one is running.
    m_bridge.on ("updates.fetch",
                 [this] (const juce::var&)
                 {
                     if (m_updateBusy.exchange (true))
                         return juce::var (false);

                     if (m_updateFetch.joinable())
                         m_updateFetch.join(); // the last fetch, which has finished

                     m_updateFetch = std::thread (
                         [this, safe = juce::Component::SafePointer<MainComponent> (this)]
                         {
                             int status = 0;
                             juce::String body;
                             juce::String error;
                             // Only what any request carries, and the User-Agent GitHub requires (UC4).
                             const auto options =
                                 juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                     .withConnectionTimeoutMs (connectionTimeoutMs)
                                     .withExtraHeaders (juce::String ("User-Agent: DAWg/")
                                                        + JUCE_APPLICATION_VERSION_STRING
                                                        + "\r\nAccept: application/vnd.github+json")
                                     .withStatusCode (&status)
                                     .withProgressCallback ([this] (int, int) { return ! m_updateCancel.load(); });

                             if (auto stream = juce::URL (releasesUrl).createInputStream (options))
                                 body = stream->readEntireStreamAsString();
                             else
                                 error = "GitHub could not be reached";

                             juce::MessageManager::callAsync (
                                 [safe, status, body, error]
                                 {
                                     if (safe == nullptr)
                                         return;

                                     safe->m_updateBusy.store (false);
                                     safe->m_bridge.emit (
                                         "dawg_updates",
                                         makeObject ({ { "status", status }, { "body", body }, { "error", error } }));
                                 });
                         });

                     return juce::var (true);
                 });

    // Opens a page of DAWg's repository (a release's page) in the browser; any other address is refused.
    m_bridge.on ("app.openUrl",
                 [] (const juce::var& p)
                 {
                     const auto url = p["url"].toString();

                     if (! url.startsWith (releasePages) || url.containsAnyOf (" \"<>\\"))
                         throw std::runtime_error ("Only DAWg's own pages open from here");

                     return juce::var (juce::URL (url).launchInDefaultBrowser());
                 });
}

void MainComponent::stopUpdateFetch()
{
    m_updateCancel.store (true);

    if (m_updateFetch.joinable())
        m_updateFetch.join();
}

} // namespace dawg
