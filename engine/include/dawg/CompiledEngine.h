#pragma once

#include "dawg/AudioPlayer.h"
#include "dawg/Node.h"
#include "dawg/Sequence.h"
#include "dawg/VoiceAllocator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace dawg
{

/**
    Per-track state that outlives compilations (fader gain, pan, meters), shared between compiled engines.
*/
struct TrackState
{
    /**
        Linear fader gain, written by the message thread and read by the audio thread.
    */
    std::atomic<float> gain { 1.0f };

    /**
        Pan position from -1 (left) to +1 (right).
    */
    std::atomic<float> pan { 0.0f };

    /**
        Highest absolute left-channel sample after fader, pan, mute and solo since the meters were last read
        (docs/design/mixing-basics.md, M2). Raised by the audio thread, reset by the reader.
    */
    std::atomic<float> peakLeft { 0.0f };

    /**
        Like peakLeft, for the right channel.
    */
    std::atomic<float> peakRight { 0.0f };

    /**
        Highest absolute sample of the track's input channels since the meters were last read (audio tracks
        with an input).
    */
    std::atomic<float> inputPeak { 0.0f };

    /**
        Blocks whose output had NaN or infinity and were silenced since the meters were last read
        (docs/design/output-protection.md, S1). Raised by the audio thread, reset by the reader.
    */
    std::atomic<int> invalidBlocks { 0 };

    /**
        The track's AutomationMode as int (docs/design/phase10-automation.md, A6).
    */
    std::atomic<int> automationMode { static_cast<int> (AutomationMode::read) };

    /**
        Whether the track is muted.
    */
    std::atomic<bool> mute { false };

    /**
        Whether the track is soloed.
    */
    std::atomic<bool> solo { false };

    /**
        Whether the track is armed for recording (session state, set by the app; phase 7, T9).
    */
    std::atomic<bool> armed { false };

    /**
        Input monitoring (MonitorMode as int).
    */
    std::atomic<int> monitor { static_cast<int> (MonitorMode::automatic) };

    /**
        First input channel, or -1 for none.
    */
    std::atomic<int> inputChannel { -1 };

    /**
        Whether the input is a stereo pair.
    */
    std::atomic<bool> inputStereo { false };
};

/**
    State of one automation lane shared across compilations (docs/design/phase10-automation.md, A4, A6).
*/
struct LaneState
{
    /**
        The value the lane played last (normalised), or a negative value when it didn't play. For the UI.
    */
    std::atomic<float> value { -1.0f };

    /**
        Set while the lane is being written: the parameter then keeps its manual value.
    */
    std::atomic<bool> overridden { false };
};

/**
    A send's level and switches, shared across compilations so changing them needs no recompilation
    (docs/design/groups-and-sends.md, R4).
*/
struct SendState
{
    /**
        Linear send level.
    */
    std::atomic<float> gain { 1.0f };

    /**
        Whether the send is on.
    */
    std::atomic<bool> active { true };

    /**
        Whether the send taps the signal before the track's fader and pan.
    */
    std::atomic<bool> preFader { false };
};

/**
    A send ready for the audio thread: where its signal comes from and which bus it feeds.
*/
struct CompiledSend
{
    /**
        The return track's bus (CompiledTrack::bus), or -1 for a send that can't play (its target is missing, or
        it would close a loop).
    */
    int bus = -1;

    /**
        Index into CompiledEngine::tracks of the return track.
    */
    int target = -1;

    /**
        The track's output before its fader, delayed so it reaches the return in step with the return's other inputs.
    */
    AudioInput source {};

    /**
        Level and switches.
    */
    std::shared_ptr<SendState> state;

    /**
        Whether the send keys a node (docs/design/sidechain.md, K2): its bus is the node's key bus, and it runs
        whatever the track's mute, solo or a stem export.
    */
    bool key = false;
};

/**
    An Output to track node's signal on its way to its output track (docs/design/multi-output-instruments.md, MO7).
*/
struct CompiledBusOutput
{
    /**
        The output track's bus.
    */
    int bus = -1;

    /**
        The node's input, delayed so it reaches the bus in step with the bus's other inputs.
    */
    AudioInput source {};

    /**
        The Output to track node, which a freeze render taps (docs/design/audio-features.md, AU15).
    */
    ObjectId node = 0;
};

class PluginNode;

/**
    An automation lane ready for the audio thread.
*/
struct CompiledLane
{
    /**
        What the lane controls.
    */
    AutomationTargetKind kind = AutomationTargetKind::gain;

    /**
        Points at absolute ticks, normalised values.
    */
    Curve points;

    /**
        State shared with the message thread.
    */
    std::shared_ptr<LaneState> state;

    /**
        Node parameters: the node and the parameter's index and description.
    */
    Node* node = nullptr;

    /**
        Node parameters: the parameter index.
    */
    size_t param = 0;

    /**
        Node parameters of a voice-graph node: the instances in the other voice slots, written like `node`
        (docs/design/phase12-voice-scope.md, V11).
    */
    std::vector<Node*> voiceNodes;

    /**
        Node parameters: the parameter's description (range and curve).
    */
    ParamInfo info {};

    /**
        Plugin parameters: the plugin node and the parameter index (-1 while unresolved).
    */
    PluginNode* plugin = nullptr;

    /**
        Plugin parameters: the parameter index.
    */
    int pluginParam = -1;

    /**
        Send levels: the index into CompiledTrack::sends.
    */
    int send = -1;
};

/**
    The linear gain of a normalised fader position: the square law of MIDI learn (0 dB at about 0.71).

    @param position  0..1.
    @return          Linear gain, 0..2.
*/
[[nodiscard]] inline float faderGain (double position) noexcept
{
    const auto p = std::clamp (position, 0.0, 1.0);
    return static_cast<float> (2.0 * p * p);
}

/**
    Raises a peak-hold meter value (lock-free; only ever increases it).

    @param meter  The meter.
    @param peak   The new peak.
*/
inline void raisePeak (std::atomic<float>& meter, float peak) noexcept
{
    auto current = meter.load();

    while (peak > current && ! meter.compare_exchange_weak (current, peak))
    {
    }
}

/**
    Equal-power pan gains with unity at the centre (docs/design/mixing-basics.md, M1): sqrt(2)*cos and
    sqrt(2)*sin of (pan + 1) * pi/4, each capped at 1.

    @param pan  Pan position from -1 to +1.
    @return     The left and right gains.
*/
[[nodiscard]] inline std::array<float, 2> panGains (float pan) noexcept
{
    const auto angle = (std::clamp (pan, -1.0f, 1.0f) + 1.0f) * juce::MathConstants<float>::pi / 4.0f;
    return { std::min (1.0f, juce::MathConstants<float>::sqrt2 * std::cos (angle)),
             std::min (1.0f, juce::MathConstants<float>::sqrt2 * std::sin (angle)) };
}

/**
    State shared between the audio and message threads for one modulation slot (docs/design/modulation.md, M5).

    The base is the parameter's value before modulation: the manual value, or an automation lane's current value.
    lastWritten lets the audio thread notice a manual or automated change and adopt it as the new base.
*/
struct ModulationState
{
    /**
        The parameter value modulation is applied around.
    */
    std::atomic<float> base { 0.0f };

    /**
        The value the audio thread last wrote, or NaN before the first write.
    */
    std::atomic<float> lastWritten { std::numeric_limits<float>::quiet_NaN() };

    /**
        Whether the slot, once gone, has handed its plugin parameter back at the base (CompiledModulation::restore).
    */
    std::atomic<bool> restored { false };

    /**
        The slot's depth, -1..1 (0 while inactive). Set when compiling and by GraphCompiler::update() without a
        recompile, so dragging a depth only stores this.
    */
    std::atomic<float> depth { 0.0f };
};

/**
    One modulation input ready for the audio thread.
*/
struct CompiledModulation
{
    /**
        The input port carrying the summed modulation signal.
    */
    int port = 0;

    /**
        Depth, -1..1.
    */
    float depth = 1.0f;

    /**
        Built-in target: the node, or null for a plugin target.
    */
    Node* node = nullptr;

    /**
        Built-in target: the parameter's index in the node's descriptor.
    */
    size_t param = 0;

    /**
        Built-in target: the parameter's description (range, kind and modulation polarity).
    */
    ParamInfo info {};

    /**
        Plugin target: the plugin node, or null for a built-in target.
    */
    PluginNode* plugin = nullptr;

    /**
        Plugin target: the plugin parameter index, or -1 while unresolved.
    */
    int pluginParam = -1;

    /**
        Plugin target: which output of Note Expression feeds this slot alone, when the plugin takes that per note, or -1
        for a slot with one value for the whole plugin (docs/design/clap-hosting.md, C10).
    */
    int noteSource = -1;

    /**
        Plugin target fed by an LFO or ADSR alone: that source's instances per note, which replace one value for the whole
        plugin. Empty otherwise.
    */
    std::vector<Node*> noteNodes;

    /**
        With noteNodes: the source's output port.
    */
    int notePort = 0;

    /**
        Shared base and last-written value.
    */
    std::shared_ptr<ModulationState> state;

    /**
        True for a slot that no longer exists: it reads no input, and only sets its plugin parameter back to the base,
        once, unless the parameter was changed since modulation last wrote it.
    */
    bool restore = false;

    /**
        The depth to apply now: the shared state's (which follows the model without a recompile), or `depth` for a slot
        without one and for a restore.
    */
    [[nodiscard]] float currentDepth() const noexcept
    {
        return state != nullptr && ! restore ? state->depth.load (std::memory_order_relaxed) : depth;
    }
};

/**
    State of one parameter link shared across the audio thread's passes (docs/design/parameter-linking.md, L8, L10).

    The history is written and read by the one thread that renders the link's target; published is written once per
    block by the audio thread after every track has run, and read by the target's track in the next block.
*/
struct LinkState
{
    /**
        How many past source values the history holds.
    */
    static constexpr size_t historySize = 64;

    /**
        A cross-track link's source value at the end of the last block, or NaN before the first.
    */
    std::atomic<float> published { std::numeric_limits<float>::quiet_NaN() };

    /**
        Past source values, oldest overwritten first, for a link that follows its source late by the latency between
        them.
    */
    std::array<float, historySize> history {};

    /**
        The sample (on the link's own clock) of each value in history.
    */
    std::array<juce::int64, historySize> times {};

    /**
        Where the next value goes in history.
    */
    size_t next = 0;

    /**
        How many values history holds.
    */
    size_t count = 0;

    /**
        The link's clock: the samples it has followed for.
    */
    juce::int64 clock = 0;
};

/**
    A parameter link ready for the audio thread (docs/design/parameter-linking.md, L6). One end is a built-in
    parameter (node and index) or a plugin parameter (plugin and index).
*/
struct CompiledLink
{
    /**
        How the source's value becomes the target's.
    */
    LinkMapping mapping;

    /**
        A built-in source: its node, or null.
    */
    Node* sourceNode = nullptr;

    /**
        A built-in source: the parameter's index.
    */
    size_t sourceParam = 0;

    /**
        A built-in source: its description.
    */
    ParamInfo sourceInfo {};

    /**
        A plugin source: its node, or null.
    */
    PluginNode* sourcePlugin = nullptr;

    /**
        A plugin source: the parameter's index.
    */
    int sourcePluginParam = -1;

    /**
        Whether the source is on another track: the link then reads LinkState::published (L10).
    */
    bool crossTrack = false;

    /**
        How late the target follows its source, in samples: the latency between them on one path (L8).
    */
    int delaySamples = 0;

    /**
        A built-in target: its node, or null.
    */
    Node* targetNode = nullptr;

    /**
        A built-in target: the parameter's index.
    */
    size_t targetParam = 0;

    /**
        A built-in target: its description.
    */
    ParamInfo targetInfo {};

    /**
        A built-in target in a voice graph: its instances in the other voice slots (docs/design/phase12-voice-scope.md,
        V11).
    */
    std::vector<Node*> targetVoices;

    /**
        A plugin target: its node, or null.
    */
    PluginNode* targetPlugin = nullptr;

    /**
        A plugin target: the parameter's index.
    */
    int targetPluginParam = -1;

    /**
        State shared across the audio thread's passes.
    */
    std::shared_ptr<LinkState> state;
};

/**
    The source of a link to another track, which the audio thread publishes after each block (L10).
*/
struct CompiledLinkSource
{
    /**
        A built-in source: its node, or null.
    */
    Node* node = nullptr;

    /**
        A built-in source: the parameter's index.
    */
    size_t param = 0;

    /**
        A plugin source: its node, or null.
    */
    PluginNode* plugin = nullptr;

    /**
        A plugin source: the parameter's index.
    */
    int pluginParam = -1;

    /**
        Where the value goes.
    */
    std::shared_ptr<LinkState> state;
};

/**
    One step of a compiled track program.
*/
struct CompiledStep
{
    /**
        Non-zero on the first step of a loop in the graph: how many steps, this one included, run again and again in small
        chunks until the block is done (docs/design/feedback-routing.md, R2).
    */
    size_t regionSteps = 0;

    /**
        Which part of a Feedback node this step runs.
    */
    LoopPart loopPart = LoopPart::whole;
    /**
        The node to run, or nullptr for a mix step (sum mixSources into the output buffer).
    */
    Node* node = nullptr;

    /**
        Audio input views, one per input port (silence for unconnected or non-audio ports).
    */
    std::vector<AudioInput> audioInputs;

    /**
        Audio output views, one per output port (scratch for unconnected or non-audio ports).
    */
    std::vector<AudioOutput> audioOutputs;

    /**
        Index of the event buffer feeding the node's event input, or -1.
    */
    int eventInput = -1;

    /**
        Index of the event buffer the node writes, or -1.
    */
    int eventOutput = -1;

    /**
        Whether the node has an event input, connected or not.
    */
    bool takesEvents = false;

    /**
        The serial (Node::serial()) of the node writing the buffer at eventInput, or 0. When it differs from what the
        node heard last, the engine sends it NoteEventType::sourceChanged first (docs/design/midi-effects.md, ME6).
    */
    juce::uint64 eventSource = 0;

    /**
        For mix steps: the buffers to sum.
    */
    std::vector<AudioInput> mixSources;

    /**
        Node steps: ProcessContext::channels. Mix steps: the output's width; a mono one sums each source's
        (L + R) / 2 into its first channel (docs/design/mono-ports.md, MP2).
    */
    int channels = 2;

    /**
        Bit n set: output port n is mono, and its second channel is scratch.
    */
    juce::uint32 monoOutputs = 0;

    /**
        Modulation slots applied just before this node processes (docs/design/modulation.md, M5).
    */
    std::vector<CompiledModulation> modulations;

    /**
        Parameter links into this step's node, applied just before its modulation (docs/design/parameter-linking.md, L6).
    */
    std::vector<CompiledLink> links;

    /**
        Room for the per-sample values of the built-in parameters the slots target (ModulatedParam): one block per
        slot, or null when no slot targets a built-in parameter. Written by the audio thread only.
    */
    std::unique_ptr<float[]> modulationValues; // NOLINT(modernize-avoid-c-arrays): fixed-size, never resized

    /**
        Voice Instruments: the voice graph's program, handed to the node in ProcessContext::voice.
    */
    std::shared_ptr<const CompiledVoice> voice;

    /**
        Samplers: the compiled zones, handed to the node in ProcessContext::samples.
    */
    std::shared_ptr<const SampleMap> samples;

    /**
        The output ports that lead somewhere, a bit each (ProcessContext::wiredOutputs).
    */
    juce::uint32 wiredOutputs = 1;
};

/**
    One voice slot's copy of a compiled voice graph (docs/design/phase12-voice-scope.md, V8): its own node instances,
    buffers and event buffers, so voices never share state.
*/
struct CompiledVoiceSlot
{
    /**
        Steps in topological order, ending before the Voice Output.
    */
    std::vector<CompiledStep> steps;

    /**
        The slot's nodes, reset when a new note starts in the slot.
    */
    std::vector<Node*> nodes;

    /**
        The slot's audio scratch memory.
    */
    std::unique_ptr<float[]> audioMemory; // NOLINT(modernize-avoid-c-arrays): one flat, fixed allocation

    /**
        The slot's event buffers. Written by the audio thread only.
    */
    std::unique_ptr<EventBuffer[]> eventBuffers; // NOLINT(modernize-avoid-c-arrays): fixed-size, never resized

    /**
        The event buffer the Voice Input feeds: the Voice Instrument writes the voice's events into it, or -1 when
        nothing reads them.
    */
    int inputEvents = -1;

    /**
        The Voice Output's input: what the voice plays (silence if nothing is connected).
    */
    AudioInput output {};
};

/**
    A Voice Instrument's compiled voice graph: one program per voice slot.
*/
struct CompiledVoice
{
    /**
        The slots, indexed like VoiceAllocator::voices().
    */
    std::array<CompiledVoiceSlot, VoiceAllocator::numVoices> slots;

    /**
        Latency of the voice graph in samples, the same in every slot.
    */
    int latencySamples = 0;
};

/**
    Runs compiled steps in order: applies their modulation, then processes their nodes.

    @param steps         The steps.
    @param eventBuffers  The event buffers the steps' indices refer to.
    @param base          The context every step shares: numSamples, transport, sequence, live events, audio and input
                         (input unshifted). Ports, events and modulation are filled in per step.
    @param sampleOffset  Where in the steps' buffers to read and write: non-zero for the second half of a block split
                         at a loop wrap.
    @param maxBlockSize  The block size the steps' modulation buffers were made for.
*/
void runSteps (std::span<const CompiledStep> steps, EventBuffer* eventBuffers, const ProcessContext& base,
               int sampleOffset, int maxBlockSize) noexcept;

/**
    A compiled track: steps in execution order and where its output ends up.
*/
struct CompiledTrack
{
    /**
        The track's id.
    */
    ObjectId id = 0;

    /**
        Steps in topological order.
    */
    std::vector<CompiledStep> steps;

    /**
        The track output's channels (silence if nothing is connected).
    */
    AudioInput output {};

    /**
        The track's notes.
    */
    std::shared_ptr<const TrackSequence> sequence;

    /**
        The track's audio clips (audio tracks).
    */
    std::shared_ptr<const AudioSequence> audio;

    /**
        Fader gain shared across compilations.
    */
    std::shared_ptr<TrackState> state;

    /**
        Processing latency of the track's graph in samples, before alignment with the other tracks.
    */
    int latencySamples = 0;

    /**
        Automation lanes that play (docs/design/phase10-automation.md, A3).
    */
    std::vector<CompiledLane> lanes;

    /**
        Parameter links into nodes of the track that have no step of the track's own, such as those in a voice graph:
        applied after the track's automation (docs/design/parameter-linking.md, L6, L11).
    */
    std::vector<CompiledLink> links;

    /**
        Groups and returns: the bus the members or sends are summed into, which feeds the track's chain; -1 for other
        tracks (docs/design/groups-and-sends.md, R6).
    */
    int bus = -1;

    /**
        The bus the track's output is summed into: its group's, or -1 for the master mix.
    */
    int outputBus = -1;

    /**
        How far ahead of the playhead the track runs: the latency between its input and the output.
    */
    int aheadSamples = 0;

    /**
        Indices into CompiledEngine::tracks of the groups the track is in, innermost first, so mute and solo can
        follow the routing (R5).
    */
    std::vector<int> enclosing;

    /**
        Groups: indices into CompiledEngine::tracks of the tracks inside it, at any depth, and the instruments of the
        output tracks inside it (docs/design/multi-output-instruments.md, MO6).
    */
    std::vector<int> inside;

    /**
        The track's sends, in the model's order (R4).
    */
    std::vector<CompiledSend> sends;

    /**
        Whether this is a return track: those stay audible while other tracks are soloed (R5).
    */
    bool isReturn = false;

    /**
        Output tracks: the index into CompiledEngine::tracks of the track whose output it is, or -1
        (docs/design/multi-output-instruments.md, MO6).
    */
    int source = -1;

    /**
        What the track's Output to track nodes add to its output tracks, whatever its fader (MO7).
    */
    std::vector<CompiledBusOutput> busOutputs;

    /**
        Whether the track has output tracks, which makes it only a header: its fader, pan and sends play nothing, and
        what reaches its Track Output goes to its Main output track through busOutputs
        (docs/design/multi-output-instruments.md, MO12). Key sends still run.
    */
    bool header = false;

    /**
        Whether the track reads a key bus (docs/design/sidechain.md, K2): it runs after its key sources (fedBy).
    */
    bool keyed = false;

    /**
        Indices into CompiledEngine::tracks of the tracks that write a bus this track reads (its group, return or
        output track bus, or a key bus of its nodes), in ascending order: it renders once they all have, and their
        contributions to its buses are summed in this order (docs/design/phase2-engine.md, 5.6).
    */
    std::vector<int> fedBy;

    /**
        Indices into CompiledEngine::tracks of the tracks that read a bus this track writes, in ascending order.
    */
    std::vector<int> feeds;
};

/**
    A track's fader, pan and send levels for one chunk, from the track state or ramped by automation
    (docs/design/phase10-automation.md, A3).
*/
struct TrackMix
{
    /**
        Fader gain (linear) at the start of the chunk.
    */
    float gainStart = 1.0f;

    /**
        Fader gain (linear) at the end of the chunk.
    */
    float gainEnd = 1.0f;

    /**
        Pan, -1..1, at the start of the chunk.
    */
    float panStart = 0.0f;

    /**
        Pan, -1..1, at the end of the chunk.
    */
    float panEnd = 0.0f;

    /**
        Each send's level at the start and the end of the chunk (docs/design/groups-and-sends.md, R8).
    */
    std::array<std::array<float, 2>, limits::maxSends> sends {};

    /**
        Whether the track's output had NaN or infinity in this chunk: it is then left out of the mix and its sends
        (docs/design/output-protection.md, S1).
    */
    bool invalid = false;
};

/**
    Everything the audio thread runs, produced by GraphCompiler. Immutable once published, except for the
    scratch memory its steps point into and trackMix, which only the audio thread writes while it runs this engine.
*/
struct CompiledEngine
{
    /**
        The spec the nodes were prepared for. The audio thread outputs silence if the device differs.
    */
    ProcessSpec spec;

    /**
        The tempo points and time signatures (docs/design/tempo-map.md). The audio thread's TimeMap refers to it
        while this snapshot is current.
    */
    TempoMap tempoMap;

    /**
        Whether the metronome clicks.
    */
    bool metronome = false;

    /**
        The transport loop range (docs/design/phase6-arrangement.md, A9).
    */
    LoopRange loop;

    /**
        The largest track latency plus the master track's latency, in samples. Every track is delayed to the
        largest track latency, and tracks see the transport this many samples ahead, so the output lines up
        with the playhead (docs/design/phase4-plugin-hosting.md, P7).
    */
    int latencySamples = 0;

    /**
        Latency of the master track's chain in samples (included in latencySamples).
    */
    int masterLatencySamples = 0;

    /**
        Tracks in mixing order: a group's members before the group.
    */
    std::vector<CompiledTrack> tracks;

    /**
        Fader, pan and send levels per track for the chunk being rendered (audio-thread scratch, one entry per
        entry in tracks).
    */
    mutable std::vector<TrackMix> trackMix;

    /**
        Two channels of spec.maxBlockSize samples per group or return bus (CompiledTrack::bus), then per key bus.
    */
    std::unique_ptr<float[]> busMemory; // NOLINT(modernize-avoid-c-arrays): one flat, fixed allocation

    /**
        Number of group, return and key buses in busMemory.
    */
    int numBuses = 0;

    /**
        Per bus: the index into tracks of the track that reads it (the group, return or output track, or the keyed
        track whose node reads the key bus), or -1.
    */
    std::vector<int> busReaders;

    /**
        Per track: how many of its CompiledTrack::fedBy tracks have still to render in the chunk being rendered
        (audio-thread and track-thread scratch).
    */
    std::unique_ptr<std::atomic<int>[]> waiting; // NOLINT(modernize-avoid-c-arrays): fixed-size, never resized

    /**
        The order the tracks became ready to render in the chunk being rendered, -1 for a place not filled yet
        (audio-thread and track-thread scratch).
    */
    std::unique_ptr<std::atomic<int>[]> readyOrder; // NOLINT(modernize-avoid-c-arrays): fixed-size, never resized

    /**
        The master track, run on the summed tracks (docs/design/mixing-basics.md, M4). Without one, the tracks
        mix straight to the output.
    */
    std::optional<CompiledTrack> master;

    /**
        The sources of links that cross tracks, published after every block (docs/design/parameter-linking.md, L10).
    */
    std::vector<CompiledLinkSource> linkSources;

    /**
        Two channels of spec.maxBlockSize samples where the tracks are summed for the master track.
    */
    std::unique_ptr<float[]> mixMemory; // NOLINT(modernize-avoid-c-arrays): one flat, fixed allocation

    /**
        Node instances referenced by the steps; keeps them alive while this engine may run.
    */
    std::vector<std::shared_ptr<Node>> nodes;

    /**
        Audio scratch memory: every buffer the steps read or write lives here. Each track has its own arena, so
        tracks with no bus input can run at the same time without overwriting each other's buffers.
    */
    std::unique_ptr<float[]> audioMemory; // NOLINT(modernize-avoid-c-arrays): one flat, fixed allocation

    /**
        Number of audio buffer slots in audioMemory: the sum of every track's arena (each starts with a silence and
        a scratch slot), plus the master track's if there is one.
    */
    int numAudioSlots = 0;

    /**
        Event buffers the steps read and write.
    */
    std::unique_ptr<EventBuffer[]> eventBuffers; // NOLINT(modernize-avoid-c-arrays): fixed-size, never resized

    /**
        Number of entries in eventBuffers.
    */
    size_t numEventBuffers = 0;
};

} // namespace dawg
