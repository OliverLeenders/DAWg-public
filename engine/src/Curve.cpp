#include "dawg/Curve.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace dawg
{

namespace
{
// Tension t maps to the exponent 2^(3t): t = 1 gives x^8 (slow start), t = -1 gives x^(1/8) (fast start). Tension
// is clamped to +-maxTension (Curve.h), wider than the usual musical range of -1..1.
constexpr double tensionRange = 3.0;

// Whether the segment from points[index] to the next point is smooth or rounded (both cubic Hermite runs).
[[nodiscard]] bool smoothAt (std::span<const CurvePoint> points, size_t index) noexcept
{
    return index + 1 < points.size()
           && (points[index].shape == CurveShape::smooth || points[index].shape == CurveShape::rounded);
}

[[nodiscard]] double gap (std::span<const CurvePoint> points, size_t index) noexcept
{
    return static_cast<double> (points[index + 1].time - points[index].time);
}

[[nodiscard]] double secant (std::span<const CurvePoint> points, size_t index) noexcept
{
    return (points[index + 1].value - points[index].value) / gap (points, index);
}

// The slope at the end of a run from the two segments next to it (h0 and d0 the nearer), kept to the data's shape:
// 0 if it points against the nearer secant, at most three times that secant where the secants differ in sign.
[[nodiscard]] double edgeSlope (double h0, double h1, double d0, double d1) noexcept
{
    const auto slope = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);

    if (slope * d0 <= 0.0)
        return 0.0;

    if (d0 * d1 < 0.0 && std::abs (slope) > 3.0 * std::abs (d0))
        return 3.0 * d0;

    return slope;
}

[[nodiscard]] double shapeSegment (std::span<const CurvePoint> points, size_t index, double time) noexcept
{
    const auto& from = points[index];
    const auto& to = points[index + 1];
    const auto span = static_cast<double> (to.time - from.time);

    if (span <= 0.0)
        return from.value;

    const auto x = std::clamp ((time - static_cast<double> (from.time)) / span, 0.0, 1.0);

    switch (from.shape)
    {
        case CurveShape::hold:
            return from.value;

        case CurveShape::power:
        {
            const auto exponent = std::exp2 (std::clamp (from.tension, -maxTension, maxTension) * tensionRange);
            return from.value + (to.value - from.value) * std::pow (x, exponent);
        }

        case CurveShape::free:
            return from.value + (to.value - from.value) * freeSegment (x, from.handleTime, from.handleValue);

        case CurveShape::smooth:
        case CurveShape::rounded:
        {
            // Cubic Hermite between the two nodes with their slopes.
            const auto m0 = smoothSlope (points, index) * span;
            const auto m1 = smoothSlope (points, index + 1) * span;
            const auto x2 = x * x;
            const auto x3 = x2 * x;
            return (2.0 * x3 - 3.0 * x2 + 1.0) * from.value + (x3 - 2.0 * x2 + x) * m0
                   + (-2.0 * x3 + 3.0 * x2) * to.value + (x3 - x2) * m1;
        }

        case CurveShape::linear:
            return from.value + (to.value - from.value) * x;
    }

    return from.value;
}
} // namespace

const std::array<LaneInfo, numLanes>& laneInfos() noexcept
{
    static constexpr std::array<LaneInfo, numLanes> infos { {
        { "pitch", 0.0, -48.0, 48.0 },
        { "pressure", 0.0, 0.0, 1.0 },
        { "timbre", 0.5, 0.0, 1.0 },
        { "vibrato", 0.0, 0.0, 1.0 },
    } };

    return infos;
}

bool laneFromId (const juce::String& id, Lane& result) noexcept
{
    const auto& infos = laneInfos();

    for (size_t i = 0; i < infos.size(); ++i)
    {
        if (id == infos[i].id)
        {
            result = static_cast<Lane> (i);
            return true;
        }
    }

    return false;
}

void anchorCurve (Curve& curve, double defaultValue)
{
    if (std::ranges::any_of (curve, [] (const CurvePoint& p) { return p.time == 0; }))
        return;

    CurvePoint start;
    start.value = evaluateCurve (curve, 0.0, defaultValue);
    curve.insert (curve.begin(), start);
}

std::pair<Curve, Curve> splitCurve (const Curve& curve, Ticks at, double defaultValue)
{
    if (curve.empty())
        return {};

    const auto value = evaluateCurve (curve, static_cast<double> (at), defaultValue);
    const auto later = std::ranges::upper_bound (curve, at, {}, &CurvePoint::time);

    // The left part keeps the points before the split. It needs a point at the split only where the curve was
    // still moving there; a flat end holds by itself.
    Curve left (curve.begin(),
                std::find_if (curve.begin(), later, [at] (const CurvePoint& p) { return p.time >= at; }));

    if (left.empty() || ! juce::exactlyEqual (left.back().value, value))
    {
        CurvePoint end;
        end.time = at;
        end.value = value;
        left.push_back (end);
    }

    // The right part starts with the value at the split, leaving it the way the cut segment did.
    CurvePoint start;
    start.value = value;

    if (later != curve.begin())
    {
        const auto& previous = *std::prev (later);

        // A free segment's handle belongs to its original two nodes; the right half starts a new segment,
        // so it falls back to a straight line (the left half keeps its handle).
        if (previous.shape != CurveShape::free)
        {
            start.shape = previous.shape;
            start.tension = previous.tension;
        }
    }

    Curve right { start };

    for (auto it = later; it != curve.end(); ++it)
    {
        auto point = *it;
        point.time -= at;
        right.push_back (point);
    }

    return { std::move (left), std::move (right) };
}

// The control points from the tension point (see Curve.h), then the Bezier's x is inverted before y is evaluated. The
// start control point never lies right of the end one (c1x <= hx <= c2x), so x rises with u: a safeguarded Newton
// iteration keeps [low, high] around the root and bisects whenever a step would leave it (or the slope is 0, as at an
// end whose handle lies on the time axis). Converged, u is within about 1e-15 of the root, finer than the 32 halvings
// this replaced (2^-33).
double freeSegment (double x, double hx, double hy, double* hint) noexcept
{
    hx = std::clamp (hx, 0.0, 1.0);
    hy = std::clamp (hy, 0.0, 1.0);

    const auto start = std::max (hx, hy);
    const auto end = std::max (1.0 - hx, 1.0 - hy);
    const auto c1x = hx * start;
    const auto c1y = hy * start;
    const auto c2x = 1.0 + (hx - 1.0) * end;
    const auto c2y = 1.0 + (hy - 1.0) * end;
    const auto bezier = [] (double u, double c1, double c2)
    {
        const auto v = 1.0 - u;
        return 3.0 * v * v * u * c1 + 3.0 * v * u * u * c2 + u * u * u;
    };

    // dx/du, from the same control points.
    const auto slope = [c1x, c2x] (double u)
    {
        const auto v = 1.0 - u;
        return 3.0 * v * v * c1x + 6.0 * v * u * (c2x - c1x) + 3.0 * u * u * (1.0 - c2x);
    };

    if (! (x > 0.0))
        return 0.0;

    if (x >= 1.0)
        return 1.0;

    constexpr auto tolerance = 1.0e-15;
    auto low = 0.0;
    auto high = 1.0;
    auto u = std::clamp (hint != nullptr ? *hint : x, 0.0, 1.0);

    for (int i = 0; i < 100; ++i)
    {
        const auto error = bezier (u, c1x, c2x) - x;

        if (error < 0.0)
            low = u;
        else
            high = u;

        if (std::abs (error) <= tolerance || high - low <= tolerance)
            break;

        const auto d = slope (u);
        const auto next = d > 0.0 ? u - error / d : -1.0;
        u = next > low && next < high ? next : (low + high) * 0.5;
    }

    if (hint != nullptr)
        *hint = u;

    return bezier (u, c1y, c2y);
}

double evaluateCurve (std::span<const CurvePoint> points, double time, double defaultValue) noexcept
{
    if (points.empty())
        return defaultValue;

    if (time <= static_cast<double> (points.front().time))
        return points.front().value;

    if (time >= static_cast<double> (points.back().time))
        return points.back().value;

    // First point strictly after `time`; the segment starts at the point before it.
    const auto next = std::upper_bound (points.begin(), points.end(), time, [] (double t, const CurvePoint& p)
                                        { return t < static_cast<double> (p.time); });

    return shapeSegment (points, static_cast<size_t> (next - points.begin() - 1), time);
}

double smoothSlope (std::span<const CurvePoint> points, size_t node) noexcept
{
    const auto left = node > 0 && smoothAt (points, node - 1);
    const auto right = smoothAt (points, node);

    // A rounded curve's first and last nodes are level: the curve holds their values beyond them, so any other slope
    // would leave a corner there.
    if ((node == 0 && right && points[node].shape == CurveShape::rounded)
        || (node + 1 == points.size() && left && points[node - 1].shape == CurveShape::rounded))
        return 0.0;

    if (left && right)
    {
        const auto d0 = secant (points, node - 1);
        const auto d1 = secant (points, node);

        // A rounded run's inner node: the slope through its neighbours (Catmull-Rom, for uneven spacing).
        if (points[node].shape == CurveShape::rounded && points[node - 1].shape == CurveShape::rounded)
            return (points[node + 1].value - points[node - 1].value)
                   / static_cast<double> (points[node + 1].time - points[node - 1].time);

        if (d0 * d1 <= 0.0)
            return 0.0;

        const auto h0 = gap (points, node - 1);
        const auto h1 = gap (points, node);
        const auto w0 = 2.0 * h1 + h0;
        const auto w1 = h1 + 2.0 * h0;
        return (w0 + w1) / (w0 / d0 + w1 / d1);
    }

    if (right)
        return smoothAt (points, node + 1) ? edgeSlope (gap (points, node), gap (points, node + 1),
                                                        secant (points, node), secant (points, node + 1))
                                           : secant (points, node);

    if (left)
        return node >= 2 && smoothAt (points, node - 2)
                   ? edgeSlope (gap (points, node - 1), gap (points, node - 2), secant (points, node - 1),
                                secant (points, node - 2))
                   : secant (points, node - 1);

    return 0.0;
}

} // namespace dawg
