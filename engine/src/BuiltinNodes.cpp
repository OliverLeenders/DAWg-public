#include "dawg/BuiltinNodes.h"

#include "dawg/AudioPlayer.h"
#include "dawg/DrumSynth.h"
#include "dawg/FaustNode.h"
#include "dawg/MidiEffects.h"
#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"
#include "dawg/PluginNode.h"
#include "dawg/Sequence.h"
#include "dawg/VoiceAllocator.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace dawg
{

namespace
{
// ---------------------------------------------------------------------------------------------------------
// Port and parameter tables (static storage, referenced by the descriptors).

constexpr std::array<PortInfo, 1> stereoPort { { { PortType::audio, "Audio" } } };
constexpr std::array<PortInfo, 1> eventPort { { { PortType::events, "Notes" } } };
constexpr auto stereoModPorts = withModulationInputs (stereoPort);

// A port that is mono when everything feeding the node is (docs/design/mono-ports.md, MP1).
constexpr std::array<PortInfo, 1> adaptivePort { { { PortType::audio, "Audio", adaptiveChannels } } };
constexpr auto adaptiveModPorts = withModulationInputs (adaptivePort);
constexpr auto synthInputs = withModulationInputs (eventPort);

[[nodiscard]] float decibelsToGain (float db) noexcept
{
    return std::pow (10.0f, db / 20.0f);
}

[[nodiscard]] int sampleOffsetOf (const Transport& transport, Ticks tick, int numSamples) noexcept
{
    const auto sample = std::llround (transport.timeMap.tickToSample (static_cast<double> (tick)));
    return static_cast<int> (std::clamp<juce::int64> (sample - transport.startSample, 0, numSamples - 1));
}

// ---------------------------------------------------------------------------------------------------------
// Sequencer: turns the track's compiled notes into note events.

const NodeDescriptor sequencerDescriptor {
    nodetype::sequencer, "Sequencer", {}, eventPort, {}, false, NodeCategory::io
};

class SequencerNode final : public Node
{
public:
    SequencerNode()
        : Node (sequencerDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        auto& out = *context.eventsOut;
        const auto& transport = *context.transport;
        const auto& sequence = *context.sequence;

        if (&sequence != m_sequence)
            adoptSequence (sequence, transport.startTick);

        if (transport.stopped && m_activeCount > 0)
        {
            std::ignore = out.add ({ 0, NoteEventType::releaseAll, 0, nullptr });
            m_activeCount = 0;
        }

        if (transport.stopped && m_sustainDown)
            sendController (sustainKey, 0.0, 0, out);

        for (const auto& live : context.liveEvents)
            std::ignore = out.add (live);

        if (! transport.playing)
        {
            if (transport.jumped)
                m_cursor = context.sequence->firstStartingAtOrAfter (transport.startTick);
            return;
        }

        if (transport.jumped && m_activeCount > 0)
        {
            std::ignore = out.add ({ 0, NoteEventType::chokeAll, 0, nullptr });
            m_activeCount = 0;
        }

        if (transport.looped)
        {
            // The loop wrapped: release what is sounding (tails ring on), then chase at the loop start.
            for (size_t i = 0; i < m_activeCount; ++i)
                std::ignore = out.add ({ 0, NoteEventType::noteOff, m_active[i].id, nullptr });

            m_activeCount = 0;
        }

        if (transport.started || transport.jumped || transport.looped)
            chase (context, out);

        endActiveNotes (context, out);
        startNewNotes (context, out);
        playControllers (context, out);
    }

private:
    struct ActiveNote
    {
        ObjectId id;
        Ticks end;
        bool linked; // the note that continues its voice starts at the end: no note-off
    };

    static constexpr size_t maxActiveNotes = 256;

    // Re-resolves sounding notes against a new sequence; notes that no longer exist are simply forgotten
    // (instruments choke them when they re-resolve their own pointers).
    void adoptSequence (const TrackSequence& sequence, double startTick) noexcept
    {
        size_t kept = 0;

        for (size_t i = 0; i < m_activeCount; ++i)
            if (const auto* note = sequence.find (m_active[i].id))
                m_active[kept++] = { note->id, note->holdUntil, note->linkTo != 0 };

        m_activeCount = kept;
        m_sequence = &sequence;
        m_cursor = sequence.firstStartingAtOrAfter (startTick);
    }

    void chase (const ProcessContext& context, EventBuffer& out) noexcept
    {
        const auto startTick = context.transport->startTick;
        const auto notes = context.sequence->notes();
        m_activeCount = 0;
        m_cursor = context.sequence->firstStartingAtOrAfter (startTick);

        for (size_t i = 0; i < m_cursor; ++i)
        {
            const auto& note = notes[i];

            // A linked note holds until the next note starts, there too, so that note has a voice to continue.
            const auto until = static_cast<double> (note.holdUntil);

            if ((until > startTick || (note.linkTo != 0 && until >= startTick)) && m_activeCount < maxActiveNotes)
            {
                std::ignore = out.add ({ 0, NoteEventType::noteOn, note.id, &note });
                m_active[m_activeCount++] = { note.id, note.holdUntil, note.linkTo != 0 };
            }
        }
    }

    void endActiveNotes (const ProcessContext& context, EventBuffer& out) noexcept
    {
        const auto& transport = *context.transport;
        size_t kept = 0;

        for (size_t i = 0; i < m_activeCount; ++i)
        {
            const auto& active = m_active[i];

            if (static_cast<double> (active.end) < transport.endTick)
            {
                // A linked note's voice goes on to the next note, unless an edit left that behind the playhead.
                if (! active.linked || static_cast<double> (active.end) < transport.startTick)
                    std::ignore = out.add ({ sampleOffsetOf (transport, active.end, context.numSamples),
                                             NoteEventType::noteOff, active.id, nullptr });
            }
            else
            {
                m_active[kept++] = active;
            }
        }

        m_activeCount = kept;
    }

    void startNewNotes (const ProcessContext& context, EventBuffer& out) noexcept
    {
        const auto& transport = *context.transport;
        const auto notes = context.sequence->notes();

        for (; m_cursor < notes.size() && static_cast<double> (notes[m_cursor].start) < transport.endTick; ++m_cursor)
        {
            const auto& note = notes[m_cursor];

            if (static_cast<double> (note.start) < transport.startTick)
                continue; // added behind the playhead while playing; not chased mid-playback

            NoteEvent start { sampleOffsetOf (transport, note.start, context.numSamples), NoteEventType::noteOn,
                              note.id, &note };

            if (note.linkFrom != 0)
            {
                start.type = NoteEventType::legato;
                start.fromNoteId = note.linkFrom;
            }

            std::ignore = out.add (start);

            if (static_cast<double> (note.holdUntil) < transport.endTick)
            {
                if (note.linkTo == 0)
                    std::ignore = out.add ({ sampleOffsetOf (transport, note.end, context.numSamples),
                                             NoteEventType::noteOff, note.id, nullptr });
            }
            else if (m_activeCount < maxActiveNotes)
            {
                m_active[m_activeCount++] = { note.id, note.holdUntil, note.linkTo != 0 };
            }
        }
    }

    // Clip controller curves (docs/design/phase10-automation.md, A5): the value at the chunk start, then every point
    // inside it, sent only when the 7-bit (pitch bend: 14-bit) value changes. A jump, start or loop resends.
    void playControllers (const ProcessContext& context, EventBuffer& out) noexcept
    {
        const auto& transport = *context.transport;
        const auto segments = context.sequence->controllerSegments();

        if (transport.started || transport.jumped || transport.looped)
            m_lastSent.fill (-1);

        auto sustainCovered = false;

        for (const auto& segment : segments)
        {
            if (static_cast<double> (segment.start) >= transport.endTick)
                break;

            if (static_cast<double> (segment.end) <= transport.startTick)
                continue;

            sustainCovered = sustainCovered || segment.controller == sustainKey;
            const auto from = std::max (transport.startTick, static_cast<double> (segment.start));
            sendController (segment.controller, segment.valueAt (from), offsetOf (transport, from, context.numSamples),
                            out);

            for (const auto& point : segment.points)
            {
                const auto tick = static_cast<double> (segment.start + point.time - segment.contentAt);

                if (tick > from && tick < transport.endTick && tick < static_cast<double> (segment.end))
                    sendController (segment.controller, point.value, offsetOf (transport, tick, context.numSamples),
                                    out);
            }
        }

        // Leaving the clips with the pedal down lifts it.
        if (m_sustainDown && ! sustainCovered)
            sendController (sustainKey, 0.0, 0, out);
    }

    [[nodiscard]] static int offsetOf (const Transport& transport, double tick, int numSamples) noexcept
    {
        return sampleOffsetOf (transport, std::llround (tick), numSamples);
    }

    void sendController (int controller, double value, int offset, EventBuffer& out) noexcept
    {
        const auto range = controller == controllerKeyPitchBend ? 16383.0 : 127.0;
        const auto quantised = static_cast<int> (std::lround (std::clamp (value, 0.0, 1.0) * range));
        auto& last = m_lastSent[static_cast<size_t> (controller)];

        if (quantised == last)
            return;

        last = quantised;
        NoteEvent event { offset, NoteEventType::controller, 0, nullptr };

        if (controller == sustainKey)
        {
            m_sustainDown = value >= 0.5;
            event.type = NoteEventType::sustain;
            event.value = m_sustainDown ? 1.0f : 0.0f;
        }
        else
        {
            event.lane = controller;
            event.value = static_cast<float> (value);

            if (controller == controllerKeyPitchBend)
            {
                event.lane = controllerPitchBend;
                event.value = static_cast<float> (2.0 * value - 1.0);
            }
            else if (controller == controllerKeyPressure)
            {
                event.lane = controllerPressure;
            }
        }

        std::ignore = out.add (event);
    }

    static constexpr int sustainKey = 64;

    std::array<int, controllerKeyPressure + 1> m_lastSent = []
    {
        std::array<int, controllerKeyPressure + 1> values {};
        values.fill (-1);
        return values;
    }();
    bool m_sustainDown = false;
    const TrackSequence* m_sequence = nullptr;
    size_t m_cursor = 0;
    std::array<ActiveNote, maxActiveNotes> m_active {};
    size_t m_activeCount = 0;
};

// ---------------------------------------------------------------------------------------------------------
// MPE synth: sine voices shaped by per-note expression.

constexpr std::array<ParamInfo, 4> synthParams { {
    { "gain", "Gain", -60.0f, 6.0f, -12.0f, ParamKind::continuous, "dB" },
    { "attack", "Attack", 0.001f, 2.0f, 0.005f, ParamKind::continuous, "s", 0.1f },
    { "release", "Release", 0.005f, 5.0f, 0.3f, ParamKind::continuous, "s", 0.5f },
    { "pressureAmount", "Pressure amount", 0.0f, 1.0f, 0.7f },
} };

const NodeDescriptor synthDescriptor { nodetype::mpeSynth,      "MPE Synth", synthInputs, stereoPort, synthParams, true,
                                       NodeCategory::instrument };

class MpeSynthNode final : public Node
{
public:
    MpeSynthNode()
        : Node (synthDescriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_sampleRate = spec.sampleRate;
        reset();
    }

    void reset() noexcept override
    {
        m_voices.clear();
        m_sounds = {};
    }

    void process (const ProcessContext& context) noexcept override
    {
        m_voices.adoptSequence (*context.sequence);

        const auto& out = context.audioOutputs[0];
        juce::FloatVectorOperations::clear (out[0], context.numSamples);
        juce::FloatVectorOperations::clear (out[1], context.numSamples);

        readParams();

        int position = 0;

        if (context.eventsIn != nullptr)
        {
            for (const auto& event : context.eventsIn->events())
            {
                render (context, position, event.sampleOffset);

                if (const auto started = m_voices.handle (event))
                    m_sounds[*started] = {};

                position = event.sampleOffset;
            }
        }

        render (context, position, context.numSamples);
        juce::FloatVectorOperations::copy (out[1], out[0], context.numSamples);
    }

private:
    // A voice's sound. Its note is the allocator's voice at the same index.
    struct Sound
    {
        double phase = 0.0;
        float level = 0.0f;
    };

    static constexpr int controlInterval = 32; // samples between expression evaluations
    static constexpr float silence = 1.0e-4f;
    static constexpr float chokeSeconds = 0.005f;

    void readParams() noexcept
    {
        const auto& p = params();
        m_gain = decibelsToGain (p.get (0));
        m_attackStep = 1.0f / std::max (1.0f, p.get (1) * static_cast<float> (m_sampleRate));
        m_releaseCoeff = std::exp (-1.0f / std::max (1.0f, p.get (2) * static_cast<float> (m_sampleRate) / 5.0f));
        m_chokeCoeff = std::exp (-1.0f / std::max (1.0f, chokeSeconds * static_cast<float> (m_sampleRate) / 5.0f));
        m_pressureAmount = static_cast<double> (p.get (3));
    }

    void render (const ProcessContext& context, int from, int to) noexcept
    {
        float* out = context.audioOutputs[0][0];

        for (size_t i = 0; i < VoiceAllocator::numVoices; ++i)
        {
            if (m_voices.voices()[i].state == VoiceState::free)
                continue;

            for (int s = from; s < to; s += controlInterval)
                renderSegment (context, i, out, s, std::min (to, s + controlInterval));
        }
    }

    // Renders [from, to) of one voice with expression interpolated linearly across the segment.
    void renderSegment (const ProcessContext& context, size_t index, float* out, int from, int to) noexcept
    {
        const auto& transport = *context.transport;
        const auto ticksPerSample = (transport.endTick - transport.startTick) / context.numSamples;
        const auto start = m_voices.voices()[index].expression;
        const auto end = m_voices.updateExpression (index, transport.startTick + ticksPerSample * to, transport);
        const auto velocity = m_voices.voices()[index].velocity;
        auto& sound = m_sounds[index];
        const auto length = static_cast<double> (to - from);

        for (int s = from; s < to; ++s)
        {
            const auto x = static_cast<double> (s - from + 1) / length;
            const auto pitch = start.pitch + (end.pitch - start.pitch) * x;
            const auto pressure = start.pressure + (end.pressure - start.pressure) * x;
            const auto timbre = start.timbre + (end.timbre - start.timbre) * x;

            advanceEnvelope (index);

            if (m_voices.voices()[index].state == VoiceState::free)
                return;

            const auto frequency = 440.0 * std::exp2 ((pitch - 69.0) / 12.0);
            const auto drive = 1.0 + timbre * 7.0;
            const auto shaped = std::tanh (drive * std::sin (sound.phase)) / std::tanh (drive);
            const auto expressionGain = 1.0 - m_pressureAmount + m_pressureAmount * (0.2 + 0.8 * pressure);

            out[s] += static_cast<float> (shaped * expressionGain) * velocity * sound.level * m_gain;

            sound.phase += juce::MathConstants<double>::twoPi * frequency / m_sampleRate;

            if (sound.phase > juce::MathConstants<double>::twoPi)
                sound.phase -= juce::MathConstants<double>::twoPi;
        }
    }

    void advanceEnvelope (size_t index) noexcept
    {
        auto& sound = m_sounds[index];
        const auto state = m_voices.voices()[index].state;

        switch (state)
        {
            case VoiceState::held:
                sound.level = std::min (1.0f, sound.level + m_attackStep);
                break;

            case VoiceState::releasing:
                sound.level *= m_releaseCoeff;
                break;

            case VoiceState::choking:
                sound.level *= m_chokeCoeff;
                break;

            case VoiceState::free:
                return;
        }

        if (state != VoiceState::held && sound.level < silence)
        {
            m_voices.free (index);
            sound = {};
        }
    }

    double m_sampleRate = 48000.0;
    VoiceAllocator m_voices;
    std::array<Sound, VoiceAllocator::numVoices> m_sounds {};
    float m_gain = 1.0f;
    float m_attackStep = 1.0f;
    float m_releaseCoeff = 0.0f;
    float m_chokeCoeff = 0.0f;
    double m_pressureAmount = 0.0;
};

// ---------------------------------------------------------------------------------------------------------
// Gain

// The gain in dB is a trim and is not modulated; the amplitude is the modulation target (docs/design/modulation.md, M6):
// tremolo around 100 %, or an envelope opening it from 0 %.
constexpr std::array<ParamInfo, 2> gainParams { {
    { "gainDb",
      "Gain",
      -96.0f,
      12.0f,
      0.0f,
      ParamKind::continuous,
      "dB",
      0.0f,
      {},
      ModulationPolarity::bipolar,
      false },
    { "amplitude", "Amplitude", 0.0f, 200.0f, 100.0f, ParamKind::continuous, "%" },
} };

const NodeDescriptor gainDescriptor { nodetype::gain, "Gain", adaptiveModPorts,     adaptivePort,
                                      gainParams,     true,   NodeCategory::utility };

class GainNode final : public Node
{
public:
    GainNode()
        : Node (gainDescriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_modulationCoefficient = modulationSmoothingCoefficient (spec.sampleRate);
        reset();
    }

    void reset() noexcept override { m_current = gainFor (params().get (0), params().get (1)); }

    void process (const ProcessContext& context) noexcept override
    {
        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];
        const auto* modulatedAmplitude = context.modulatedValues (1);
        const auto channels = static_cast<size_t> (context.channels);

        // Modulated: follow the per-sample values with a short smoothing, so an envelope is a sharp amp envelope.
        if (modulatedAmplitude != nullptr)
        {
            const auto trim = params().get (0);

            for (int s = 0; s < context.numSamples; ++s)
            {
                const auto target = gainFor (trim, modulatedAmplitude[s]);
                m_current += (target - m_current) * m_modulationCoefficient;

                for (size_t ch = 0; ch < channels; ++ch)
                    out[ch][s] = in[ch][s] * m_current;
            }

            return;
        }

        // Ramp linearly over the block so parameter changes don't click.
        const auto target = gainFor (params().get (0), params().get (1));
        const auto step = (target - m_current) / static_cast<float> (context.numSamples);

        for (size_t ch = 0; ch < channels; ++ch)
            for (int s = 0; s < context.numSamples; ++s)
                out[ch][s] = in[ch][s] * (m_current + step * static_cast<float> (s + 1));

        m_current = target;
    }

private:
    [[nodiscard]] static float gainFor (float db, float amplitude) noexcept
    {
        return decibelsToGain (db) * amplitude * 0.01f; // the amplitude is a percentage
    }

    float m_current = 1.0f;
    float m_modulationCoefficient = 1.0f;
};

// ---------------------------------------------------------------------------------------------------------
// Track output: a sink; the compiler routes its input to the mixer and never runs it.

const NodeDescriptor trackOutputDescriptor { nodetype::trackOutput, "Track Output", stereoPort, {}, {}, false,
                                             NodeCategory::io };

class TrackOutputNode final : public Node
{
public:
    TrackOutputNode()
        : Node (trackOutputDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& /*context*/) noexcept override {}
};

// ---------------------------------------------------------------------------------------------------------
// Output to track (docs/design/multi-output-instruments.md, MO3): a sink like the track output; the compiler hands its
// input to the mixer, which adds it to the output track's bus.

const NodeDescriptor busOutputDescriptor { nodetype::busOutput,  "Output to track", stereoPort, {}, {}, true,
                                           NodeCategory::utility };

class BusOutputNode final : public Node
{
public:
    BusOutputNode()
        : Node (busOutputDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& /*context*/) noexcept override {}
};

// ---------------------------------------------------------------------------------------------------------
// Mix input: heads the master chain. The engine passes the summed tracks as the context's input.

const NodeDescriptor mixInputDescriptor {
    nodetype::mixInput, "Mix Input", {}, stereoPort, {}, false, NodeCategory::io
};

class MixInputNode final : public Node
{
public:
    MixInputNode()
        : Node (mixInputDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        const auto& out = context.audioOutputs[0];

        for (size_t ch = 0; ch < 2; ++ch)
        {
            if (context.input[ch] != nullptr)
                juce::FloatVectorOperations::copy (out[ch], context.input[ch], context.numSamples);
            else
                juce::FloatVectorOperations::clear (out[ch], context.numSamples);
        }
    }
};

// ---------------------------------------------------------------------------------------------------------
// Latency compensation delay (internal).

const NodeDescriptor delayDescriptor { "dawg.internal.delay", "Delay compensation", stereoPort, stereoPort, {}, false };

class DelayCompensationNode final : public Node
{
public:
    explicit DelayCompensationNode (int delaySamples)
        : Node (delayDescriptor),
          m_delay (delaySamples)
    {
        jassert (delaySamples > 0);
    }

    void prepare (const ProcessSpec& /*spec*/) override
    {
        for (auto& line : m_lines)
            line.assign (static_cast<size_t> (m_delay), 0.0f);

        m_position = 0;
    }

    void reset() noexcept override
    {
        for (auto& line : m_lines)
            std::ranges::fill (line, 0.0f);
    }

    void process (const ProcessContext& context) noexcept override
    {
        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];
        const auto length = static_cast<size_t> (m_delay);

        // Mono paths delay only their one channel (docs/design/mono-ports.md, MP6).
        for (size_t ch = 0; ch < static_cast<size_t> (context.channels); ++ch)
        {
            auto position = m_position;

            for (int s = 0; s < context.numSamples; ++s)
            {
                const auto input = in[ch][s];
                out[ch][s] = m_lines[ch][position];
                m_lines[ch][position] = input;
                position = (position + 1) % length;
            }
        }

        m_position = (m_position + static_cast<size_t> (context.numSamples)) % length;
    }

    [[nodiscard]] int latencySamples() const noexcept override { return 0; }

private:
    int m_delay;
    std::array<std::vector<float>, 2> m_lines;
    size_t m_position = 0;
};
// ---------------------------------------------------------------------------------------------------------
// A component's mono or stereo port (internal; docs/design/mono-ports.md, MP11): passes audio through at its width,
// so the compiler folds a stereo signal into a mono port, and reads a mono one on both sides of a stereo port.

constexpr std::array<PortInfo, 1> monoAudioPort { { { PortType::audio, "Audio", 1 } } };
const NodeDescriptor monoPortDescriptor { nodetype::monoPort, "Mono port", monoAudioPort, monoAudioPort, {}, false };
const NodeDescriptor stereoPortDescriptor { nodetype::stereoPort, "Stereo port", stereoPort, stereoPort, {}, false };

class PortWidthNode final : public Node
{
public:
    PortWidthNode (const NodeDescriptor& descriptor, size_t channels)
        : Node (descriptor),
          m_channels (channels)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        for (size_t ch = 0; ch < m_channels; ++ch)
            if (context.audioInputs[0][ch] != context.audioOutputs[0][ch])
                juce::FloatVectorOperations::copy (context.audioOutputs[0][ch], context.audioInputs[0][ch],
                                                   context.numSamples);
    }

private:
    size_t m_channels;
};
} // namespace

std::unique_ptr<Node> makeDelayCompensationNode (int delaySamples)
{
    return std::make_unique<DelayCompensationNode> (delaySamples);
}

void registerBuiltinNodes (NodeRegistry& registry)
{
    registry.add (sequencerDescriptor, [] { return std::make_unique<SequencerNode>(); });
    registry.add (synthDescriptor, [] { return std::make_unique<MpeSynthNode>(); });
    registry.add (gainDescriptor, [] { return std::make_unique<GainNode>(); });
    registry.add (monoPortDescriptor, [] { return std::make_unique<PortWidthNode> (monoPortDescriptor, 1); });
    registry.add (stereoPortDescriptor, [] { return std::make_unique<PortWidthNode> (stereoPortDescriptor, 2); });
    registry.add (trackOutputDescriptor, [] { return std::make_unique<TrackOutputNode>(); });
    registry.add (busOutputDescriptor, [] { return std::make_unique<BusOutputNode>(); });
    registry.add (mixInputDescriptor, [] { return std::make_unique<MixInputNode>(); });
    registry.add (pluginInstrumentDescriptor(), [] { return std::make_unique<PluginNode> (true); });
    registry.add (audioPlayerDescriptor(), [] { return makeAudioPlayerNode(); });
    registry.add (pluginEffectDescriptor(), [] { return std::make_unique<PluginNode> (false); });
    registry.add (faustDescriptor(), [] { return std::make_unique<FaustNode> (nullptr, nullptr); });
    registerModulationNodes (registry);
    registerOscillator (registry);
    registerVoiceNodes (registry);
    registerSampler (registry);
    registerDrumSynth (registry);
    registerEffectNodes (registry);
    registerSplitNodes (registry);
    registerMidiEffects (registry);
#if DAWG_PRIVATE_NODES
    registerPrivateNodes (registry);
#endif
}

} // namespace dawg
