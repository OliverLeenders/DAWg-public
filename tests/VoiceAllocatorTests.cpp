#include "TestUtils.h"
#include "dawg/Sequence.h"
#include "dawg/VoiceAllocator.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <deque>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
// Notes for the allocator, kept at stable addresses.
struct Notes
{
    const CompiledNote* make (ObjectId id, double pitch = 60.0)
    {
        auto& note = storage.emplace_back();
        note.id = id;
        note.pitch = pitch;
        return &note;
    }

    std::deque<CompiledNote> storage;
};

[[nodiscard]] NoteEvent noteOn (const CompiledNote* note)
{
    return { 0, NoteEventType::noteOn, note->id, note };
}

[[nodiscard]] NoteEvent event (NoteEventType type, ObjectId noteId = 0, int lane = 0, float value = 0.0f)
{
    return { 0, type, noteId, nullptr, lane, value };
}

[[nodiscard]] VoiceState stateOf (const VoiceAllocator& voices, ObjectId noteId)
{
    for (const auto& voice : voices.voices())
        if (voice.state != VoiceState::free && voice.noteId == noteId)
            return voice.state;

    return VoiceState::free;
}
} // namespace

TEST_CASE ("the voice allocator takes free voices, then steals the oldest", "[voice]")
{
    VoiceAllocator voices;
    Notes notes;

    for (ObjectId id = 1; id <= static_cast<ObjectId> (VoiceAllocator::numVoices); ++id)
        CHECK (voices.handle (noteOn (notes.make (id))) == static_cast<size_t> (id - 1));

    // The pool is full: the next note takes the first note's voice.
    CHECK (voices.handle (noteOn (notes.make (100))) == 0u);
    CHECK (stateOf (voices, 1) == VoiceState::free);
    CHECK (stateOf (voices, 100) == VoiceState::held);

    // A freed voice is taken before stealing.
    voices.free (5);
    CHECK (voices.handle (noteOn (notes.make (101))) == 5u);
}

TEST_CASE ("the voice allocator releases, sustains and chokes", "[voice]")
{
    VoiceAllocator voices;
    Notes notes;
    std::ignore = voices.handle (noteOn (notes.make (1)));
    std::ignore = voices.handle (noteOn (notes.make (2)));
    std::ignore = voices.handle (noteOn (notes.make (-3))); // live

    // With the pedal down a note-off waits for the pedal to lift.
    std::ignore = voices.handle (event (NoteEventType::sustain, 0, 0, 1.0f));
    std::ignore = voices.handle (event (NoteEventType::noteOff, 1));
    CHECK (stateOf (voices, 1) == VoiceState::held);
    std::ignore = voices.handle (event (NoteEventType::sustain, 0, 0, 0.0f));
    CHECK (stateOf (voices, 1) == VoiceState::releasing);

    std::ignore = voices.handle (event (NoteEventType::choke, 2));
    CHECK (stateOf (voices, 2) == VoiceState::choking);

    // Releasing everything (the transport stopped) leaves live notes playing.
    std::ignore = voices.handle (noteOn (notes.make (4)));
    std::ignore = voices.handle (event (NoteEventType::releaseAll));
    CHECK (stateOf (voices, 4) == VoiceState::releasing);
    CHECK (stateOf (voices, -3) == VoiceState::held);
}

TEST_CASE ("the voice allocator chokes voices whose notes left the sequence", "[voice]")
{
    VoiceAllocator voices;
    Notes notes;
    std::ignore = voices.handle (noteOn (notes.make (1)));
    std::ignore = voices.handle (noteOn (notes.make (-2))); // live notes belong to no sequence

    const TrackSequence empty { std::span<const ClipPtr> {} };
    voices.adoptSequence (empty);
    CHECK (stateOf (voices, 1) == VoiceState::choking);
    CHECK (stateOf (voices, -2) == VoiceState::held);
}

TEST_CASE ("the voice allocator follows curves, then live expression", "[voice]")
{
    VoiceAllocator voices;
    Notes notes;
    auto* note = const_cast<CompiledNote*> (notes.make (1, 64.0));
    const std::array<CurvePoint, 1> pressure { CurvePoint { 0, 0.25 } };
    note->lanes[static_cast<size_t> (Lane::pressure)] = pressure;

    const auto index = voices.handle (noteOn (note));
    REQUIRE (index.has_value());

    Transport transport;
    transport.timeMap = TimeMap (constantTempo(), sampleRate);
    const auto curves = voices.updateExpression (*index, 0.0, transport);
    CHECK_THAT (curves.pitch, WithinAbs (64.0, 1e-9));
    CHECK_THAT (curves.pressure, WithinAbs (0.25, 1e-9));

    // A live event takes over from the curves; live pitch is relative to the base pitch.
    std::ignore = voices.handle (event (NoteEventType::expression, 1, static_cast<int> (Lane::pitch), 2.0f));
    const auto live = voices.updateExpression (*index, 0.0, transport);
    CHECK_THAT (live.pitch, WithinAbs (66.0, 1e-9));
    CHECK_THAT (live.pressure, WithinAbs (0.25, 1e-9));
}
