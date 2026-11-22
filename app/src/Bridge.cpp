#include "Bridge.h"

#include "dawg/VarUtils.h"

#include <algorithm>

namespace dawg
{

namespace
{
[[nodiscard]] juce::var success (const juce::var& result)
{
    return makeObject ({ { "ok", true }, { "result", result } });
}

[[nodiscard]] juce::var failure (const juce::String& error)
{
    return makeObject ({ { "ok", false }, { "error", error } });
}

// A value as a JavaScript string literal holding its JSON, to JSON.parse in the page. JUCE's own events escape the JSON
// with String::replace, which copies the whole text for every backslash: a folder's listing, full of Windows paths,
// took 40 seconds and held the message thread all that time. JSON's own string encoding writes it in one pass.
[[nodiscard]] juce::String jsonLiteral (const juce::var& value)
{
    return juce::JSON::toString (juce::var (juce::JSON::toString (value, true)), true);
}
} // namespace

void Bridge::on (const juce::String& method, Handler handler)
{
    m_handlers[method] = std::move (handler);
}

juce::WebBrowserComponent::Options Bridge::install (const juce::WebBrowserComponent::Options& options)
{
    return options.withNativeIntegrationEnabled().withEventListener (
        callEventName, [this] (const juce::var& call)
        { reply (call["id"], dispatch (call["method"].toString(), call.getProperty ("params", juce::var()))); });
}

void Bridge::reply (const juce::var& id, const juce::var& answer)
{
    const auto script =
        "window.__dawgReply&&window.__dawgReply(" + juce::JSON::toString (id, true) + "," + jsonLiteral (answer) + ");";

    for (auto* target : m_targets)
        target->evaluateJavascript (script);
}

void Bridge::attach (juce::WebBrowserComponent* browser)
{
    if (browser != nullptr && std::ranges::find (m_targets, browser) == m_targets.end())
        m_targets.push_back (browser);
}

void Bridge::detach (juce::WebBrowserComponent* browser)
{
    std::erase (m_targets, browser);
}

juce::var Bridge::dispatch (const juce::String& method, const juce::var& params)
{
    const auto it = m_handlers.find (method);

    if (it == m_handlers.end())
        return failure ("Unknown method '" + method + "'");

    // Top-level boundary: no exception may escape into the web view.
    try
    {
        return success (it->second (params));
    }
    catch (const std::exception& e)
    {
        return failure (e.what());
    }
    catch (...)
    {
        return failure ("Internal error in '" + method + "'");
    }
}

void Bridge::emit (const juce::Identifier& event, const juce::var& payload)
{
    // As emitEventIfBrowserIsVisible does, but with the payload encoded in one pass (see jsonLiteral).
    juce::String script;

    for (auto* target : m_targets)
    {
        if (! target->isVisible())
            continue;

        if (script.isEmpty())
            script = "window.__JUCE__.backend.emitByBackend(" + juce::JSON::toString (event.toString(), true) + ","
                     + jsonLiteral (payload) + ");";

        target->evaluateJavascript (script);
    }
}

} // namespace dawg
