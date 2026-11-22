#include "dawg/VoiceAllocator.h"

#include "dawg/Sequence.h"

#include <algorithm>

namespace dawg
{

namespace
{
constexpr float sustainThreshold = 0.5f;
} // namespace

void VoiceAllocator::clear() noexcept
{
    m_voices = {};
    m_sustain = false;
}

void VoiceAllocator::adoptSequence (const TrackSequence& sequence) noexcept
{
    if (&sequence == m_sequence)
        return;

    for (auto& voice : m_voices)
    {
        // Live (audition) notes have negative ids and don't belong to any sequence.
        if (voice.state == VoiceState::free || voice.note == nullptr || voice.noteId < 0)
            continue;

        voice.note = sequence.find (voice.noteId);

        if (voice.note == nullptr)
            voice.state = VoiceState::choking;
    }

    m_sequence = &sequence;
}

std::optional<size_t> VoiceAllocator::handle (const NoteEvent& event) noexcept
{
    switch (event.type)
    {
        case NoteEventType::noteOn:
        {
            if (event.note == nullptr)
                return std::nullopt;

            const auto index = choose();
            start (event, index);
            return index;
        }

        case NoteEventType::legato:
        {
            if (event.note == nullptr)
                return std::nullopt;

            for (auto& voice : m_voices)
            {
                if (voice.state == VoiceState::held && voice.noteId == event.fromNoteId)
                {
                    voice.glide.begin (voice.expression.pitch, voice.expression.pressure, voice.expression.timbre,
                                       voice.velocity);
                    voice.noteId = event.noteId;
                    voice.note = event.note;
                    voice.basePitch = event.note->pitch;
                    voice.sustained = false;
                    return std::nullopt;
                }
            }

            const auto index = choose();
            start (event, index);
            return index;
        }

        case NoteEventType::noteOff:
            release (event.noteId);
            break;

        case NoteEventType::expression:
            for (auto& voice : m_voices)
                if (plays (voice, event.noteId))
                    applyExpression (voice, event);
            break;

        case NoteEventType::sustain:
            setSustain (event.value >= sustainThreshold);
            break;

        case NoteEventType::controller:
            break; // for plugins only

        case NoteEventType::choke:
            for (auto& voice : m_voices)
                if (plays (voice, event.noteId))
                    voice.state = VoiceState::choking;
            break;

        case NoteEventType::releaseAll:
        case NoteEventType::chokeAll:
            endAll (event.type == NoteEventType::releaseAll);
            break;

        case NoteEventType::sourceChanged:
            // The notes may be gone with what sent them (docs/design/midi-effects.md, ME6): every voice, live ones too,
            // fades out on the expression it has.
            for (auto& voice : m_voices)
            {
                if (voice.state == VoiceState::free)
                    continue;

                voice.state = VoiceState::choking;
                voice.note = nullptr;
                voice.glide.active = false;
            }

            m_sustain = false;
            break;
    }

    return std::nullopt;
}

const VoiceExpression& VoiceAllocator::updateExpression (size_t index, double tick, const Transport& transport) noexcept
{
    auto& voice = m_voices[index];

    if (voice.live)
    {
        voice.expression = { voice.basePitch + voice.liveExpression.pitch, voice.liveExpression.pressure,
                             voice.liveExpression.timbre };
    }
    else if (voice.note != nullptr)
    {
        const auto& map = transport.timeMap.tempoMap();
        voice.expression = { voice.note->pitch + voice.note->pitchOffset (tick, map),
                             voice.note->evaluate (Lane::pressure, tick), voice.note->evaluate (Lane::timbre, tick) };

        if (voice.glide.active)
            voice.glide.apply (*voice.note, tick, map, voice.expression.pitch, voice.expression.pressure,
                               voice.expression.timbre, voice.velocity);
    }

    return voice.expression;
}

void VoiceAllocator::free (size_t index) noexcept
{
    m_voices[index] = {};
}

// A free voice, or else one to steal: the oldest, or with releasedFirst the oldest that is no longer held.
size_t VoiceAllocator::choose() const noexcept
{
    const auto rank = [this] (const Voice& voice)
    { return m_policy == StealPolicy::releasedFirst && voice.state == VoiceState::held ? 1 : 0; };

    size_t chosen = 0;

    for (size_t i = 0; i < m_voices.size(); ++i)
    {
        if (m_voices[i].state == VoiceState::free)
            return i;

        const auto& best = m_voices[chosen];

        if (rank (m_voices[i]) < rank (best) || (rank (m_voices[i]) == rank (best) && m_voices[i].age < best.age))
            chosen = i;
    }

    return chosen;
}

void VoiceAllocator::start (const NoteEvent& event, size_t index) noexcept
{
    auto& voice = m_voices[index];
    voice = {};
    voice.state = VoiceState::held;
    voice.noteId = event.noteId;
    voice.note = event.note;
    voice.age = ++m_ageCounter;
    voice.velocity = event.note->velocity;
    voice.basePitch = event.note->pitch;
    voice.expression.pitch = event.note->pitch;
}

bool VoiceAllocator::plays (const Voice& voice, ObjectId noteId) noexcept
{
    return voice.state != VoiceState::free && voice.noteId == noteId;
}

// The transport stopped (release) or jumped (choke): every voice ends, except live notes, which the player still holds.
void VoiceAllocator::endAll (bool release) noexcept
{
    for (auto& voice : m_voices)
        if (voice.state != VoiceState::free && voice.noteId >= 0)
            voice.state = release && voice.state == VoiceState::held ? VoiceState::releasing : VoiceState::choking;
}

// A note-off: held voices release, or wait for the sustain pedal to lift.
void VoiceAllocator::release (ObjectId noteId) noexcept
{
    for (auto& voice : m_voices)
    {
        if (voice.state != VoiceState::held || voice.noteId != noteId)
            continue;

        if (m_sustain)
            voice.sustained = true;
        else
            voice.state = VoiceState::releasing;
    }
}

void VoiceAllocator::setSustain (bool down) noexcept
{
    m_sustain = down;

    if (! down)
        for (auto& voice : m_voices)
            if (voice.sustained && voice.state == VoiceState::held)
                voice.state = VoiceState::releasing;
}

// Live expression (docs/design/phase9-export-midi-input.md, M2): from now on the voice follows live values.
void VoiceAllocator::applyExpression (Voice& voice, const NoteEvent& event) noexcept
{
    if (! voice.live)
    {
        voice.live = true;
        voice.liveExpression = { voice.expression.pitch - voice.basePitch, voice.expression.pressure,
                                 voice.expression.timbre };
    }

    switch (static_cast<Lane> (event.lane))
    {
        case Lane::pitch:
            voice.liveExpression.pitch = static_cast<double> (event.value);
            break;
        case Lane::pressure:
            voice.liveExpression.pressure = std::clamp (static_cast<double> (event.value), 0.0, 1.0);
            break;
        case Lane::timbre:
            voice.liveExpression.timbre = std::clamp (static_cast<double> (event.value), 0.0, 1.0);
            break;
        case Lane::vibrato:
            break;
    }
}

} // namespace dawg
