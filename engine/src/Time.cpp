#include "dawg/Time.h"

#include <algorithm>
#include <cmath>

namespace dawg
{

namespace
{
// Built at static initialisation, so a default TimeMap made on the audio thread never allocates. If this
// allocation fails there is nothing to fall back on, so terminating before the audio thread starts is fine.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): see above
const TempoMap defaultTempoMap;
} // namespace

TimeMap::TimeMap() noexcept
    : TimeMap (defaultTempoMap, 48000.0)
{
}

TimeMap::TimeMap (const TempoMap& map, double sampleRate) noexcept
    : m_map (&map),
      m_sampleRate (sampleRate)
{
    jassert (sampleRate > 0.0);
}

void TimeMap::anchor (double tick, juce::int64 sample) noexcept
{
    m_anchorSeconds = m_map->secondsAt (tick);
    m_anchorSample = sample;
}

// ---------------------------------------------------------------------------------------------------------
// Tempo map (docs/design/tempo-map.md, TM1-TM3).

namespace
{
constexpr auto secondsPerMinute = 60.0;
const auto tpb = static_cast<double> (ticksPerBeat);

// The seconds a tempo segment takes over dt ticks from its start, at start tempo b0 changing by k BPM per tick.
// With b(t) = b0 + k t, the time is the integral of 60 / (b(t) tpb), which is 60 / (k tpb) ln (b(t) / b0).
[[nodiscard]] double segmentSeconds (double b0, double k, double dt) noexcept
{
    if (std::abs (k * dt) < 1.0e-9 * b0)
        return dt * secondsPerMinute / (b0 * tpb);

    return secondsPerMinute / (k * tpb) * std::log1p (k * dt / b0);
}

// The inverse: the ticks a segment covers in ds seconds from its start.
[[nodiscard]] double segmentTicks (double b0, double k, double ds) noexcept
{
    const auto x = ds * k * tpb / secondsPerMinute;

    if (std::abs (x) < 1.0e-12)
        return ds * b0 * tpb / secondsPerMinute;

    return b0 * std::expm1 (x) / k;
}
} // namespace

void normaliseTempoPoints (std::vector<TempoPoint>& points)
{
    for (auto& point : points)
        point.bpm = std::clamp (std::isfinite (point.bpm) ? point.bpm : 120.0, limits::minTempo, limits::maxTempo);

    // Stable, so of points on the same tick the later one in the list wins.
    std::stable_sort (points.begin(), points.end(), [] (const auto& a, const auto& b) { return a.tick < b.tick; });
    std::vector<TempoPoint> kept;

    for (const auto& point : points)
    {
        if (! kept.empty() && kept.back().tick == point.tick)
            kept.back() = point;
        else
            kept.push_back (point);
    }

    if (kept.empty())
        kept.push_back ({});
    else if (kept.front().tick > 0)
        kept.insert (kept.begin(), { 0, kept.front().bpm, false });

    // Points before tick 0 collapse onto it, keeping the last of them.
    while (kept.size() > 1 && kept[1].tick <= 0)
        kept.erase (kept.begin());

    kept.front().tick = 0;
    points = std::move (kept);
}

void normaliseSignatures (std::vector<TimeSignature>& signatures)
{
    for (auto& signature : signatures)
    {
        signature.bar = std::max (0, signature.bar);
        signature.numerator = std::clamp (signature.numerator, limits::minBeatsPerBar, limits::maxBeatsPerBar);
        const auto denominator = std::clamp (signature.denominator, 2, 16);
        signature.denominator = 1 << static_cast<int> (std::round (std::log2 (denominator)));
    }

    std::stable_sort (signatures.begin(), signatures.end(),
                      [] (const auto& a, const auto& b) { return a.bar < b.bar; });
    std::vector<TimeSignature> kept;

    for (const auto& signature : signatures)
    {
        if (! kept.empty() && kept.back().bar == signature.bar)
            kept.back() = signature;
        else
            kept.push_back (signature);
    }

    if (kept.empty())
        kept.push_back ({});
    else if (kept.front().bar > 0)
        kept.insert (kept.begin(), { 0, 4, 4 });

    signatures = std::move (kept);
}

TempoMap::TempoMap()
    : TempoMap ({}, {})
{
}

TempoMap::TempoMap (std::vector<TempoPoint> points, std::vector<TimeSignature> signatures)
    : m_points (std::move (points)),
      m_signatures (std::move (signatures))
{
    normaliseTempoPoints (m_points);
    normaliseSignatures (m_signatures);

    m_startSeconds.reserve (m_points.size());
    m_startSeconds.push_back (0.0);

    for (size_t i = 1; i < m_points.size(); ++i)
    {
        const auto dt = static_cast<double> (m_points[i].tick - m_points[i - 1].tick);
        m_startSeconds.push_back (m_startSeconds.back() + segmentSeconds (m_points[i - 1].bpm, slope (i - 1), dt));
    }

    m_signatureTicks.reserve (m_signatures.size());
    m_signatureTicks.push_back (0);

    for (size_t i = 1; i < m_signatures.size(); ++i)
        m_signatureTicks.push_back (m_signatureTicks.back()
                                    + (m_signatures[i].bar - m_signatures[i - 1].bar) * m_signatures[i - 1].barTicks());
}

double TempoMap::slope (size_t segment) const noexcept
{
    if (segment + 1 >= m_points.size() || ! m_points[segment].ramp)
        return 0.0;

    const auto& a = m_points[segment];
    const auto& b = m_points[segment + 1];
    return (b.bpm - a.bpm) / static_cast<double> (b.tick - a.tick);
}

size_t TempoMap::segmentAt (double tick) const noexcept
{
    const auto it = std::upper_bound (m_points.begin(), m_points.end(), tick, [] (double t, const TempoPoint& point)
                                      { return t < static_cast<double> (point.tick); });
    return it == m_points.begin() ? 0 : static_cast<size_t> (it - m_points.begin() - 1);
}

size_t TempoMap::segmentAtSeconds (double seconds) const noexcept
{
    const auto it = std::upper_bound (m_startSeconds.begin(), m_startSeconds.end(), seconds);
    return it == m_startSeconds.begin() ? 0 : static_cast<size_t> (it - m_startSeconds.begin() - 1);
}

double TempoMap::secondsAt (double tick) const noexcept
{
    if (tick < 0.0)
        return tick * secondsPerMinute / (m_points.front().bpm * tpb);

    const auto i = segmentAt (tick);
    return m_startSeconds[i]
           + segmentSeconds (m_points[i].bpm, slope (i), tick - static_cast<double> (m_points[i].tick));
}

double TempoMap::tickAt (double seconds) const noexcept
{
    if (seconds < 0.0)
        return seconds * m_points.front().bpm * tpb / secondsPerMinute;

    const auto i = segmentAtSeconds (seconds);
    return static_cast<double> (m_points[i].tick)
           + segmentTicks (m_points[i].bpm, slope (i), seconds - m_startSeconds[i]);
}

double TempoMap::bpmAt (double tick) const noexcept
{
    if (tick < 0.0)
        return m_points.front().bpm;

    const auto i = segmentAt (tick);
    return m_points[i].bpm + slope (i) * (tick - static_cast<double> (m_points[i].tick));
}

size_t TempoMap::signatureIndexAt (double tick) const noexcept
{
    const auto it = std::upper_bound (m_signatureTicks.begin(), m_signatureTicks.end(), tick,
                                      [] (double t, Ticks start) { return t < static_cast<double> (start); });
    return it == m_signatureTicks.begin() ? 0 : static_cast<size_t> (it - m_signatureTicks.begin() - 1);
}

const TimeSignature& TempoMap::signatureAt (double tick) const noexcept
{
    return m_signatures[signatureIndexAt (tick)];
}

Ticks TempoMap::tickOfBar (int bar) const noexcept
{
    const auto it = std::upper_bound (m_signatures.begin(), m_signatures.end(), bar,
                                      [] (int b, const TimeSignature& signature) { return b < signature.bar; });
    const auto i = it == m_signatures.begin() ? 0 : static_cast<size_t> (it - m_signatures.begin() - 1);
    return m_signatureTicks[i] + (bar - m_signatures[i].bar) * m_signatures[i].barTicks();
}

BarPosition TempoMap::barAt (double tick) const noexcept
{
    const auto i = signatureIndexAt (tick);
    const auto& signature = m_signatures[i];
    const auto barTicks = static_cast<double> (signature.barTicks());
    const auto beatTicks = static_cast<double> (signature.beatTicks());
    const auto intoSignature = tick - static_cast<double> (m_signatureTicks[i]);
    const auto bars = std::floor (intoSignature / barTicks);
    const auto intoBar = intoSignature - bars * barTicks;
    const auto beat = std::min (std::floor (intoBar / beatTicks), static_cast<double> (signature.numerator - 1));
    return { signature.bar + static_cast<int> (bars), static_cast<int> (beat), intoBar - beat * beatTicks };
}

} // namespace dawg
