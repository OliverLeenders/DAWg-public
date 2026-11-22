#include "TestUtils.h"
#include "dawg/AllocationGuard.h"
#include "dawg/FaustNode.h"
#include "dawg/GraphCompiler.h"
#include "dawg/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <utility>

// The voice compiler and runtime (docs/design/phase12-voice-scope.md, V8 to V12).

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
[[nodiscard]] juce::var list (std::initializer_list<juce::var> items)
{
    juce::Array<juce::var> result;

    for (const auto& item : items)
        result.add (item);

    return result;
}

[[nodiscard]] const GraphNode& nodeOfType (const TrackGraph& graph, const char* type)
{
    const auto it = std::ranges::find_if (graph.nodes, [type] (const GraphNode& n) { return n.type == type; });
    REQUIRE (it != graph.nodes.end());
    return *it;
}

constexpr auto samplesPerBeat = 24000; // 120 bpm at 48 kHz

// A track whose MPE Synth was converted to a Voice Instrument with the default voice graph, its oscillator a sine.
struct VoiceRig
{
    explicit VoiceRig (std::initializer_list<juce::var> notes)
    {
        auto setup = projectWithClip();
        trackId = setup.trackId;
        clipId = setup.clipId;
        project = setup.project;

        if (notes.size() > 0)
            project = apply (project, "addNotes", { { "clipId", clipId }, { "notes", list (notes) } });

        voiceId = nodeOfType (project.tracks[0]->graph, nodetype::mpeSynth).id;
        project = apply (project, "convertToVoiceGraph", { { "trackId", trackId }, { "nodeId", voiceId } });
        oscillatorId = nodeOfType (voice(), nodetype::oscillator).id;
        edit ("setNodeParam", { { "nodeId", oscillatorId }, { "param", "waveform" }, { "value", 0.0 } });
    }

    [[nodiscard]] const TrackGraph& voice() const
    {
        const auto& graph = project.tracks[0]->graph;
        return *graph.nodes[static_cast<size_t> (graph.indexOfNode (voiceId))].voiceGraph;
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        auto object = makeObject (args);
        object.getDynamicObject()->setProperty ("trackId", trackId);
        object.getDynamicObject()->setProperty ("voiceNodeId", voiceId);
        project = applyCommand (project, command, object);
    }

    void start()
    {
        engine.setEngine (compiler.compile (project, spec));
        engine.play();
    }

    [[nodiscard]] int activeVoices() const
    {
        const auto* node = compiler.findInstance (trackId, voiceId);
        REQUIRE (node != nullptr);
        return static_cast<int> (node->meters()["voices"]);
    }

    Project project;
    ObjectId trackId = 0;
    ObjectId clipId = 0;
    ObjectId voiceId = 0;
    ObjectId oscillatorId = 0;
    GraphCompiler compiler;
    AudioEngine engine;
};

[[nodiscard]] std::vector<float> renderNotes (std::initializer_list<juce::var> notes, int numSamples)
{
    VoiceRig rig (notes);
    rig.start();
    return render (rig.engine, numSamples).left();
}

[[nodiscard]] float largestStep (const std::vector<float>& samples, size_t from, size_t to)
{
    float largest = 0.0f;

    for (auto i = std::max<size_t> (from, 1); i < to; ++i)
        largest = std::max (largest, std::abs (samples[i] - samples[i - 1]));

    return largest;
}

[[nodiscard]] float peak (const std::vector<float>& samples, size_t from, size_t to)
{
    float result = 0.0f;

    for (auto i = from; i < to; ++i)
        result = std::max (result, std::abs (samples[i]));

    return result;
}
} // namespace

TEST_CASE ("a Voice Instrument plays its voice graph at the note's pitch", "[voice][engine]")
{
    VoiceRig rig ({ noteArgs (0, 4 * ticksPerBeat, 69) });
    rig.start();
    const auto output = render (rig.engine, 48000).left();

    // Half a second of A4, after the attack.
    const auto crossings = zeroCrossings (output, 2048, 2048 + 24000);
    CHECK_THAT (static_cast<double> (crossings) / 2.0 / 0.5, WithinAbs (440.0, 3.0));
    CHECK (rig.activeVoices() == 1);
}

TEST_CASE ("a bypassed Voice Instrument is silent", "[voice][engine]")
{
    VoiceRig rig ({ noteArgs (0, 4 * ticksPerBeat, 69) });
    rig.project = apply (rig.project, "setNodeBypass",
                         { { "trackId", rig.trackId }, { "nodeId", rig.voiceId }, { "bypassed", true } });
    rig.start();
    const auto output = render (rig.engine, 12000).left();
    CHECK (peak (output, 0, output.size()) == 0.0f);
}

TEST_CASE ("each note plays in its own voice, with its own envelope and pitch", "[voice][engine]")
{
    // Two notes that overlap, starting at different times, at different pitches. Voices share nothing, so together
    // they play exactly what each plays alone.
    const auto first = noteArgs (0, 2 * ticksPerBeat, 60);
    const auto second = noteArgs (ticksPerBeat / 2, 2 * ticksPerBeat, 67);
    constexpr auto length = 4 * samplesPerBeat;

    const auto alone = renderNotes ({ first }, length);
    const auto other = renderNotes ({ second }, length);
    const auto both = renderNotes ({ first, second }, length);

    float difference = 0.0f;

    for (size_t i = 0; i < both.size(); ++i)
        difference = std::max (difference, std::abs (both[i] - (alone[i] + other[i])));

    CHECK (difference < 1.0e-5f);
    CHECK (peak (other, 0, samplesPerBeat / 2) == 0.0f); // the second note starts on its own sample
    CHECK (peak (other, samplesPerBeat / 2, samplesPerBeat / 2 + 1000) > 0.01f);
}

TEST_CASE ("a released voice ends once its release tail is silent", "[voice][engine]")
{
    VoiceRig rig ({ noteArgs (0, ticksPerBeat, 60) });
    rig.start();

    std::ignore = render (rig.engine, samplesPerBeat / 2);
    CHECK (rig.activeVoices() == 1);

    // The note ends after one beat; the release takes 200 ms, then 50 ms of silence end the voice.
    const auto tail = render (rig.engine, samplesPerBeat + 24000).left();
    CHECK (peak (tail, tail.size() - 4800, tail.size()) == 0.0f);
    CHECK (rig.activeVoices() == 0);
}

TEST_CASE ("a seventeenth note steals the oldest voice after a 5 ms fade", "[voice][engine]")
{
    // Sixteen held notes, one every 1/16 beat, then a seventeenth: it takes the first note's voice.
    std::vector<juce::var> held;
    held.reserve (16);

    for (int i = 0; i < 16; ++i)
        held.push_back (noteArgs (i * ticksPerBeat / 16, 8 * ticksPerBeat, 48 + i));

    const auto stealAt = 2 * ticksPerBeat;
    const auto stealSample = size_t { 2 } * samplesPerBeat;
    const auto stealing = noteArgs (stealAt, 2 * ticksPerBeat, 84);

    VoiceRig without ({});
    VoiceRig with ({});

    for (const auto& note : held)
    {
        without.project =
            apply (without.project, "addNotes", { { "clipId", without.clipId }, { "notes", list ({ note }) } });
        with.project = apply (with.project, "addNotes", { { "clipId", with.clipId }, { "notes", list ({ note }) } });
    }

    with.project = apply (with.project, "addNotes", { { "clipId", with.clipId }, { "notes", list ({ stealing }) } });
    without.start();
    with.start();

    constexpr auto length = 3 * samplesPerBeat;
    const auto a = render (without.engine, length).left();
    const auto b = render (with.engine, length).left();
    CHECK (with.activeVoices() == 16);

    // The change: the first note fades out over 5 ms (240 samples) from the note-on, then the new note starts.
    std::vector<float> change (a.size());

    for (size_t i = 0; i < a.size(); ++i)
        change[i] = b[i] - a[i];

    CHECK (peak (change, 0, stealSample) == 0.0f);
    CHECK (peak (change, stealSample, stealSample + 240) > 0.01f);            // the fade
    CHECK (largestStep (change, stealSample - 1, stealSample + 240) < 0.01f); // no click
    CHECK (peak (b, stealSample + 240, stealSample + 2000) > 0.0f);
}

TEST_CASE ("the sustain pedal holds released voices", "[voice][engine]")
{
    VoiceRig rig ({ noteArgs (0, ticksPerBeat / 2, 60) });
    rig.start();

    LiveInput pedal;
    pedal.trackId = rig.trackId;
    pedal.type = NoteEventType::sustain;
    pedal.value = 1.0f;
    REQUIRE (rig.engine.sendLiveInput (pedal));

    // The note ends after half a beat, but the pedal is down: its envelope sustains.
    const auto held = render (rig.engine, 2 * samplesPerBeat).left();
    CHECK (peak (held, held.size() - 4800, held.size()) > 0.01f);
    CHECK (rig.activeVoices() == 1);

    pedal.value = 0.0f;
    REQUIRE (rig.engine.sendLiveInput (pedal));
    const auto released = render (rig.engine, samplesPerBeat).left();
    CHECK (peak (released, released.size() - 4800, released.size()) == 0.0f);
    CHECK (rig.activeVoices() == 0);
}

TEST_CASE ("a seek chokes the voices without a click", "[voice][engine]")
{
    VoiceRig rig ({ noteArgs (0, 8 * ticksPerBeat, 60) });
    rig.start();

    std::ignore = render (rig.engine, samplesPerBeat);
    rig.engine.seek (12.0); // past the clip
    const auto after = render (rig.engine, 4800).left();

    CHECK (largestStep (after, 0, after.size()) < 0.02f);
    CHECK (peak (after, 480, after.size()) == 0.0f);
    CHECK (rig.activeVoices() == 0);
}

TEST_CASE ("a structural edit to the voice graph keeps the sounding voices", "[voice][engine]")
{
    // Adding an unconnected LFO to the voice graph recompiles it; the note plays on as if nothing happened.
    const auto note = noteArgs (0, 4 * ticksPerBeat, 60);
    const auto reference = renderNotes ({ note }, 2 * samplesPerBeat);

    VoiceRig rig ({ note });
    rig.start();
    auto output = render (rig.engine, samplesPerBeat).left();

    rig.edit ("addNode", { { "type", nodetype::lfo } });
    const auto engine = rig.compiler.update (rig.project, spec);
    REQUIRE (engine != nullptr);
    rig.engine.setEngine (engine);
    const auto rest = render (rig.engine, samplesPerBeat).left();
    output.insert (output.end(), rest.begin(), rest.end());

    float difference = 0.0f;

    for (size_t i = 0; i < output.size(); ++i)
        difference = std::max (difference, std::abs (output[i] - reference[i]));

    CHECK (difference < 1.0e-6f);
    CHECK (rig.activeVoices() == 1);
}

TEST_CASE ("parameter edits reach every voice slot", "[voice][engine]")
{
    VoiceRig rig ({ noteArgs (0, 4 * ticksPerBeat, 60) });
    rig.start();
    rig.edit ("setNodeParam", { { "nodeId", rig.oscillatorId }, { "param", "coarse" }, { "value", 12.0 } });
    REQUIRE (rig.compiler.update (rig.project, spec) == nullptr); // in place

    for (int voice = 0; std::cmp_less (voice, VoiceAllocator::numVoices); ++voice)
    {
        const auto* instance = rig.compiler.findVoiceInstance (rig.trackId, rig.oscillatorId, voice);
        REQUIRE (instance != nullptr);
        const auto coarse = instance->descriptor().indexOfParam ("coarse");
        CHECK (instance->params().get (static_cast<size_t> (coarse)) == 12.0f);
    }
}

TEST_CASE ("a voice starts on its note's sample", "[voice][engine]")
{
    VoiceRig rig ({ noteArgs (ticksPerBeat, ticksPerBeat, 60) });
    rig.start();
    const auto output = render (rig.engine, 2 * samplesPerBeat).left();
    CHECK (firstAudible (output) >= samplesPerBeat);
    CHECK (firstAudible (output) <= samplesPerBeat + 2); // a sine starts at 0, and so does the envelope
}

TEST_CASE ("voices render without allocating", "[voice][engine][realtime]")
{
    if constexpr (! allocationGuardEnabled)
        SKIP ("Allocation guard is compiled out in this configuration");

    std::vector<juce::var> notes;
    notes.reserve (20);

    for (int i = 0; i < 20; ++i)
        notes.push_back (noteArgs (i * ticksPerBeat / 8, ticksPerBeat, 48 + i));

    VoiceRig rig ({});

    for (const auto& note : notes)
        rig.project = apply (rig.project, "addNotes", { { "clipId", rig.clipId }, { "notes", list ({ note }) } });

    rig.start();
    StereoBuffer buffer (blockSize);
    std::ignore = render (rig.engine, blockSize); // first block outside the guard: lazy setup may allocate

    const auto before = allocationViolationCount();

    {
        const ScopedNoAllocation guard;

        for (int block = 0; block < 4 * samplesPerBeat / blockSize; ++block)
            rig.engine.renderOffline (buffer.channels(), 2, blockSize, sampleRate);
    }

    CHECK (allocationViolationCount() == before);
}

TEST_CASE ("starting in the middle of a note chases it into a voice", "[voice][engine]")
{
    VoiceRig rig ({ noteArgs (0, 4 * ticksPerBeat, 69) });
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));
    rig.engine.seek (1.0);
    rig.engine.play();
    const auto output = render (rig.engine, samplesPerBeat).left();

    CHECK (firstAudible (output) <= 2);
    const auto crossings = zeroCrossings (output, 2048, 2048 + 12000);
    CHECK_THAT (static_cast<double> (crossings) / 2.0 / 0.25, WithinAbs (440.0, 6.0));
}

TEST_CASE ("voices rendering at once play exactly what they play one after another", "[voice][engine]")
{
    // More notes than voices on two Voice Instrument tracks, so the pool runs both at once: voices start mid-block, are
    // stolen and fade, and end after their release, each on whichever thread takes it.
    std::vector<juce::var> notes;
    notes.reserve (24);

    for (int i = 0; i < 24; ++i)
        notes.push_back (noteArgs (i * ticksPerBeat / 6, (i % 3 + 1) * ticksPerBeat / 2, 40 + (i * 7) % 36));

    const auto renderWith = [&notes] (bool parallel)
    {
        VoiceRig rig ({});

        for (const auto& note : notes)
            rig.project = apply (rig.project, "addNotes", { { "clipId", rig.clipId }, { "notes", list ({ note }) } });

        rig.project = apply (rig.project, "addTrack", { { "name", "B" } });
        const auto other = rig.project.tracks.back()->id;
        rig.project =
            apply (rig.project, "addClip", { { "trackId", other }, { "start", 0 }, { "length", 4 * ticksPerBeat } });
        const auto otherClip = rig.project.findTrack (other)->clips.back()->id;

        for (const auto& note : notes)
            rig.project = apply (rig.project, "addNotes", { { "clipId", otherClip }, { "notes", list ({ note }) } });

        const auto synth = nodeOfType (rig.project.findTrack (other)->graph, nodetype::mpeSynth).id;
        rig.project = apply (rig.project, "convertToVoiceGraph", { { "trackId", other }, { "nodeId", synth } });

        rig.engine.setParallelTracks (parallel);
        rig.start();

        // Blocks that don't divide the beat, so notes start and fades end anywhere in a block.
        constexpr int length = 300;
        StereoBuffer buffer (length);
        std::vector<float> left (6 * samplesPerBeat);
        std::vector<float> right (left.size());
        const auto before = allocationViolationCount();

        // After the first block (lazy setup may allocate), the helpers and the voices' own buffers allocate nothing.
        std::optional<ScopedNoAllocation> guard;

        for (size_t done = 0; done + length <= left.size(); done += length)
        {
            rig.engine.renderOffline (buffer.channels(), 2, length, sampleRate);
            std::ranges::copy (buffer.left(), left.begin() + static_cast<std::ptrdiff_t> (done));
            std::ranges::copy (buffer.right(), right.begin() + static_cast<std::ptrdiff_t> (done));

            if (! guard.has_value())
                guard.emplace();
        }

        guard.reset();
        CHECK (allocationViolationCount() == before);
        return std::pair { left, right };
    };

    const auto [serialLeft, serialRight] = renderWith (false);
    const auto [parallelLeft, parallelRight] = renderWith (true);

    CHECK (peak (serialLeft, 0, serialLeft.size()) > 0.01f);
    CHECK (serialLeft == parallelLeft);
    CHECK (serialRight == parallelRight);
}

TEST_CASE ("a track running ahead of the playhead starts a looped note once", "[voice][engine][loop]")
{
    // A short note at the loop start. A track ahead of the playhead (latency elsewhere, or a negative offset) wraps
    // early and must not be told again that it looped until it wraps again: each repeat of the flag restarted the note
    // and its envelopes, stacking voices.
    constexpr auto ahead = 2400; // 50 ms
    constexpr auto noteSamples = samplesPerBeat / 2;
    constexpr auto loopSamples = 2 * samplesPerBeat;
    const auto loopEnd = 2 * ticksPerBeat;

    VoiceRig plain ({ noteArgs (0, ticksPerBeat / 2, 60) });
    plain.project = apply (plain.project, "setLoop", { { "enabled", true }, { "start", 0 }, { "end", loopEnd } });
    plain.start();
    const auto reference = render (plain.engine, loopSamples).left();

    VoiceRig rig ({ noteArgs (0, ticksPerBeat / 2, 60) });
    rig.project = apply (rig.project, "setLoop", { { "enabled", true }, { "start", 0 }, { "end", loopEnd } });
    rig.project =
        apply (rig.project, "setTrackOffset", { { "id", rig.trackId }, { "offsetMs", -1000.0 * ahead / 48000.0 } });
    rig.start();
    const auto output = render (rig.engine, 3 * loopSamples).left();

    // Past the playhead's own wrap and its declick, each repeat plays exactly what the first pass of the plain track did.
    for (auto iteration = 1; iteration <= 2; ++iteration)
    {
        const auto start = iteration * loopSamples - ahead;
        float largest = 0.0f;

        for (auto i = ahead + 1000; i < noteSamples; ++i)
            largest = std::max (
                largest, std::abs (output[static_cast<size_t> (start + i)] - reference[static_cast<size_t> (i)]));

        CHECK (largest < 1.0e-3f);
    }
}

TEST_CASE ("automation of a voice-graph node plays in every voice", "[voice][engine][automation]")
{
    // Coarse tuning automated to +12 semitones (0.625 of -48..48): A4 sounds as A5.
    VoiceRig rig ({ noteArgs (0, 4 * ticksPerBeat, 69) });
    const auto point = list ({ 0, 0.625 });
    rig.project = apply (
        rig.project, "addAutomationLane",
        { { "trackId", rig.trackId },
          { "target", makeObject ({ { "kind", "node" }, { "nodeId", rig.oscillatorId }, { "param", "coarse" } }) },
          { "points", list ({ point }) } });
    rig.start();
    const auto output = render (rig.engine, 48000).left();

    const auto crossings = zeroCrossings (output, 2048, 2048 + 24000);
    CHECK_THAT (static_cast<double> (crossings) / 2.0 / 0.5, WithinAbs (880.0, 4.0));

    for (int voice = 0; std::cmp_less (voice, VoiceAllocator::numVoices); ++voice)
    {
        const auto* instance = rig.compiler.findVoiceInstance (rig.trackId, rig.oscillatorId, voice);
        REQUIRE (instance != nullptr);
        CHECK (instance->params().get (static_cast<size_t> (instance->descriptor().indexOfParam ("coarse"))) == 12.0f);
    }
}

namespace
{
// A voice graph playing the Voice synth example: Note Expression's Pitch and Gate drive the script's sliders
// (docs/design/phase12-voice-scope.md, V15).
void useFaustVoice (VoiceRig& rig)
{
    const auto code = juce::File (DAWG_FAUST_EXAMPLES_DIR).getChildFile ("voice-synth.dsp").loadFileAsString();
    const auto program = FaustCompiler::shared().request (code).program;
    REQUIRE (program != nullptr);

    const auto pathOf = [&] (const char* name)
    {
        const auto& params = program->descriptor().params;
        const auto it =
            std::ranges::find_if (params, [name] (const ParamInfo& p) { return std::string_view (p.name) == name; });
        REQUIRE (it != params.end());
        return juce::String (it->id);
    };

    const auto& inputs = program->descriptor().inputs;
    const auto firstModulation = static_cast<int> (
        std::ranges::find_if (inputs, [] (const PortInfo& p) { return p.type == PortType::modulation; })
        - inputs.begin());

    const auto expression = nodeOfType (rig.voice(), nodetype::noteExpression).id;
    const auto output = nodeOfType (rig.voice(), nodetype::voiceOutput).id;
    rig.edit ("removeNodes", { { "nodeIds", list ({ rig.oscillatorId, nodeOfType (rig.voice(), nodetype::adsr).id,
                                                    nodeOfType (rig.voice(), nodetype::gain).id }) } });
    rig.edit ("addNode", { { "type", nodetype::faust }, { "code", code } });
    const auto faust = rig.voice().nodes.back().id;

    const auto connect = [&] (ObjectId from, int fromPort, ObjectId to, int toPort)
    {
        rig.edit ("connect",
                  { { "fromNode", from }, { "fromPort", fromPort }, { "toNode", to }, { "toPort", toPort } });
    };

    connect (expression, 0, faust, firstModulation);     // Pitch
    connect (expression, 4, faust, firstModulation + 1); // Gate
    connect (faust, 0, output, 0);
    rig.edit ("setModulation",
              { { "nodeId", faust }, { "port", firstModulation }, { "param", pathOf ("pitch") }, { "depth", 1.0 } });
    rig.edit ("setModulation",
              { { "nodeId", faust }, { "port", firstModulation + 1 }, { "param", pathOf ("gate") }, { "depth", 1.0 } });
}

[[nodiscard]] std::vector<float> renderFaustNotes (std::initializer_list<juce::var> notes, int numSamples)
{
    VoiceRig rig (notes);
    useFaustVoice (rig);
    rig.start();
    return render (rig.engine, numSamples).left();
}
} // namespace

TEST_CASE ("a Faust script plays once per voice", "[voice][engine][faust]")
{
    // As with the built-in nodes, two overlapping notes play exactly what each plays alone: every voice has its own
    // DSP, cleared when its note starts.
    const auto first = noteArgs (0, 2 * ticksPerBeat, 57);
    const auto second = noteArgs (ticksPerBeat / 2, 2 * ticksPerBeat, 64);
    constexpr auto length = 4 * samplesPerBeat;

    const auto alone = renderFaustNotes ({ first }, length);
    const auto other = renderFaustNotes ({ second }, length);
    const auto both = renderFaustNotes ({ first, second }, length);

    float difference = 0.0f;

    for (size_t i = 0; i < both.size(); ++i)
        difference = std::max (difference, std::abs (both[i] - (alone[i] + other[i])));

    CHECK (peak (alone, 1000, samplesPerBeat) > 0.01f);
    CHECK (difference < 1.0e-5f);

    // The gate closes at the note-off: the release (0.3 s) ends, and the voice with it.
    CHECK (peak (alone, size_t { 3 } * samplesPerBeat, length) < 1.0e-4f);
}

namespace
{
// Renders 16 held voices for `seconds` and returns the time it took as a share of real time.
[[nodiscard]] double realTimeShare (VoiceRig& rig, double seconds)
{
    rig.start();
    StereoBuffer buffer (blockSize);
    const auto blocks = static_cast<int> (seconds * sampleRate / blockSize);
    const auto started = std::chrono::steady_clock::now();

    for (int block = 0; block < blocks; ++block)
        rig.engine.renderOffline (buffer.channels(), 2, blockSize, sampleRate);

    const auto elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - started).count();
    CHECK (rig.activeVoices() == 16);
    return elapsed / seconds;
}

[[nodiscard]] std::vector<juce::var> sixteenNotes()
{
    std::vector<juce::var> notes;
    notes.reserve (16);

    for (int i = 0; i < 16; ++i)
        notes.push_back (noteArgs (0, 16 * ticksPerBeat, 40 + 2 * i));

    return notes;
}
} // namespace

// A measurement, not a check: run with "[benchmark]" to see what 16 voices cost (docs/design/phase12-voice-scope.md,
// step 6).
TEST_CASE ("sixteen voices: CPU", "[.][benchmark][voice]")
{
    VoiceRig builtin ({});
    VoiceRig faust ({});

    for (auto* rig : { &builtin, &faust })
        rig->project = apply (rig->project, "addNotes",
                              { { "clipId", rig->clipId },
                                { "notes", []
                                  {
                                      juce::Array<juce::var> all;
                                      for (const auto& note : sixteenNotes())
                                          all.add (note);
                                      return juce::var (all);
                                  }() } });

    useFaustVoice (faust);
    const auto builtinShare = realTimeShare (builtin, 8.0);
    const auto faustShare = realTimeShare (faust, 8.0);
    WARN ("16 voices of the default voice graph: " << builtinShare * 100.0 << " % of real time");
    WARN ("16 voices of the Voice synth Faust script: " << faustShare * 100.0 << " % of real time");
}
