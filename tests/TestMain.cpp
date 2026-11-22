#include "dawg/ClapPlugin.h"
#include "dawg/RemotePlugin.h"

#include <catch2/catch_session.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>

// The tests' entry point. With a helper's command line the executable is a plugin helper instead, so tests of plugins in
// other processes (docs/design/out-of-process-plugins.md) needn't build a second program holding the same code.

namespace
{
// Tests shouldn't open windows on the screen. This one shows in its position what was asked of it, and a title of
// "close me" plays the user closing it.
class PretendEditorWindow final : public dawg::RemoteEditorWindow
{
public:
    PretendEditorWindow (const juce::String& title, std::optional<dawg::RemoteEditorPosition> position,
                         std::function<void()> onUserClose)
        : m_position (position.value_or (dawg::RemoteEditorPosition { 1, 2 }))
    {
        if (title == "close me")
            juce::MessageManager::callAsync (std::move (onUserClose));
    }

    void toFront() override { ++m_position.x; }

    void setShown (bool onTop, bool visible) override { m_position = { onTop ? 100 : 200, visible ? 10 : 20 }; }

    [[nodiscard]] dawg::RemoteEditorPosition position() const override { return m_position; }

private:
    dawg::RemoteEditorPosition m_position;
};
} // namespace

int main (int argc, char* argv[])
{
    juce::String commandLine;

    for (int i = 1; i < argc; ++i)
        commandLine << argv[i] << " ";

    if (commandLine.contains (juce::String (dawg::remotePluginHelperId) + ":"))
    {
        const juce::ScopedJuceInitialiser_GUI gui;
        juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread();
        auto helper = dawg::startRemotePluginHelper (
            commandLine.trim(), [] (juce::AudioPluginFormatManager& formats)
            { formats.addFormat (std::make_unique<dawg::ClapPluginFormat>()); },
            [] { juce::MessageManager::getInstance()->stopDispatchLoop(); },
            [] (juce::AudioPluginInstance&, const juce::String& title,
                std::optional<dawg::RemoteEditorPosition> position, bool,
                std::function<void()> onUserClose) -> std::unique_ptr<dawg::RemoteEditorWindow>
            { return std::make_unique<PretendEditorWindow> (title, position, std::move (onUserClose)); });

        if (helper == nullptr)
            return 1;

        juce::MessageManager::getInstance()->runDispatchLoop();
        return 0;
    }

    const auto result = Catch::Session().run (argc, argv);

#ifdef __APPLE__
    // On macOS, Faust's LLVM backend aborts while the process's statics are destroyed ("recursive_mutex lock failed")
    // after every test that compiled a script has passed. The results are written; the statics are skipped.
    std::fflush (nullptr);
    std::_Exit (result);
#else
    return result;
#endif
}
