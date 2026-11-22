#include "dawg/BuiltinNodes.h"
#include "dawg/CompiledEngine.h"
#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"
#include "dawg/VoiceAllocator.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

// The voice scope's node types (docs/design/phase12-voice-scope.md): the Voice Instrument, which owns a voice graph and
// plays it once per note (V8, V9), and the voice graph's fixed Voice Input and Voice Output, which the compiler
// handles itself (V1).

namespace dawg
{

namespace
{
constexpr std::array<PortInfo, 1> notesPort { { { PortType::events, "Notes" } } };
constexpr std::array<PortInfo, 1> audioPort { { { PortType::audio, "Audio" } } };
constexpr auto voiceInstrumentInputs = withModulationInputs (notesPort);

const NodeDescriptor voiceInstrumentDescriptor {
    nodetype::voiceInstrument, "Voice Instrument", voiceInstrumentInputs, audioPort, {}, true, NodeCategory::instrument
};

const NodeDescriptor voiceInputDescriptor { nodetype::voiceInput, "Voice Input", {}, notesPort, {}, false,
                                            NodeCategory::io };

const NodeDescriptor voiceOutputDescriptor { nodetype::voiceOutput, "Voice Output", audioPort, {}, {}, false,
                                             NodeCategory::io };

// A component's ports are its own (ComponentGraph), and it and its two fixed nodes are gone before the compiler plans.
const NodeDescriptor componentDescriptor { nodetype::component, "Component", {}, {}, {}, true, NodeCategory::effect };

const NodeDescriptor componentInputDescriptor {
    nodetype::componentInput, "Input", {}, {}, {}, false, NodeCategory::io
};

const NodeDescriptor componentOutputDescriptor { nodetype::componentOutput, "Output", {}, {}, {}, false,
                                                 NodeCategory::io };

// The Voice Input and Output never run: the compiler makes no step for them. Should one run anyway, it is silent.
class SilentVoiceNode final : public Node
{
public:
    explicit SilentVoiceNode (const NodeDescriptor& descriptor)
        : Node (descriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        for (const auto& out : context.audioOutputs)
            for (auto* channel : out)
                juce::FloatVectorOperations::clear (channel, context.numSamples);
    }
};

constexpr double fadeSeconds = 0.005;        // a stolen or choked voice fades out over 5 ms (V9)
constexpr double silenceSeconds = 0.05;      // a released voice ends after 50 ms of silence
constexpr double safetyTailSeconds = 10.0;   // or 10 s after its release, whatever it plays
constexpr float silenceThreshold = 3.16e-5f; // -90 dB

// Plays a voice graph once per note (docs/design/phase12-voice-scope.md, V9). The VoiceAllocator decides which slot a
// note plays in; this node keeps each slot's sound: whether its program runs, its fade-out, a note waiting for that
// fade to end, and the events the slot has not played yet. Events arrive at block offsets; each slot renders its block
// in pieces split where its program starts, and where its fade ends.
//
// On the engine's track threads (ProcessContext::parallel), the slots with something to play render at the same time,
// each into its own buffer; their buffers are then added up in slot order, as the slots add up one after another,
// so the sum comes out the same to the bit.
class VoiceInstrumentNode final : public Node
{
public:
    VoiceInstrumentNode()
        : Node (voiceInstrumentDescriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_maxBlockSize = spec.maxBlockSize;
        m_fadeSamples = std::max (1, static_cast<int> (std::lround (fadeSeconds * spec.sampleRate)));
        m_silenceSamples = static_cast<juce::int64> (std::llround (silenceSeconds * spec.sampleRate));
        m_safetySamples = static_cast<juce::int64> (std::llround (safetyTailSeconds * spec.sampleRate));
        m_slotAudio.assign (m_slots.size() * 2 * static_cast<size_t> (std::max (m_maxBlockSize, 1)), 0.0f);
        reset();
    }

    void reset() noexcept override
    {
        m_voices.clear();

        for (auto& slot : m_slots)
            slot = Slot {};
        m_active.store (0);
    }

    void process (const ProcessContext& context) noexcept override
    {
        for (const auto& out : context.audioOutputs)
            for (auto* channel : out)
                juce::FloatVectorOperations::clear (channel, context.numSamples);

        if (context.voice == nullptr || context.transport == nullptr || context.audioOutputs.empty())
            return;

        if (context.sequence != nullptr)
        {
            m_voices.adoptSequence (*context.sequence);
            endChokedVoices (0);
        }

        if (context.eventsIn != nullptr)
            for (const auto& event : context.eventsIn->events())
                handle (event);

        const auto& out = context.audioOutputs[0];
        std::array<size_t, VoiceAllocator::numVoices> playing {};
        size_t numPlaying = 0;

        for (size_t i = 0; i < m_slots.size(); ++i)
            if (m_slots[i].running || m_slots[i].starting)
                playing[numPlaying++] = i;
            else
                m_peaks[i] = render (i, context, { out[0], out[1] }); // plays nothing; skips its events

        if (context.parallel != nullptr && numPlaying > 1 && ! m_slotAudio.empty())
        {
            renderAtOnce (context, std::span (playing).first (numPlaying));
        }
        else
        {
            for (const auto i : std::span (playing).first (numPlaying))
                m_peaks[i] = render (i, context, { out[0], out[1] });
        }

        int active = 0;

        for (size_t i = 0; i < m_slots.size(); ++i)
        {
            if (m_slots[i].freeing)
            {
                m_slots[i].freeing = false;
                m_voices.free (i);
            }

            finishBlock (i, context.numSamples, m_peaks[i]);
            active += m_voices.voices()[i].state != VoiceState::free ? 1 : 0;
        }

        m_active.store (active);

        // Bypassed, it's muted like any instrument; its notes go on, so unbypassing joins them where they are.
        if (m_bypassed.load())
            for (const auto& port : context.audioOutputs)
                for (auto* channel : port)
                    juce::FloatVectorOperations::clear (channel, context.numSamples);
    }

    void configure (const GraphNode& node) override { m_bypassed.store (node.bypassed); }

    [[nodiscard]] juce::var meters() const override
    {
        juce::var result (new juce::DynamicObject());
        result.getDynamicObject()->setProperty ("voices", m_active.load());
        return result;
    }

private:
    static constexpr size_t queueCapacity = 64;

    struct Slot
    {
        bool running = false;            // the slot's program plays
        bool fading = false;             // its output fades out, ending at fadeEnd
        int fadeEnd = 0;                 // block offset (may lie beyond the block)
        bool starting = false;           // a note waits to start at startAt, after the fade if there is one
        int startAt = 0;                 // block offset (may lie beyond the block)
        juce::int64 releasedSamples = 0; // since its note was released
        juce::int64 silentSamples = 0;   // since its output last rose above the threshold
        std::array<NoteEvent, queueCapacity> queue {}; // events not played yet, by offset (may lie beyond the block)
        size_t queued = 0;
        size_t played = 0;    // queue entries already handed to the program
        bool freeing = false; // its voice is freed when the block's slots are done (they may render at once)
    };

    // Inserts an event after those at the same offset or earlier. A full queue drops it.
    static void enqueue (Slot& slot, NoteEvent event, int offset) noexcept
    {
        if (slot.queued == slot.queue.size())
            return;

        event.sampleOffset = offset;
        auto at = slot.queued;

        while (at > slot.played && slot.queue[at - 1].sampleOffset > offset)
        {
            slot.queue[at] = slot.queue[at - 1];
            --at;
        }

        slot.queue[at] = event;
        ++slot.queued;
    }

    // An event for a slot's note plays at its time, or when the note starts if that is later.
    static void route (Slot& slot, const NoteEvent& event) noexcept
    {
        enqueue (slot, event, slot.starting ? std::max (event.sampleOffset, slot.startAt) : event.sampleOffset);
    }

    // Drops a note that waits to start, with its events.
    static void cancelStart (Slot& slot) noexcept
    {
        if (! slot.starting)
            return;

        auto kept = slot.played;

        for (auto i = slot.played; i < slot.queued; ++i)
            if (slot.queue[i].sampleOffset < slot.startAt)
                slot.queue[kept++] = slot.queue[i];

        slot.queued = kept;
        slot.starting = false;
    }

    void startFade (Slot& slot, int offset) const noexcept
    {
        if (! slot.fading)
        {
            slot.fading = true;
            slot.fadeEnd = offset + m_fadeSamples;
        }
    }

    // A note-on: its slot starts it now, or after the slot's current sound has faded (a stolen voice).
    void startNote (const NoteEvent& event) noexcept
    {
        const auto index = m_voices.handle (event);

        if (! index.has_value())
            return;

        auto& slot = m_slots[*index];
        cancelStart (slot);

        if (slot.running)
            startFade (slot, event.sampleOffset);

        slot.starting = true;
        slot.startAt = slot.fading ? std::max (event.sampleOffset, slot.fadeEnd) : event.sampleOffset;

        // A note starting while the pedal is down is sustained from the start.
        if (m_voices.sustainDown())
        {
            NoteEvent pedal;
            pedal.type = NoteEventType::sustain;
            pedal.value = 1.0f;
            enqueue (slot, pedal, slot.startAt);
        }

        enqueue (slot, event, slot.startAt);
    }

    void handle (const NoteEvent& event) noexcept
    {
        switch (event.type)
        {
            case NoteEventType::noteOn:
                startNote (event);
                return;

            case NoteEventType::legato:
            {
                // The voice's program goes on as the new note; with no voice to continue, the note starts as usual.
                const auto voices = m_voices.voices();
                const auto continued = std::ranges::find_if (
                    voices, [&event] (const Voice& voice)
                    { return voice.state == VoiceState::held && voice.noteId == event.fromNoteId; });

                if (continued == voices.end())
                {
                    auto start = event;
                    start.type = NoteEventType::noteOn;
                    startNote (start);
                    return;
                }

                route (m_slots[static_cast<size_t> (continued - voices.begin())], event);
                break;
            }

            case NoteEventType::noteOff:
            case NoteEventType::choke:
            case NoteEventType::expression:
            {
                const auto voices = m_voices.voices();

                for (size_t i = 0; i < voices.size(); ++i)
                    if (voices[i].state != VoiceState::free && voices[i].noteId == event.noteId)
                        route (m_slots[i], event);

                break;
            }

            case NoteEventType::sustain:
            case NoteEventType::releaseAll:
            case NoteEventType::chokeAll:
            {
                const auto voices = m_voices.voices();

                for (size_t i = 0; i < voices.size(); ++i)
                    if (voices[i].state != VoiceState::free)
                        route (m_slots[i], event);

                break;
            }

            case NoteEventType::sourceChanged:
            {
                // The voices' programs see a choke; the allocator forgets the notes.
                auto choke = event;
                choke.type = NoteEventType::chokeAll;
                const auto voices = m_voices.voices();

                for (size_t i = 0; i < voices.size(); ++i)
                    if (voices[i].state != VoiceState::free)
                        route (m_slots[i], choke);

                break;
            }

            case NoteEventType::controller:
                return; // for plugins only
        }

        std::ignore = m_voices.handle (event);
        endChokedVoices (event.sampleOffset);
    }

    // Choked voices fade out; one choked before it started never starts.
    void endChokedVoices (int offset) noexcept
    {
        const auto voices = m_voices.voices();

        for (size_t i = 0; i < voices.size(); ++i)
        {
            if (voices[i].state != VoiceState::choking)
                continue;

            auto& slot = m_slots[i];
            cancelStart (slot);

            if (slot.running)
                startFade (slot, offset);
            else
                m_voices.free (i);
        }
    }

    // What each slot rendering at once needs: the context and the slots, by piece index.
    struct AtOnce
    {
        VoiceInstrumentNode* node = nullptr;
        const ProcessContext* context = nullptr;
        std::span<const size_t> slots;
    };

    [[nodiscard]] std::array<float*, 2> slotAudio (size_t index) noexcept
    {
        const auto length = static_cast<size_t> (m_maxBlockSize);
        auto* first = m_slotAudio.data() + index * 2 * length;
        return { first, first + length };
    }

    // Renders the playing slots at the same time, each into its own buffer, then adds the buffers to the output in
    // slot order. Touches nothing the slots share: freeing a voice waits until they are all done.
    void renderAtOnce (const ProcessContext& context, std::span<const size_t> slots) noexcept
    {
        AtOnce atOnce { this, &context, slots };

        context.parallel->forEach (
            slots.size(),
            [] (void* data, size_t piece) noexcept
            {
                const auto& work = *static_cast<const AtOnce*> (data);
                const auto index = work.slots[piece];
                const auto audio = work.node->slotAudio (index);

                for (auto* channel : audio)
                    juce::FloatVectorOperations::clear (channel, work.context->numSamples);

                work.node->m_peaks[index] = work.node->render (index, *work.context, audio);
            },
            &atOnce);

        const auto& out = context.audioOutputs[0];

        for (const auto index : slots)
        {
            const auto audio = slotAudio (index);

            for (size_t ch = 0; ch < out.size(); ++ch)
                if (out[ch] != nullptr && context.voice->slots[index].output[ch] != nullptr)
                    juce::FloatVectorOperations::add (out[ch], audio[ch], context.numSamples);
        }
    }

    // Renders one slot's block, adding it into `destination`. Returns its output's peak.
    float render (size_t index, const ProcessContext& context, std::array<float*, 2> destination) noexcept
    {
        auto& slot = m_slots[index];
        const auto numSamples = context.numSamples;
        float peak = 0.0f;
        int position = 0;

        while (true)
        {
            if (slot.fading && slot.fadeEnd <= position)
            {
                slot.fading = false;
                slot.running = false;

                if (! slot.starting)
                    slot.freeing = true;
            }

            if (slot.starting && ! slot.fading && slot.startAt <= position)
                begin (index, *context.voice);

            if (position >= numSamples)
                return peak;

            // The next piece ends where the fade ends or the waiting note starts, both later than here.
            auto end = numSamples;

            if (slot.fading)
                end = std::min (end, slot.fadeEnd);
            else if (slot.starting)
                end = std::min (end, slot.startAt);

            if (slot.running)
                peak = std::max (peak, run (index, context, destination, position, end));
            else
                skipEvents (slot, end);

            position = end;
        }
    }

    // A note starts in a slot: its nodes start from their prepared state (V9).
    void begin (size_t index, const CompiledVoice& voice) noexcept
    {
        auto& slot = m_slots[index];

        for (auto* node : voice.slots[index].nodes)
            node->reset();

        slot.starting = false;
        slot.running = true;
        slot.releasedSamples = 0;
        slot.silentSamples = 0;
    }

    static void skipEvents (Slot& slot, int end) noexcept
    {
        while (slot.played < slot.queued && slot.queue[slot.played].sampleOffset < end)
            ++slot.played;
    }

    // Runs a slot's program over [from, to) of the block and adds its output. Returns the output's peak.
    float run (size_t index, const ProcessContext& context, std::array<float*, 2> into, int from, int to) noexcept
    {
        auto& slot = m_slots[index];
        const auto& program = context.voice->slots[index];

        if (program.inputEvents >= 0)
        {
            auto& events = program.eventBuffers[static_cast<size_t> (program.inputEvents)];
            events.clear();

            for (; slot.played < slot.queued && slot.queue[slot.played].sampleOffset < to; ++slot.played)
            {
                auto event = slot.queue[slot.played];
                event.sampleOffset = std::max (0, event.sampleOffset - from);
                std::ignore = events.add (event);
            }
        }
        else
        {
            skipEvents (slot, to);
        }

        const auto numSamples = to - from;
        const auto& outer = *context.transport;
        const auto tickAt = [&] (int sample)
        {
            return outer.startTick
                   + (outer.endTick - outer.startTick) * static_cast<double> (sample) / context.numSamples;
        };

        auto transport = outer;
        transport.startTick = tickAt (from);
        transport.endTick = tickAt (to);
        transport.startSample += from;

        if (from > 0)
            transport.started = transport.stopped = transport.jumped = transport.looped = false;

        ProcessContext base;
        base.numSamples = numSamples;
        base.transport = &transport;
        base.sequence = context.sequence;
        runSteps (program.steps, program.eventBuffers.get(), base, 0, m_maxBlockSize);

        float peak = 0.0f;
        for (size_t ch = 0; ch < into.size(); ++ch)
        {
            const auto* source = program.output[ch];

            if (source == nullptr || into[ch] == nullptr)
                continue;

            auto* destination = into[ch] + from;

            for (int s = 0; s < numSamples; ++s)
            {
                const auto gain = slot.fading ? std::clamp (static_cast<float> (slot.fadeEnd - (from + s))
                                                                / static_cast<float> (m_fadeSamples),
                                                            0.0f, 1.0f)
                                              : 1.0f;
                const auto sample = source[s] * gain;
                destination[s] += sample;
                peak = std::max (peak, std::abs (sample));
            }
        }

        return peak;
    }

    // Carries a slot's state into the next block, and ends a released voice once it is silent (V9).
    void finishBlock (size_t index, int numSamples, float peak) noexcept
    {
        auto& slot = m_slots[index];
        auto kept = size_t { 0 };

        for (auto i = slot.played; i < slot.queued; ++i)
        {
            slot.queue[kept] = slot.queue[i];
            slot.queue[kept++].sampleOffset -= numSamples;
        }

        slot.queued = kept;
        slot.played = 0;
        slot.fadeEnd -= numSamples;
        slot.startAt -= numSamples;

        if (! slot.running || slot.fading || m_voices.voices()[index].state != VoiceState::releasing)
            return;

        slot.releasedSamples += numSamples;
        slot.silentSamples = peak < silenceThreshold ? slot.silentSamples + numSamples : 0;

        if (slot.silentSamples >= m_silenceSamples)
        {
            slot.running = false;
            m_voices.free (index);
        }
        else if (slot.releasedSamples >= m_safetySamples)
        {
            startFade (slot, 0);
        }
    }

    VoiceAllocator m_voices { StealPolicy::releasedFirst };
    std::array<Slot, VoiceAllocator::numVoices> m_slots {};
    std::array<float, VoiceAllocator::numVoices> m_peaks {};
    std::vector<float> m_slotAudio; // each slot's two channels, for slots rendering at once
    int m_maxBlockSize = 0;
    int m_fadeSamples = 1;
    juce::int64 m_silenceSamples = 0;
    juce::int64 m_safetySamples = 0;
    std::atomic<int> m_active { 0 };
    std::atomic<bool> m_bypassed { false };
};
} // namespace

void registerVoiceNodes (NodeRegistry& registry)
{
    registry.add (voiceInstrumentDescriptor, [] { return std::make_unique<VoiceInstrumentNode>(); });

    for (const auto* descriptor : { &voiceInputDescriptor, &voiceOutputDescriptor, &componentDescriptor,
                                    &componentInputDescriptor, &componentOutputDescriptor })
        registry.add (*descriptor, [descriptor] { return std::make_unique<SilentVoiceNode> (*descriptor); });
}

} // namespace dawg
