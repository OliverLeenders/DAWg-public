#pragma once

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <functional>
#include <optional>
#include <utility>

namespace dawg
{

/**
    What the app's editor window needs from a CLAP plugin (docs/design/clap-hosting.md, section 3.3). Sizes are the
    plugin's: physical pixels on Windows.

    @note  Message thread.
*/
class ClapEditorAccess
{
public:
    /**
        Destroys the interface.
    */
    virtual ~ClapEditorAccess() = default;

    /**
        Returns whether the plugin has an editor that embeds in a native window of this platform.
    */
    [[nodiscard]] virtual bool hasEmbeddedEditor() const = 0;

    /**
        Creates the editor, which then knows its size.

        @param scale  The display's scale.
        @return       False if it couldn't.
    */
    [[nodiscard]] virtual bool createEditor (double scale) = 0;

    /**
        Puts the editor in a window that is on screen, and shows it.

        @param parent  The native window.
        @return        False if it couldn't.
    */
    [[nodiscard]] virtual bool attachEditor (void* parent) = 0;

    /**
        Destroys the editor.
    */
    virtual void destroyEditor() = 0;

    /**
        Returns the editor's size.
    */
    [[nodiscard]] virtual std::pair<uint32_t, uint32_t> editorSize() const = 0;

    /**
        Returns whether the editor can be resized.
    */
    [[nodiscard]] virtual bool editorCanResize() const = 0;

    /**
        Asks the editor to take a size, as near as it can.

        @param width   The width.
        @param height  The height.
    */
    virtual void setEditorSize (uint32_t width, uint32_t height) = 0;

    /**
        Sets who hears when the plugin asks to resize its editor.

        @param callback  Called with the size it wants, or nullptr.
    */
    virtual void setEditorResizeCallback (std::function<void (uint32_t, uint32_t)> callback) = 0;
};

/**
    A plugin instance whose parameter list can change: CLAP's `params.rescan` with `CLAP_PARAM_RESCAN_ALL`
    (docs/design/clap-hosting.md, section 3.6). Until the instance is first prepared it reads its list again itself, as
    a state set then can bring other parameters. After that JUCE fixes a processor's parameter list and the compiled
    engine holds indices into it, so the host answers with a new instance, made from this one's state, which lists the
    new parameters. Automation of a parameter that went stays in the model, unused.
*/
class ParameterListSource
{
public:
    /**
        Destroys the interface.
    */
    virtual ~ParameterListSource() = default;

    /**
        Sets what is called when the plugin's parameters were added to or removed after it was prepared.

        @param callback  Called on any thread, from inside the instance: it mustn't destroy it. Or nullptr.
    */
    virtual void setParameterListCallback (std::function<void()> callback) = 0;

protected:
    /** @cond */
    ParameterListSource() = default;
    ParameterListSource (const ParameterListSource&) = default;
    ParameterListSource& operator= (const ParameterListSource&) = default;
    ParameterListSource (ParameterListSource&&) = default;
    ParameterListSource& operator= (ParameterListSource&&) = default;
    /** @endcond */
};

/**
    A plugin instance that saves differently for a copy of itself: CLAP's `state-context` with
    `CLAP_STATE_CONTEXT_FOR_DUPLICATE` (docs/design/clap-hosting.md, section 3.5), so that a duplicated node or track can,
    for example, get new ids where the plugin keeps some.
*/
class DuplicateStateSource
{
public:
    /**
        Destroys the interface.
    */
    virtual ~DuplicateStateSource() = default;

    /**
        Saves the state a copy of this instance starts from.

        @param destData  Receives the state; the project state where the plugin has no context for copies.
    */
    virtual void getStateForDuplicate (juce::MemoryBlock& destData) = 0;

protected:
    /** @cond */
    DuplicateStateSource() = default;
    DuplicateStateSource (const DuplicateStateSource&) = default;
    DuplicateStateSource& operator= (const DuplicateStateSource&) = default;
    DuplicateStateSource (DuplicateStateSource&&) = default;
    DuplicateStateSource& operator= (DuplicateStateSource&&) = default;
    /** @endcond */
};

/**
    Hears from a CLAP plugin that a voice has finished.
*/
class ClapNoteEndListener
{
public:
    /**
        Destroys the interface.
    */
    virtual ~ClapNoteEndListener() = default;

    /**
        A voice has finished its release.

        @param noteId  The note id it was started with.
        @note  Audio thread, while the plugin processes.
    */
    virtual void noteEnded (int32_t noteId) noexcept = 0;
};

/**
    What a CLAP plugin takes directly, beside a JUCE plugin's MidiBuffer (docs/design/clap-hosting.md, C2, section 5).
    What goes in here plays in the plugin's next block.

    @note  Audio thread, on the thread that runs the plugin, except where noted.
*/
class ClapNativeInput
{
public:
    /**
        The per-note values a note can be sent.
    */
    enum class Expression
    {
        tuning,     ///< Semitones relative to the key.
        pressure,   ///< 0..1.
        brightness, ///< 0..1.
    };

    /**
        Destroys the interface.
    */
    virtual ~ClapNativeInput() = default;

    /**
        Returns whether the plugin takes CLAP's own notes (C7).

        @note  Any thread.
    */
    [[nodiscard]] virtual bool takesNativeNotes() const noexcept = 0;

    /**
        Returns whether the plugin plays two notes on one key at once.

        @note  Any thread.
    */
    [[nodiscard]] virtual bool playsOverlappingNotes() const noexcept = 0;

    /**
        Starts a note.

        @param time      The sample in the block.
        @param noteId    The note's id.
        @param key       Its key.
        @param velocity  Its velocity, 0..1.
    */
    virtual void noteOn (uint32_t time, int32_t noteId, int key, double velocity) noexcept = 0;

    /**
        Releases a note.

        @param time      The sample in the block.
        @param noteId    The note's id.
        @param key       Its key.
        @param velocity  Its release velocity, 0..1.
    */
    virtual void noteOff (uint32_t time, int32_t noteId, int key, double velocity) noexcept = 0;

    /**
        Silences a note at once.

        @param time    The sample in the block.
        @param noteId  The note's id.
        @param key     Its key.
    */
    virtual void noteChoke (uint32_t time, int32_t noteId, int key) noexcept = 0;

    /**
        Silences every note at once, release tails too.

        @param time  The sample in the block.
    */
    virtual void chokeAll (uint32_t time) noexcept = 0;

    /**
        Sends a note a new value.

        @param time    The sample in the block.
        @param noteId  The note's id.
        @param key     Its key.
        @param which   Which value.
        @param value   The value.
    */
    virtual void noteExpression (uint32_t time, int32_t noteId, int key, Expression which, double value) noexcept = 0;

    /**
        Sends a controller as MIDI, if the plugin takes MIDI too.

        @param time    The sample in the block.
        @param status  The status byte.
        @param first   The first data byte.
        @param second  The second data byte.
    */
    virtual void controller (uint32_t time, uint8_t status, uint8_t first, uint8_t second) noexcept = 0;

    /**
        Returns whether a parameter takes modulation apart from its value (C9).

        @param parameter  The parameter's index.
        @note  Any thread.
    */
    [[nodiscard]] virtual bool isModulatable (int parameter) const noexcept = 0;

    /**
        Sets how far modulation moves a parameter from its value, until changed. The value stays the plugin's and
        automation's.

        @param parameter  The parameter's index.
        @param amount     The amount, in units of the parameter's whole range.
        @param time       The sample in the block.
    */
    virtual void modulateParameter (int parameter, double amount, uint32_t time) noexcept = 0;

    /**
        Sets a parameter from automation at a sample of the next block (C4).

        @param parameter  The parameter's index.
        @param value      The normalised value.
        @param time       The sample in the block.
    */
    virtual void automateParameter (int parameter, float value, uint32_t time) noexcept = 0;

    /**
        Returns whether a parameter takes modulation per note (C10).

        @param parameter  The parameter's index.
        @note  Any thread.
    */
    [[nodiscard]] virtual bool isModulatablePerNote (int parameter) const noexcept = 0;

    /**
        Sets how far modulation moves a parameter for one note. It replaces what modulateParameter() sets, for that note.

        @param time       The sample in the block.
        @param noteId     The note's id.
        @param key        Its key.
        @param parameter  The parameter's index.
        @param amount     The amount, in units of the parameter's whole range.
    */
    virtual void modulateNoteParameter (uint32_t time, int32_t noteId, int key, int parameter,
                                        double amount) noexcept = 0;

    /**
        Sets who hears when a voice has finished.

        @param listener  The listener, or nullptr. Must outlive the plugin or be taken away first.
        @note  Message thread, while the plugin isn't processing.
    */
    virtual void setNoteEndListener (ClapNoteEndListener* listener) noexcept = 0;
};

/**
    Finds a CLAP plugin by the identifier a project saved, wherever its file is now (docs/design/clap-hosting.md, section 8):
    an identifier names the file, but a CLAP plugin is its id, which the identifier's last part holds.

    @param known       The scanned plugins.
    @param identifier  The saved identifier.
    @return            The plugin, if one with that id is installed.
*/
[[nodiscard]] std::optional<juce::PluginDescription>
findMovedClapPlugin (const juce::Array<juce::PluginDescription>& known, const juce::String& identifier);

/**
    Hosts CLAP plugins as a JUCE plugin format (docs/design/clap-hosting.md, C1, C2), so the scanner, the state blob,
    editor windows, latency changes and parameter lists work as for VST3. Plugins are found by their factory's
    descriptors without being instantiated; a description keeps the CLAP plugin id in its descriptiveName.

    @note  Message thread, or the scanner's child process.
*/
class ClapPluginFormat final : public juce::AudioPluginFormat
{
public:
    /**
        The format's name.
    */
    static constexpr const char* formatName = "CLAP";

    /**
        Returns "CLAP".
    */
    [[nodiscard]] juce::String getName() const override { return formatName; }

    /**
        Lists the plugins a .clap file holds.

        @param results           Receives their descriptions.
        @param fileOrIdentifier  The file.
    */
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results,
                              const juce::String& fileOrIdentifier) override;

    /**
        Returns whether a file is a .clap.

        @param fileOrIdentifier  The file.
    */
    [[nodiscard]] bool fileMightContainThisPluginType (const juce::String& fileOrIdentifier) override;

    /**
        Returns the file's name.

        @param fileOrIdentifier  The file.
    */
    [[nodiscard]] juce::String getNameOfPluginFromIdentifier (const juce::String& fileOrIdentifier) override;

    /**
        Returns whether a plugin's file changed since it was scanned.

        @param description  The plugin.
    */
    [[nodiscard]] bool pluginNeedsRescanning (const juce::PluginDescription& description) override;

    /**
        Returns whether a plugin's file is still there.

        @param description  The plugin.
    */
    [[nodiscard]] bool doesPluginStillExist (const juce::PluginDescription& description) override;

    /**
        CLAP plugins can be scanned.
    */
    [[nodiscard]] bool canScanForPlugins() const override { return true; }

    /**
        Scanning runs a plugin's library, so it's left to a child process.
    */
    [[nodiscard]] bool isTrivialToScan() const override { return false; }

    /**
        Finds the .clap files in some folders.

        @param directoriesToSearch  The folders.
        @param recursive            Whether to look in their subfolders.
        @param allowAsync           Unused.
        @return                     The files.
    */
    [[nodiscard]] juce::StringArray searchPathsForPlugins (const juce::FileSearchPath& directoriesToSearch,
                                                           bool recursive, bool allowAsync = false) override;

    /**
        Returns the folders the CLAP specification names for this platform, and CLAP_PATH.
    */
    [[nodiscard]] juce::FileSearchPath getDefaultLocationsToSearch() override;

    /**
        Instances are created on the message thread without waiting for it.
    */
    [[nodiscard]] bool
    requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription& /*description*/) const override
    {
        return false;
    }

private:
    void createPluginInstance (const juce::PluginDescription& description, double initialSampleRate,
                               int initialBufferSize, PluginCreationCallback callback) override;
};

} // namespace dawg
