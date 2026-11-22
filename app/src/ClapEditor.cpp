#include "ClapEditor.h"

#include "dawg/ClapPlugin.h"

namespace dawg
{

#if JUCE_WINDOWS || JUCE_LINUX || JUCE_BSD

namespace
{
 #if JUCE_WINDOWS
// A window of our own for the plugin to draw into, as JUCE's VST3 hosting does.
struct PluginView final : public juce::HWNDComponent
{
    PluginView()
    {
        setOpaque (true);
        inner.setOpaque (true); // else its window is layered, and Windows would not show the plugin inside
        inner.addToDesktop (0);

        if (auto* peer = inner.getPeer())
            setHWND (peer->getNativeHandle());
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }

    // The inner window follows this component once that's on screen.
    void addToPeerIfNeeded() { updateHWNDBounds(); }

    // The window the plugin's editor goes into.
    [[nodiscard]] void* handle() const { return getHWND(); }

    struct Inner final : public juce::Component
    {
        void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }
    } inner;
};
 #else
// An X11 window the plugin's editor goes into, through XEmbed, as JUCE's VST3 hosting does on Linux.
struct PluginView final : public juce::XEmbedComponent
{
    PluginView()
        : juce::XEmbedComponent (juce::XEmbedComponentOptions {}
                                     .withWantsKeyboardFocus (true)
                                     .withAllowForeignWidgetToResizeComponent (false)
                                     .withIgnoreXembedMapped())
    {
    }

    ~PluginView() override { removeClient(); }

    PluginView (const PluginView&) = delete;
    PluginView& operator= (const PluginView&) = delete;
    PluginView (PluginView&&) = delete;
    PluginView& operator= (PluginView&&) = delete;

    void addToPeerIfNeeded() {}

    // The window the plugin's editor goes into.
    [[nodiscard]] void* handle() { return reinterpret_cast<void*> (static_cast<uintptr_t> (getHostWindowID())); }
};
 #endif

class ClapEditor final : public juce::AudioProcessorEditor
{
public:
    ClapEditor (juce::AudioProcessor& plugin, ClapEditorAccess& access)
        : juce::AudioProcessorEditor (plugin),
          m_access (access)
    {
        // CLAP sizes are physical pixels on Windows and X11; JUCE's are logical.
        if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            m_scale = display->scale;

        addAndMakeVisible (m_view);
        setOpaque (true);
        m_created = m_access.createEditor (m_scale);

        if (! m_created)
            return;

        m_access.setEditorResizeCallback (
            [safe = juce::Component::SafePointer<ClapEditor> (this)] (uint32_t width, uint32_t height)
            {
                if (safe != nullptr)
                    safe->resizeTo (width, height);
            });

        const auto size = m_access.editorSize();
        setResizable (m_access.editorCanResize(), false);
        resizeTo (size.first, size.second);
    }

    ~ClapEditor() override
    {
        m_access.setEditorResizeCallback (nullptr);

        if (m_created)
            m_access.destroyEditor();
    }

    ClapEditor (const ClapEditor&) = delete;
    ClapEditor& operator= (const ClapEditor&) = delete;
    ClapEditor (ClapEditor&&) = delete;
    ClapEditor& operator= (ClapEditor&&) = delete;

    [[nodiscard]] bool created() const noexcept { return m_created; }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }

    // The plugin goes in once its window is on screen, as JUCE's VST3 hosting does.
    void parentHierarchyChanged() override { attachWhenShowing(); }

    void visibilityChanged() override { attachWhenShowing(); }

    void resized() override
    {
        m_view.setBounds (getLocalBounds());

        if (m_created && ! m_applyingPluginSize)
            m_access.setEditorSize (static_cast<uint32_t> (std::lround (getWidth() * m_scale)),
                                    static_cast<uint32_t> (std::lround (getHeight() * m_scale)));
    }

private:
    void attachWhenShowing()
    {
        if (! m_created || m_attached || ! isShowing() || getPeer() == nullptr)
            return;

        m_view.addToPeerIfNeeded();
        m_attached = m_access.attachEditor (m_view.handle());

        // Some plugins lay themselves out only when told their size in their new window.
        if (m_attached)
        {
            const auto size = m_access.editorSize();
            m_access.setEditorSize (size.first, size.second);
        }
    }

    void resizeTo (uint32_t width, uint32_t height)
    {
        const juce::ScopedValueSetter<bool> applying (m_applyingPluginSize, true);
        setSize (std::max (1, static_cast<int> (std::lround (width / m_scale))),
                 std::max (1, static_cast<int> (std::lround (height / m_scale))));
    }

    ClapEditorAccess& m_access;
    PluginView m_view;
    double m_scale = 1.0;
    bool m_created = false;
    bool m_attached = false;
    bool m_applyingPluginSize = false;
};
} // namespace

std::unique_ptr<juce::AudioProcessorEditor> createClapEditor (juce::AudioProcessor& processor)
{
    auto* access = dynamic_cast<ClapEditorAccess*> (&processor);

    if (access == nullptr || ! access->hasEmbeddedEditor())
        return nullptr;

    auto editor = std::make_unique<ClapEditor> (processor, *access);
    return editor->created() ? std::unique_ptr<juce::AudioProcessorEditor> (std::move (editor)) : nullptr;
}

#else

// Embedding on macOS isn't built yet: its CLAP plugins get the generic editor.
std::unique_ptr<juce::AudioProcessorEditor> createClapEditor (juce::AudioProcessor& /*processor*/)
{
    return nullptr;
}

#endif

} // namespace dawg
