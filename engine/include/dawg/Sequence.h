#pragma once

#include "dawg/Model.h"
#include "dawg/StepSequencer.h"

#include <array>
#include <span>
#include <vector>

namespace dawg
{

/**
    The id a note plays under in its n-th placement on a track: loop repeats and linked clips play one note id
    several times (docs/design/phase6-arrangement.md, A7). Placement 0 keeps the note's own id, so plain clips
    are unaffected; further placements add multiples of 2^40 (note ids stay far below that, and a track has
    fewer than 2^20 placements of one note).

    @param noteId      The model note id.
    @param repetition  The placement, 0 for the first.
    @return            A unique id for this sounding instance.
*/
[[nodiscard]] constexpr ObjectId runtimeNoteId (ObjectId noteId, int repetition) noexcept
{
    return noteId + static_cast<ObjectId> (repetition) * (ObjectId { 1 } << 40);
}

/**
    A note flattened for playback: absolute positions and curves in contiguous storage.
*/
struct CompiledNote
{
    /**
        The id this instance plays under: the note id, plus a repetition term for repeats of a looped clip
        (see runtimeNoteId()).
    */
    ObjectId id = 0;

    /**
        For a note a MIDI effect made from another (docs/design/midi-effects.md, ME4): the sequence note whose start,
        curves and vibrato it plays, so it can be found again when the sequence changes. 0 for a sequence note, and
        for one made from a live note.
    */
    ObjectId source = 0;

    /**
        Absolute start on the timeline.
    */
    Ticks start = 0;

    /**
        Absolute end on the timeline (after cutting at the clip end). Greater than start.
    */
    Ticks end = 0;

    /**
        Base pitch in MIDI semitones.
    */
    double pitch = 60.0;

    /**
        Note-on velocity, 0..1.
    */
    float velocity = 0.8f;

    /**
        Note-off velocity, 0..1.
    */
    float releaseVelocity = 0.5f;

    /**
        Curve points per lane (times relative to start), viewing the owning TrackSequence's storage.
    */
    std::array<std::span<const CurvePoint>, numLanes> lanes {};

    /**
        The note's vibrato.
    */
    Vibrato vibrato;

    /**
        The note that continues this note's voice (docs/design/linked-notes.md), or 0.
    */
    ObjectId linkTo = 0;

    /**
        The note whose voice this note continues, or 0.
    */
    ObjectId linkFrom = 0;

    /**
        With linkFrom: how long it takes to reach this note's expression, in seconds.
    */
    double glideSeconds = 0.0;

    /**
        Where the note's voice stops being this note's: its end, or the start of the note that continues it.
    */
    Ticks holdUntil = 0;

    /**
        Evaluates one expression lane.

        @param lane   The lane.
        @param tick   Absolute tick position (may be fractional and past the end).
        @return       The lane value, or the lane default when it has no points.
    */
    [[nodiscard]] double evaluate (Lane lane, double tick) const noexcept;

    /**
        Returns the total pitch offset at a time: the pitch curve plus the vibrato.

        @param tick  Absolute tick position (may be fractional).
        @param map   The tempo map, for the real time a free-running vibrato has run.
        @return      The offset in semitones from the base pitch.
    */
    [[nodiscard]] double pitchOffset (double tick, const TempoMap& map) const noexcept;
};

/**
    A voice's way from one linked note to the next (docs/design/linked-notes.md, L5, L6): where the voice was at the
    hand-over fades into the new note over its glide time.
*/
struct LinkGlide
{
    /**
        Starts a glide from what the voice sounds like now.

        @param fromPitch     Its pitch in MIDI semitones.
        @param fromPressure  Its pressure.
        @param fromTimbre    Its timbre.
        @param fromVelocity  Its velocity.
    */
    void begin (double fromPitch, double fromPressure, double fromTimbre, float fromVelocity) noexcept;

    /**
        Moves a note's values at a time to where the glide has them. Does nothing once the glide is over.

        @param note      The note the voice now plays.
        @param tick      Absolute tick position.
        @param map       The tempo map, since glide times are real time.
        @param pitch     The note's pitch there, changed to the voice's.
        @param pressure  The note's pressure there, changed to the voice's.
        @param timbre    The note's timbre there, changed to the voice's.
        @param velocity  Receives the voice's velocity while gliding.
    */
    void apply (const CompiledNote& note, double tick, const TempoMap& map, double& pitch, double& pressure,
                double& timbre, float& velocity) noexcept;

    /**
        Whether a glide is under way.
    */
    bool active = false;

private:
    bool m_measured = false;
    double m_pitch = 0.0; // the voice's pitch at the hand-over, then the offset from the note's that fades out
    double m_pressure = 0.0;
    double m_timbre = 0.0;
    float m_velocity = 0.0f;
};

/**
    A stretch of the timeline where one clip's controller curve plays (docs/design/phase10-automation.md, A5).
*/
struct ControllerSegment
{
    /**
        NoteClip::controllers key: CC 0–127, controllerKeyPitchBend or controllerKeyPressure.
    */
    int controller = 0;

    /**
        Timeline start of the stretch.
    */
    Ticks start = 0;

    /**
        Timeline end of the stretch.
    */
    Ticks end = 0;

    /**
        The content position at start (the curve's time there).
    */
    Ticks contentAt = 0;

    /**
        The curve (content ticks, values 0..1), viewing the sequence's storage.
    */
    std::span<const CurvePoint> points;

    /**
        The curve's value at a timeline position within the stretch.

        @param tick  The timeline position.
        @return      The value, 0..1.
    */
    [[nodiscard]] double valueAt (double tick) const noexcept
    {
        return evaluateCurve (points, static_cast<double> (contentAt) + tick - static_cast<double> (start), 0.0);
    }
};

/**
    All notes of one track, compiled for playback.

    Immutable after construction; shared by the compiled engine and read by the audio thread.
*/
class TrackSequence final
{
public:
    /**
        Builds the sequence from a track's clips.

        @param clips  The clips. Notes starting at or after their clip's end are skipped; notes reaching past
                      it are cut.
    */
    explicit TrackSequence (std::span<const ClipPtr> clips);

    TrackSequence (const TrackSequence&) = delete;
    TrackSequence& operator= (const TrackSequence&) = delete;
    TrackSequence (TrackSequence&&) = delete;
    TrackSequence& operator= (TrackSequence&&) = delete;
    ~TrackSequence() = default;

    /**
        Returns the notes sorted by start, then id.
    */
    [[nodiscard]] std::span<const CompiledNote> notes() const noexcept { return m_notes; }

    /**
        Finds a note by id.

        @param noteId  The note id.
        @return        The note, or nullptr if this sequence has no such note.
        @note  Allocation-free; safe on the audio thread.
    */
    [[nodiscard]] const CompiledNote* find (ObjectId noteId) const noexcept;

    /**
        Returns the index of the first note starting at or after a tick.

        @param tick  The tick position.
        @return      An index into notes() (notes().size() if none).
    */
    [[nodiscard]] size_t firstStartingAtOrAfter (double tick) const noexcept;

    /**
        Returns the clips' controller curves on the timeline, sorted by start.
    */
    [[nodiscard]] std::span<const ControllerSegment> controllerSegments() const noexcept { return m_segments; }

    /**
        Returns the sequencer clips (docs/design/step-sequencer.md, SS2), sorted by start.
    */
    [[nodiscard]] std::span<const stepseq::Span> sequencerSpans() const noexcept { return m_spans; }

private:
    std::vector<CurvePoint> m_points;
    std::vector<CurvePoint> m_controllerPoints;
    std::vector<ControllerSegment> m_segments;
    std::vector<CompiledNote> m_notes;
    std::vector<size_t> m_byId; // indices into m_notes sorted by note id
    std::vector<stepseq::Span> m_spans;
};

} // namespace dawg
