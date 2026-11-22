#include "dawg/ClapNoteEncoder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dawg
{

namespace
{
constexpr int controlInterval = 32;            // samples between evaluations of a note's expression, as for MPE
constexpr double noteEndTimeoutSeconds = 10.0; // a plugin that never sends NOTE_END (C11)
constexpr double changeThreshold = 1.0e-4;
constexpr float sustainThreshold = 0.5f;

[[nodiscard]] uint32_t timeOf (const NoteEvent& event, int numSamples) noexcept
{
    return static_cast<uint32_t> (std::clamp (event.sampleOffset, 0, numSamples - 1));
}

[[nodiscard]] double tickAt (const ProcessContext& context, int sample) noexcept
{
    const auto& transport = *context.transport;
    return transport.startTick
           + (transport.endTick - transport.startTick) * static_cast<double> (sample) / context.numSamples;
}

[[nodiscard]] uint8_t sevenBit (double value) noexcept
{
    return static_cast<uint8_t> (std::clamp (static_cast<int> (std::lround (value * 127.0)), 0, 127));
}
} // namespace

void ClapNoteEncoder::prepare (double sampleRate, int maxBlockSize)
{
    m_sampleRate = sampleRate;
    m_sourceOutput.assign (static_cast<size_t> (maxBlockSize), 0.0f);
    m_sourceSilence.assign (static_cast<size_t> (maxBlockSize), 0.0f);
    m_slots = {};
    m_sequence = nullptr;
    m_pedal = false;
}

size_t ClapNoteEncoder::soundingNotes() const noexcept
{
    return static_cast<size_t> (std::ranges::count_if (m_slots, [] (const Slot& slot) { return slot.used; }));
}

void ClapNoteEncoder::noteEnded (int32_t noteId) noexcept
{
    for (auto& slot : m_slots)
        if (slot.used && slot.clapId == noteId && slot.released)
            slot = {};
}

ClapNoteEncoder::Slot* ClapNoteEncoder::holding (ObjectId noteId) noexcept
{
    for (auto& slot : m_slots)
        if (slot.used && ! slot.released && slot.noteId == noteId)
            return &slot;

    return nullptr;
}

void ClapNoteEncoder::process (const ProcessContext& context, ClapNativeInput& output) noexcept
{
    if (context.sequence != nullptr && context.sequence != m_sequence)
        adoptSequence (*context.sequence, output);

    auto position = 0;

    if (context.eventsIn != nullptr)
    {
        for (const auto& event : context.eventsIn->events())
        {
            updateAll (position, event.sampleOffset, context, output);
            handle (event, context, output);
            position = std::max (position, event.sampleOffset);
        }
    }

    updateAll (position, context.numSamples, context, output);
    runSources (context, output);

    // A plugin that never sends NOTE_END: its notes end by the clock.
    const auto timeout = static_cast<juce::int64> (noteEndTimeoutSeconds * m_sampleRate);

    for (auto& slot : m_slots)
    {
        if (slot.used && slot.released)
        {
            slot.releasedSamples += context.numSamples;

            if (slot.releasedSamples >= timeout)
                slot = {};
        }
    }
}

// Notes that are gone from the sequence: held ones end, released ones just stop following.
void ClapNoteEncoder::adoptSequence (const TrackSequence& sequence, ClapNativeInput& output) noexcept
{
    for (auto& slot : m_slots)
    {
        if (! slot.used || slot.noteId < 0 || slot.note == nullptr)
            continue;

        slot.note = sequence.find (slot.noteId);

        if (slot.note == nullptr && ! slot.released)
        {
            output.noteChoke (0, slot.clapId, slot.key);
            slot = {};
        }
    }

    m_sequence = &sequence;
}

void ClapNoteEncoder::handle (const NoteEvent& event, const ProcessContext& context, ClapNativeInput& output) noexcept
{
    const auto time = timeOf (event, context.numSamples);

    switch (event.type)
    {
        case NoteEventType::noteOn:
            if (event.note != nullptr)
                start (event, context, output);
            break;

        case NoteEventType::legato:
            if (event.note != nullptr)
                legato (event, context, output);
            break;

        case NoteEventType::noteOff:
            if (auto* slot = holding (event.noteId))
                release (*slot, time, output);
            break;

        case NoteEventType::choke:
            if (auto* slot = holding (event.noteId))
            {
                output.noteChoke (time, slot->clapId, slot->key);
                *slot = {};
            }
            break;

        case NoteEventType::releaseAll:
            // Live notes have negative ids: the player still holds them.
            for (auto& slot : m_slots)
                if (slot.used && ! slot.released && slot.noteId >= 0)
                    release (slot, time, output);
            break;

        case NoteEventType::chokeAll:
            output.chokeAll (time);

            for (auto& slot : m_slots)
                if (slot.used && slot.noteId >= 0)
                    slot = {};

            break;

        case NoteEventType::sourceChanged:
            // Every note ends, live ones too, and no pointer is kept (docs/design/midi-effects.md, ME6).
            output.chokeAll (time);

            for (auto& slot : m_slots)
                slot = {};

            m_pedal = false;
            break;

        case NoteEventType::sustain:
            m_pedal = event.value >= sustainThreshold;

            if (! m_pedal)
            {
                for (auto& slot : m_slots)
                {
                    if (slot.used && slot.sustained)
                    {
                        slot.sustained = false;
                        release (slot, time, output);
                    }
                }
            }

            break;

        case NoteEventType::expression:
            if (auto* slot = holding (event.noteId))
            {
                slot->live = true;

                switch (static_cast<Lane> (event.lane))
                {
                    case Lane::pitch:
                        slot->livePitch = static_cast<double> (event.value);
                        break;
                    case Lane::pressure:
                        slot->livePressure = static_cast<double> (event.value);
                        break;
                    case Lane::timbre:
                        slot->liveTimbre = static_cast<double> (event.value);
                        break;
                    case Lane::vibrato:
                        break;
                }

                update (*slot, event.sampleOffset, context, output);
            }
            break;

        case NoteEventType::controller:
            controller (event, output);
            break;
    }
}

void ClapNoteEncoder::controller (const NoteEvent& event, ClapNativeInput& output) noexcept
{
    const auto time = static_cast<uint32_t> (std::max (0, event.sampleOffset));

    if (event.lane == controllerPitchBend)
    {
        const auto bend =
            std::clamp (static_cast<int> (std::lround ((static_cast<double> (event.value) + 1.0) * 8192.0)), 0, 16383);
        output.controller (time, 0xe0, static_cast<uint8_t> (bend & 127), static_cast<uint8_t> (bend >> 7));
    }
    else if (event.lane == controllerPressure)
    {
        output.controller (time, 0xd0, sevenBit (static_cast<double> (event.value)), 0);
    }
    else if (event.lane >= 0 && event.lane < 128)
    {
        output.controller (time, 0xb0, static_cast<uint8_t> (event.lane), sevenBit (static_cast<double> (event.value)));
    }
}

// A free slot, or else the oldest: released notes first, since their voices are on the way out.
ClapNoteEncoder::Slot& ClapNoteEncoder::allocate (uint32_t time, ClapNativeInput& output) noexcept
{
    Slot* best = nullptr;

    for (auto& slot : m_slots)
    {
        if (! slot.used)
            return slot;

        const auto better = best == nullptr || (slot.released && ! best->released)
                            || (slot.released == best->released && slot.age < best->age);

        if (better)
            best = &slot;
    }

    if (! best->released)
        output.noteChoke (time, best->clapId, best->key);

    return *best;
}

void ClapNoteEncoder::start (const NoteEvent& event, const ProcessContext& context, ClapNativeInput& output) noexcept
{
    const auto time = timeOf (event, context.numSamples);
    const auto key = std::clamp (static_cast<int> (std::lround (event.note->pitch)), 0, 127);

    // A plugin that can't overlap notes on a key plays them as MIDI would (C8).
    if (! output.playsOverlappingNotes())
        for (auto& other : m_slots)
            if (other.used && ! other.released && other.key == key)
                release (other, time, output);

    auto& slot = allocate (time, output);
    slot = {};
    slot.used = true;
    slot.noteId = event.noteId;
    slot.note = event.note;
    slot.clapId = m_nextId;
    m_nextId = m_nextId == std::numeric_limits<int32_t>::max() ? 0 : m_nextId + 1;
    slot.key = key;
    slot.age = ++m_counter;

    output.noteOn (time, slot.clapId, key, static_cast<double> (event.note->velocity));
    slot.startedAt = static_cast<int> (time);
    addSourceEvent (slot, event);
    update (slot, static_cast<int> (time), context, output);
}

// A linked note goes on under the id of the note before it (C12). With none to continue, it starts.
void ClapNoteEncoder::legato (const NoteEvent& event, const ProcessContext& context, ClapNativeInput& output) noexcept
{
    auto* slot = holding (event.fromNoteId);

    if (slot == nullptr || slot->note == nullptr)
    {
        start (event, context, output);
        return;
    }

    const auto tick = tickAt (context, event.sampleOffset);
    const auto& map = context.transport->timeMap.tempoMap();
    auto pitch = slot->note->pitch + slot->note->pitchOffset (tick, map);
    auto pressure = slot->note->evaluate (Lane::pressure, tick);
    auto timbre = slot->note->evaluate (Lane::timbre, tick);
    auto velocity = slot->note->velocity;
    slot->glide.apply (*slot->note, tick, map, pitch, pressure, timbre, velocity);
    slot->glide.begin (pitch, pressure, timbre, velocity);
    slot->noteId = event.noteId;
    slot->note = event.note;
    addSourceEvent (*slot, event);
    update (*slot, event.sampleOffset, context, output);
}

void ClapNoteEncoder::release (Slot& slot, uint32_t time, ClapNativeInput& output) const noexcept
{
    if (m_pedal)
    {
        slot.sustained = true;
        return;
    }

    const auto velocity = slot.note != nullptr ? static_cast<double> (slot.note->releaseVelocity) : 0.0;
    output.noteOff (time, slot.clapId, slot.key, velocity);
    slot.released = true;
    addSourceEvent (slot, { static_cast<int> (time), NoteEventType::noteOff, slot.noteId, nullptr });
}

void ClapNoteEncoder::update (Slot& slot, int sample, const ProcessContext& context, ClapNativeInput& output) noexcept
{
    if (slot.note == nullptr)
        return;

    const auto tick = tickAt (context, sample);
    const auto& note = *slot.note;
    const auto& map = context.transport->timeMap.tempoMap();
    auto pitch = note.pitch + (slot.live ? slot.livePitch : note.pitchOffset (tick, map));
    auto pressure = slot.live ? slot.livePressure : note.evaluate (Lane::pressure, tick);
    auto timbre = slot.live ? slot.liveTimbre : note.evaluate (Lane::timbre, tick);
    auto velocity = note.velocity;

    if (! slot.live)
        slot.glide.apply (note, tick, map, pitch, pressure, timbre, velocity);

    const auto time = static_cast<uint32_t> (std::clamp (sample, 0, context.numSamples - 1));
    const auto send = [&] (ClapNativeInput::Expression which, double value, double& last)
    {
        if (std::abs (value - last) > changeThreshold)
        {
            output.noteExpression (time, slot.clapId, slot.key, which, value);
            last = value;
        }
    };

    send (ClapNativeInput::Expression::tuning, pitch - slot.key, slot.tuning);
    send (ClapNativeInput::Expression::pressure, pressure, slot.pressure);
    send (ClapNativeInput::Expression::brightness, timbre, slot.brightness);
    updateModulations (slot, time, context, pitch, pressure, timbre, static_cast<double> (velocity), output);
}

// Each note's own amount for the slots fed by Note Expression (docs/design/clap-hosting.md, C10). A slot that went away
// takes its amount back from the notes still sounding.
void ClapNoteEncoder::updateModulations (Slot& slot, uint32_t time, const ProcessContext& context, double pitch,
                                         double pressure, double timbre, double velocity,
                                         ClapNativeInput& output) noexcept
{
    for (auto& sent : slot.modulations)
    {
        if (sent.parameter >= 0
            && std::ranges::none_of (context.noteModulations, [&sent] (const NoteModulation& modulation)
                                     { return modulation.parameter == sent.parameter; }))
        {
            output.modulateNoteParameter (time, slot.clapId, slot.key, sent.parameter, 0.0);
            sent = {};
        }
    }

    for (const auto& modulation : context.noteModulations)
    {
        if (! modulation.nodes.empty())
            continue;

        const std::array sources { pitchSignal (pitch), pressure, timbre, velocity, slot.released ? 0.0 : 1.0 };
        const auto source = sources[static_cast<size_t> (std::clamp (modulation.source, 0, 4))];
        sendModulation (slot, time, modulation.parameter,
                        source * static_cast<double> (modulation.depth) + static_cast<double> (modulation.shared),
                        output);
    }
}

void ClapNoteEncoder::sendModulation (Slot& slot, uint32_t time, int parameter, double amount,
                                      ClapNativeInput& output) noexcept
{
    auto found = std::ranges::find (slot.modulations, parameter, &SentModulation::parameter);

    if (found == slot.modulations.end())
        found = std::ranges::find (slot.modulations, -1, &SentModulation::parameter);

    if (found != slot.modulations.end() && std::abs (amount - found->amount) > changeThreshold)
    {
        output.modulateNoteParameter (time, slot.clapId, slot.key, parameter, amount);
        found->parameter = parameter;
        found->amount = amount;
    }
}

void ClapNoteEncoder::addSourceEvent (Slot& slot, const NoteEvent& event) noexcept
{
    if (slot.sourceEventCount < slot.sourceEvents.size())
        slot.sourceEvents[slot.sourceEventCount++] = event;
}

// Each note's own LFO or ADSR, run over the block with that note's events (docs/design/clap-hosting.md, C10).
void ClapNoteEncoder::runSources (const ProcessContext& context, ClapNativeInput& output) noexcept
{
    constexpr size_t maxPorts = 16;
    const auto numSamples = context.numSamples;

    for (size_t index = 0; index < m_slots.size(); ++index)
    {
        auto& slot = m_slots[index];

        if (! slot.used)
            continue;

        for (const auto& modulation : context.noteModulations)
        {
            if (index >= modulation.nodes.size() || numSamples > static_cast<int> (m_sourceOutput.size()))
                continue;

            auto& node = *modulation.nodes[index];

            if (slot.startedAt >= 0)
                node.reset();

            m_sourceEvents.clear();

            for (size_t i = 0; i < slot.sourceEventCount; ++i)
                std::ignore = m_sourceEvents.add (slot.sourceEvents[i]);

            std::array<AudioInput, maxPorts> inputs {};
            std::array<AudioOutput, maxPorts> outputs {};
            inputs.fill ({ m_sourceSilence.data(), m_sourceSilence.data() });
            outputs.fill ({ m_sourceOutput.data(), m_sourceOutput.data() });

            auto run = context;
            run.audioInputs = std::span (inputs).first (std::min (maxPorts, node.descriptor().inputs.size()));
            run.audioOutputs = std::span (outputs).first (std::min (maxPorts, node.descriptor().outputs.size()));
            run.eventsIn = &m_sourceEvents;
            run.eventsOut = nullptr;
            run.modulatedParams = {};
            run.noteModulations = {};
            node.process (run);

            const auto first = std::max (slot.startedAt, 0);

            for (auto s = first; s < numSamples; s = (s / controlInterval + 1) * controlInterval)
                sendModulation (slot, static_cast<uint32_t> (s), modulation.parameter,
                                static_cast<double> (m_sourceOutput[static_cast<size_t> (s)])
                                        * static_cast<double> (modulation.depth)
                                    + static_cast<double> (modulation.shared),
                                output);
        }

        slot.sourceEventCount = 0;
        slot.startedAt = -1;
    }
}

// On the grid of whole intervals of the block, so what is sent doesn't depend on where events fall.
void ClapNoteEncoder::updateAll (int from, int to, const ProcessContext& context, ClapNativeInput& output) noexcept
{
    auto sample = from + (controlInterval - from % controlInterval) % controlInterval;

    for (; sample < to; sample += controlInterval)
        for (auto& slot : m_slots)
            if (slot.used)
                update (slot, sample, context, output);
}

} // namespace dawg
