#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// In DAWg: the clean-room workspace's copy of the node API, as the reverb's implementer saw it. It lives in
// dawg::reverb so that the reverb's files compile unchanged; engine/src/LoopReverb.cpp adapts the node to DAWg's own
// API (engine/include/dawg/Node.h). Don't use it elsewhere.
//
// The subset of DAWg's node API that an audio effect uses, without JUCE. Names, field order and semantics match
// DAWg's engine (engine/include/dawg/Node.h there), so a node written against this header is integrated by
// including DAWg's header instead. Where DAWg uses a JUCE type, this header uses a standard one; those places are
// marked "DAWg:".

namespace dawg::reverb
{

/**
    Ticks per beat (quarter note). Positions in the transport are ticks.
*/
inline constexpr double ticksPerBeat = 15360.0;

/**
    Kinds of node ports.
*/
enum class PortType
{
    audio,      ///< Stereo audio.
    events,     ///< Note events.
    modulation, ///< A mono control signal, -1..1. Applied by the engine to parameters; nodes don't read it.
};

/**
    Static description of a port.
*/
struct PortInfo
{
    /**
        What the port carries.
    */
    PortType type;

    /**
        Display name.
    */
    const char* name;
};

/**
    How a parameter is shown and edited. Values are floats either way.
*/
enum class ParamKind
{
    continuous, ///< Any value in the range.
    integer,    ///< Whole numbers.
    choice,     ///< An index into ParamInfo::choices.
    toggle,     ///< 0 (off) or 1 (on).
};

/**
    The source polarity that is meaningful when modulating a parameter.
*/
enum class ModulationPolarity
{
    bipolar,  ///< Both positive and negative source values are meaningful.
    positive, ///< Only positive source values are applied.
    negative, ///< Only negative source values are applied.
};

/**
    Static description of a parameter.
*/
struct ParamInfo
{
    /**
        Stable identifier used in projects and commands, e.g. "gain".
    */
    const char* id = "";

    /**
        Display name.
    */
    const char* name = "";

    /**
        Lowest allowed value.
    */
    float minValue = 0.0f;

    /**
        Highest allowed value.
    */
    float maxValue = 1.0f;

    /**
        Value used when the project does not set one.
    */
    float defaultValue = 0.0f;

    /**
        How the value is edited.
    */
    ParamKind kind = ParamKind::continuous;

    /**
        Unit shown after the value, e.g. "Hz", "dB", "ms", "%"; empty for none.
    */
    const char* unit = "";

    /**
        The value a control shows at its middle, for skewed ranges such as frequencies; 0 for a linear range.
    */
    float midValue = 0.0f;

    /**
        Labels for choice parameters, by value.
    */
    std::span<const char* const> choices {};

    /**
        Which part of a modulation source this parameter accepts.
    */
    ModulationPolarity modulationPolarity = ModulationPolarity::bipolar;
};

/**
    What a node type is, for grouping and colouring in the UI.
*/
enum class NodeCategory
{
    io,         ///< Fixed nodes.
    instrument, ///< Note events in, audio out.
    effect,     ///< Audio in, audio out.
    utility,    ///< Gain, routing and mixing helpers.
};

/**
    Static description of a node type.
*/
struct NodeDescriptor
{
    /**
        Registry key, e.g. "dawg.utility".
    */
    const char* type = "";

    /**
        Display name.
    */
    const char* displayName = "";

    /**
        Input ports, by index. DAWg appends its modulation input pool to effects' inputs when it registers them.
    */
    std::span<const PortInfo> inputs;

    /**
        Output ports, by index.
    */
    std::span<const PortInfo> outputs;

    /**
        Parameters, by index.
    */
    std::span<const ParamInfo> params;

    /**
        Whether users may add or remove nodes of this type.
    */
    bool userEditable = true;

    /**
        The type's category.
    */
    NodeCategory category = NodeCategory::effect;

    /**
        Finds a parameter by id. DAWg: takes a juce::String.

        @param id  The parameter id.
        @return    The parameter index, or -1 if there is no such parameter.
    */
    [[nodiscard]] int indexOfParam (std::string_view id) const noexcept;
};

/**
    Settings a node is prepared for.
*/
struct ProcessSpec
{
    /**
        Sample rate in Hz: 44,100 to 192,000.
    */
    double sampleRate = 48000.0;

    /**
        Largest number of samples passed to one process() call.
    */
    int maxBlockSize = 512;
};

/**
    Transport state for one block (the subset effects may use). DAWg: has more fields (loop and seek flags, a tempo
    map).
*/
struct Transport
{
    /**
        Whether the transport is running during this block.
    */
    bool playing = false;

    /**
        Tick position at the first sample of the block.
    */
    double startTick = 0.0;

    /**
        Tick position just after the last sample of the block.
    */
    double endTick = 0.0;
};

/**
    A stereo input port's channels. Unconnected inputs point at silence.
*/
using AudioInput = std::array<const float*, 2>;

/**
    A stereo output port's channels. A node must overwrite every sample of every output.
*/
using AudioOutput = std::array<float*, 2>;

/**
    Everything an effect sees during process(). DAWg: also carries note events, the track's clips and the device
    input, which effects don't use.
*/
struct ProcessContext
{
    /**
        Number of samples to process, at most ProcessSpec::maxBlockSize. May differ from block to block.
    */
    int numSamples = 0;

    /**
        Audio inputs, by input port index.
    */
    std::span<const AudioInput> audioInputs;

    /**
        Audio outputs, by output port index.
    */
    std::span<const AudioOutput> audioOutputs;

    /**
        Transport state for this block. Never null.
    */
    const Transport* transport = nullptr;
};

/**
    Parameter values shared between the message thread (writer) and the audio thread (reader).
*/
class ParamBlock final
{
public:
    /**
        Creates storage for the given parameters, initialised to their defaults.

        @param params  The parameter descriptions.
    */
    explicit ParamBlock (std::span<const ParamInfo> params);

    /**
        Sets a parameter, clamped to its range.

        @param index  The parameter index.
        @param value  The new value.
        @note  Message thread, or the engine applying automation and modulation on the audio thread before process().
    */
    void set (size_t index, float value) noexcept;

    /**
        Reads a parameter.

        @param index  The parameter index.
        @return       The current value.
        @note  Any thread.
    */
    [[nodiscard]] float get (size_t index) const noexcept { return m_values[index].load(); }

    /**
        Returns the number of parameters.
    */
    [[nodiscard]] size_t size() const noexcept { return m_infos.size(); }

private:
    std::span<const ParamInfo> m_infos;
    std::unique_ptr<std::atomic<float>[]> m_values; // NOLINT(modernize-avoid-c-arrays): atomics cannot live in a vector
};

/**
    Live measurements for a node's device panel, by name. DAWg: a juce::var object with the same keys and arrays.
*/
using Meters = std::map<std::string, std::vector<double>>;

/**
    A processing node.

    @note  Construction, prepare() and destruction: message thread. process(): audio thread, never allocating,
           locking or throwing.
*/
class Node
{
public:
    /**
        Creates a node with parameters at their defaults.

        @param descriptor  The node type's description. Must outlive the node (use static storage).
    */
    explicit Node (const NodeDescriptor& descriptor);

    /**
        Destroys the node (message thread).
    */
    virtual ~Node() = default;

    Node (const Node&) = delete;
    Node& operator= (const Node&) = delete;
    Node (Node&&) = delete;
    Node& operator= (Node&&) = delete;

    /**
        Returns the node type's description.
    */
    [[nodiscard]] const NodeDescriptor& descriptor() const noexcept { return m_descriptor; }

    /**
        Returns the node's parameter values.
    */
    [[nodiscard]] ParamBlock& params() noexcept { return m_params; }

    /**
        Returns the node's parameter values.
    */
    [[nodiscard]] const ParamBlock& params() const noexcept { return m_params; }

    /**
        Prepares for processing, and clears any state (a reverb's tail). May allocate.

        @param spec  The sample rate and maximum block size.
        @note  Message thread, before the node is used by the audio thread.
    */
    virtual void prepare (const ProcessSpec& spec) = 0;

    /**
        Processes one block.

        @param context  Inputs, outputs and transport.
        @note  Audio thread.
    */
    virtual void process (const ProcessContext& context) noexcept = 0;

    /**
        Returns the node's processing latency in samples, used for delay compensation.
    */
    [[nodiscard]] virtual int latencySamples() const noexcept { return 0; }

    /**
        Turns bypass on or off. DAWg: configure() with the node's model data.

        @param bypassed  True to bypass.
        @note  Message thread.
    */
    virtual void setBypassed (bool bypassed) { static_cast<void> (bypassed); }

    /**
        Returns the node's live measurements for its device panel, or nothing.

        @note  Message thread; implementations read atomics the audio thread writes.
    */
    [[nodiscard]] virtual Meters meters() const { return {}; }

private:
    const NodeDescriptor& m_descriptor;
    ParamBlock m_params;
};

/**
    An audio effect: DAWg's built-in effects derive from this. Bypass copies the input to the output; otherwise
    render() runs with denormals flushed to zero.
*/
class EffectNode : public Node
{
public:
    using Node::Node;

    void setBypassed (bool bypassed) override { m_bypassed.store (bypassed); }

    void process (const ProcessContext& context) noexcept final;

protected:
    /**
        Processes one block (not bypassed).

        @param context  Inputs, outputs and transport.
        @note  Audio thread.
    */
    virtual void render (const ProcessContext& context) noexcept = 0;

    /**
        Reads a parameter.

        @param index  The parameter index.
        @return       Its current value.
    */
    [[nodiscard]] float param (size_t index) const noexcept { return params().get (index); }

private:
    std::atomic<bool> m_bypassed { false };
};

} // namespace dawg::reverb
