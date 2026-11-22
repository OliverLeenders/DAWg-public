#include "dawg/TimingMap.h"

#include <algorithm>
#include <cmath>

namespace dawg
{

namespace
{
// How far a sampled tempo ramp may stray from the exact curve, in seconds.
constexpr double rampTolerance = 1.0e-5;
constexpr size_t maxRampSamples = 20000;
} // namespace

void normaliseTimingMarkers (std::vector<TimingMarker>& markers)
{
    std::ranges::stable_sort (markers, {}, &TimingMarker::tick);
    std::vector<TimingMarker> kept;
    TimingMarker last;

    for (const auto& marker : markers)
    {
        if (! std::isfinite (marker.tick) || ! std::isfinite (marker.frame) || marker.tick <= last.tick
            || marker.frame <= last.frame)
            continue;

        kept.push_back (marker);
        last = marker;
    }

    markers = std::move (kept);
}

TimingMap::TimingMap()
    : TimingMap (FileTiming {}, 48000.0)
{
}

TimingMap::TimingMap (const FileTiming& timing, double fileRate)
    : m_sourceRate (timing.musical() ? framesPerTick (fileRate, timing.sourceBpm) : fileRate)
{
    m_points.reserve (timing.markers.size() + 1);
    m_points.push_back ({});
    m_points.insert (m_points.end(), timing.markers.begin(), timing.markers.end());
}

size_t TimingMap::segmentAt (double tick) const noexcept
{
    // The last point at or before the tick (0 before the second point).
    const auto it = std::upper_bound (m_points.begin() + 1, m_points.end(), tick,
                                      [] (double t, const TimingMarker& p) { return t < p.tick; });
    return static_cast<size_t> (it - m_points.begin()) - 1;
}

double TimingMap::slope (size_t segment) const noexcept
{
    if (segment + 1 >= m_points.size())
        return m_sourceRate;

    const auto& a = m_points[segment];
    const auto& b = m_points[segment + 1];
    return (b.frame - a.frame) / (b.tick - a.tick);
}

double TimingMap::frameAt (double tick) const noexcept
{
    if (tick < 0.0)
        return tick * m_sourceRate;

    const auto segment = segmentAt (tick);
    const auto& p = m_points[segment];
    return p.frame + (tick - p.tick) * slope (segment);
}

double TimingMap::frameAt (double tick, size_t& hint) const noexcept
{
    if (tick < 0.0)
        return tick * m_sourceRate;

    auto segment = std::min (hint, m_points.size() - 1);

    if (m_points[segment].tick > tick || (segment + 1 < m_points.size() && m_points[segment + 1].tick <= tick))
    {
        // A step to the neighbour covers monotonic playback; anything else searches.
        const auto nextHolds = segment + 1 < m_points.size() && m_points[segment + 1].tick <= tick
                               && (segment + 2 == m_points.size() || m_points[segment + 2].tick > tick);

        if (nextHolds)
            ++segment;
        else
            segment = segmentAt (tick);
    }

    hint = segment;
    const auto& p = m_points[segment];
    return p.frame + (tick - p.tick) * slope (segment);
}

double TimingMap::tickAt (double frame) const noexcept
{
    if (frame < 0.0)
        return frame / m_sourceRate;

    const auto it = std::upper_bound (m_points.begin() + 1, m_points.end(), frame,
                                      [] (double f, const TimingMarker& p) { return f < p.frame; });
    const auto segment = static_cast<size_t> (it - m_points.begin()) - 1;
    const auto& p = m_points[segment];
    return p.tick + (frame - p.frame) / slope (segment);
}

double TimingMap::rateAt (double tick) const noexcept
{
    return tick < 0.0 ? m_sourceRate : slope (segmentAt (tick));
}

namespace
{
// Drops the markers the map would run through anyway: those within frameTolerance of the straight line from the
// point before to the point after (on at the natural rate past the last one).
void dropRedundantMarkers (std::vector<TimingMarker>& markers, double naturalRate, double frameTolerance)
{
    std::vector<TimingMarker> kept;
    kept.reserve (markers.size());

    for (size_t i = 0; i < markers.size(); ++i)
    {
        const auto before = kept.empty() ? TimingMarker {} : kept.back();
        const auto& marker = markers[i];
        const auto predicted = i + 1 < markers.size()
                                   ? before.frame
                                         + (markers[i + 1].frame - before.frame) * (marker.tick - before.tick)
                                               / (markers[i + 1].tick - before.tick)
                                   : before.frame + (marker.tick - before.tick) * naturalRate;

        if (std::abs (marker.frame - predicted) > frameTolerance)
            kept.push_back (marker);
    }

    markers = std::move (kept);
}
} // namespace

std::vector<TimingMarker> convertTimingMarkers (const TempoMap& map, double anchorTick, const FileTiming& from,
                                                juce::int64 fileFrames, double fileRate, double sourceBpm)
{
    const TimingMap source (from, fileRate);
    const auto toMusical = ! from.musical();
    const auto anchorSeconds = map.secondsAt (anchorTick);

    // Where a position of the old content sounds on the timeline, and what sounds at a timeline tick.
    const auto tickOf = [&] (double position)
    { return toMusical ? map.tickAt (anchorSeconds + position) : anchorTick + position; };
    const auto marker = [&] (double tick)
    {
        const auto seconds = map.secondsAt (tick) - anchorSeconds;
        return TimingMarker { toMusical ? tick - anchorTick : seconds,
                              source.frameAt (toMusical ? seconds : tick - anchorTick) };
    };

    const auto endTick = tickOf (source.tickAt (static_cast<double> (fileFrames)));
    std::vector<double> ticks;

    for (const auto& old : from.markers)
        if (const auto tick = tickOf (old.tick); tick < endTick)
            ticks.push_back (tick);

    const auto& points = map.points();

    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto& point = points[i];
        const auto at = static_cast<double> (point.tick);

        // Tick 0 changes nothing: the first tempo also runs before it.
        if (at >= endTick)
            break;

        if (at > anchorTick && i > 0)
            ticks.push_back (at);

        if (! point.ramp || i + 1 >= points.size())
            continue;

        // Sample the ramp: the chords stray from the curve by at most h^2 / 8 times the curve's second derivative.
        const auto to = static_cast<double> (points[i + 1].tick);
        const auto lo = std::max (at, anchorTick);
        const auto hi = std::min (to, endTick);

        if (hi <= lo)
            continue;

        const auto k = (points[i + 1].bpm - point.bpm) / (to - at);
        const auto slowest = std::min (point.bpm, points[i + 1].bpm);
        const auto curvature = 60.0 * std::abs (k) / (static_cast<double> (ticksPerBeat) * slowest * slowest);
        const auto spacing = curvature > 0.0 ? std::sqrt (8.0 * rampTolerance / curvature) : hi - lo;
        const auto count =
            std::clamp (static_cast<size_t> (std::ceil ((hi - lo) / spacing)), size_t { 1 }, maxRampSamples);

        for (size_t s = 1; s < count; ++s)
            ticks.push_back (lo + (hi - lo) * static_cast<double> (s) / static_cast<double> (count));
    }

    // The file's end, where the natural rate past the last marker may differ from how it plays now.
    ticks.push_back (endTick);
    std::ranges::sort (ticks);

    std::vector<TimingMarker> markers;
    markers.reserve (ticks.size());

    for (const auto tick : ticks)
        if (tick > anchorTick)
            markers.push_back (marker (tick));

    normaliseTimingMarkers (markers);

    // Musical markers pin tempo ramps finely, so only exact repeats go. Back in real time, a ramp's samples lie up to
    // rampTolerance off the straight line they came from, so anything within a frame goes.
    if (toMusical)
        dropRedundantMarkers (markers, framesPerTick (fileRate, sourceBpm), 1.0e-6);
    else
        dropRedundantMarkers (markers, fileRate, 1.0);
    return markers;
}

std::vector<TimingMarker> markersFromTempo (const TempoMap& map, double anchorTick, juce::int64 fileFrames,
                                            double fileRate, double sourceBpm)
{
    return convertTimingMarkers (map, anchorTick, FileTiming {}, fileFrames, fileRate, sourceBpm);
}

double tempoFromName (const juce::String& name)
{
    // The last number before "bpm", with an optional space, underscore or dash between them.
    const auto lower = name.toLowerCase();

    for (auto at = lower.indexOf ("bpm"); at > 0; at = lower.indexOf (at + 3, "bpm"))
    {
        auto end = at;

        while (end > 0 && (lower[end - 1] == ' ' || lower[end - 1] == '_' || lower[end - 1] == '-'))
            --end;

        auto start = end;

        while (start > 0 && (juce::CharacterFunctions::isDigit (lower[start - 1]) || lower[start - 1] == '.'))
            --start;

        // Not part of a longer word or number before it.
        if (start == end || (start > 0 && juce::CharacterFunctions::isLetterOrDigit (lower[start - 1])))
            continue;

        const auto bpm = lower.substring (start, end).getDoubleValue();

        if (bpm >= 60.0 && bpm <= 200.0)
            return bpm;
    }

    return 0.0;
}

double tempoFromLength (double seconds, double barQuarters, double projectBpm)
{
    constexpr double longest = 30.0;

    if (seconds <= 0.0 || seconds >= longest || barQuarters <= 0.0)
        return 0.0;

    auto best = 0.0;

    for (const auto bars : { 1.0, 2.0, 4.0, 8.0 })
    {
        const auto bpm = 60.0 * barQuarters * bars / seconds;

        if (bpm >= 80.0 && bpm <= 160.0
            && (juce::exactlyEqual (best, 0.0) || std::abs (bpm - projectBpm) < std::abs (best - projectBpm)))
            best = bpm;
    }

    return std::round (best * 100.0) / 100.0;
}

double suggestTempo (double metadataBpm, const juce::String& name, double seconds, double barQuarters,
                     double projectBpm)
{
    if (metadataBpm >= 20.0 && metadataBpm <= 999.0)
        return metadataBpm;

    if (const auto named = tempoFromName (name); named > 0.0)
        return named;

    return tempoFromLength (seconds, barQuarters, projectBpm);
}

} // namespace dawg
