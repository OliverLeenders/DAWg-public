#include "dawg/MpeEncoder.h"

#include "dawg/Sequence.h"

#include <algorithm>
#include <cmath>

namespace dawg
{

namespace
{
constexpr int masterChannel = 1;
constexpr int firstMemberChannel = 2;
constexpr int timbreController = 74;
constexpr int allSoundOffController = 120;
constexpr int sustainController = 64;
constexpr int pitchWheelCentre = 8192;
constexpr int pitchWheelMax = 16383;

// Registered parameter numbers.
constexpr int rpnPitchBendSensitivity = 0;
constexpr int rpnMpeConfiguration = 6;

// Generous upper bounds per block, used to size the output buffer.
constexpr size_t bytesPerMessage = 16; // MidiBuffer stores a timestamp, a size and up to 3 data bytes
constexpr size_t configurationMessages = size_t { 6 } * 16;
constexpr size_t messagesPerNoteEvent = 8;

void addRpn (juce::MidiBuffer& midi, int channel, int parameter, int value)
{
    midi.addEvent (juce::MidiMessage::controllerEvent (channel, 101, 0), 0);
    midi.addEvent (juce::MidiMessage::controllerEvent (channel, 100, parameter), 0);
    midi.addEvent (juce::MidiMessage::controllerEvent (channel, 6, value), 0);
    midi.addEvent (juce::MidiMessage::controllerEvent (channel, 38, 0), 0);
    // RPN null, so later data-entry messages don't change the parameter by accident.
    midi.addEvent (juce::MidiMessage::controllerEvent (channel, 101, 127), 0);
    midi.addEvent (juce::MidiMessage::controllerEvent (channel, 100, 127), 0);
}

[[nodiscard]] int toSevenBit (double value) noexcept
{
    return std::clamp (static_cast<int> (std::lround (value * 127.0)), 0, 127);
}
} // namespace

size_t MpeEncoder::requiredBufferBytes (int maxBlockSize) noexcept
{
    const auto controlSteps = static_cast<size_t> (maxBlockSize / controlInterval) + 2;
    const auto expression = controlSteps * numMemberChannels * 3;
    const auto notes = (EventBuffer::capacity + 64) * messagesPerNoteEvent; // sequencer + live events
    return (configurationMessages + expression + notes + size_t { 2 } * numMemberChannels) * bytesPerMessage;
}

int MpeEncoder::pack (const MidiOutputSettings& settings) noexcept
{
    return (settings.bendRange << 1) | (settings.mpe ? 1 : 0);
}

void MpeEncoder::prepare (double sampleRate) noexcept
{
    m_sampleRate = sampleRate;
    m_channels = {};
    m_needsConfiguration = true;
    m_sequence = nullptr;
}

void MpeEncoder::setSettings (const MidiOutputSettings& settings) noexcept
{
    m_requestedSettings.store (pack (settings));
}

int MpeEncoder::midiChannel (const Channel& channel) const noexcept
{
    if (! m_settings.mpe)
        return masterChannel;

    return firstMemberChannel + static_cast<int> (&channel - m_channels.data());
}

void MpeEncoder::process (const ProcessContext& context, juce::MidiBuffer& midi) noexcept
{
    if (const auto requested = m_requestedSettings.load(); requested != m_appliedSettings)
    {
        // End sounding notes on the channels they were started on, then switch.
        for (auto& channel : m_channels)
            if (channel.note != nullptr)
                noteOff (channel, 0.0f, 0, midi);

        m_appliedSettings = requested;
        m_settings.mpe = (requested & 1) != 0;
        m_settings.bendRange = requested >> 1;
        m_needsConfiguration = true;
    }

    if (m_needsConfiguration)
    {
        sendConfiguration (midi);
        m_needsConfiguration = false;
    }

    if (context.sequence != nullptr && context.sequence != m_sequence)
        adoptSequence (*context.sequence, midi);

    // Live events arrive through the sequencer, like sequenced ones.
    int position = 0;

    if (context.eventsIn != nullptr)
    {
        for (const auto& event : context.eventsIn->events())
        {
            updateAll (context, position, event.sampleOffset, midi);
            handle (event, context, midi);
            position = event.sampleOffset;
        }
    }

    updateAll (context, position, context.numSamples, midi);
}

void MpeEncoder::sendConfiguration (juce::MidiBuffer& midi) const noexcept
{
    if (! m_settings.mpe)
        return;

    // MPE Configuration Message: a lower zone with all 15 member channels.
    addRpn (midi, masterChannel, rpnMpeConfiguration, numMemberChannels);
    addRpn (midi, masterChannel, rpnPitchBendSensitivity, 2);

    for (int i = 0; i < numMemberChannels; ++i)
        addRpn (midi, firstMemberChannel + i, rpnPitchBendSensitivity, m_settings.bendRange);
}

void MpeEncoder::adoptSequence (const TrackSequence& sequence, juce::MidiBuffer& midi) noexcept
{
    for (auto& channel : m_channels)
    {
        // Live (audition) notes have negative ids and don't belong to any sequence.
        if (channel.note == nullptr || channel.noteId < 0)
            continue;

        channel.note = sequence.find (channel.noteId);

        if (channel.note == nullptr)
        {
            // The note was deleted while it sounded: end it with the values it had.
            midi.addEvent (juce::MidiMessage::noteOff (midiChannel (channel), channel.noteNumber), 0);
            channel.noteId = 0;
            channel.age = ++m_counter;
        }
    }

    m_sequence = &sequence;
}

void MpeEncoder::handle (const NoteEvent& event, const ProcessContext& context, juce::MidiBuffer& midi) noexcept
{
    const auto offset = event.sampleOffset;

    switch (event.type)
    {
        case NoteEventType::noteOn:
        {
            if (event.note == nullptr)
                return;

            const auto& transport = *context.transport;
            const auto ticksPerSample = (transport.endTick - transport.startTick) / context.numSamples;
            noteOn (event, transport.startTick + ticksPerSample * offset, transport.timeMap.tempoMap(), offset, midi);
            return;
        }

        case NoteEventType::legato:
        {
            if (event.note == nullptr)
                return;

            const auto& transport = *context.transport;
            const auto ticksPerSample = (transport.endTick - transport.startTick) / context.numSamples;
            legato (event, transport.startTick + ticksPerSample * offset, transport.timeMap.tempoMap(), offset, midi);
            return;
        }

        case NoteEventType::noteOff:
        case NoteEventType::choke:
            endNotes (event.noteId, event.type == NoteEventType::noteOff, offset, midi);
            return;

        case NoteEventType::releaseAll:
            endNotes (std::nullopt, true, offset, midi);
            return;

        case NoteEventType::chokeAll:
            endNotes (std::nullopt, false, offset, midi);

            // Cut release tails too (a seek should not leave notes ringing).
            for (int ch = firstChannel(); ch <= lastChannel(); ++ch)
                midi.addEvent (juce::MidiMessage::controllerEvent (ch, allSoundOffController, 0), offset);

            return;

        case NoteEventType::sourceChanged:
            // Every note ends, live ones too, and no pointer is kept (docs/design/midi-effects.md, ME6).
            for (auto& channel : m_channels)
                if (channel.note != nullptr)
                    noteOff (channel, 0.0f, offset, midi);

            for (int ch = firstChannel(); ch <= lastChannel(); ++ch)
                midi.addEvent (juce::MidiMessage::controllerEvent (ch, allSoundOffController, 0), offset);

            return;

        case NoteEventType::expression:
        case NoteEventType::sustain:
        case NoteEventType::controller:
            handleLive (event, midi);
            return;
    }
}

void MpeEncoder::handleLive (const NoteEvent& event, juce::MidiBuffer& midi) noexcept
{
    const auto offset = event.sampleOffset;

    if (event.type == NoteEventType::expression)
    {
        // The next update sends the live value (docs/design/phase9-export-midi-input.md, M2).
        for (auto& channel : m_channels)
        {
            if (channel.note == nullptr || channel.noteId != event.noteId)
                continue;

            if (! channel.live)
            {
                channel.live = true;
                channel.livePitch = 0.0;
                channel.livePressure = 0.0;
                channel.liveTimbre = 0.5;
            }

            switch (static_cast<Lane> (event.lane))
            {
                case Lane::pitch:
                    channel.livePitch = static_cast<double> (event.value);
                    break;
                case Lane::pressure:
                    channel.livePressure = static_cast<double> (event.value);
                    break;
                case Lane::timbre:
                    channel.liveTimbre = static_cast<double> (event.value);
                    break;
                case Lane::vibrato:
                    break;
            }
        }

        return;
    }

    // Sustain and other controllers go to the zone's master channel (channel 1 in plain MIDI mode).
    const auto channel = masterChannel;

    if (event.type == NoteEventType::sustain)
    {
        midi.addEvent (juce::MidiMessage::controllerEvent (channel, sustainController, event.value >= 0.5f ? 127 : 0),
                       offset);
        return;
    }

    if (event.lane == controllerPitchBend)
        midi.addEvent (
            juce::MidiMessage::pitchWheel (
                channel, std::clamp (static_cast<int> (std::lround (pitchWheelCentre * (1.0f + event.value))), 0,
                                     pitchWheelMax)),
            offset);
    else if (event.lane == controllerPressure)
        midi.addEvent (juce::MidiMessage::channelPressureChange (channel, toSevenBit (event.value)), offset);
    else if (event.lane >= 0 && event.lane < 128)
        midi.addEvent (juce::MidiMessage::controllerEvent (channel, event.lane, toSevenBit (event.value)), offset);
}

void MpeEncoder::endNotes (std::optional<ObjectId> noteId, bool release, int offset, juce::MidiBuffer& midi) noexcept
{
    // Without a note id, every sequenced note ends; live (audition) notes have negative ids and stay.
    for (auto& channel : m_channels)
        if (channel.note != nullptr && (noteId.has_value() ? channel.noteId == *noteId : channel.noteId >= 0))
            noteOff (channel, release ? channel.note->releaseVelocity : 0.0f, offset, midi);
}

int MpeEncoder::firstChannel() const noexcept
{
    return m_settings.mpe ? firstMemberChannel : masterChannel;
}

int MpeEncoder::lastChannel() const noexcept
{
    return m_settings.mpe ? firstMemberChannel + numMemberChannels - 1 : masterChannel;
}

MpeEncoder::Channel& MpeEncoder::allocate (int offset, juce::MidiBuffer& midi) noexcept
{
    Channel* best = nullptr;

    // The least recently released free channel, so release tails ring on as long as possible.
    for (auto& channel : m_channels)
        if (channel.note == nullptr && (best == nullptr || channel.age < best->age))
            best = &channel;

    if (best != nullptr)
        return *best;

    // All channels hold notes: release the oldest.
    best = m_channels.data();

    for (auto& channel : m_channels)
        if (channel.age < best->age)
            best = &channel;

    noteOff (*best, 0.0f, offset, midi);
    return *best;
}

void MpeEncoder::noteOn (const NoteEvent& event, double tick, const TempoMap& map, int offset,
                         juce::MidiBuffer& midi) noexcept
{
    auto& channel = allocate (offset, midi);
    channel = {};
    channel.noteId = event.noteId;
    channel.note = event.note;
    channel.noteNumber = std::clamp (static_cast<int> (std::lround (event.note->pitch)), 0, 127);
    channel.age = ++m_counter;

    // Expression first, so the note starts with the right bend, pressure and timbre.
    updateExpression (channel, tick, map, offset, midi);

    const auto velocity = static_cast<juce::uint8> (std::max (1, toSevenBit (event.note->velocity)));
    midi.addEvent (juce::MidiMessage::noteOn (midiChannel (channel), channel.noteNumber, velocity), offset);
}

// A linked note (docs/design/linked-notes.md, L10). MPE keeps the channel and bends there, so nothing retriggers, as far as
// the bend range reaches. Plain MIDI has no per-note bend: the new note starts before the old one ends, which mono and legato
// synths play as a slide with their own portamento.
void MpeEncoder::legato (const NoteEvent& event, double tick, const TempoMap& map, int offset,
                         juce::MidiBuffer& midi) noexcept
{
    const auto from = std::ranges::find_if (m_channels, [&event] (const Channel& c)
                                            { return c.note != nullptr && c.noteId == event.fromNoteId; });
    const auto key = std::clamp (static_cast<int> (std::lround (event.note->pitch)), 0, 127);

    if (from != m_channels.end())
    {
        const auto reachable =
            m_settings.mpe && ! from->live && std::abs (event.note->pitch - from->noteNumber) <= m_settings.bendRange;

        if (reachable || (! m_settings.mpe && key == from->noteNumber))
        {
            auto pitch = from->note->pitch + from->note->pitchOffset (tick, map);
            auto pressure = from->note->evaluate (Lane::pressure, tick);
            auto timbre = from->note->evaluate (Lane::timbre, tick);
            auto velocity = from->note->velocity;
            from->glide.apply (*from->note, tick, map, pitch, pressure, timbre, velocity);
            from->glide.begin (pitch, pressure, timbre, velocity);
            from->noteId = event.noteId;
            from->note = event.note;
            updateExpression (*from, tick, map, offset, midi);
            return;
        }
    }

    // The old note ends after the new one started, on another channel or key.
    const auto fromIndex = static_cast<size_t> (from - m_channels.begin());
    auto start = event;
    start.type = NoteEventType::noteOn;

    if (from == m_channels.end())
    {
        noteOn (start, tick, map, offset, midi);
        return;
    }

    const auto wasId = from->noteId;
    noteOn (start, tick, map, offset, midi);

    // Every channel may have been in use, in which case noteOn already ended the old note.
    if (m_channels[fromIndex].note != nullptr && m_channels[fromIndex].noteId == wasId)
        noteOff (m_channels[fromIndex], m_channels[fromIndex].note->releaseVelocity, offset, midi);
}

void MpeEncoder::noteOff (Channel& channel, float velocity, int offset, juce::MidiBuffer& midi) noexcept
{
    midi.addEvent (juce::MidiMessage::noteOff (midiChannel (channel), channel.noteNumber,
                                               static_cast<juce::uint8> (toSevenBit (velocity))),
                   offset);
    channel.note = nullptr;
    channel.noteId = 0;
    channel.age = ++m_counter;
}

void MpeEncoder::updateExpression (Channel& channel, double tick, const TempoMap& map, int offset,
                                   juce::MidiBuffer& midi) noexcept
{
    const auto& note = *channel.note;
    const auto midiCh = midiChannel (channel);
    auto gliding = note.pitch + note.pitchOffset (tick, map);
    auto glidingPressure = note.evaluate (Lane::pressure, tick);
    auto glidingTimbre = note.evaluate (Lane::timbre, tick);
    auto velocity = note.velocity;
    channel.glide.apply (note, tick, map, gliding, glidingPressure, glidingTimbre, velocity);
    const auto pressure = toSevenBit (channel.live ? channel.livePressure : glidingPressure);

    if (! m_settings.mpe)
    {
        if (pressure != channel.pressure)
            midi.addEvent (juce::MidiMessage::aftertouchChange (midiCh, channel.noteNumber, pressure), offset);

        channel.pressure = pressure;

        // Live pitch (a keyboard's pitch wheel) becomes channel pitch bend over the node's bend range.
        if (channel.live)
        {
            const auto bend =
                std::clamp (static_cast<int> (std::lround (
                                pitchWheelCentre + channel.livePitch / m_settings.bendRange * pitchWheelCentre)),
                            0, pitchWheelMax);

            if (bend != channel.bend)
                midi.addEvent (juce::MidiMessage::pitchWheel (midiCh, bend), offset);

            channel.bend = bend;
        }

        return;
    }

    const auto semitones = (channel.live ? note.pitch + channel.livePitch : gliding) - channel.noteNumber;
    const auto bend = std::clamp (
        static_cast<int> (std::lround (pitchWheelCentre + semitones / m_settings.bendRange * pitchWheelCentre)), 0,
        pitchWheelMax);
    const auto timbre = toSevenBit (channel.live ? channel.liveTimbre : glidingTimbre);

    if (bend != channel.bend)
        midi.addEvent (juce::MidiMessage::pitchWheel (midiCh, bend), offset);

    if (pressure != channel.pressure)
        midi.addEvent (juce::MidiMessage::channelPressureChange (midiCh, pressure), offset);

    if (timbre != channel.timbre)
        midi.addEvent (juce::MidiMessage::controllerEvent (midiCh, timbreController, timbre), offset);

    channel.bend = bend;
    channel.pressure = pressure;
    channel.timbre = timbre;
}

void MpeEncoder::updateAll (const ProcessContext& context, int from, int to, juce::MidiBuffer& midi) noexcept
{
    const auto& transport = *context.transport;
    const auto ticksPerSample = (transport.endTick - transport.startTick) / context.numSamples;

    // Update points are the multiples of controlInterval within the block.
    for (auto s = (from + controlInterval - 1) / controlInterval * controlInterval; s < to; s += controlInterval)
        for (auto& channel : m_channels)
            if (channel.note != nullptr)
                updateExpression (channel, transport.startTick + ticksPerSample * s, transport.timeMap.tempoMap(), s,
                                  midi);
}

} // namespace dawg
