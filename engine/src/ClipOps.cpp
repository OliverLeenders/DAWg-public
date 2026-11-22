#include "dawg/ClipOps.h"

#include "dawg/NodeRegistry.h"
#include "dawg/TimingMap.h"
#include "dawg/VarUtils.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <tuple>

namespace dawg
{
namespace
{
[[nodiscard]] Ticks roundTicks (double ticks) noexcept
{
    return static_cast<Ticks> (std::llround (ticks));
}

[[nodiscard]] Ticks endOf (const NoteClip& clip) noexcept
{
    return clip.start + clip.length;
}

[[nodiscard]] bool isMusical (const NoteClip& clip, const Project& project)
{
    const auto* file = project.findAudioFile (clip.fileId);
    return file != nullptr && file->timing.musical();
}

// The real time of a clip's fade: content ticks at the start tempo for one-shot files, timeline ticks for musical
// ones.
[[nodiscard]] double fadeSeconds (const NoteClip& clip, bool atStart, bool musical, const TempoMap& map)
{
    const auto ticks = static_cast<double> (atStart ? clip.fadeIn : clip.fadeOut);

    if (! musical)
        return ticks * 60.0 / (map.bpmAt (static_cast<double> (clip.start)) * ticksPerBeat);

    const auto edge = static_cast<double> (atStart ? clip.start : endOf (clip));
    return atStart ? map.secondsAt (edge + ticks) - map.secondsAt (edge)
                   : map.secondsAt (edge) - map.secondsAt (edge - ticks);
}

// Whether a is above b in the stack (takes-lanes.md, T1).
[[nodiscard]] bool above (const NoteClip& a, const NoteClip& b) noexcept
{
    return std::tie (a.order, a.id) > std::tie (b.order, b.id);
}

// File frames per content tick of a one-shot clip: the tick length at the tempo where it starts.
[[nodiscard]] double oneShotFramesPerTick (const AudioFile& file, const TempoMap& map, Ticks clipStart)
{
    return 60.0 / (map.bpmAt (static_cast<double> (clipStart)) * ticksPerBeat) * file.sampleRate;
}
} // namespace

double fileContentTicks (const AudioFile& file, const TempoMap& map, Ticks clipStart)
{
    // Ticks for musical files; content seconds for one-shot ones, which run at the tempo where the clip starts.
    const auto content = TimingMap (file.timing, file.sampleRate).tickAt (static_cast<double> (file.frames));
    return file.timing.musical() ? content : content * file.sampleRate / oneShotFramesPerTick (file, map, clipStart);
}

FileTiming reversedTiming (const FileTiming& timing, double fileRate, juce::int64 frames)
{
    auto result = timing;

    if (timing.markers.empty() && ! timing.musical())
        return result;

    // The original's points, (0, 0) included, mirrored: what was at frame f is now at frames - f, and content ticks
    // run backwards from the file's end. The original's run past its last marker (at the source rate, up to the end
    // of the file) becomes the copy's first segment, from its implicit (0, 0).
    const TimingMap map (timing, fileRate);
    const auto total = static_cast<double> (frames);
    const auto end = map.tickAt (total);
    result.markers.clear();

    for (const auto& point : map.points())
        if (point.frame < total)
            result.markers.push_back ({ end - point.tick, total - point.frame });

    normaliseTimingMarkers (result.markers);
    return result;
}

void mirrorClip (NoteClip& clip, double contentTicks)
{
    const auto mirror = [contentTicks] (Ticks t) { return contentTicks - static_cast<double> (t); };

    if (clip.loopLength > 0)
    {
        const auto loopEnd = clip.loopStart + clip.loopLength;
        // Each pass plays backwards: the loop range mirrors, and the first pass keeps its length, now ending at the
        // mirrored loop's end.
        const auto firstPass = clip.offset < loopEnd ? loopEnd - clip.offset : clip.loopLength;
        const auto newLoopStart = std::max (Ticks { 0 }, roundTicks (mirror (loopEnd)));
        clip.offset = std::max (Ticks { 0 }, newLoopStart + clip.loopLength - firstPass);
        clip.loopStart = newLoopStart;
    }
    else
    {
        // A clip reaching past its file's end starts at the copy's start (there is nothing before it to play).
        clip.offset = std::max (Ticks { 0 }, roundTicks (mirror (clip.offset + clip.length)));
    }

    std::swap (clip.fadeIn, clip.fadeOut);
    std::swap (clip.fadeInShape, clip.fadeOutShape);
}

std::vector<std::pair<juce::int64, juce::int64>> playedFrameRanges (const NoteClip& clip, const AudioFile& file,
                                                                    const TempoMap& map)
{
    // The clip's length in content ticks: timeline ticks for musical files, else its real time at the start tempo.
    const auto musical = file.timing.musical();
    const auto perTick = oneShotFramesPerTick (file, map, clip.start);
    const auto start = static_cast<double> (clip.start);
    const auto length = musical ? static_cast<double> (clip.length)
                                : (map.secondsAt (start + static_cast<double> (clip.length)) - map.secondsAt (start))
                                      * file.sampleRate / perTick;

    std::vector<std::pair<double, double>> content;
    const auto offset = static_cast<double> (clip.offset);

    if (clip.loopLength > 0)
    {
        const auto loopStart = static_cast<double> (clip.loopStart);
        const auto loopEnd = loopStart + static_cast<double> (clip.loopLength);
        const auto from = offset < loopEnd ? offset : loopStart;
        const auto firstPass = loopEnd - from;

        if (length <= firstPass)
        {
            content.emplace_back (from, from + length);
        }
        else
        {
            content.emplace_back (from, loopEnd);
            content.emplace_back (loopStart, std::min (loopEnd, loopStart + length - firstPass));
        }
    }
    else
    {
        content.emplace_back (offset, offset + length);
    }

    const TimingMap timing (file.timing, file.sampleRate);
    // One-shot content ticks are content seconds at the start tempo.
    const auto frameAt = [&] (double tick)
    { return timing.frameAt (musical ? tick : tick * perTick / file.sampleRate); };
    std::vector<std::pair<juce::int64, juce::int64>> frames;

    for (const auto& [from, to] : content)
    {
        const auto a =
            std::clamp (static_cast<juce::int64> (std::floor (frameAt (from))), juce::int64 { 0 }, file.frames);
        const auto b = std::clamp (static_cast<juce::int64> (std::ceil (frameAt (to))), juce::int64 { 0 }, file.frames);

        if (a < b)
            frames.emplace_back (a, b);
    }

    std::ranges::sort (frames);
    std::vector<std::pair<juce::int64, juce::int64>> merged;

    for (const auto& range : frames)
    {
        if (! merged.empty() && range.first <= merged.back().second)
            merged.back().second = std::max (merged.back().second, range.second);
        else
            merged.push_back (range);
    }

    return merged;
}

std::vector<double> normaliseChanges (const std::vector<float>& peaks, double targetDb, bool together)
{
    const auto change = [targetDb] (float peak)
    { return peak > 0.0f ? targetDb - 20.0 * std::log10 (static_cast<double> (peak)) : 0.0; };

    if (together)
    {
        const auto loudest = peaks.empty() ? 0.0f : std::ranges::max (peaks);
        return std::vector<double> (peaks.size(), change (loudest));
    }

    std::vector<double> result;
    result.reserve (peaks.size());

    for (const auto peak : peaks)
        result.push_back (change (peak));

    return result;
}

bool writeReversed (juce::AudioFormatReader& reader, const juce::File& target)
{
    const auto channels = static_cast<int> (reader.numChannels);
    const auto frames = reader.lengthInSamples;

    if (channels < 1 || frames < 1 || ! target.getParentDirectory().createDirectory())
        return false;

    std::ignore = target.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (target.createOutputStream());
    juce::WavAudioFormat wav;
    auto writer =
        stream == nullptr
            ? nullptr
            : wav.createWriterFor (stream,
                                   juce::AudioFormatWriterOptions {}
                                       .withSampleRate (reader.sampleRate)
                                       .withNumChannels (channels)
                                       .withBitsPerSample (32)
                                       .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));

    if (writer == nullptr)
        return false;

    // Chunks from the end backwards, each reversed in place.
    constexpr juce::int64 chunk = 65536;
    juce::AudioBuffer<float> buffer (channels, static_cast<int> (chunk));

    for (auto end = frames; end > 0; end -= chunk)
    {
        const auto start = std::max (juce::int64 { 0 }, end - chunk);
        const auto count = static_cast<int> (end - start);

        if (! reader.read (&buffer, 0, count, start, true, true))
            return false;

        for (int ch = 0; ch < channels; ++ch)
            std::reverse (buffer.getWritePointer (ch), buffer.getWritePointer (ch) + count);

        if (! writer->writeFromAudioSampleBuffer (buffer, 0, count))
            return false;
    }

    return true;
}
} // namespace dawg

namespace dawg
{
CoveredEdges coveredEdges (const std::vector<ClipPtr>& clips, const NoteClip& clip)
{
    CoveredEdges edges;

    if (clip.fileId == 0)
        return edges;

    for (const auto& other : clips)
    {
        if (other->id == clip.id || other->fileId == 0 || ! above (clip, *other))
            continue;

        edges.start = edges.start || (other->start < clip.start && endOf (*other) > clip.start);
        edges.end = edges.end || (other->start < endOf (clip) && endOf (*other) > endOf (clip));
    }

    return edges;
}

Ticks fadeTicksFor (const NoteClip& clip, bool atStart, double seconds, bool musical, const TempoMap& map)
{
    double ticks = 0.0;

    if (! musical)
    {
        ticks = seconds * map.bpmAt (static_cast<double> (clip.start)) * ticksPerBeat / 60.0;
    }
    else
    {
        const auto edge = static_cast<double> (atStart ? clip.start : endOf (clip));
        const auto edgeSeconds = map.secondsAt (edge);
        ticks = atStart ? map.tickAt (edgeSeconds + seconds) - edge : edge - map.tickAt (edgeSeconds - seconds);
    }

    return std::clamp (roundTicks (ticks), Ticks { 1 }, std::max (Ticks { 1 }, clip.length / 2));
}

std::vector<ClipPtr> crossfadedParts (const std::vector<ClipPtr>& clips, const Project& project)
{
    std::vector<ClipPtr> audio;
    std::ranges::copy_if (clips, std::back_inserter (audio), [] (const ClipPtr& c) { return c->fileId != 0; });
    auto parts = playedClips (audio);

    if (std::ranges::equal (parts, audio))
        return parts; // nothing is covered

    std::map<ObjectId, const NoteClip*> originals;
    for (const auto& clip : audio)
        originals[clip->id] = clip.get();

    const auto& map = project.tempoMap;

    // The clip above `under` with a part that starts (or ends) at a tick at its own edge, with a fade there.
    const auto fadingAt = [&] (Ticks tick, bool atStart, const NoteClip& under) -> const NoteClip*
    {
        for (const auto& part : parts)
        {
            const auto& own = *originals.at (part->id);
            const auto at =
                atStart ? part->start == tick && own.start == tick : endOf (*part) == tick && endOf (own) == tick;

            if (at && above (own, under) && (atStart ? own.fadeIn : own.fadeOut) > 0)
                return &own;
        }

        return nullptr;
    };

    std::vector<ClipPtr> result;
    result.reserve (parts.size());

    for (const auto& part : parts)
    {
        const auto& own = *originals.at (part->id);
        auto from = part->start;
        auto to = endOf (*part);
        const NoteClip* before = from > own.start ? fadingAt (from, false, own) : nullptr;
        const NoteClip* after = to < endOf (own) ? fadingAt (to, true, own) : nullptr;

        // Cut on the left by a clip above that fades out there: play on under its fade-out, which ends there.
        if (before != nullptr)
        {
            const auto edge = map.secondsAt (static_cast<double> (from));
            const auto seconds = fadeSeconds (*before, false, isMusical (*before, project), map);
            from = std::max (own.start, static_cast<Ticks> (std::floor (map.tickAt (edge - seconds) + 1.0e-6)));
        }

        // Cut on the right by a clip above that fades in there: play on under its fade-in.
        if (after != nullptr)
        {
            const auto edge = map.secondsAt (static_cast<double> (to));
            const auto seconds = fadeSeconds (*after, true, isMusical (*after, project), map);
            to = std::min (endOf (own), static_cast<Ticks> (std::ceil (map.tickAt (edge + seconds) - 1.0e-6)));
        }

        if (from == part->start && to == endOf (*part))
        {
            result.push_back (part);
            continue;
        }

        auto extended = clipPart (own, from, to);
        const auto musical = isMusical (own, project);

        // The mirrored fades, in this part's content ticks, over the time the extension lasts.
        const auto ticksBetween = [&] (Ticks a, Ticks b)
        {
            if (musical)
                return b - a;

            const auto seconds = map.secondsAt (static_cast<double> (b)) - map.secondsAt (static_cast<double> (a));
            return std::max (Ticks { 1 }, roundTicks (seconds * map.bpmAt (static_cast<double> (extended.start))
                                                      * ticksPerBeat / 60.0));
        };

        if (from < part->start)
        {
            extended.fadeIn = ticksBetween (from, part->start);
            extended.fadeInShape = before->fadeOutShape;
        }

        if (to > endOf (*part))
        {
            extended.fadeOut = ticksBetween (endOf (*part), to);
            extended.fadeOutShape = after->fadeInShape;
        }

        result.push_back (std::make_shared<const NoteClip> (std::move (extended)));
    }

    std::ranges::sort (result, [] (const ClipPtr& a, const ClipPtr& b)
                       { return std::tie (a->start, a->id) < std::tie (b->start, b->id); });
    return result;
}
} // namespace dawg

namespace dawg
{
Project clipsRenderProject (const Project& project, ObjectId trackId, const std::vector<ObjectId>& clipIds, bool dry)
{
    auto result = project;

    for (auto& trackPtr : result.tracks)
    {
        if (trackPtr->id != trackId)
            continue;

        auto track = *trackPtr;

        if (! clipIds.empty())
            std::erase_if (track.clips,
                           [&] (const ClipPtr& c) { return std::ranges::find (clipIds, c->id) == clipIds.end(); });

        if (dry)
        {
            const auto byType = [&] (const char* type)
            { return std::ranges::find (track.graph.nodes, juce::String (type), &GraphNode::type); };
            const auto player = byType (nodetype::audioPlayer);
            const auto output = byType (nodetype::trackOutput);

            if (player != track.graph.nodes.end() && output != track.graph.nodes.end())
            {
                const auto outputId = output->id;
                std::erase_if (track.graph.connections,
                               [outputId] (const Connection& c) { return c.toNode == outputId; });
                track.graph.connections.push_back ({ player->id, 0, outputId, 0 });
            }
        }

        trackPtr = std::make_shared<const Track> (std::move (track));
    }

    return result;
}
} // namespace dawg

namespace dawg
{
ClipFrames::ClipFrames (const NoteClip& clip, const AudioFile& file, const TempoMap& map)
    : m_clip (clip),
      m_file (file),
      m_map (&map)
{
}

double ClipFrames::frameAt (double tick) const
{
    const auto offset = static_cast<double> (m_clip.offset);
    const auto start = static_cast<double> (m_clip.start);

    if (m_file.timing.musical())
        return TimingMap (m_file.timing, m_file.sampleRate).frameAt (offset + tick - start);

    const auto perTick = 60.0 / (m_map->bpmAt (start) * ticksPerBeat);
    const auto content = offset + (m_map->secondsAt (tick) - m_map->secondsAt (start)) / perTick;
    return TimingMap (m_file.timing, m_file.sampleRate).frameAt (content * perTick);
}

double ClipFrames::tickAt (double frame) const
{
    const auto offset = static_cast<double> (m_clip.offset);
    const auto start = static_cast<double> (m_clip.start);

    if (m_file.timing.musical())
        return start + TimingMap (m_file.timing, m_file.sampleRate).tickAt (frame) - offset;

    const auto perTick = 60.0 / (m_map->bpmAt (start) * ticksPerBeat);
    const auto content = TimingMap (m_file.timing, m_file.sampleRate).tickAt (frame) / perTick;
    return m_map->tickAt (m_map->secondsAt (start) + (content - offset) * perTick);
}

std::pair<juce::int64, juce::int64> ClipFrames::range() const
{
    const auto clamp = [this] (double frame)
    { return std::clamp (static_cast<juce::int64> (std::llround (frame)), juce::int64 { 0 }, m_file.frames); };

    return { clamp (frameAt (static_cast<double> (m_clip.start))),
             clamp (frameAt (static_cast<double> (m_clip.start + m_clip.length))) };
}

std::vector<juce::int64> gridSlices (const ClipFrames& frames, const NoteClip& clip, Ticks division)
{
    std::vector<juce::int64> cuts;

    if (division <= 0)
        return cuts;

    const auto [from, to] = frames.range();

    for (auto tick = (clip.start / division + 1) * division; tick < clip.start + clip.length; tick += division)
    {
        const auto frame = static_cast<juce::int64> (std::llround (frames.frameAt (static_cast<double> (tick))));

        if (frame > from && frame < to && (cuts.empty() || frame > cuts.back()))
            cuts.push_back (frame);
    }

    return cuts;
}

std::vector<juce::int64> onsetSlices (const std::vector<std::pair<juce::int64, float>>& onsets,
                                      std::pair<juce::int64, juce::int64> range, double sensitivity,
                                      juce::int64 minFrames)
{
    std::vector<juce::int64> cuts;
    const auto threshold = 1.0 - std::clamp (sensitivity, 0.0, 1.0);
    auto previous = range.first;

    for (const auto& [frame, strength] : onsets)
    {
        if (frame <= range.first || frame >= range.second || static_cast<double> (strength) < threshold - 1.0e-9)
            continue;

        if (frame - previous < minFrames || range.second - frame < minFrames)
            continue;

        cuts.push_back (frame);
        previous = frame;
    }

    return cuts;
}
} // namespace dawg

namespace dawg
{
juce::var notesVar (const std::vector<DetectedNote>& notes, const ClipFrames& frames, Ticks clipStart,
                    juce::int64 rangeStart, double sampleRate, bool curves)
{
    juce::Array<juce::var> result;
    const auto tickAt = [&] (double seconds)
    {
        return frames.tickAt (static_cast<double> (rangeStart) + seconds * sampleRate)
               - static_cast<double> (clipStart);
    };

    for (const auto& note : notes)
    {
        const auto start = std::max (Ticks { 0 }, roundTicks (tickAt (note.start)));
        const auto length = std::max (Ticks { 1 }, roundTicks (tickAt (note.end)) - start);
        auto value = makeObject (
            { { "start", start }, { "length", length }, { "pitch", note.pitch }, { "velocity", note.velocity } });

        if (curves && (! note.bend.empty() || ! note.level.empty()))
        {
            // Curve times are ticks from the note's start; one point per tick at most.
            const auto curveOf = [&] (const std::vector<std::pair<double, double>>& points)
            {
                juce::Array<juce::var> curve;
                Ticks previous = -1;

                for (const auto& [time, v] : points)
                {
                    const auto t = std::clamp (roundTicks (tickAt (note.start + time)) - start, Ticks { 0 }, length);

                    if (t != previous && curve.size() < static_cast<int> (limits::maxCurvePoints))
                        curve.add (juce::Array<juce::var> { t, v });

                    previous = t;
                }

                return juce::var (curve);
            };

            auto expression = makeObject ({ { "pitch", curveOf (note.bend) } });

            if (! note.level.empty())
                expression.getDynamicObject()->setProperty ("pressure", curveOf (note.level));

            value.getDynamicObject()->setProperty ("expression", expression);
        }

        result.add (value);
    }

    return result;
}
} // namespace dawg
