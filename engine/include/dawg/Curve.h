#pragma once

#include "dawg/Time.h"

#include <array>
#include <span>
#include <utility>
#include <vector>

namespace dawg
{

/**
    Shape of a curve segment, from one point to the next.
*/
enum class CurveShape
{
    linear,  ///< Straight line.
    hold,    ///< Keep the start value until the next point (a step).
    power,   ///< Eased line; see CurvePoint::tension.
    free,    ///< Cubic Bezier whose handles point at the tension point (CurvePoint::handleTime, handleValue).
    smooth,  ///< Part of a monotone cubic through the nodes (see smoothSlope()): no corners, never past its nodes.
    rounded, ///< Part of a cubic through the nodes with centred slopes (see smoothSlope()): round at its nodes, may pass them a little.
};

/**
    The largest tension magnitude a power segment uses. The usual musical range is -1..1; the wider
    limit lets a free tension handle pass through any point inside its segment's rectangle.
*/
inline constexpr double maxTension = 4.0;

/**
    A breakpoint of an expression curve.
*/
struct CurvePoint
{
    /**
        Time relative to the note start. May exceed the note length (release phase).
    */
    Ticks time = 0;

    /**
        Value at this point, in the lane's unit.
    */
    double value = 0.0;

    /**
        Shape of the segment that leaves this point.
    */
    CurveShape shape = CurveShape::linear;

    /**
        Easing for CurveShape::power, from -maxTension (fast start) through 0 (linear) to maxTension
        (slow start). Values within -1..1 are the usual musical range.
    */
    double tension = 0.0;

    /**
        Horizontal position of the tension point, 0..1 across the segment (CurveShape::free): both Bezier handles
        point at it (see freeSegment()).
    */
    double handleTime = 0.5;

    /**
        Vertical position of the tension point, 0..1 across the segment's value range (CurveShape::free).
    */
    double handleValue = 0.5;
};

/**
    Expression lanes every note has. See docs/design/phase2-engine.md, D2.
*/
enum class Lane
{
    pitch,    ///< Semitones relative to the note's base pitch.
    pressure, ///< Normalised 0..1 (MPE "Z").
    timbre,   ///< Normalised 0..1 (MPE "Y").
    vibrato,  ///< Vibrato intensity 0..1, multiplies the depth (see Vibrato.h). Default 0: no vibrato.
};

/**
    Number of expression lanes.
*/
inline constexpr size_t numLanes = 4;

/**
    Static description of a lane.
*/
struct LaneInfo
{
    /**
        Identifier used in commands and serialisation, e.g. "pitch".
    */
    const char* id;

    /**
        Value used when the lane has no points.
    */
    double defaultValue;

    /**
        Lowest allowed value.
    */
    double minValue;

    /**
        Highest allowed value.
    */
    double maxValue;
};

/**
    Returns the description of every lane, indexed by Lane.
*/
[[nodiscard]] const std::array<LaneInfo, numLanes>& laneInfos() noexcept;

/**
    Finds a lane by its identifier.

    @param id      The lane identifier, e.g. "pressure".
    @param result  Receives the lane if found.
    @return        True if the identifier names a lane.
*/
[[nodiscard]] bool laneFromId (const juce::String& id, Lane& result) noexcept;

/**
    Breakpoints of one lane, sorted by time with unique times.
*/
using Curve = std::vector<CurvePoint>;

/**
    The expression curves of one note, indexed by Lane.
*/
using Expression = std::array<Curve, numLanes>;

/**
    Evaluates a curve.

    Before the first point the curve holds the first value; after the last point it holds the last value.

    @param points        The points, sorted by time with unique times.
    @param time          The time to evaluate at, relative to the note start (may be fractional).
    @param defaultValue  The value returned when there are no points.
    @return              The curve value.
*/
[[nodiscard]] double evaluateCurve (std::span<const CurvePoint> points, double time, double defaultValue) noexcept;

/**
    A free segment's shape (CurveShape::free): a cubic Bezier from (0, 0) to (1, 1) whose two handles point from
    their nodes at the tension point T = (hx, hy). A handle reaches at most to T, scaled by how far T lies along the
    line from its node through T to the opposite side of the segment's rectangle: the start handle ends at
    T * max (hx, hy), the end handle at (1, 1) + (T - (1, 1)) * max (1 - hx, 1 - hy). T in the middle gives a
    straight line, T in a corner the sharpest bend.

    The Bezier's x is inverted by Newton's method, kept inside a bracket that falls back to bisection, so it converges
    from any start. An envelope that moves along the segment sample by sample passes `hint`: the previous sample's
    solution is a start one or two steps from this one's.

    @param x     Position across the segment, 0..1.
    @param hx    Horizontal position of the tension point, 0..1 (clamped).
    @param hy    Vertical position of the tension point, 0..1 (clamped).
    @param hint  The Bezier parameter to start from, updated to the solution; null starts from x.
    @return      How far the segment has travelled at x, 0..1.
*/
[[nodiscard]] double freeSegment (double x, double hx, double hy, double* hint = nullptr) noexcept;

/**
    The slope a smooth segment (CurveShape::smooth) has at a node, in value per tick. A run of consecutive smooth
    segments is one monotone cubic through its nodes (Fritsch-Carlson, as PCHIP): at an inner node of the run the
    weighted harmonic mean of the two neighbouring secants, 0 where they differ in sign (a peak or a dip, so the curve
    never passes its nodes); at the run's ends a three-point estimate kept to the shape of the data. A run of one
    segment is a straight line. Only the run's own nodes count, so a smooth run ends where another shape begins.

    A run of rounded segments (CurveShape::rounded, pitch shapes) is a Catmull-Rom spline instead: at an inner node
    the slope of the line through its two neighbours, so a node where a steep rise meets a flat stretch is rounded
    rather than a knee; the curve may pass a node by up to about 15 % of the neighbouring step. At the curve's first
    and last nodes its slope is 0, so it meets the values held beyond them without a corner; a run's other ends are
    as a smooth run's.

    @param points  The curve, sorted by time with unique times.
    @param node    The node's index.
    @return        The slope; 0 for a node no smooth segment touches.
*/
[[nodiscard]] double smoothSlope (std::span<const CurvePoint> points, size_t node) noexcept;

/**
    Gives a curve a point at the note start (time 0), with the value the curve already has there, so the sound
    doesn't change. MPE lanes (pitch, pressure, timbre) always start with this anchor. The end of a note has no
    anchor: a curve holds its last value to the end by itself.

    @param curve         The curve, sorted by time with unique times.
    @param defaultValue  The lane's default, for an empty curve.
*/
void anchorCurve (Curve& curve, double defaultValue);

/**
    Splits a note's curve in two at a time inside the note, for the two halves of a split note.

    The left part keeps the points before `at`, plus a point at `at` if the curve was still changing there. The right
    part starts at time 0 with the value the curve had at `at`, followed by the later points (release points
    included), moved earlier by `at`. A power segment cut in two keeps its tension on both sides, so its bend is
    close to, not exactly, the original.

    @param curve         The curve, sorted by time with unique times.
    @param at            The split time, relative to the note start.
    @param defaultValue  The lane's default, for an empty curve.
    @return              The left and right curves; both empty for an empty curve.
*/
[[nodiscard]] std::pair<Curve, Curve> splitCurve (const Curve& curve, Ticks at, double defaultValue);

} // namespace dawg
