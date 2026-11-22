#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <map>
#include <vector>

namespace dawg
{

/**
    Routes calls from the web UI to C++ handlers and pushes events back.

    - JS side: `await call(method, params)` (ui/src/bridge/bridge.ts) sends a dawg_call event and resolves to the
      answer, { ok: true, result } or { ok: false, error }.
    - C++ side: `bridge.on ("model.apply", handler)`.
    - Events: `bridge.emit ("dawg_patch", payload)` reaches window.__JUCE__.backend listeners in every attached
      web view (the main window and the mixer window, docs/design/mixer.md, X11).

    A single native function keeps the protocol in one place and makes the transport replaceable later
    (e.g. a WebSocket for an out-of-process UI).

    @note  Message thread only. Handlers run on the message thread.
*/
class Bridge final
{
public:
    /**
        A method handler.

        Throw std::runtime_error (or a subclass such as CommandError) to report a failure to the caller;
        the bridge catches it and returns { ok: false, error }.
    */
    using Handler = std::function<juce::var (const juce::var& params)>;

    /**
        The event a web view sends a call as: { id, method, params }. The answer comes back to the web views as
        window.__dawgReply (id, json), which only the one waiting for that id reads.
    */
    static constexpr const char* callEventName = "dawg_call";

    /**
        Registers (or replaces) the handler for a method.

        @param method   The method name, e.g. "model.apply".
        @param handler  The handler.
    */
    void on (const juce::String& method, Handler handler);

    /**
        Adds the bridge's call event to the browser options.

        @param options  The options to extend.
        @return         The options with native integration enabled and the call event listened for.
        @note  The bridge must outlive the WebBrowserComponent created from the returned options.
    */
    [[nodiscard]] juce::WebBrowserComponent::Options install (const juce::WebBrowserComponent::Options& options);

    /**
        Adds a browser that emit() sends events to. Calls are answered in the browser that made them.

        @param browser  The browser, created from options returned by install().
    */
    void attach (juce::WebBrowserComponent* browser);

    /**
        Stops sending events to a browser. Call before it is destroyed.

        @param browser  The browser.
    */
    void detach (juce::WebBrowserComponent* browser);

    /**
        Sends an event to every attached browser that is visible.

        @param event    The event name, e.g. "dawg_patch".
        @param payload  The event payload.
    */
    void emit (const juce::Identifier& event, const juce::var& payload);

private:
    [[nodiscard]] juce::var dispatch (const juce::String& method, const juce::var& params);

    // Answers a call in every attached web view; the one that made it reads the answer.
    void reply (const juce::var& id, const juce::var& answer);

    std::map<juce::String, Handler> m_handlers;
    std::vector<juce::WebBrowserComponent*> m_targets;
};

} // namespace dawg
