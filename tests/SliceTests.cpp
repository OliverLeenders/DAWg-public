// Slice to Sampler (docs/design/audio-features.md, AU3).

#include "TestUtils.h"
#include "dawg/ClipOps.h"
#include "dawg/Export.h"
#include "dawg/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr Ticks beat = ticksPerBeat;
constexpr int fileFrames = 2 * 48000; // four beats

[[nodiscard]] juce::var fileArgs()
{
    return makeObject ({ { "path", "Audio/loop.wav" },
                         { "name", "loop" },
                         { "sampleRate", 48000.0 },
                         { "channels", 1 },
                         { "frames", fileFrames } });
}

[[nodiscard]] juce::var frames (std::initializer_list<juce::int64> values)
{
    juce::Array<juce::var> list;
    for (const auto v : values)
        list.add (v);
    return list;
}

// An audio track with a four-beat clip of a slow ramp at beat 4.
struct SliceRig
{
    SliceRig()
    {
        juce::AudioBuffer<float> buffer (1, fileFrames);
        for (int i = 0; i < fileFrames; ++i)
            buffer.setSample (0, i, 0.1f + 0.4f * static_cast<float> (i) / fileFrames);
        source = AudioSource::fromBuffer (std::move (buffer), 48000.0);
        compiler.setAudioSourceResolver ([this] (const AudioFile&) { return source; });

        project = apply ({}, "addTrack", { { "kind", "audio" } });
        trackId = project.tracks[0]->id;
        project =
            apply (project, "addAudioClip", { { "trackId", trackId }, { "start", 4 * beat }, { "file", fileArgs() } });
        clipId = project.tracks[0]->clips[0]->id;
    }

    [[nodiscard]] const NoteClip& clip() const { return *project.tracks[0]->clips[0]; }

    [[nodiscard]] std::vector<float> render (const Project& p, ObjectId track)
    {
        AudioEngine engine;
        engine.setEngine (compiler.compile (p, spec));
        ExportRange range { 0.0, 9.0, false, 0.0, { track }, false, {} };
        range.preFader = true;
        std::vector<float> left;
        std::ignore = renderRange (engine, range, sampleRate, blockSize,
                                   [&] (std::array<const float*, 2> channels, int count)
                                   {
                                       left.insert (left.end(), channels[0], channels[0] + count);
                                       return true;
                                   });
        return left;
    }

    std::shared_ptr<const AudioSource> source;
    Project project;
    ObjectId trackId = 0;
    ObjectId clipId = 0;
    GraphCompiler compiler;
};
} // namespace

TEST_CASE ("a clip's timeline maps to its file's frames and back", "[slice]")
{
    SliceRig rig;
    rig.project = apply (rig.project, "resizeClip", { { "id", rig.clipId }, { "edge", "start" }, { "to", 5 * beat } });
    const auto& clip = rig.clip();
    REQUIRE (clip.offset == beat);

    const ClipFrames map (clip, *rig.project.findAudioFile (clip.fileId), rig.project.tempoMap);
    CHECK_THAT (map.frameAt (static_cast<double> (5 * beat)), WithinAbs (24000.0, 1e-6));
    CHECK_THAT (map.tickAt (36000.0), WithinAbs (static_cast<double> (5 * beat + beat / 2), 1e-6));
    CHECK (map.range() == std::pair<juce::int64, juce::int64> { 24000, 96000 });

    SECTION ("the grid cuts it at every sixteenth")
    {
        const auto cuts = gridSlices (map, clip, beat / 4);
        REQUIRE (cuts.size() == 11); // three beats, twelve sixteenths
        CHECK (cuts.front() == 30000);
        CHECK (cuts.back() == 90000);
    }
}

TEST_CASE ("transients cut a range by strength and distance", "[slice]")
{
    const std::vector<std::pair<juce::int64, float>> onsets {
        { 0, 1.0f }, { 1000, 0.9f }, { 1500, 1.0f }, { 5000, 0.3f }, { 9000, 0.7f }
    };

    CHECK (onsetSlices (onsets, { 0, 10000 }, 1.0, 100) == std::vector<juce::int64> { 1000, 1500, 5000, 9000 });
    // The onset at the range start is no cut; weak ones go with less sensitivity.
    CHECK (onsetSlices (onsets, { 0, 10000 }, 0.5, 100) == std::vector<juce::int64> { 1000, 1500, 9000 });
    // Too close to the range start, the previous cut or the end.
    CHECK (onsetSlices (onsets, { 0, 10000 }, 1.0, 1200) == std::vector<juce::int64> { 1500, 5000 });
}

TEST_CASE ("sliceToSampler makes a Sampler track that plays the slices where they were", "[slice][commands]")
{
    SliceRig rig;
    const auto project = apply (rig.project, "sliceToSampler",
                                { { "clipId", rig.clipId }, { "slices", frames ({ 24000, 48000, 72000 }) } });

    REQUIRE (project.tracks.size() == 2);
    const auto& track = *project.tracks[1];
    CHECK (track.kind == TrackKind::instrument);
    CHECK (track.name == "loop slices");
    const auto sampler = std::ranges::find (track.graph.nodes, juce::String (nodetype::sampler), &GraphNode::type);
    REQUIRE (sampler != track.graph.nodes.end());
    REQUIRE (sampler->sampleZones.size() == 4);

    for (size_t i = 0; i < 4; ++i)
    {
        const auto& zone = sampler->sampleZones[i];
        CHECK (zone.lowKey == 36 + static_cast<int> (i));
        CHECK (zone.highKey == zone.lowKey);
        CHECK (zone.rootKey == zone.lowKey);
        CHECK (zone.start == static_cast<juce::int64> (i) * 24000);
        CHECK (zone.end == static_cast<juce::int64> (i + 1) * 24000);
        CHECK (zone.mode == SampleMode::oneShot);
    }

    REQUIRE (track.clips.size() == 1);
    const auto& clip = *track.clips[0];
    CHECK (clip.start == 4 * beat);
    CHECK (clip.length == 4 * beat);
    REQUIRE (clip.notes.size() == 4);

    for (size_t i = 0; i < 4; ++i)
    {
        CHECK (clip.notes[i]->start == static_cast<Ticks> (i) * beat);
        CHECK (clip.notes[i]->length == beat);
        CHECK (clip.notes[i]->pitch == 36.0 + static_cast<double> (i));
    }

    SECTION ("played, the slices sound like the clip")
    {
        const auto original = rig.render (project, rig.trackId);
        const auto sliced = rig.render (project, track.id);
        // One velocity for every slice: a constant ratio, away from each slice's 1 ms attack.
        const auto ratio = sliced[4 * 24000 + 12000] / original[4 * 24000 + 12000];
        REQUIRE (ratio > 0.1f);

        for (int s = 4 * 24000 + 200; s < 8 * 24000 - 200; s += 97)
            if (s % 24000 > 200 && s % 24000 < 23800)
                CHECK_THAT (sliced[static_cast<size_t> (s)] / original[static_cast<size_t> (s)],
                            WithinAbs (ratio, 1e-3));
    }

    SECTION ("gate mode, and a start key")
    {
        const auto gated = apply (
            rig.project, "sliceToSampler",
            { { "clipId", rig.clipId }, { "slices", frames ({ 48000 }) }, { "mode", "gate" }, { "startKey", 60 } });
        const auto& zones =
            std::ranges::find (gated.tracks[1]->graph.nodes, juce::String (nodetype::sampler), &GraphNode::type)
                ->sampleZones;
        REQUIRE (zones.size() == 2);
        CHECK (zones[0].mode == SampleMode::gate);
        CHECK (zones[0].lowKey == 60);
    }

    SECTION ("many slices start lower, and too many are refused")
    {
        juce::Array<juce::var> cuts;
        for (int i = 1; i < 100; ++i)
            cuts.add (i * 900);

        const auto many = apply (rig.project, "sliceToSampler", { { "clipId", rig.clipId }, { "slices", cuts } });
        const auto& zones =
            std::ranges::find (many.tracks[1]->graph.nodes, juce::String (nodetype::sampler), &GraphNode::type)
                ->sampleZones;
        REQUIRE (zones.size() == 100);
        CHECK (zones.back().lowKey == 127);
        CHECK (zones.front().lowKey == 28);

        juce::Array<juce::var> tooMany;
        for (int i = 1; i <= 128; ++i)
            tooMany.add (i * 700);

        CHECK_THROWS_AS (apply (rig.project, "sliceToSampler", { { "clipId", rig.clipId }, { "slices", tooMany } }),
                         CommandError);
    }

    SECTION ("looped and note clips are refused")
    {
        const auto looped = apply (rig.project, "setClipLoop", { { "id", rig.clipId }, { "loopLength", beat } });
        CHECK_THROWS_AS (apply (looped, "sliceToSampler", { { "clipId", rig.clipId } }), CommandError);
        const auto notes = projectWithClip();
        CHECK_THROWS_AS (apply (notes.project, "sliceToSampler", { { "clipId", notes.clipId } }), CommandError);
    }
}
