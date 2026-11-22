#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace dawg
{

/**
    A musical position or duration in ticks.

    Integer ticks keep edits (move, snap, quantise) exact. See docs/design/phase2-engine.md, D1.
*/
using Ticks = juce::int64;

/**
    Ticks per quarter-note beat. Divisible by 2, 3, 4, 5, 6, 8 and 16, so common grids are exact.
*/
inline constexpr Ticks ticksPerBeat = Ticks { 960 } * 16;

/**
    Converts beats to ticks, rounding to the nearest tick.

    @param beats  A position or duration in beats.
    @return       The same in ticks.
*/
[[nodiscard]] constexpr Ticks beatsToTicks (double beats) noexcept
{
    const auto scaled = beats * static_cast<double> (ticksPerBeat);
    return static_cast<Ticks> (scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
}

/**
    Converts ticks to beats.

    @param ticks  A position or duration in ticks.
    @return       The same in beats.
*/
[[nodiscard]] constexpr double ticksToBeats (double ticks) noexcept
{
    return ticks / static_cast<double> (ticksPerBeat);
}

namespace limits
{
/**
    Lowest allowed tempo in BPM.
*/
constexpr double minTempo = 20.0;

/**
    Highest allowed tempo in BPM.
*/
constexpr double maxTempo = 999.0;

/**
    Fewest allowed beats per bar (a time signature's numerator).
*/
constexpr int minBeatsPerBar = 1;

/**
    Most allowed beats per bar.
*/
constexpr int maxBeatsPerBar = 32;
} // namespace limits

/**
    A tempo change (docs/design/tempo-map.md, TM1): from its tick on, the tempo is bpm (quarter notes per minute).
*/
struct TempoPoint
{
    /**
        Where the tempo applies from.
    */
    Ticks tick = 0;

    /**
        The tempo in quarter notes per minute.
    */
    double bpm = 120.0;

    /**
        Whether the tempo moves linearly (in BPM over ticks) to the next point's; otherwise it holds until there.
    */
    bool ramp = false;

    /**
        Returns whether two points are the same.
    */
    // Exact comparison of the floating-point members is intended (an edit changed them or not).
    JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
    bool operator== (const TempoPoint&) const = default;
    JUCE_END_IGNORE_WARNINGS_GCC_LIKE
};

/**
    A time-signature change on a bar line (docs/design/tempo-map.md, TM2).
*/
struct TimeSignature
{
    /**
        The 0-based bar it starts on.
    */
    int bar = 0;

    /**
        Beats per bar.
    */
    int numerator = 4;

    /**
        The beat's note value: 2, 4, 8 or 16.
    */
    int denominator = 4;

    /**
        Returns the length of one beat (a 1/denominator note) in ticks.
    */
    [[nodiscard]] constexpr Ticks beatTicks() const noexcept { return ticksPerBeat * 4 / denominator; }

    /**
        Returns the length of one bar in ticks.
    */
    [[nodiscard]] constexpr Ticks barTicks() const noexcept { return beatTicks() * numerator; }

    /**
        Returns whether two signatures are the same.
    */
    bool operator== (const TimeSignature&) const = default;
};

/**
    A position in bars and beats (all 0-based): the bar, the beat within it in the signature's beat unit, and the
    ticks into that beat.
*/
struct BarPosition
{
    /**
        The 0-based bar.
    */
    int bar = 0;

    /**
        The 0-based beat within the bar.
    */
    int beat = 0;

    /**
        The ticks into the beat.
    */
    double tickInBeat = 0.0;
};

/**
    Sorts tempo points by tick, keeps the last of points on the same tick, clamps tempos, and makes sure the first
    point is at tick 0 (an empty list becomes 120 BPM).

    @param points  The points to repair.
*/
void normaliseTempoPoints (std::vector<TempoPoint>& points);

/**
    Sorts time signatures by bar, keeps the last of changes on the same bar, clamps numerators to 1..32 and
    denominators to 2, 4, 8 or 16, and makes sure the first change is at bar 0 (an empty list becomes 4/4).

    @param signatures  The signatures to repair.
*/
void normaliseSignatures (std::vector<TimeSignature>& signatures);

/**
    The song's musical time (docs/design/tempo-map.md, TM3): real time from tempo points with steps and ramps, and
    bars from time-signature changes.

    Immutable once built. Each segment's start time and each signature's start tick are precomputed, so every
    lookup is a binary search. Before tick 0 the first tempo and signature continue backwards.
*/
class TempoMap final
{
public:
    /**
        Creates a map of 120 BPM in 4/4.
    */
    TempoMap();

    /**
        Creates a map from points and signatures, normalising both (see normaliseTempoPoints() and
        normaliseSignatures()).

        @param points      The tempo points.
        @param signatures  The time signatures.
    */
    TempoMap (std::vector<TempoPoint> points, std::vector<TimeSignature> signatures);

    /**
        Returns the time in seconds of a tick position, from tick 0.

        @param tick  The tick position (may be fractional or negative).
    */
    [[nodiscard]] double secondsAt (double tick) const noexcept;

    /**
        Returns the tick position at a time in seconds, the inverse of secondsAt().

        @param seconds  The time from tick 0.
    */
    [[nodiscard]] double tickAt (double seconds) const noexcept;

    /**
        Returns the tempo at a tick position.

        @param tick  The tick position.
    */
    [[nodiscard]] double bpmAt (double tick) const noexcept;

    /**
        Returns the time signature in force at a tick position.

        @param tick  The tick position.
    */
    [[nodiscard]] const TimeSignature& signatureAt (double tick) const noexcept;

    /**
        Returns the tick where a bar starts.

        @param bar  The 0-based bar (may be negative).
    */
    [[nodiscard]] Ticks tickOfBar (int bar) const noexcept;

    /**
        Returns the bar, beat and ticks into the beat of a tick position.

        @param tick  The tick position.
    */
    [[nodiscard]] BarPosition barAt (double tick) const noexcept;

    /**
        Returns whether the map has more than one tempo point.
    */
    [[nodiscard]] bool hasTempoChanges() const noexcept { return m_points.size() > 1; }

    /**
        Returns the tempo points, normalised.
    */
    [[nodiscard]] const std::vector<TempoPoint>& points() const noexcept { return m_points; }

    /**
        Returns the time signatures, normalised.
    */
    [[nodiscard]] const std::vector<TimeSignature>& signatures() const noexcept { return m_signatures; }

    /**
        Returns whether two maps have the same points and signatures.
    */
    bool operator== (const TempoMap& other) const noexcept
    {
        return m_points == other.m_points && m_signatures == other.m_signatures;
    }

private:
    [[nodiscard]] size_t segmentAt (double tick) const noexcept;
    [[nodiscard]] size_t segmentAtSeconds (double seconds) const noexcept;
    [[nodiscard]] size_t signatureIndexAt (double tick) const noexcept;
    [[nodiscard]] double slope (size_t segment) const noexcept;

    std::vector<TempoPoint> m_points;
    std::vector<double> m_startSeconds;
    std::vector<TimeSignature> m_signatures;
    std::vector<Ticks> m_signatureTicks;
};

/**
    Maps between ticks and samples through a tempo map (docs/design/tempo-map.md, TM7).

    The mapping is anchored at a point (anchorTick, anchorSample): the sample counter keeps running across seeks
    and loops, which re-anchor it, and a new tempo map is adopted by anchoring at the current tick, so the playhead
    never jumps. Real time is linear in samples, so all the tempo lies in the map's seconds.

    Copying is cheap and never allocates: the map is referenced, not owned, and must outlive every copy (the engine
    keeps it in its compiled snapshot).
*/
class TimeMap final
{
public:
    /**
        Creates a map of 120 BPM in 4/4 at 48 kHz, anchored at tick 0 / sample 0.
    */
    TimeMap() noexcept;

    /**
        Creates a map anchored at tick 0 / sample 0.

        @param map         The tempo map. Must outlive this object and its copies.
        @param sampleRate  Sample rate in Hz. Must be positive.
    */
    TimeMap (const TempoMap& map, double sampleRate) noexcept;

    /**
        Converts a tick position to a sample position.

        @param tick  The tick position (may be fractional).
        @return      The sample position (fractional; round once where an integer is needed).
    */
    [[nodiscard]] double tickToSample (double tick) const noexcept
    {
        return static_cast<double> (m_anchorSample) + (m_map->secondsAt (tick) - m_anchorSeconds) * m_sampleRate;
    }

    /**
        Converts a sample position to a tick position.

        @param sample  The sample position.
        @return        The tick position.
    */
    [[nodiscard]] double sampleToTick (double sample) const noexcept
    {
        return m_map->tickAt (m_anchorSeconds + (sample - static_cast<double> (m_anchorSample)) / m_sampleRate);
    }

    /**
        Returns the number of samples per tick at a tick position (the local rate; it changes along ramps and at
        tempo points).

        @param tick  The tick position.
    */
    [[nodiscard]] double samplesPerTickAt (double tick) const noexcept
    {
        return m_sampleRate * 60.0 / (m_map->bpmAt (tick) * static_cast<double> (ticksPerBeat));
    }

    /**
        Returns the tempo map.
    */
    [[nodiscard]] const TempoMap& tempoMap() const noexcept { return *m_map; }

    /**
        Returns the sample rate in Hz.
    */
    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }

    /**
        Re-anchors the map so that a tick lands on a sample, e.g. after a seek.

        @param tick    The tick position.
        @param sample  The sample position it maps to.
    */
    void anchor (double tick, juce::int64 sample) noexcept;

private:
    const TempoMap* m_map;
    double m_sampleRate;
    double m_anchorSeconds = 0.0;
    juce::int64 m_anchorSample = 0;
};

} // namespace dawg
