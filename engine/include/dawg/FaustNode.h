#pragma once

#include "dawg/Node.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dawg
{

/**
    The script a new Faust node starts with: a stereo pass-through.
*/
inline constexpr const char* defaultFaustCode = "import(\"stdfaust.lib\");\n\nprocess = _, _;\n";

/**
    A compiled Faust script: the libfaust factory and the node description derived from it (ports, and parameters
    keyed by their Faust path). See docs/design/phase11-faust-panels.md, F2 and F3.

    Programs are immutable and shared by every node running the same code.

    @note  compile() blocks; FaustCompiler runs it on a background thread.
*/
class FaustProgram final
{
public:
    /**
        Compiles a script.

        @param code       The Faust source.
        @param libraries  The directory holding stdfaust.lib and the other Faust libraries.
        @param error      Set to the compiler's messages on failure.
        @return           The program, or nullptr if the script doesn't compile.
    */
    [[nodiscard]] static std::shared_ptr<const FaustProgram> compile (const juce::String& code,
                                                                      const juce::File& libraries, juce::String& error);

    /**
        Releases the factory.
    */
    ~FaustProgram();

    FaustProgram (const FaustProgram&) = delete;
    FaustProgram& operator= (const FaustProgram&) = delete;
    FaustProgram (FaustProgram&&) = delete;
    FaustProgram& operator= (FaustProgram&&) = delete;

    /**
        Returns the node description: type nodetype::faust, one port per channel pair, parameters by path.
    */
    [[nodiscard]] const NodeDescriptor& descriptor() const noexcept;

    /**
        Returns the number of audio channels the DSP reads.
    */
    [[nodiscard]] int numInputs() const noexcept;

    /**
        Returns the number of audio channels the DSP writes.
    */
    [[nodiscard]] int numOutputs() const noexcept;

    /**
        Returns the script's control layout for the device dock (F8): a tree of groups
        ({ kind: "hgroup" | "vgroup" | "tgroup", label, items }) and controls ({ kind: "hslider" | "vslider" | "nentry" |
        "checkbox" | "button" | "hbargraph" | "vbargraph", label, path, style, meter }), where `path` is a parameter id
        and `meter` is a bargraph's index into FaustNode::meterValue().
    */
    [[nodiscard]] const juce::var& layout() const noexcept;

    /**
        Returns the number of bargraphs.
    */
    [[nodiscard]] size_t numMeters() const noexcept;

    /**
        Describes the program for the UI.

        @return  toVar() of the descriptor, plus { channels: { inputs, outputs }, layout }.
    */
    [[nodiscard]] juce::var toVar() const;

    /**
        The private implementation (libfaust types stay out of this header).
    */
    struct Impl;

    /**
        Returns the implementation.
    */
    [[nodiscard]] const Impl& impl() const noexcept { return *m_impl; }

private:
    explicit FaustProgram (std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

/**
    Compiles Faust scripts and caches the results by source text, so undo and redo swap back instantly (F4).

    With background compiling on (the app), request() returns at once and compiles on a worker thread, then calls the
    listener. Off (the default, for tests and offline tools), request() compiles before it returns.

    @note  All functions may be called from any thread except the audio thread.
*/
class FaustCompiler final
{
public:
    /**
        What is known about a script.
    */
    struct Result
    {
        /**
            The compiled program, or nullptr if the script failed or is still compiling.
        */
        std::shared_ptr<const FaustProgram> program;

        /**
            The compiler's messages if the script failed, or an empty string.
        */
        juce::String error;

        /**
            True while the script is waiting to be compiled or compiling.
        */
        bool pending = false;
    };

    /**
        Creates a compiler that looks for the Faust libraries in the build's copy (tests) until
        setLibraryDirectory() is called.
    */
    FaustCompiler();

    /**
        Stops the worker thread.
    */
    ~FaustCompiler();

    FaustCompiler (const FaustCompiler&) = delete;
    FaustCompiler& operator= (const FaustCompiler&) = delete;
    FaustCompiler (FaustCompiler&&) = delete;
    FaustCompiler& operator= (FaustCompiler&&) = delete;

    /**
        Returns the compiler the graph compiler and the commands use.
    */
    [[nodiscard]] static FaustCompiler& shared();

    /**
        Sets where stdfaust.lib and the other libraries are (F5). Forgets failed results, which may have been
        missing a library.

        @param directory  The libraries' directory.
    */
    void setLibraryDirectory (const juce::File& directory);

    /**
        Returns the libraries' directory.
    */
    [[nodiscard]] juce::File libraryDirectory() const;

    /**
        Turns background compiling on or off.

        @param background  True to compile on a worker thread.
    */
    void setBackground (bool background);

    /**
        Sets the function called (on the worker thread) after each background compilation finishes.

        @param listener  The function, or nullptr.
    */
    void setListener (std::function<void()> listener);

    /**
        Returns what is known about a script, starting to compile it if it is new.

        @param code  The Faust source.
        @return      The result; pending while a background compilation runs.
    */
    [[nodiscard]] Result request (const juce::String& code);

    /**
        Returns what is known about a script without compiling it.

        @param code  The Faust source.
        @return      The cached result, or an empty one (no program, no error, not pending) if it is unknown.
    */
    [[nodiscard]] Result find (const juce::String& code) const;

    /**
        Most compiled programs kept in the cache besides those in use.
    */
    static constexpr size_t cacheSize = 32;

private:
    struct Entry
    {
        std::shared_ptr<const FaustProgram> program;
        juce::String error;
        bool pending = false;
        juce::uint64 lastUse = 0;
    };

    void run();
    void store (const std::string& code, std::shared_ptr<const FaustProgram> program, const juce::String& error);
    void evict();

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::map<std::string, Entry> m_entries;
    std::vector<std::string> m_queue;
    juce::File m_libraries;
    std::function<void()> m_listener;
    juce::uint64 m_useCounter = 0;
    bool m_background = false;
    bool m_quit = false;
    std::thread m_thread;
};

/**
    Runs a Faust program (nodetype::faust). Its ports and parameters are the program's; a node whose script has not
    compiled yet passes its stereo input through.

    When a script changes, the graph compiler creates a new node from the new program and hands it the old node: the
    new node crossfades from the old one over 20 ms (F4).

    @note  Construction, prepare(), configure(), releaseFinishedFade() and destruction: message thread. process():
           audio thread.
*/
class FaustNode final : public Node
{
public:
    /**
        Length of the crossfade from a replaced node, in seconds.
    */
    static constexpr double crossfadeSeconds = 0.02;

    /**
        Creates a node.

        @param program      The program to run, or nullptr for an empty pass-through node.
        @param predecessor  The node this one replaces (it fades out), or nullptr.
    */
    FaustNode (std::shared_ptr<const FaustProgram> program, std::shared_ptr<FaustNode> predecessor);

    /**
        Destroys the DSP before its program.
    */
    ~FaustNode() override;

    FaustNode (const FaustNode&) = delete;
    FaustNode& operator= (const FaustNode&) = delete;
    FaustNode (FaustNode&&) = delete;
    FaustNode& operator= (FaustNode&&) = delete;

    /**
        Returns the program, or nullptr for an empty node.
    */
    [[nodiscard]] const std::shared_ptr<const FaustProgram>& program() const noexcept { return m_program; }

    /**
        Returns a bargraph's latest value.

        @param index  The bargraph index (see FaustProgram::layout()).
        @return       Its value, or 0 for an unknown index.
    */
    [[nodiscard]] float meterValue (size_t index) const noexcept;

    /**
        Drops the replaced node once the crossfade has finished, so replaced nodes don't pile up.

        @return  True if a replaced node was dropped.
    */
    bool releaseFinishedFade();

    /**
        Initialises the DSP at the sample rate and allocates its buffers.

        @param spec  The sample rate and maximum block size.
    */
    void prepare (const ProcessSpec& spec) override;

    /**
        Runs one block, fading from the replaced node if there is one.

        @param context  Inputs and outputs.
    */
    void process (const ProcessContext& context) noexcept override;

    /**
        Clears the DSP's state (delay lines, envelopes, oscillator phases), as for a new note in a voice slot
        (docs/design/phase12-voice-scope.md, V9, V15), and ends a crossfade from a replaced node.
    */
    void reset() noexcept override;

    /**
        Applies bypass.

        @param node  The node's model data.
    */
    void configure (const GraphNode& node) override;

private:
    struct Dsp;

    void render (const ProcessContext& context) noexcept;
    void passThrough (const ProcessContext& context) const noexcept;
    void mixFade (const ProcessContext& context) noexcept;

    std::shared_ptr<const FaustProgram> m_program;
    std::unique_ptr<Dsp> m_dsp;
    std::shared_ptr<FaustNode> m_predecessor;
    std::atomic<FaustNode*> m_fadeFrom { nullptr };
    std::atomic<bool> m_bypassed { false };
    ProcessSpec m_spec;
    int m_fadeLength = 0;
    int m_fadePosition = 0;
};

/**
    Description of an empty Faust node (no program): one stereo input and output, no parameters.
*/
[[nodiscard]] const NodeDescriptor& faustDescriptor() noexcept;

/**
    Returns the program a Faust node's commands check its parameters against, if its script has compiled.

    @param code  The node's script.
    @return      The program, or nullptr if the script is unknown, still compiling or failed.
*/
[[nodiscard]] std::shared_ptr<const FaustProgram> compiledFaustProgram (const juce::String& code);

} // namespace dawg
