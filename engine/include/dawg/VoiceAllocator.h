#pragma once

#include "dawg/Node.h"
#include "dawg/Sequence.h"

#include <array>
#include <optional>
#include <span>

namespace dawg
{

class TrackSequence;

/**
    Where a voice is in its life.
*/
enum class VoiceState
{
    free,      ///< Not sounding; may be started.
    held,      ///< Its note is held (or released while the sustain pedal is down).
    releasing, ///< Its note was released; the voice plays its release tail.
    choking,   ///< Its note was choked or removed; the voice fades out quickly.
};

/**
    A note's expression at one moment: what a voice's sound follows.
*/
struct VoiceExpression
{
    /**
        MIDI semitones: the base pitch plus the pitch curve and vibrato, or plus live pitch.
    */
    double pitch = 60.0;

    /**
        0..1.
    */
    double pressure = 0.0;

    /**
        0..1.
    */
    double timbre = 0.5;
};

/**
    One voice's note bookkeeping. The owner of the allocator keeps the sound (phase, envelope, nodes) itself, in a
    parallel array indexed like VoiceAllocator::voices().
*/
struct Voice
{
    /**
        Where the voice is in its life.
    */
    VoiceState state = VoiceState::free;

    /**
        The note it plays. Live (audition and MIDI input) notes have negative ids.
    */
    ObjectId noteId = 0;

    /**
        The compiled note, or null once it is gone; the last expression is then kept.
    */
    const CompiledNote* note = nullptr;

    /**
        Start order: higher is younger.
    */
    juce::uint64 age = 0;

    /**
        The note-on velocity, 0..1.
    */
    float velocity = 0.0f;

    /**
        The note's base pitch in MIDI semitones.
    */
    double basePitch = 60.0;

    /**
        The expression updateExpression() last evaluated.
    */
    VoiceExpression expression;

    /**
        Whether expression comes from live events (MIDI input) instead of the note's curves
        (docs/design/phase9-export-midi-input.md, M2).
    */
    bool live = false;

    /**
        The live expression; its pitch is in semitones relative to the base pitch.
    */
    VoiceExpression liveExpression { 0.0, 0.0, 0.5 };

    /**
        Whether the note was released while the sustain pedal was down.
    */
    bool sustained = false;

    /**
        The way from a linked note to the note it plays now (docs/design/linked-notes.md).
    */
    LinkGlide glide;
};

/**
    Which voice a note-on takes when every voice is busy.
*/
enum class StealPolicy
{
    oldest,        ///< The oldest voice (the MPE Synth).
    releasedFirst, ///< The oldest releasing or choking voice, else the oldest held one (the voice scope, V9).
};

/**
    The note side of a polyphonic instrument: assigns notes to a fixed pool of voices and follows note-offs, the
    sustain pedal, chokes, live expression and edits to the sequence. Used by the MPE Synth, and by the voice scope
    (docs/design/phase12-voice-scope.md, V7).

    @note  Audio thread; never allocates.
*/
class VoiceAllocator final
{
public:
    /**
        How many voices play at once.
    */
    static constexpr size_t numVoices = 16;

    /**
        Creates an allocator with every voice free.

        @param policy  Which voice to steal when all are busy.
    */
    explicit VoiceAllocator (StealPolicy policy = StealPolicy::oldest) noexcept
        : m_policy (policy)
    {
    }

    /**
        Frees every voice and lifts the sustain pedal.
    */
    void clear() noexcept;

    /**
        Re-resolves the voices' notes in a new sequence; a voice whose note is gone is choked. Does nothing if the
        sequence is the one the voices already refer to.

        @param sequence  The track's current compiled notes.
    */
    void adoptSequence (const TrackSequence& sequence) noexcept;

    /**
        Handles one note event.

        @param event  The event.
        @return       For a note-on, the index of the voice that now plays the note (the voice's sound should start
                      afresh); otherwise nothing. A legato whose note has no voice left to continue counts as a
                      note-on; one that continues a voice returns nothing, and the voice's sound goes on.
    */
    std::optional<size_t> handle (const NoteEvent& event) noexcept;

    /**
        Evaluates a voice's expression at a time and stores it in the voice: its live values, or its note's curves.
        A voice without either keeps its last values.

        @param index      The voice.
        @param tick       The absolute tick.
        @param transport  The block's transport, for the tempo map.
        @return           The voice's expression.
    */
    const VoiceExpression& updateExpression (size_t index, double tick, const Transport& transport) noexcept;

    /**
        Frees a voice whose sound has ended.

        @param index  The voice.
    */
    void free (size_t index) noexcept;

    /**
        Returns the voices.
    */
    [[nodiscard]] std::span<const Voice, numVoices> voices() const noexcept { return m_voices; }

    /**
        Returns whether the sustain pedal is down.
    */
    [[nodiscard]] bool sustainDown() const noexcept { return m_sustain; }

private:
    void start (const NoteEvent& event, size_t index) noexcept;
    [[nodiscard]] size_t choose() const noexcept;
    [[nodiscard]] static bool plays (const Voice& voice, ObjectId noteId) noexcept;
    void endAll (bool release) noexcept;
    void release (ObjectId noteId) noexcept;
    void setSustain (bool down) noexcept;
    static void applyExpression (Voice& voice, const NoteEvent& event) noexcept;

    StealPolicy m_policy;
    std::array<Voice, numVoices> m_voices {};
    const TrackSequence* m_sequence = nullptr;
    juce::uint64 m_ageCounter = 0;
    bool m_sustain = false;
};

} // namespace dawg
