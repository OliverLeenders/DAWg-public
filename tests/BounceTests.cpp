// Bounce in place, consolidate and render selection (docs/design/audio-features.md, AU16 and AU17).

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
constexpr int fileFrames = 4 * 48000; // eight beats

[[nodiscard]] juce::var fileArgs (const char* path)
{
    return makeObject (
        { { "path", path }, { "name", path }, { "sampleRate", 48000.0 }, { "channels", 1 }, { "frames", fileFrames } });
}

// Audio tracks of a constant 0.25.
struct BounceRig
{
    BounceRig()
    {
        juce::AudioBuffer<float> buffer (1, fileFrames);
        for (int i = 0; i < fileFrames; ++i)
            buffer.setSample (0, i, 0.25f);
        level = AudioSource::fromBuffer (std::move (buffer), 48000.0);
        compiler.setAudioSourceResolver ([this] (const AudioFile&) { return level; });
    }

    ObjectId addTrack()
    {
        project = apply (project, "addTrack", { { "kind", "audio" } });
        return project.tracks.back()->id;
    }

    ObjectId addClip (ObjectId trackId, Ticks start, Ticks length)
    {
        project = apply (
            project, "addAudioClip",
            { { "trackId", trackId }, { "start", start }, { "length", length }, { "file", fileArgs ("Audio/a.wav") } });
        return std::ranges::max (project.findTrack (trackId)->clips, {}, [] (const ClipPtr& c) { return c->order; })
            ->id;
    }

    // Renders beats [from, to) of the given tracks, before or after their faders.
    [[nodiscard]] std::vector<float> render (const Project& p, std::vector<ObjectId> tracks, bool preFader,
                                             double from = 0.0, double to = 8.0)
    {
        AudioEngine engine;
        engine.setEngine (compiler.compile (p, spec));
        ExportRange range { from, to, false, 0.0, std::move (tracks), false, {} };
        range.preFader = preFader;
        std::vector<float> left;
        std::ignore = renderRange (engine, range, sampleRate, blockSize,
                                   [&] (std::array<const float*, 2> channels, int count)
                                   {
                                       left.insert (left.end(), channels[0], channels[0] + count);
                                       return true;
                                   });
        return left;
    }

    [[nodiscard]] ObjectId gainNodeOf (ObjectId trackId) const
    {
        for (const auto& node : project.findTrack (trackId)->graph.nodes)
            if (node.type == nodetype::gain)
                return node.id;

        return 0;
    }

    std::shared_ptr<const AudioSource> level;
    Project project;
    GraphCompiler compiler;
};

[[nodiscard]] float at (const std::vector<float>& samples, double beats)
{
    return samples[static_cast<size_t> (beats * 24000.0)];
}
} // namespace

TEST_CASE ("a clip render keeps only its clips, and dry leaves the inserts out", "[bounce][engine]")
{
    BounceRig rig;
    const auto track = rig.addTrack();
    const auto first = rig.addClip (track, 0, 2 * beat);
    rig.addClip (track, 4 * beat, 2 * beat);
    rig.project =
        apply (rig.project, "setNodeParam",
               { { "trackId", track }, { "nodeId", rig.gainNodeOf (track) }, { "param", "gainDb" }, { "value", 6.0 } });

    const auto wet = rig.render (clipsRenderProject (rig.project, track, { first }, false), { track }, true);
    CHECK_THAT (at (wet, 1.0), WithinAbs (0.25 * 1.9953, 1e-3));
    CHECK_THAT (at (wet, 5.0), WithinAbs (0.0, 1e-6)); // the other clip is left out

    const auto dry = rig.render (clipsRenderProject (rig.project, track, { first }, true), { track }, true);
    CHECK_THAT (at (dry, 1.0), WithinAbs (0.25, 1e-4));

    // Every clip, with none given; the project's own graph is unchanged.
    const auto all = rig.render (clipsRenderProject (rig.project, track, {}, true), { track }, true);
    CHECK_THAT (at (all, 5.0), WithinAbs (0.25, 1e-4));
    CHECK (rig.project.findTrack (track)->clips.size() == 2);
}

TEST_CASE ("a render of several tracks is their sum after the faders", "[bounce][engine]")
{
    BounceRig rig;
    const auto a = rig.addTrack();
    const auto b = rig.addTrack();
    rig.addClip (a, 0, 4 * beat);
    rig.addClip (b, 2 * beat, 4 * beat);
    rig.project = apply (rig.project, "setTrackGain", { { "id", b }, { "gainDb", -6.0206 } });

    const auto both = rig.render (rig.project, { a, b }, false);
    const auto onlyA = rig.render (rig.project, { a }, false);
    const auto onlyB = rig.render (rig.project, { b }, false);

    for (const auto beats : { 1.0, 3.0, 5.0 })
        CHECK_THAT (at (both, beats), WithinAbs (at (onlyA, beats) + at (onlyB, beats), 1e-6));

    // The fader applies: b is at half the level of a.
    CHECK_THAT (at (onlyB, 5.0) / at (onlyA, 1.0), WithinAbs (0.5, 1e-3));
}

TEST_CASE ("addRenderTrack puts a render on a new audio track after another", "[bounce][commands]")
{
    BounceRig rig;
    const auto first = rig.addTrack();
    const auto last = rig.addTrack();
    rig.project = apply (rig.project, "setTrackGain", { { "id", first }, { "gainDb", -3.0 } });
    rig.project = apply (rig.project, "setTrackPan", { { "id", first }, { "pan", 0.25 } });

    const auto project = apply (
        rig.project, "addRenderTrack",
        { { "after", first }, { "start", 2 * beat }, { "like", true }, { "file", fileArgs ("Audio/render.wav") } });
    REQUIRE (project.tracks.size() == 3);
    const auto& added = *project.tracks[1];
    CHECK (project.tracks[2]->id == last);
    CHECK (added.kind == TrackKind::audio);
    CHECK (added.name == project.tracks[0]->name + " (audio)");
    CHECK (added.gainDb == -3.0);
    CHECK (added.pan == 0.25);
    REQUIRE (added.clips.size() == 1);
    CHECK (added.clips[0]->start == 2 * beat);
    CHECK (project.findAudioFile (added.clips[0]->fileId)->path == "Audio/render.wav");

    SECTION ("without like, it keeps its own fader")
    {
        const auto plain = apply (
            rig.project, "addRenderTrack",
            { { "after", last }, { "start", 0 }, { "name", "Render" }, { "file", fileArgs ("Audio/render.wav") } });
        CHECK (plain.tracks.back()->name == "Render");
        CHECK (plain.tracks.back()->gainDb == 0.0);
    }
}
