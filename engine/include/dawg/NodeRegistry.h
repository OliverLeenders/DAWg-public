#pragma once

#include "dawg/Node.h"

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace dawg
{

/**
    Type identifiers of the built-in nodes.
*/
namespace nodetype
{
/**
    Turns the track's clips into note events.
*/
inline constexpr const char* sequencer = "dawg.sequencer";

/**
    Built-in MPE synthesiser.
*/
inline constexpr const char* mpeSynth = "dawg.mpeSynth";

/**
    Stereo gain.
*/
inline constexpr const char* gain = "dawg.gain";

/**
    The track output. Exactly one per track; cannot be added or removed by users.
*/
inline constexpr const char* trackOutput = "dawg.trackOutput";

/**
    Output to track: takes the signal wired to it out of the graph to one of the track's output tracks
    (docs/design/multi-output-instruments.md, MO3). Track graphs only.
*/
inline constexpr const char* busOutput = "dawg.busOutput";

/**
    Plays an audio track's clips (docs/design/phase7-audio-tracks.md, T8). Exactly one per audio track;
    cannot be added or removed by users.
*/
inline constexpr const char* audioPlayer = "dawg.audioPlayer";

/**
    Heads the master track's chain: outputs the sum of all tracks (docs/design/mixing-basics.md, M4). Exactly
    one per master track; cannot be added or removed by users.
*/
inline constexpr const char* mixInput = "dawg.mixInput";

/**
    Four-band EQ (docs/design/phase8-node-graph.md, G1).
*/
inline constexpr const char* eq = "dawg.eq";

/**
    Multimode state-variable filter with drive.
*/
inline constexpr const char* filter = "dawg.filter";

/**
    Anti-aliased tanh saturation with a bias for even harmonics (docs/design/saturator.md).
*/
inline constexpr const char* saturator = "dawg.saturator";

/**
    A hard clip with an adjustable knee at a ceiling, anti-aliased like the Saturator (docs/design/clipper.md).
*/
inline constexpr const char* clipper = "dawg.clipper";

/**
    Waveshaping with a choice of curves (folds, rectifiers, a tube-like curve, a drawn one), anti-aliased like the
    Saturator (docs/design/waveshaper.md).
*/
inline constexpr const char* waveshaper = "dawg.waveshaper";

/**
    An echo in time with the tempo or free, with ping-pong and filtered repeats
    (docs/design/sampler-midi-delay-limiter.md, section 1).
*/
inline constexpr const char* delay = "dawg.delay";

/**
    A bit-depth and sample-rate reducer: a quantiser with fractional depths and a sample-and-hold at a rate in Hz or
    a note (docs/design/bit-crusher.md).
*/
inline constexpr const char* bitcrusher = "dawg.bitcrusher";

/**
    A delay line that a loop in a graph may close through, and the only thing that may close one
    (docs/design/feedback-routing.md, R1).
*/
inline constexpr const char* feedback = "dawg.feedback";

/**
    A look-ahead brickwall limiter (docs/design/sampler-midi-delay-limiter.md, section 2).
*/
inline constexpr const char* limiter = "dawg.limiter";

/**
    Compressor.
*/
inline constexpr const char* compressor = "dawg.compressor";

/**
    A noise gate with hysteresis, hold, a range floor, lookahead and a sidechain (docs/design/gate-expander.md).
*/
inline constexpr const char* gate = "dawg.gate";

/**
    A downward expander: 1:ratio below the threshold, limited by a range (docs/design/gate-expander.md, N10).
*/
inline constexpr const char* expander = "dawg.expander";

/**
    Gain, pan, width, phase and mono.
*/
inline constexpr const char* utility = "dawg.utility";

/**
    Four stereo inputs, each with a gain, summed.
*/
inline constexpr const char* mix = "dawg.mix";

/**
    Splits its input into 2 to 4 bands, one output each, with Linkwitz-Riley crossovers, and a Dry output through the
    same all-passes (docs/design/split-nodes.md, SP1-SP3).
*/
inline constexpr const char* bandSplit = "dawg.bandSplit";

/**
    Splits a stereo input into mid and side, or left and right, each a mono output (docs/design/split-nodes.md, SP4).
*/
inline constexpr const char* stereoSplit = "dawg.stereoSplit";

/**
    Joins mid and side, or left and right, each a mono input, into a stereo output, with a gain each (SP5).
*/
inline constexpr const char* stereoMerge = "dawg.stereoMerge";

/**
    An algorithmic reverb: seeded early reflections and a feedback delay network (docs/design/loop-reverb.md).
*/
inline constexpr const char* loopReverb = "dawg.loopReverb";

/**
    A convolution reverb: the signal convolved with an impulse response from the audio pool
    (docs/design/convolution-reverb.md).
*/
inline constexpr const char* convolutionReverb = "dawg.convolutionReverb";

/**
    A stereo chorus: up to four modulated delay taps per channel (docs/design/modulation-effects.md, X5).
*/
inline constexpr const char* chorus = "dawg.chorus";

/**
    A stereo flanger with feedback and through-zero flanging (docs/design/modulation-effects.md, X7, X8).
*/
inline constexpr const char* flanger = "dawg.flanger";

/**
    A stereo phaser: a swept chain of 2 to 12 first-order allpasses (docs/design/modulation-effects.md, X10).
*/
inline constexpr const char* phaser = "dawg.phaser";

/**
    A low-frequency oscillator modulation source (docs/design/modulation.md, M4).
*/
inline constexpr const char* lfo = "dawg.lfo";

/**
    A MIDI- and audio-triggered ADSR modulation source (docs/design/modulation.md, M3).
*/
inline constexpr const char* adsr = "dawg.adsr";

/**
    A modulation source with a note's pitch, pressure, timbre, velocity and gate (docs/design/phase12-voice-scope.md,
    V2).
*/
inline constexpr const char* noteExpression = "dawg.noteExpression";

/**
    A track's eight macro knobs, a modulation source whose knobs feed macro slots anywhere on the track
    (docs/design/macros-and-modulation-column.md, MC5). One per track, in the track's own graph.
*/
inline constexpr const char* macros = "dawg.macros";

/**
    A band-limited oscillator with unison; its pitch is a modulation target (docs/design/phase12-voice-scope.md, V3,
    V14).
*/
inline constexpr const char* oscillator = "dawg.oscillator";

/**
    An instrument that plays a sample per note from key zones (docs/design/sampler-midi-delay-limiter.md, section 4).
*/
inline constexpr const char* sampler = "dawg.sampler";

/**
    A synthesised drum kit: sixteen pads on keys 36-51, each a voice model with its own parameters
    (docs/design/drum-synth.md, DR1).
*/
inline constexpr const char* drumSynth = "dawg.drumSynth";

/**
    The Sampler's sound for a voice graph, one note per instance; what a converted Sampler holds. Only in voice graphs.
*/
inline constexpr const char* sampleVoice = "dawg.sampleVoice";

/**
    A Faust script (docs/design/phase11-faust-panels.md, F2): ports and parameters come from the compiled script.
*/
inline constexpr const char* faust = "dawg.faust";

/**
    An instrument whose voice graph runs once per sounding note (docs/design/phase12-voice-scope.md, V1).
*/
inline constexpr const char* voiceInstrument = "dawg.voiceInstrument";

/**
    A voice graph's fixed source: the events of the voice's note (V1). Only in voice graphs.
*/
inline constexpr const char* voiceInput = "dawg.voiceInput";

/**
    A voice graph's fixed sink: the voice's audio, which the Voice Instrument sums (V1). Only in voice graphs.
*/
inline constexpr const char* voiceOutput = "dawg.voiceOutput";

/**
    A box around part of a graph, with the ports it was given (docs/design/component-nodes.md, N1). The compiler replaces it by what's in it.
*/
inline constexpr const char* component = "dawg.component";

/**
    A component's fixed source: what comes in through its input ports. Only in components.
*/
inline constexpr const char* componentInput = "dawg.componentInput";

/**
    A component's fixed sink: what goes out through its output ports. Only in components.
*/
inline constexpr const char* componentOutput = "dawg.componentOutput";

/**
    The Arpeggiator, a MIDI effect (docs/design/midi-effects.md, ME10).
*/
inline constexpr const char* arpeggiator = "dawg.arpeggiator";

/**
    The Chord tool, a MIDI effect: one note in, a chord out (docs/design/midi-effects.md, ME11).
*/
inline constexpr const char* chord = "dawg.chord";

/**
    Scale Lock, a MIDI effect: notes moved to, or filtered by, a scale (docs/design/midi-effects.md, ME12).
*/
inline constexpr const char* scaleLock = "dawg.scaleLock";

/**
    Note Repeat, a MIDI effect (docs/design/midi-effects.md, ME13).
*/
inline constexpr const char* noteRepeat = "dawg.noteRepeat";

/**
    The Step Sequencer, a device of the MIDI effect kind that makes notes from patterns of steps inside sequencer clips
    (docs/design/step-sequencer.md, SS1).
*/
inline constexpr const char* stepSequencer = "dawg.stepSequencer";

/**
    Returns true for the MIDI effects (docs/design/midi-effects.md, ME1): note events in, note events out.

    @param type  A node type identifier.
    @return      True for an Arpeggiator, Chord, Scale Lock, Note Repeat or Step Sequencer.
*/
[[nodiscard]] inline bool isMidiEffect (const juce::String& type)
{
    return type == arpeggiator || type == chord || type == scaleLock || type == noteRepeat || type == stepSequencer;
}

/**
    Returns true for the node types a component may hold (docs/design/component-nodes.md, N3): all but those there can only be one of.

    @param type  A node type identifier.
    @return      True if a component can hold it.
*/
[[nodiscard]] inline bool allowedInComponent (const juce::String& type)
{
    return type != sequencer && type != mixInput && type != trackOutput && type != voiceInput && type != voiceOutput
           && type != macros && type != busOutput;
}

/**
    A hosted instrument plugin (note events in, audio out). Added with the addPlugin command only.
*/
inline constexpr const char* pluginInstrument = "dawg.pluginInstrument";

/**
    A hosted effect plugin (audio in, audio out). Added with the addPlugin command only.
*/
inline constexpr const char* pluginEffect = "dawg.pluginEffect";

/**
    Returns true for the plugin node types.

    @param type  A node type identifier.
    @return      True if the type hosts a plugin.
*/
[[nodiscard]] inline bool isPlugin (const juce::String& type)
{
    return type == pluginInstrument || type == pluginEffect;
}

/**
    Returns true for the modulation source types (docs/design/modulation.md).

    @param type  A node type identifier.
    @return      True if the type produces a modulation signal.
*/
[[nodiscard]] inline bool isModulationSource (const juce::String& type)
{
    return type == lfo || type == adsr || type == noteExpression || type == macros;
}

/**
    Returns true for the node types a voice graph may hold (docs/design/phase12-voice-scope.md, V6): the built-in
    sources, effects and utilities, Faust, and its two fixed nodes. Plugins and the nodes that belong to a track
    (sequencer, audio player, mix input, track output, instruments) are not allowed.

    @param type  A node type identifier.
    @return      True if the type can run per voice.
*/
[[nodiscard]] inline bool allowedInVoiceGraph (const juce::String& type)
{
    return ! isPlugin (type) && type != sequencer && type != audioPlayer && type != mixInput && type != trackOutput
           && type != mpeSynth && type != sampler && type != drumSynth && type != voiceInstrument
           && type != convolutionReverb && type != macros && type != busOutput && ! isMidiEffect (type);
}

/**
    Returns true for the node types that play sample zones.

    @param type  A node type identifier.
    @return      True for a Sampler or a Sample Voice.
*/
[[nodiscard]] inline bool playsSamples (const juce::String& type)
{
    return type == sampler || type == sampleVoice;
}

/**
    Returns true for the node types that read one pool file, named by GraphNode::fileId.

    @param type  A node type identifier.
    @return      True for a Convolution Reverb.
*/
[[nodiscard]] inline bool usesAudioFile (const juce::String& type)
{
    return type == convolutionReverb;
}

/**
    Internal: a component's mono port, put where the port is when components are flattened, so what crosses it is
    folded to mono (docs/design/mono-ports.md, MP11). Never in a project.
*/
inline constexpr const char* monoPort = "dawg.internal.monoPort";

/**
    Internal: a component's stereo port, which makes what crosses it stereo (MP11). Never in a project.
*/
inline constexpr const char* stereoPort = "dawg.internal.stereoPort";
} // namespace nodetype

/**
    Returns true if a node type plays a track's notes: its first input takes note events. Modulation sources take
    note events too, but they are extras any track can hold, not its instrument.

    @param descriptor  The node type.
    @return            True for an instrument.
*/
[[nodiscard]] inline bool isInstrument (const NodeDescriptor& descriptor)
{
    return ! nodetype::isModulationSource (descriptor.type) && descriptor.category != NodeCategory::midiEffect
           && ! descriptor.inputs.empty() && descriptor.inputs[0].type == PortType::events;
}

/**
    Maps node type identifiers to descriptions and factories.

    @note  Build a registry on the message thread, then treat it as immutable.
*/
class NodeRegistry final
{
public:
    /**
        Creates a node instance.
    */
    using Factory = std::function<std::unique_ptr<Node>()>;

    /**
        Registers a node type, replacing any earlier registration of the same type.

        @param descriptor  The type's description. Must have static storage duration.
        @param factory     Creates instances of the type.
    */
    void add (const NodeDescriptor& descriptor, Factory factory);

    /**
        Looks up a node type's description.

        @param type  The type identifier.
        @return      The description, or nullptr if the type is unknown.
    */
    [[nodiscard]] const NodeDescriptor* find (const juce::String& type) const;

    /**
        Creates an instance of a node type.

        @param type  The type identifier.
        @return      The new node, or nullptr if the type is unknown.
    */
    [[nodiscard]] std::unique_ptr<Node> create (const juce::String& type) const;

    /**
        Returns all registered node types, sorted by type identifier.
    */
    [[nodiscard]] std::vector<const NodeDescriptor*> descriptors() const;

    /**
        Returns the registry of DAWg's built-in node types.
    */
    [[nodiscard]] static const NodeRegistry& builtin();

private:
    struct Entry
    {
        const NodeDescriptor* descriptor = nullptr;
        Factory factory;
    };

    std::map<juce::String, Entry> m_entries;
};

/**
    Describes a node type for the UI (docs/design/phase8-node-graph.md, G2).

    @param descriptor  The node type.
    @return            { type, name, category, userEditable, inputs: [{ type, name }], outputs: [...],
                         params: [{ id, name, min, max, default, kind, unit, mid, choices }] }
*/
[[nodiscard]] juce::var toVar (const NodeDescriptor& descriptor);

} // namespace dawg
