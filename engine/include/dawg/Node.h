#pragma once

#include "dawg/Model.h"
#include "dawg/Time.h"

#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>

namespace dawg
{

class Node;
struct CompiledNote;
struct CompiledVoice;
struct AudioSequence;
struct SampleMap;
class AudioSource;
class TrackSequence;

/**
    An audio port's width that follows the node's audio inputs (PortInfo::channels; docs/design/mono-ports.md, MP1).
*/
inline constexpr int adaptiveChannels = 0;

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

    /**
        Audio ports: 1 (mono), 2 (stereo) or adaptiveChannels (docs/design/mono-ports.md, MP1). An adaptive port is
        mono when every source feeding any of the node's adaptive inputs is mono, else stereo; the node learns which
        from ProcessContext::channels. Ignored for other port types.
    */
    int channels = 2;
};

/**
    How a parameter is shown and edited (docs/design/phase8-node-graph.md, G2). Values are floats either way.
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
    How a modulation depth moves a parameter (docs/design/macros-and-modulation-column.md, MC1, MC2).
*/
enum class ModulationScale
{
    linear,  ///< In the parameter's units: a depth of 1 moves it by its range or declared span.
    octaves, ///< A frequency: a depth of 1 multiplies it by 2 to the power of its octave span or declared span.
    travel,  ///< A skewed parameter: a depth of 1 moves its knob through its whole travel.
};

/**
    Static description of a parameter.
*/
struct ParamInfo
{
    /**
        Stable identifier used in the model and commands, e.g. "gain".
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
        Value used when the model does not set one.
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
    // NOLINTNEXTLINE(readability-redundant-member-init): GCC needs it for partial aggregate initialisation
    std::span<const char* const> choices {};

    /**
        Which part of a modulation source this parameter accepts.
    */
    ModulationPolarity modulationPolarity = ModulationPolarity::bipolar;

    /**
        Whether the parameter can be a modulation target at all (docs/design/modulation.md, M2). Choice and toggle
        parameters never are; see acceptsModulation().
    */
    bool modulatable = true;

    /**
        Whether changing the parameter changes the node's latencySamples(), such as the Saturator's oversampling
        (docs/design/saturator.md, W7). A change recompiles the track, which reads the latency again; such a parameter
        can't be modulated or automated, which would change it behind the compiler's back.
    */
    bool changesLatency = false;

    /**
        How far a depth of 1 moves the parameter at a full-scale source; 0 for its whole range. For a range so wide that the small
        moves it's modulated by would be out of reach, as with the Feedback node's Time. A frequency's span is in octaves
        (docs/design/macros-and-modulation-column.md, MC1); a skewed parameter that declares one is modulated linearly
        by it (MC2).
    */
    float modulationSpan = 0.0f;

    /**
        Whether automation may move the parameter. False for parameters whose change rebuilds something off the audio
        thread, such as the Convolution Reverb's IR shaping (docs/design/convolution-reverb.md, I14): their changes
        come through configure(), which automation doesn't call.
    */
    bool automatable = true;

    /**
        Returns how a modulation depth moves the parameter (docs/design/macros-and-modulation-column.md, MC1, MC2): in
        octaves for a frequency (unit "Hz", range above 0), along its knob for another skewed one without a declared
        span, and linearly otherwise.
    */
    [[nodiscard]] constexpr ModulationScale modulationScale() const noexcept
    {
        if (std::string_view (unit) == "Hz" && minValue > 0.0f && maxValue > minValue)
            return ModulationScale::octaves;

        if (midValue > 0.0f && modulationSpan <= 0.0f && kind != ParamKind::choice && kind != ParamKind::toggle)
            return ModulationScale::travel;

        return ModulationScale::linear;
    }

    /**
        Returns how far a depth of 1 moves the parameter, in modulationScale()'s terms: octaves, knob travel (1) or the
        parameter's units.
    */
    [[nodiscard]] float modulationRange() const noexcept
    {
        switch (modulationScale())
        {
            case ModulationScale::octaves:
                return modulationSpan > 0.0f ? modulationSpan : std::log2 (maxValue / minValue);
            case ModulationScale::travel:
                return 1.0f;
            case ModulationScale::linear:
                break;
        }

        return modulationSpan > 0.0f ? modulationSpan : maxValue - minValue;
    }

    /**
        Returns whether a modulation slot may target this parameter: a modulatable continuous or integer one.
    */
    [[nodiscard]] constexpr bool acceptsModulation() const noexcept
    {
        return modulatable && ! changesLatency && kind != ParamKind::choice && kind != ParamKind::toggle;
    }

    /**
        Returns whether an automation lane may target this parameter: an automatable one that doesn't change the node's
        latency.
    */
    [[nodiscard]] constexpr bool acceptsAutomation() const noexcept { return automatable && ! changesLatency; }
};

/**
    Maps a normalised control position (0..1) to a parameter value: the curve the UI's sliders use, with midValue at
    the middle, rounded for integer, choice and toggle parameters (docs/design/phase10-automation.md, A1).

    @param info  The parameter.
    @param t     The position, 0..1.
    @return      The value.
*/
[[nodiscard]] float paramFromNormalised (const ParamInfo& info, double t) noexcept;

/**
    A built-in parameter's modulated value (docs/design/modulation.md, M5; macros-and-modulation-column.md, MC1 to MC3):
    its base moved by a summed modulation amount (source times depth) in the parameter's modulationScale(), clamped to
    its range and whole for an integer parameter. Real-time safe.

    @param info    The parameter.
    @param base    The unmodulated value.
    @param amount  The summed amount, nominally -1..1 per slot.
    @return        The value.
*/
[[nodiscard]] float modulatedParamValue (const ParamInfo& info, float base, float amount) noexcept;

/**
    The inverse of paramFromNormalised().

    @param info   The parameter.
    @param value  A value in the parameter's range.
    @return       The position, 0..1.
*/
[[nodiscard]] double paramToNormalised (const ParamInfo& info, double value) noexcept;

/**
    What a node type is, for grouping and colouring in the UI.
*/
enum class NodeCategory
{
    io,         ///< Fixed nodes: sequencer, audio player, mix input, track output.
    instrument, ///< Note events in, audio out.
    midiEffect, ///< Note events in, note events out (docs/design/midi-effects.md, ME1).
    effect,     ///< Audio in, audio out.
    utility,    ///< Gain, routing and mixing helpers.
};

/**
    Static description of a node type.
*/
struct NodeDescriptor
{
    /**
        Registry key, e.g. "dawg.mpeSynth".
    */
    const char* type = "";

    /**
        Display name.
    */
    const char* displayName = "";

    /**
        Input ports, by index.
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
        Whether a track may hold more than one node of this type, and whether users may add or remove it.
    */
    bool userEditable = true;

    /**
        The type's category.
    */
    NodeCategory category = NodeCategory::effect;

    /**
        Finds a parameter by id.

        @param id  The parameter id.
        @return    The parameter index, or -1 if there is no such parameter.
    */
    [[nodiscard]] int indexOfParam (const juce::String& id) const noexcept;
};

/**
    Settings a node is prepared for.
*/
struct ProcessSpec
{
    /**
        Sample rate in Hz.
    */
    double sampleRate = 48000.0;

    /**
        Largest number of samples passed to one process() call.
    */
    int maxBlockSize = 512;

    /**
        Compares both fields.

        @param other  The spec to compare with.
        @return       True if both specs are equal.
    */
    [[nodiscard]] bool operator== (const ProcessSpec& other) const noexcept
    {
        // Exact match wanted (any change means re-preparing); written without == on doubles for -Wfloat-equal.
        return sampleRate >= other.sampleRate && sampleRate <= other.sampleRate && maxBlockSize == other.maxBlockSize;
    }
};

/**
    Kinds of note events.
*/
enum class NoteEventType
{
    noteOn,     ///< Start a note. note is set.
    noteOff,    ///< Release a note (by noteId).
    choke,      ///< Stop a note quickly, without a release tail (by noteId).
    releaseAll, ///< Release every sounding note (transport stopped).
    chokeAll,   ///< Stop every note quickly (seek).
    expression, ///< A live note's expression changed: lane (a Lane) and value (docs/design/phase9-export-midi-input.md, M2).
    sustain,    ///< The sustain pedal: value >= 0.5 is down.
    controller, ///< Any other controller for plugins: lane is the CC number, controllerPitchBend or controllerPressure.
    legato,     ///< A linked note continues another's voice: note is set, fromNoteId is the note it continues.
    /**
        What feeds the node's event input is no longer what fed it (docs/design/midi-effects.md, ME6): a MIDI effect was
        added, removed or moved before it, or its cable changed. Every note ends quickly and every CompiledNote pointer
        is forgotten, live notes' too, since the notes they came from may be gone. Sent by the engine at offset 0.
    */
    sourceChanged,
};

/**
    NoteEvent::lane of a controller event carrying channel pitch bend (value -1..1).
*/
inline constexpr int controllerPitchBend = 128;

/**
    NoteEvent::lane of a controller event carrying channel pressure (value 0..1).
*/
inline constexpr int controllerPressure = 129;

/**
    A note event within a block.
*/
struct NoteEvent
{
    /**
        Sample offset within the block, 0 <= offset < numSamples.
    */
    int sampleOffset = 0;

    /**
        What happens.
    */
    NoteEventType type = NoteEventType::noteOn;

    /**
        The note this event refers to (0 for releaseAll / chokeAll).
    */
    ObjectId noteId = 0;

    /**
        The compiled note for noteOn, owned by the current TrackSequence. Null for other types.
    */
    const CompiledNote* note = nullptr;

    /**
        expression: the Lane as int. controller: the CC number, controllerPitchBend or controllerPressure.
    */
    int lane = 0;

    /**
        expression: pitch in semitones relative to the base pitch, or pressure / timbre 0..1. sustain: 0 or 1.
        controller: 0..1 (pitch bend -1..1).
    */
    float value = 0.0f;

    /**
        legato: the note whose voice this note continues.
    */
    ObjectId fromNoteId = 0;
};

/**
    Fixed-capacity, allocation-free list of note events, kept sorted by sample offset.
*/
class EventBuffer final
{
public:
    /**
        Most events one buffer holds per block. Further events are dropped and counted.
    */
    static constexpr size_t capacity = 512;

    /**
        Removes all events.
    */
    void clear() noexcept { m_size = 0; }

    /**
        Inserts an event, keeping the buffer sorted by offset (stable for equal offsets).

        @param event  The event to add.
        @return       False if the buffer was full and the event was dropped.
    */
    [[nodiscard]] bool add (const NoteEvent& event) noexcept;

    /**
        Returns the events in offset order.
    */
    [[nodiscard]] std::span<const NoteEvent> events() const noexcept { return { m_events.data(), m_size }; }

    /**
        Returns the number of events dropped because the buffer was full, since creation.
    */
    [[nodiscard]] size_t droppedCount() const noexcept { return m_dropped; }

private:
    std::array<NoteEvent, capacity> m_events {};
    size_t m_size = 0;
    size_t m_dropped = 0;
};

/**
    Transport state for one block.
*/
struct Transport
{
    /**
        Whether the transport is running during this block.
    */
    bool playing = false;

    /**
        True in the first block after playback started; notes overlapping the start must be chased.
    */
    bool started = false;

    /**
        True in the first block after playback stopped; sounding notes must be released.
    */
    bool stopped = false;

    /**
        True in the first block after the playhead was moved (seek); sounding notes must be choked and, while
        playing, notes overlapping the new position chased.
    */
    bool jumped = false;

    /**
        True in the first block after the transport loop wrapped from its end back to its start; sounding notes
        must be released (not choked) and notes overlapping the loop start chased.
    */
    bool looped = false;

    /**
        Tick position at the first sample of the block.
    */
    double startTick = 0.0;

    /**
        Tick position just after the last sample of the block.
    */
    double endTick = 0.0;

    /**
        Sample position (since the transport anchor) of the first sample of the block.
    */
    juce::int64 startSample = 0;

    /**
        The mapping between ticks and samples for this block.
    */
    TimeMap timeMap;
};

/**
    An input port's channels. Unconnected inputs point at silence. A mono signal has the same channel twice, so a node
    reading both channels of a mono input hears it on both (docs/design/mono-ports.md, MP6).
*/
using AudioInput = std::array<const float*, 2>;

/**
    An output port's channels. A node must overwrite every sample of every output. Of a mono output, only the first
    channel is read; the second is scratch, so writing it does no harm.
*/
using AudioOutput = std::array<float*, 2>;

/**
    A modulated built-in parameter's value at every sample of the block (docs/design/modulation.md, M5): its base
    plus the modulation, clamped to its range. The parameter itself holds the value at the middle of the block; a
    node that follows modulation faster than once per block reads these instead.
*/
struct ModulatedParam
{
    /**
        The parameter's index in the node's descriptor.
    */
    size_t param = 0;

    /**
        ProcessContext::numSamples values.
    */
    const float* values = nullptr;
};

/**
    How many notes at once get their own instance of an LFO or ADSR modulating a plugin per note
    (docs/design/clap-hosting.md, C10). Later notes get no amount of their own from it.
*/
inline constexpr int maxNoteSourceVoices = 32;

/**
    A modulation slot a plugin node works out per note (docs/design/clap-hosting.md, C10): its source is a value each
    note has, as Note Expression's outputs are.
*/
struct NoteModulation
{
    /**
        The plugin parameter's index.
    */
    int parameter = -1;

    /**
        Which output of Note Expression: 0 pitch, 1 pressure, 2 timbre, 3 velocity, 4 gate.
    */
    int source = 0;

    /**
        The slot's depth.
    */
    float depth = 0.0f;

    /**
        What the parameter's other slots add, which a note's amount must include: a voice heeds only its own.
    */
    float shared = 0.0f;

    /**
        For an LFO or ADSR source: its instances, one per note slot, which the plugin node runs. Otherwise empty, and
        source says which of a note's values to use.
    */
    std::span<Node* const> nodes;

    /**
        With nodes: the source's output port.
    */
    int port = 0;
};

/**
    The largest piece of a block a loop in a graph runs at a time, and so about its shortest length
    (docs/design/feedback-routing.md, R2, R3).
*/
inline constexpr int loopChunkSamples = 16;

/**
    Which part of a Feedback node a step runs: in a loop, reading before the loop's other nodes and writing after them.
*/
enum class LoopPart
{
    whole, ///< Everything, as for any other node.
    read,  ///< Only the output, and the modulation inputs.
    write  ///< Only the audio input.
};

/**
    Runs independent pieces of a node's work at the same time (docs/design/phase2-engine.md, 5.6): on the engine's
    track threads that have no track left to render, and on the calling thread.
*/
class Parallel
{
public:
    /**
        One piece of work. Called once per index, on any of the engine's threads, maybe at the same time as other
        indices. It must not block, allocate or touch anything another index touches.
    */
    using Task = void (*) (void* context, size_t index) noexcept;

    /**
        Runs task (context, i) for every i in [0, count), and returns once all of them have finished.

        @param count    How many pieces.
        @param task     The work.
        @param context  Passed to the task.
        @note  Audio thread (or a track thread), from inside a node's process().
    */
    virtual void forEach (size_t count, Task task, void* context) noexcept = 0;

protected:
    /** @cond */
    Parallel() = default;
    ~Parallel() = default;
    Parallel (const Parallel&) = default;
    Parallel& operator= (const Parallel&) = default;
    Parallel (Parallel&&) = default;
    Parallel& operator= (Parallel&&) = default;
    /** @endcond */
};

/**
    Everything a node sees during process().
*/
struct ProcessContext
{
    /**
        Number of samples to process, at most ProcessSpec::maxBlockSize.
    */
    int numSamples = 0;

    /**
        Audio inputs, by input port index (only audio ports have entries; others are silent).
    */
    std::span<const AudioInput> audioInputs;

    /**
        Audio outputs, by output port index (only audio ports have valid entries).
    */
    std::span<const AudioOutput> audioOutputs;

    /**
        Events arriving at the node's event input, or nullptr if it has none or it is unconnected.
    */
    const EventBuffer* eventsIn = nullptr;

    /**
        Buffer for the node's event output, or nullptr if it has none. Cleared before process().
    */
    EventBuffer* eventsOut = nullptr;

    /**
        Transport state for this block.
    */
    const Transport* transport = nullptr;

    /**
        The track's compiled notes. Never null. Pointers taken from an earlier sequence are only valid until
        this pointer changes; nodes holding CompiledNote pointers must re-resolve them when it does.
    */
    const TrackSequence* sequence = nullptr;

    /**
        Live notes for this track (audition), at offset 0. Their CompiledNote pointers stay valid across
        sequence changes and their ids are negative, so they never clash with model note ids.
    */
    std::span<const NoteEvent> liveEvents;

    /**
        The track's compiled audio clips (audio tracks), or nullptr.
    */
    const AudioSequence* audio = nullptr;

    /**
        Monitored device input for this block (audio tracks while monitoring), or nullptrs. A mono input has
        the same channel twice.
    */
    std::array<const float*, 2> input { nullptr, nullptr };

    /**
        The node's modulated parameters, per sample. Empty when nothing modulates it.
    */
    std::span<const ModulatedParam> modulatedParams;

    /**
        Plugin nodes: the slots to work out per note. Empty when there are none.
    */
    std::span<const NoteModulation> noteModulations;

    /**
        Voice Instruments only: the compiled voice graph, one program per voice slot
        (docs/design/phase12-voice-scope.md, V8, V9). Null for every other node.
    */
    const CompiledVoice* voice = nullptr;

    /**
        Samplers only: the node's compiled zones. Null for every other node.
    */
    const SampleMap* samples = nullptr;

    /**
        Bit n set: output port n leads somewhere that is heard (docs/design/multi-output-instruments.md, MO2). Bit 0
        (the main output) is always set. A Sampler plays a zone whose output isn't wired into its main output.
    */
    juce::uint32 wiredOutputs = 1;

    /**
        Feedback nodes only: which part of the node this step runs.
    */
    LoopPart loopPart = LoopPart::whole;

    /**
        The width of the node's adaptive ports for this step, 1 or 2 (docs/design/mono-ports.md, MP6, MP7): with 1, its
        adaptive outputs are mono and only their first channel needs writing. Always 2 for a node without adaptive
        ports. Carried per step, not set on the node, because the old and the new compiled engine share instances.
    */
    int channels = 2;

    /**
        Where the node may run independent pieces of its work at the same time (a Voice Instrument's voices), or
        nullptr to run them one after another. Set only while independent tracks render on the engine's track
        threads; never inside a voice.
    */
    Parallel* parallel = nullptr;

    /**
        Returns a parameter's per-sample values, or nullptr when it is not modulated.

        @param param  The parameter's index in the node's descriptor.
    */
    [[nodiscard]] const float* modulatedValues (size_t param) const noexcept
    {
        for (const auto& modulated : modulatedParams)
            if (modulated.param == param)
                return modulated.values;

        return nullptr;
    }
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
        @note  Message thread.
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
    A copy of a node's output for the analysers of its device panel (docs/design/phase11-faust-panels.md, P2): a
    lock-free ring of the most recent stereo frames, written by the audio thread and read by the message thread.
*/
class AnalyserTap final
{
public:
    /**
        Frames the ring holds (a power of two).
    */
    static constexpr size_t capacity = 16384;

    /**
        Creates an empty (silent) ring.
    */
    AnalyserTap();

    /**
        Appends a block of the node's first output.

        @param output      The output channels.
        @param numSamples  The block length.
        @note  Audio thread.
    */
    void push (const AudioOutput& output, int numSamples) noexcept;

    /**
        Copies the newest frames, oldest first. Frames written while copying may be mixed in; analysers don't mind.

        @param left   Receives the left channel; its size is the number of frames copied (at most capacity).
        @param right  Receives the right channel; the same size as left.
        @note  Message thread.
    */
    void read (std::span<float> left, std::span<float> right) const noexcept;

private:
    std::unique_ptr<std::atomic<float>[]> m_left;  // NOLINT(modernize-avoid-c-arrays): atomics cannot live in a vector
    std::unique_ptr<std::atomic<float>[]> m_right; // NOLINT(modernize-avoid-c-arrays)
    std::atomic<size_t> m_written { 0 };
};

/**
    A processing node. See docs/design/phase2-engine.md, section 5.2.

    @note  Construction, prepare() and destruction: message thread. process() and reset(): audio thread,
           never allocating, locking or throwing.
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
        Returns a number no other node created in this process has, which never changes.
    */
    [[nodiscard]] juce::uint64 serial() const noexcept { return m_serial; }

    /**
        Notes which node now feeds this node's event input (its serial, 0 for none), and returns whether that differs
        from the last call: the engine then sends NoteEventType::sourceChanged first (docs/design/midi-effects.md, ME6).
        The first call reports a change.

        @param source  The feeding node's serial.
        @return        True if the source changed.
        @note  Audio thread.
    */
    [[nodiscard]] bool takeEventSource (juce::uint64 source) noexcept
    {
        const auto changed = ! m_heardSource.has_value() || *m_heardSource != source;
        m_heardSource = source;
        return changed;
    }

    /**
        Returns the node's parameter values.
    */
    [[nodiscard]] ParamBlock& params() noexcept { return m_params; }

    /**
        Returns the node's parameter values.
    */
    [[nodiscard]] const ParamBlock& params() const noexcept { return m_params; }

    /**
        Prepares for processing. May allocate.

        @param spec  The sample rate and maximum block size.
        @note  Message thread, before the node is used by the audio thread.
    */
    virtual void prepare (const ProcessSpec& spec) = 0;

    /**
        Processes one block.

        @param context  Inputs, outputs, events and transport.
        @note  Audio thread.
    */
    virtual void process (const ProcessContext& context) noexcept = 0;

    /**
        Returns the node to the state prepare() left it in, for a new note (docs/design/phase12-voice-scope.md, V8):
        clears oscillator phases, filter history and envelopes, and moves smoothed parameters straight to their
        current values. The default does nothing.

        @note  Audio thread, between process() calls. Never allocates, locks or throws.
    */
    virtual void reset() noexcept {}

    /**
        Asks the node to forget what's ringing in it at its next process(), for a loop the user wants to stop
        (docs/design/feedback-routing.md, R7). The default does nothing.

        @note  Any thread.
    */
    virtual void requestClear() noexcept {}

    /**
        Tells a Feedback node the latency of the other nodes on its loop, which it takes off its own delay so that the whole trip
        has the length it was set to (docs/design/feedback-routing.md, R4). The default does nothing.

        @param samples  The latency.
        @note  Message thread.
    */
    virtual void setLoopLatency (int samples) noexcept { std::ignore = samples; }

    /**
        Tells the node whether it runs inside a feedback loop (docs/design/feedback-routing.md), where latency and
        uneven phase change the loop's pitch: the Filter then runs without oversampling. The compiler calls it on every
        compile, before it reads latencySamples().

        @param inLoop  Whether the node is in a loop.
        @note  Message thread.
    */
    virtual void setInLoop (bool inLoop) noexcept { std::ignore = inLoop; }

    /**
        Returns the node's processing latency in samples while it runs, used for delay compensation. The compiler counts
        none for a bypassed node, which passes its input straight through.
    */
    [[nodiscard]] virtual int latencySamples() const noexcept { return 0; }

    /**
        Applies the model's non-parameter settings (bypass, MIDI settings). Called after every change to the
        graph node, alongside the parameter update.

        @param node  The node's current model data.
        @note  Message thread. Must only write values the audio thread reads atomically.
    */
    virtual void configure (const GraphNode& node) { std::ignore = node; }

    /**
        The width of an audio output port that the node decides itself when the graph compiles, as a plugin does from
        the bus layout it takes (docs/design/mono-ports.md, MP4). Must not change anything: the compiler may ask with
        several widths before it settles.

        @param port          The output port.
        @param width         The width of the node's adaptive inputs, 1 or 2 (2 for a node without any).
        @param wiredOutputs  Bit i set when output port i is wired.
        @return              1 or 2, or adaptiveChannels to take the width from the descriptor.
        @note  Message thread, while the graph compiles.
    */
    [[nodiscard]] virtual int outputWidth (size_t port, int width, juce::uint32 wiredOutputs) const
    {
        std::ignore = port;
        std::ignore = width;
        std::ignore = wiredOutputs;
        return adaptiveChannels;
    }

    /**
        Tells the node the width it was compiled with, before the compiler reads its latency, so that it can take the
        layout outputWidth() answered for (a plugin's main buses, MP4).

        @param width  The width of the node's adaptive inputs, 1 or 2.
        @note  Message thread, while the graph compiles. The node may still be run by the previous compiled engine.
    */
    virtual void useWidth (int width) { std::ignore = width; }

    /**
        Samplers: their compiled zones. The graph compiler hands them over when it compiles the node, and again
        whenever only the zones change, which needs no recompile. Other nodes ignore it.

        The argument is the compiled zones, not null.
        @note  Message thread.
    */
    // NOLINTNEXTLINE(performance-unnecessary-value-param): by value, as the nodes that use it keep it
    virtual void setSampleMap (std::shared_ptr<const SampleMap> /*map*/) {}

    /**
        Nodes that read a pool file (nodetype::usesAudioFile): the file their GraphNode::fileId names, opened. The graph
        compiler hands it over when it compiles the node, and again whenever only the file changes, which needs no
        recompile (docs/design/convolution-reverb.md, I11). Other nodes ignore it.

        The argument is the file, or null when the node has none or it is missing.
        @note  Message thread, after configure().
    */
    // NOLINTNEXTLINE(performance-unnecessary-value-param): by value, as the nodes that use it keep it
    virtual void setAudioFile (std::shared_ptr<const AudioSource> /*source*/) {}

    /**
        Audio players: play only the track's clips of this pool file, or all of them with 0 (GraphNode::fileId). A
        frozen track plays each of its renders through a player of its own (docs/design/audio-features.md, AU15).
        Other nodes ignore it.

        @note  Message thread, after configure().
    */
    virtual void setOnlyFile (ObjectId /*fileId*/) {}

    /**
        The project's keys on the timeline (docs/design/scale-highlighting.md, SH6, SH9). The graph compiler hands them
        to every node when it compiles, and again whenever only the keys change, which needs no recompile. Scale Lock
        follows them; other nodes ignore them.

        The argument is the keys, in ticks, not null.
        @note  Message thread.
    */
    // NOLINTNEXTLINE(performance-unnecessary-value-param): by value, as the nodes that use it keep it
    virtual void setKeys (std::shared_ptr<const KeyTimeline> /*keys*/) {}

    /**
        Step sequencers: previews a pattern while the transport is stopped (docs/design/step-sequencer.md, SS8), or
        stops the preview. Other nodes ignore it.

        @param pattern  The pattern, or -1 for none.
        @note  Message thread.
    */
    virtual void setStepPreview (int pattern) { std::ignore = pattern; }

    /**
        Step sequencers: the pattern a held key plays while the transport is stopped (SS8), the one the panel shows.
        Other nodes ignore it.

        @param pattern  The pattern.
        @note  Message thread.
    */
    virtual void setStepAudition (int pattern) { std::ignore = pattern; }

    /**
        Whether the graph compiler keeps this instance and calls prepare() again when the device spec changes,
        instead of creating a new one. True for nodes that are expensive to create or carry user state
        (plugins).

        prepare() is then called while an older compiled engine may still reference the node, but that engine
        is not processed any more (the audio engine stays silent until an engine with the new spec arrives),
        except that the block size may change at the same sample rate, so such nodes must guard process()
        against a concurrent prepare() themselves.
    */
    [[nodiscard]] virtual bool keepAcrossSpecChange() const noexcept { return false; }

    /**
        Turns the analyser tap on or off: while on, the engine copies the node's first output into tap() after each
        block (docs/design/phase11-faust-panels.md, P2). Nothing is measured while it is off.

        @param enabled  True to tap the output.
        @note  Message thread. The tap is created the first time and lives as long as the node.
    */
    void setTapEnabled (bool enabled);

    /**
        Returns the tap while it is on, or nullptr.

        @note  Audio thread (the engine) and message thread.
    */
    [[nodiscard]] AnalyserTap* activeTap() const noexcept { return m_activeTap.load(); }

    /**
        Returns the node's live measurements for its device panel, e.g. a compressor's gain reduction, or a void var.

        @note  Message thread; implementations read atomics the audio thread writes.
    */
    [[nodiscard]] virtual juce::var meters() const { return {}; }

    /**
        What the device panel shows of this node: meters(), plus anything too costly to work out for every instance.

        @return  The panel's data.
    */
    [[nodiscard]] virtual juce::var panelMeters() const { return meters(); }

    /**
        Tells a keyed node how late its key from other tracks arrives, when a key source is slower than the path to
        the node (docs/design/sidechain.md, K5); 0 when it is on time.

        @param samples  The lateness in samples.
        @note  Message thread, while compiling.
    */
    virtual void setKeyLateSamples (int samples) { std::ignore = samples; }

private:
    const NodeDescriptor& m_descriptor;
    const juce::uint64 m_serial;
    std::optional<juce::uint64> m_heardSource;
    ParamBlock m_params;
    std::unique_ptr<AnalyserTap> m_tap;
    std::atomic<AnalyserTap*> m_activeTap { nullptr };
};

} // namespace dawg
