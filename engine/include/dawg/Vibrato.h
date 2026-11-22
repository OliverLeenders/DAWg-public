#pragma once

#include "dawg/Time.h"

namespace dawg
{

/**
    Waveform of a vibrato.
*/
enum class VibratoShape
{
    sine,     ///< Smooth.
    triangle, ///< Linear up and down.
    square,   ///< Trill-like jumps between +depth and -depth.
    sawUp,    ///< Rises slowly, drops at once.
    sawDown,  ///< Drops slowly, rises at once.
};

/**
    Number of vibrato shapes.
*/
inline constexpr int numVibratoShapes = 5;

/**
    Parametric vibrato of a note, added on top of its pitch curve by the engine.
    See docs/design/phase3-piano-roll.md, "Vibrato".
*/
struct Vibrato
{
    /**
        Peak deviation in semitones (both directions) at full intensity. Whether vibrato sounds at all is set
        by the note's vibrato intensity lane, which defaults to 0.
    */
    double depth = 1.0;

    /**
        Rate in Hz, used when syncBeats is 0.
    */
    double rateHz = 5.5;

    /**
        Cycle length in beats for tempo-synced vibrato, or 0 for free-running (rateHz).
    */
    double syncBeats = 0.0;

    /**
        Waveform.
    */
    VibratoShape shape = VibratoShape::sine;

    /**
        Time from the note start before the vibrato begins.
    */
    Ticks delay = 0;

    /**
        Ramp-up time after the delay, from no vibrato to full depth.
    */
    Ticks fadeIn = 0;

    /**
        Returns true if the vibrato has any effect.
    */
    [[nodiscard]] bool active() const noexcept { return depth > 0.0; }
};

/**
    Bounds for vibrato parameters, enforced by commands.
*/
namespace limits
{
/**
    Shallowest allowed vibrato depth, in semitones (depth is never "off"; intensity turns vibrato off).
*/
constexpr double minVibratoDepth = 0.05;

/**
    Deepest allowed vibrato, in semitones.
*/
constexpr double maxVibratoDepth = 2.0;

/**
    Slowest allowed free-running vibrato rate, in Hz.
*/
constexpr double minVibratoRate = 0.5;

/**
    Fastest allowed free-running vibrato rate, in Hz.
*/
constexpr double maxVibratoRate = 12.0;

/**
    Longest allowed tempo-synced vibrato cycle, in beats.
*/
constexpr double maxVibratoSyncBeats = 4.0;
} // namespace limits

/**
    Evaluates a vibrato's pitch offset.

    @param vibrato         The vibrato.
    @param ticksFromStart  Time since the note start, in ticks (may be fractional).
    @param secondsPerTick  Duration of one tick, on average since the vibrato began (after its delay), for
                           free-running rates: elapsed ticks times this is the real time it has run.
    @param depthScale      The note's vibrato intensity (vibrato lane), 0..1.
    @return                The offset in semitones to add to the note's pitch.
*/
[[nodiscard]] double vibratoOffset (const Vibrato& vibrato, double ticksFromStart, double secondsPerTick,
                                    double depthScale) noexcept;

} // namespace dawg
