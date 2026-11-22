#include "dawg/Sequence.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace dawg
{

double CompiledNote::evaluate (Lane lane, double tick) const noexcept
{
    const auto index = static_cast<size_t> (lane);
    return evaluateCurve (lanes[index], tick - static_cast<double> (start), laneInfos()[index].defaultValue);
}

double CompiledNote::pitchOffset (double tick, const TempoMap& map) const noexcept
{
    const auto curve = evaluate (Lane::pitch, tick);

    if (! vibrato.active())
        return curve;

    // A free-running vibrato runs in real time: the seconds since it began, through the tempo map
    // (docs/design/tempo-map.md, TM7), as an average duration per tick.
    const auto began = static_cast<double> (start + vibrato.delay);
    const auto secondsPerTick = tick > began ? (map.secondsAt (tick) - map.secondsAt (began)) / (tick - began)
                                             : 60.0 / (map.bpmAt (tick) * static_cast<double> (ticksPerBeat));

    return curve
           + vibratoOffset (vibrato, tick - static_cast<double> (start), secondsPerTick,
                            evaluate (Lane::vibrato, tick));
}

void LinkGlide::begin (double fromPitch, double fromPressure, double fromTimbre, float fromVelocity) noexcept
{
    active = true;
    m_measured = false;
    m_pitch = fromPitch;
    m_pressure = fromPressure;
    m_timbre = fromTimbre;
    m_velocity = fromVelocity;
}

void LinkGlide::apply (const CompiledNote& note, double tick, const TempoMap& map, double& pitch, double& pressure,
                       double& timbre, float& velocity) noexcept
{
    velocity = note.velocity;

    if (! active)
        return;

    if (! m_measured)
    {
        m_pitch -= note.pitch + note.pitchOffset (static_cast<double> (note.start), map);
        m_measured = true;
    }

    // Levels never jump: a link without a glide still takes a moment, since velocity scales the output.
    constexpr double shortestCrossfade = 0.005;
    const auto elapsed = map.secondsAt (tick) - map.secondsAt (static_cast<double> (note.start));
    const auto glided = note.glideSeconds > 0.0 ? std::clamp (elapsed / note.glideSeconds, 0.0, 1.0) : 1.0;
    const auto faded = std::clamp (elapsed / std::max (note.glideSeconds, shortestCrossfade), 0.0, 1.0);

    pitch += m_pitch * (1.0 - glided);
    pressure = m_pressure + (pressure - m_pressure) * faded;
    timbre = m_timbre + (timbre - m_timbre) * faded;
    velocity = m_velocity + (velocity - m_velocity) * static_cast<float> (faded);
    active = glided < 1.0 || faded < 1.0;
}

TrackSequence::TrackSequence (std::span<const ClipPtr> clips)
{
    struct LaneRange
    {
        size_t offset;
        size_t count;
    };

    std::vector<std::array<LaneRange, numLanes>> ranges;

    // Links join notes of one pass through one clip: (clip, repetition, note id) -> index, and who wants to link where.
    std::map<std::tuple<size_t, int, ObjectId>, size_t> placed;
    struct PendingLink
    {
        size_t from;
        std::tuple<size_t, int, ObjectId> to;
        double glideMs;
    };
    std::vector<PendingLink> pendingLinks;
    size_t clipIndex = 0;

    // Linked clips and loop repeats play the same note id several times: number the placements per note id
    // (deterministically, in clip order), so every sounding instance has its own runtime id.
    std::map<ObjectId, int> placements;

    for (const auto& clip : clips)
    {
        // A sequencer clip plays no notes of its own: the step sequencer plays inside it (docs/design/step-sequencer.md).
        if (clip->isSequencer())
            m_spans.push_back ({ clip->start, clip->start + clip->length, clip->start - clip->offset,
                                 clip->sequencerPattern, clip->sequencerStartStep, clip->id });

        forEachAudibleNote (*clip,
                            [&] (const Note& note, Ticks start, Ticks end, int repetition)
                            {
                                placed[{ clipIndex, repetition, note.id }] = m_notes.size();

                                if (note.link.has_value())
                                    pendingLinks.push_back ({ m_notes.size(),
                                                              { clipIndex, repetition, note.link->to },
                                                              note.link->glideMs });

                                CompiledNote compiled;
                                compiled.id = runtimeNoteId (note.id, placements[note.id]++);
                                compiled.start = start;
                                compiled.end = end;
                                compiled.pitch = note.pitch;
                                compiled.velocity = static_cast<float> (note.velocity);
                                compiled.releaseVelocity = static_cast<float> (note.releaseVelocity);
                                compiled.vibrato = note.vibrato;
                                compiled.holdUntil = end;

                                std::array<LaneRange, numLanes> laneRanges {};

                                for (size_t lane = 0; lane < numLanes; ++lane)
                                {
                                    const auto& curve = note.expression[lane];
                                    laneRanges[lane] = { m_points.size(), curve.size() };
                                    m_points.insert (m_points.end(), curve.begin(), curve.end());
                                }

                                m_notes.push_back (compiled);
                                ranges.push_back (laneRanges);
                            });
        ++clipIndex;
    }

    // A link plays when its target is there, starts later and isn't taken (docs/design/linked-notes.md, L3).
    for (const auto& link : pendingLinks)
    {
        const auto target = placed.find (link.to);

        if (target == placed.end())
            continue;

        auto& from = m_notes[link.from];
        auto& to = m_notes[target->second];

        if (to.start > from.start && to.linkFrom == 0)
        {
            from.linkTo = to.id;
            from.holdUntil = to.start;
            to.linkFrom = from.id;
            to.glideSeconds = link.glideMs / 1000.0;
        }
    }

    // Controller curves: one stretch per clip pass (like the notes' placements), sorted by start.
    std::vector<std::pair<size_t, size_t>> curveRanges;

    for (const auto& clip : clips)
    {
        for (const auto& [controller, curve] : clip->controllers)
        {
            const auto range = std::pair { m_controllerPoints.size(), curve.size() };
            m_controllerPoints.insert (m_controllerPoints.end(), curve.begin(), curve.end());

            forEachPass (*clip,
                         [&] (Ticks start, Ticks end, Ticks contentAt)
                         {
                             m_segments.push_back ({ controller, start, end, contentAt, {} });
                             curveRanges.push_back (range);
                         });
        }
    }

    // The point storage is complete: spans into it stay valid from here on.
    for (size_t i = 0; i < m_segments.size(); ++i)
        m_segments[i].points =
            std::span<const CurvePoint> (m_controllerPoints).subspan (curveRanges[i].first, curveRanges[i].second);

    std::ranges::stable_sort (m_segments, {}, &ControllerSegment::start);

    // m_points is complete, so spans into it stay valid from here on.
    for (size_t i = 0; i < m_notes.size(); ++i)
        for (size_t lane = 0; lane < numLanes; ++lane)
            m_notes[i].lanes[lane] =
                std::span<const CurvePoint> (m_points).subspan (ranges[i][lane].offset, ranges[i][lane].count);

    std::ranges::sort (m_notes, [] (const CompiledNote& a, const CompiledNote& b)
                       { return a.start != b.start ? a.start < b.start : a.id < b.id; });

    m_byId.resize (m_notes.size());

    for (size_t i = 0; i < m_byId.size(); ++i)
        m_byId[i] = i;

    std::ranges::sort (m_byId, [this] (size_t a, size_t b) { return m_notes[a].id < m_notes[b].id; });
}

const CompiledNote* TrackSequence::find (ObjectId noteId) const noexcept
{
    const auto it = std::ranges::lower_bound (m_byId, noteId, {}, [this] (size_t i) { return m_notes[i].id; });

    if (it == m_byId.end() || m_notes[*it].id != noteId)
        return nullptr;

    return &m_notes[*it];
}

size_t TrackSequence::firstStartingAtOrAfter (double tick) const noexcept
{
    const auto it = std::ranges::lower_bound (m_notes, tick, {},
                                              [] (const CompiledNote& n) { return static_cast<double> (n.start); });
    return static_cast<size_t> (it - m_notes.begin());
}

} // namespace dawg
