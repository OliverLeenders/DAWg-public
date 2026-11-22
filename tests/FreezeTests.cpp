// Freeze and track renders (docs/design/audio-features.md, AU14 and AU15).

#include "TestUtils.h"
#include "dawg/AllocationGuard.h"
#include "dawg/Export.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ProjectFile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <map>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr Ticks beat = ticksPerBeat;
constexpr int samplesPerBeat = 24000; // 120 BPM at 48 kHz

[[nodiscard]] juce::var array (std::initializer_list<juce::var> items)
{
    return juce::Array<juce::var> (items);
}

// A project with an instrument track playing two notes (the built-in synth), and an engine that plays it; renders
// are served to the compiler by path.
struct FreezeRig
{
    FreezeRig()
    {
        auto setup = projectWithClip();
        trackId = setup.trackId;
        clipId = setup.clipId;
        project = apply (setup.project, "addNotes",
                         { { "clipId", clipId },
                           { "notes", array ({ noteArgs (beat, beat, 60), noteArgs (3 * beat, beat, 67) }) } });
        project = apply (project, "setTrackGain", { { "id", trackId }, { "gainDb", -6.0 } });
        compiler.setAudioSourceResolver ([this] (const AudioFile& file) { return renders[file.path]; });
        compile();
    }

    // An output track fed by an Output to track node from the instrument (multi-output-instruments.md), so the
    // instrument is heard twice: through the track's Main output track (0 dB; the track is only a header now, so its
    // -6 dB fader plays no part, MO12) and through the output track (0 dB).
    void addOutputTrack()
    {
        project = apply (project, "addTrack", { { "kind", "output" }, { "source", trackId }, { "name", "out" } });
        const auto outputTrack = project.tracks.back()->id;
        project = apply (project, "addNode", { { "trackId", trackId }, { "type", nodetype::busOutput } });
        const auto& graph = track().graph;
        // The instrument: what feeds the Track Output.
        const auto trackOutput =
            std::ranges::find (graph.nodes, juce::String (nodetype::trackOutput), &GraphNode::type)->id;
        const auto feed = std::ranges::find (graph.connections, trackOutput, &Connection::toNode);
        REQUIRE (feed != graph.connections.end());
        const auto instrumentId = feed->fromNode;
        outputNode = std::ranges::find (graph.nodes, juce::String (nodetype::busOutput), &GraphNode::type)->id;
        project = apply (project, "setNodeTarget",
                         { { "trackId", trackId }, { "nodeId", outputNode }, { "target", outputTrack } });
        project = apply (project, "connect",
                         { { "trackId", trackId },
                           { "fromNode", instrumentId },
                           { "fromPort", 0 },
                           { "toNode", outputNode },
                           { "toPort", 0 } });
        compile();
    }

    void compile() { engine.setEngine (compiler.compile (project, spec)); }

    // Renders beats [from, to) of the mix, or of the track before its fader.
    [[nodiscard]] std::vector<float> renderOf (double from, double to, bool preFader)
    {
        ExportRange range { from, to, true, 4.0, {}, false, {} };

        if (preFader)
        {
            range.tracks = { trackId };
            range.preFader = true;
        }

        std::vector<float> left;
        std::vector<float> right;
        if (preFader && withTaps && outputNode != 0)
            range.taps = { outputNode };

        tapped.assign (range.taps.size() * 2, {});
        std::ignore = renderRange (
            engine, range, sampleRate, blockSize,
            [&] (std::array<const float*, 2> channels, int count)
            {
                left.insert (left.end(), channels[0], channels[0] + count);
                right.insert (right.end(), channels[1], channels[1] + count);
                return true;
            },
            [&] (std::span<const float* const> channels, int count)
            {
                for (size_t ch = 0; ch < tapped.size(); ++ch)
                    tapped[ch].insert (tapped[ch].end(), channels[ch], channels[ch] + count);

                return true;
            });
        renderedRight = right;
        return left;
    }

    [[nodiscard]] static juce::var fileArgs (const juce::String& path, size_t frames)
    {
        return makeObject ({ { "path", path },
                             { "name", path },
                             { "sampleRate", sampleRate },
                             { "channels", 2 },
                             { "frames", static_cast<juce::int64> (frames) } });
    }

    [[nodiscard]] static std::shared_ptr<const AudioSource> sourceOf (const std::vector<float>& left,
                                                                      const std::vector<float>& right)
    {
        juce::AudioBuffer<float> buffer (2, static_cast<int> (left.size()));
        buffer.copyFrom (0, 0, left.data(), static_cast<int> (left.size()));
        buffer.copyFrom (1, 0, right.data(), static_cast<int> (right.size()));
        return AudioSource::fromBuffer (std::move (buffer), sampleRate);
    }

    // Freezes the track with a pre-fader render from its first clip on, and its Output to track's (AU15).
    void freeze()
    {
        const auto left = renderOf (0.0, 16.0, true);
        renders["Audio/Frozen/track.wav"] = sourceOf (left, renderedRight);
        juce::Array<juce::var> outputs;

        if (! tapped.empty())
        {
            renders["Audio/Frozen/out.wav"] = sourceOf (tapped[0], tapped[1]);
            outputs.add (makeObject (
                { { "node", outputNode }, { "file", fileArgs ("Audio/Frozen/out.wav", tapped[0].size()) } }));
        }

        project = apply (project, "freezeTrack",
                         { { "trackId", trackId },
                           { "start", 0 },
                           { "file", fileArgs ("Audio/Frozen/track.wav", left.size()) },
                           { "outputs", outputs } });
        compile();
    }

    [[nodiscard]] const Track& track() const { return *project.findTrack (trackId); }

    Project project;
    ObjectId trackId = 0;
    ObjectId clipId = 0;
    ObjectId outputNode = 0;
    bool withTaps = true;
    std::map<juce::String, std::shared_ptr<const AudioSource>> renders;
    std::vector<float> renderedRight;
    std::vector<std::vector<float>> tapped; // left and right of each tap
    GraphCompiler compiler;
    AudioEngine engine;
};

[[nodiscard]] double largestDifference (const std::vector<float>& a, const std::vector<float>& b, size_t from,
                                        size_t to)
{
    double worst = 0.0;

    for (auto i = from; i < std::min ({ to, a.size(), b.size() }); ++i)
        worst = std::max (worst, std::abs (static_cast<double> (a[i]) - static_cast<double> (b[i])));

    return worst;
}

[[nodiscard]] float peakOf (const std::vector<float>& samples)
{
    float peak = 0.0f;

    for (const auto s : samples)
        peak = std::max (peak, std::abs (s));

    return peak;
}
} // namespace

TEST_CASE ("a pre-fader track render leaves out the fader", "[freeze][engine]")
{
    FreezeRig rig;
    const auto mix = rig.renderOf (0.0, 6.0, false);
    const auto stem = rig.renderOf (0.0, 6.0, true);

    REQUIRE (peakOf (stem) > 0.05f);
    // The mix is the stem through the fader (-6 dB) and the centre pan.
    const auto ratio = peakOf (mix) / peakOf (stem);
    rig.project = apply (rig.project, "setTrackGain", { { "id", rig.trackId }, { "gainDb", 0.0 } });
    rig.compile();
    const auto unity = rig.renderOf (0.0, 6.0, false);
    CHECK_THAT (ratio, WithinAbs (peakOf (unity) / peakOf (stem) * 0.5012, 1e-3));
    CHECK_THAT (peakOf (rig.renderOf (0.0, 6.0, true)), WithinAbs (peakOf (stem), 1e-6));
}

TEST_CASE ("a frozen track plays its render, with its fader still live", "[freeze][engine]")
{
    FreezeRig rig;
    const auto live = rig.renderOf (0.0, 6.0, false);
    rig.freeze();

    REQUIRE (rig.track().freeze.active());
    const auto frozen = rig.renderOf (0.0, 6.0, false);
    // Past the render clip's own 2 ms declick, the same output.
    CHECK (largestDifference (live, frozen, 200, 6 * samplesPerBeat) < 1.0e-4);

    // The fader is applied live.
    rig.project = apply (rig.project, "setTrackGain", { { "id", rig.trackId }, { "gainDb", -12.0 } });
    rig.compile();
    const auto quieter = rig.renderOf (0.0, 6.0, false);
    CHECK_THAT (peakOf (quieter) / peakOf (frozen), WithinAbs (0.5012, 1e-3));

    // Unfrozen, it plays live again.
    rig.project = apply (rig.project, "unfreezeTrack", { { "trackId", rig.trackId } });
    rig.project = apply (rig.project, "setTrackGain", { { "id", rig.trackId }, { "gainDb", -6.0 } });
    rig.compile();
    CHECK_FALSE (rig.track().freeze.active());
    CHECK (largestDifference (live, rig.renderOf (0.0, 6.0, false), 0, 6 * samplesPerBeat) < 1.0e-6);
}

TEST_CASE ("a frozen track's output tracks play what its Output to track nodes sent them", "[freeze][outputs][engine]")
{
    FreezeRig rig;
    rig.addOutputTrack();
    const auto live = rig.renderOf (0.0, 6.0, false);
    const auto stem = rig.renderOf (0.0, 6.0, true);

    // The render taps the Output to track: the instrument, as the track hears it before its fader.
    REQUIRE (rig.tapped.size() == 2);
    CHECK (peakOf (rig.tapped[0]) > 0.05f);
    CHECK (largestDifference (stem, rig.tapped[0], 0, 6 * samplesPerBeat) < 1.0e-6);

    rig.freeze();
    REQUIRE (rig.track().freeze.outputs.size() == 1);
    CHECK (rig.track().freeze.outputs[0].node == rig.outputNode);
    // Past the render clips' own 2 ms declick, the same mix: the track and its output track.
    CHECK (largestDifference (live, rig.renderOf (0.0, 6.0, false), 200, 6 * samplesPerBeat) < 1.0e-4);

    // The renders survive a save and open.
    const juce::TemporaryFile temporary (juce::String ("Frozen") + projectfile::extension);
    REQUIRE (projectfile::write (rig.project, { "id", "v" }, temporary.getFile(), false).isEmpty());
    const auto loaded = projectfile::read (temporary.getFile()).project;
    REQUIRE (loaded.findTrack (rig.trackId)->freeze.outputs.size() == 1);
    CHECK (loaded.findTrack (rig.trackId)->freeze.outputs[0].fileId == rig.track().freeze.outputs[0].fileId);

    SECTION ("an Output to track without a render is silent while frozen")
    {
        rig.project = apply (rig.project, "unfreezeTrack", { { "trackId", rig.trackId } });
        rig.compile();
        rig.withTaps = false;
        rig.freeze();
        CHECK (rig.track().freeze.outputs.empty());
        const auto frozen = rig.renderOf (0.0, 6.0, false);
        // Only the track's Main, of the Main and the output track, both at 0 dB.
        CHECK_THAT (peakOf (frozen) / peakOf (live), WithinAbs (0.5, 0.02));
    }

    SECTION ("only the track's own Output to track nodes can be frozen with it")
    {
        rig.project = apply (rig.project, "unfreezeTrack", { { "trackId", rig.trackId } });
        const auto other = rig.track().graph.nodes.front().id;
        CHECK_THROWS_AS (
            apply (rig.project, "freezeTrack",
                   { { "trackId", rig.trackId },
                     { "start", 0 },
                     { "file", FreezeRig::fileArgs ("a.wav", 100) },
                     { "outputs", array ({ makeObject (
                                      { { "node", other }, { "file", FreezeRig::fileArgs ("b.wav", 100) } }) }) } }),
            CommandError);
    }
}

TEST_CASE ("a frozen track's clips and graph are locked", "[freeze][commands]")
{
    FreezeRig rig;
    rig.freeze();

    CHECK_THROWS_AS (
        apply (rig.project, "addNotes", { { "clipId", rig.clipId }, { "notes", array ({ noteArgs (0, beat, 72) }) } }),
        CommandError);
    CHECK_THROWS_AS (apply (rig.project, "deleteClips", { { "ids", array ({ rig.clipId }) } }), CommandError);
    const auto nodeId = rig.track().graph.nodes.front().id;
    CHECK_THROWS_AS (apply (rig.project, "setNodeBypass",
                            { { "trackId", rig.trackId }, { "nodeId", nodeId }, { "bypassed", true } }),
                     CommandError);

    // Its fader, name and pan, and the tempo, are not.
    CHECK_NOTHROW (apply (rig.project, "setTrackPan", { { "id", rig.trackId }, { "pan", 0.5 } }));
    CHECK_NOTHROW (apply (rig.project, "renameTrack", { { "id", rig.trackId }, { "name", "Frozen" } }));
    CHECK_NOTHROW (apply (rig.project, "setTempo", { { "bpm", 140.0 } }));
}

TEST_CASE ("the freeze key follows what the render depends on", "[freeze]")
{
    FreezeRig rig;
    const auto key = freezeKey (rig.project, rig.track());

    CHECK (freezeKey (apply (rig.project, "setTrackGain", { { "id", rig.trackId }, { "gainDb", 3.0 } }), rig.track())
           == key);
    const auto louder = apply (rig.project, "setTrackGain", { { "id", rig.trackId }, { "gainDb", 3.0 } });
    CHECK (freezeKey (louder, *louder.findTrack (rig.trackId)) == key);
    const auto faster = apply (rig.project, "setTempo", { { "bpm", 140.0 } });
    CHECK (freezeKey (faster, *faster.findTrack (rig.trackId)) != key);
    const auto more =
        apply (rig.project, "addNotes", { { "clipId", rig.clipId }, { "notes", array ({ noteArgs (0, beat, 72) }) } });
    CHECK (freezeKey (more, *more.findTrack (rig.trackId)) != key);

    SECTION ("a tempo change makes the freeze stale")
    {
        rig.freeze();
        const auto fresh = toVar (rig.project)["tracks"][juce::Identifier (juce::String (rig.trackId))]["freeze"];
        CHECK_FALSE (static_cast<bool> (fresh["stale"]));

        const auto later = apply (rig.project, "setTempo", { { "bpm", 140.0 } });
        const auto stale = toVar (later)["tracks"][juce::Identifier (juce::String (rig.trackId))]["freeze"];
        CHECK (static_cast<bool> (stale["stale"]));
    }

    SECTION ("groups can't be frozen")
    {
        auto project = apply (rig.project, "addTrack", { { "kind", "group" } });
        const auto group = project.tracks.back()->id;
        CHECK_THROWS_AS (apply (project, "freezeTrack",
                                { { "trackId", group },
                                  { "start", 0 },
                                  { "file", makeObject ({ { "path", "a.wav" },
                                                          { "name", "a" },
                                                          { "sampleRate", sampleRate },
                                                          { "channels", 2 },
                                                          { "frames", 100 } }) } }),
                         CommandError);
    }
}

TEST_CASE ("playing a frozen track never allocates", "[freeze][realtime]")
{
    if constexpr (! allocationGuardEnabled)
        SKIP ("Allocation guard is compiled out in this configuration");

    FreezeRig rig;
    rig.freeze();
    rig.engine.play();

    const auto before = allocationViolationCount();
    StereoBuffer buffer (8192);

    for (int i = 0; i < 20; ++i)
        rig.engine.renderOffline (buffer.channels(), 2, buffer.size(), sampleRate);

    CHECK (allocationViolationCount() == before);
}
