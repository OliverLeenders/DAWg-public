// The MIDI effects (docs/design/midi-effects.md).

#include "TestUtils.h"
#include "dawg/AllocationGuard.h"
#include "dawg/GraphCompiler.h"
#include "dawg/MidiEffects.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ProjectFile.h"
#include "dawg/Scales.h"
#include "dawg/Sequence.h"
#include "dawg/StepSequencer.h"
#include "dawg/VoiceAllocator.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr juce::int64 samplesPerSixteenth = 6000; // 120 bpm at 48 kHz

[[nodiscard]] juce::var list (std::initializer_list<juce::var> items)
{
    juce::Array<juce::var> result;

    for (const auto& item : items)
        result.add (item);

    return result;
}

// One event an effect sent, at its sample on the timeline.
struct Heard
{
    juce::int64 sample = 0;
    NoteEventType type = NoteEventType::noteOn;
    ObjectId id = 0;
    double pitch = 0.0;
    float velocity = 0.0f;
    ObjectId from = 0;
    const CompiledNote* note = nullptr;
};

// A track's sequencer feeding a chain of MIDI effects, run block by block as the engine would.
struct Rig
{
    explicit Rig (std::initializer_list<const char*> types, const std::vector<juce::var>& notes = {})
    {
        auto setup = projectWithClip();
        clipId = setup.clipId;
        project = setup.project;

        if (! notes.empty())
        {
            juce::Array<juce::var> array;

            for (const auto& n : notes)
                array.add (n);

            project = applyCommand (project, "addNotes", makeObject ({ { "clipId", clipId }, { "notes", array } }));
        }

        sequence = std::make_shared<TrackSequence> (project.tracks[0]->clips);
        sequencer = NodeRegistry::builtin().create (nodetype::sequencer);
        sequencer->prepare (spec);

        for (const auto* type : types)
        {
            effects.push_back (NodeRegistry::builtin().create (type));
            effects.back()->prepare (spec);
        }

        buffers.resize (effects.size() + 1);
    }

    void set (size_t effect, const char* id, float value)
    {
        auto& node = *effects[effect];

        if (const auto index = node.descriptor().indexOfParam (id); index >= 0)
            node.params().set (static_cast<size_t> (index), value);
    }

    void bypass (size_t effect, bool bypassed)
    {
        GraphNode node;
        node.bypassed = bypassed;
        effects[effect]->configure (node);
    }

    void setNotes (const Project& edited)
    {
        project = edited;
        sequence = std::make_shared<TrackSequence> (project.tracks[0]->clips);
    }

    // Runs `samples` more samples in blocks of `block`, the transport playing or not.
    void run (juce::int64 samples, int block = blockSize, bool guarded = false)
    {
        for (juce::int64 done = 0; done < samples;)
        {
            const auto n = static_cast<int> (std::min<juce::int64> (block, samples - done));
            Transport transport;
            transport.timeMap = timeMap;
            transport.playing = playing;
            transport.started = playing && ! wasPlaying;
            transport.stopped = ! playing && wasPlaying;
            transport.startSample = position;
            transport.startTick = timeMap.sampleToTick (static_cast<double> (position));
            transport.endTick =
                playing ? timeMap.sampleToTick (static_cast<double> (position + n)) : transport.startTick;
            wasPlaying = playing;

            ProcessContext context;
            context.numSamples = n;
            context.transport = &transport;
            context.sequence = sequence.get();
            context.liveEvents = live;

            const auto runAll = [&]
            {
                buffers[0].clear();
                context.eventsIn = nullptr;
                context.eventsOut = buffers.data();
                sequencer->process (context);

                for (size_t i = 0; i < effects.size(); ++i)
                {
                    buffers[i + 1].clear();
                    context.eventsIn = &buffers[i];
                    context.eventsOut = &buffers[i + 1];
                    effects[i]->process (context);
                }
            };

            if (guarded)
            {
                const ScopedNoAllocation guard;
                runAll();
            }
            else
            {
                runAll();
            }

            live.clear();

            for (const auto& event : buffers.back().events())
            {
                Heard one {
                    position + event.sampleOffset, event.type, event.noteId, 0.0, 0.0f, event.fromNoteId, event.note
                };

                if (event.note != nullptr)
                {
                    one.pitch = event.note->pitch;
                    one.velocity = event.note->velocity;
                }

                if (event.type == NoteEventType::expression)
                {
                    one.pitch = static_cast<double> (event.value);
                    one.from = event.lane;
                }

                heard.push_back (one);
            }

            position += n;
            done += n;
        }
    }

    // Live notes (negative ids) arrive at the start of the next block.
    void press (ObjectId id, double pitch, float velocity = 0.8f)
    {
        auto& note = liveNotes[id];
        note.id = id;
        note.pitch = pitch;
        note.velocity = velocity;
        live.push_back ({ 0, NoteEventType::noteOn, id, &note });
    }

    void release (ObjectId id) { live.push_back ({ 0, NoteEventType::noteOff, id, nullptr }); }

    [[nodiscard]] std::vector<Heard> of (NoteEventType type) const
    {
        std::vector<Heard> result;
        std::ranges::copy_if (heard, std::back_inserter (result), [type] (const Heard& h) { return h.type == type; });
        return result;
    }

    // The ids still sounding: started and not ended. A releaseAll or chokeAll ends the sequence's own notes.
    [[nodiscard]] std::set<ObjectId> sounding() const
    {
        std::set<ObjectId> result;

        for (const auto& h : heard)
        {
            if (h.type == NoteEventType::noteOn || h.type == NoteEventType::legato)
            {
                if (h.type == NoteEventType::legato)
                    result.erase (h.from);

                result.insert (h.id);
            }
            else if (h.type == NoteEventType::noteOff || h.type == NoteEventType::choke)
            {
                result.erase (h.id);
            }
            else if (h.type == NoteEventType::releaseAll || h.type == NoteEventType::chokeAll)
            {
                std::erase_if (result, [] (ObjectId id) { return id >= 0; });
            }
            else if (h.type == NoteEventType::sourceChanged)
            {
                result.clear();
            }
        }

        return result;
    }

    Project project;
    ObjectId clipId = 0;
    std::shared_ptr<TrackSequence> sequence;
    std::unique_ptr<Node> sequencer;
    std::vector<std::unique_ptr<Node>> effects;
    std::vector<EventBuffer> buffers;
    TimeMap timeMap { constantTempo(), sampleRate };
    juce::int64 position = 0;
    bool playing = true;
    bool wasPlaying = false;
    std::vector<NoteEvent> live;
    std::map<ObjectId, CompiledNote> liveNotes;
    std::vector<Heard> heard;
};

[[nodiscard]] juce::var note (Ticks start, Ticks length, double pitch)
{
    return noteArgs (start, length, pitch);
}

[[nodiscard]] juce::var bentNote (Ticks start, Ticks length, double pitch, double bend)
{
    auto n = noteArgs (start, length, pitch);
    juce::Array<juce::var> first;
    first.add (0);
    first.add (0.0);
    juce::Array<juce::var> last;
    last.add (length);
    last.add (bend);
    n.getDynamicObject()->setProperty ("expression", makeObject ({ { "pitch", list ({ first, last }) } }));
    return n;
}

constexpr auto beat = ticksPerBeat;
constexpr auto sixteenth = ticksPerBeat / 4;
} // namespace

// ---------------------------------------------------------------------------------------------------------
// Shared pieces.

TEST_CASE ("scales and locking a key into one", "[midifx][scales]")
{
    const auto major = scales[0].mask;
    CHECK (inScale (60, 0, major));
    CHECK_FALSE (inScale (61, 0, major));
    CHECK (inScale (61, 1, major));                                    // C# major
    CHECK (lockToScale (61, 0, major, ScaleDirection::nearest) == 60); // a tie goes down
    CHECK (lockToScale (61, 0, major, ScaleDirection::up) == 62);
    CHECK (lockToScale (66, 0, major, ScaleDirection::nearest) == 65);
    CHECK (lockToScale (66, 0, major, ScaleDirection::up) == 67);
    CHECK (lockToScale (63, 0, scales[10].mask, ScaleDirection::nearest) == 63); // minor pentatonic holds Eb
    CHECK (lockToScale (61, 0, scales[14].mask, ScaleDirection::nearest) == 61); // chromatic

    // The same masks as ui/src/music/scales.test.ts.
    constexpr std::array<std::uint16_t, 15> masks { 0xab5, 0x5ad, 0x9ad, 0xaad, 0x6ad, 0x5ab, 0xad5, 0x6b5,
                                                    0x56b, 0x295, 0x4a9, 0x4e9, 0x555, 0x6db, 0xfff };

    for (size_t i = 0; i < scales.size(); ++i)
        CHECK (scales[i].mask == masks[i]);
}

TEST_CASE ("arpeggio patterns", "[midifx][arp]")
{
    using midifx::ArpMode;
    const auto keys = [] (ArpMode mode, int count, int octaves, int steps)
    {
        std::vector<int> result;

        for (int step = 0; step < steps; ++step)
        {
            const auto pick = midifx::arpPick (mode, count, octaves, static_cast<juce::uint64> (step), 0);
            result.push_back (pick.key + 10 * pick.octave);
        }

        return result;
    };

    CHECK (keys (ArpMode::up, 3, 1, 6) == std::vector { 0, 1, 2, 0, 1, 2 });
    CHECK (keys (ArpMode::down, 3, 1, 4) == std::vector { 2, 1, 0, 2 });
    CHECK (keys (ArpMode::upDown, 3, 1, 6) == std::vector { 0, 1, 2, 1, 0, 1 });
    CHECK (keys (ArpMode::downUp, 3, 1, 5) == std::vector { 2, 1, 0, 1, 2 });
    CHECK (keys (ArpMode::upAndDown, 3, 1, 7) == std::vector { 0, 1, 2, 2, 1, 0, 0 });
    CHECK (keys (ArpMode::up, 2, 2, 5) == std::vector { 0, 1, 10, 11, 0 });
    CHECK (keys (ArpMode::upDown, 1, 1, 3) == std::vector { 0, 0, 0 });
    CHECK (midifx::arpPick (ArpMode::chord, 3, 2, 1, 0).key == -1);
}

TEST_CASE ("chord inversions keep the played note first", "[midifx][chord]")
{
    const std::array<int, 6> major { 0, 4, 7 };
    CHECK (midifx::invert (major, 3, 1) == std::array<int, 6> { 12, 4, 7 });
    CHECK (midifx::invert (major, 3, 2) == std::array<int, 6> { 12, 16, 7 });
    CHECK (midifx::invert (major, 3, -1) == std::array<int, 6> { 0, 4, -5 });
    CHECK (midifx::invert (major, 3, 5) == std::array<int, 6> { 12, 16, 7 }); // at most count - 1
}

TEST_CASE ("grid lines restart at bars and swing", "[midifx][swing]")
{
    const TempoMap map ({}, { { 0, 4, 4 }, { 1, 7, 8 } });
    const auto eighth = static_cast<double> (beat / 2);
    CHECK (midifx::nextGridLine (map, 0.0, eighth, 0.5, false) == 0.0);
    CHECK (midifx::nextGridLine (map, 0.0, eighth, 0.5, true) == eighth);
    CHECK (midifx::nextGridLine (map, 1.0, eighth, 0.75, false) == 0.75 * static_cast<double> (beat));
    // 7/8: its seventh 8th has no partner, and the next bar starts the grid again.
    const auto bar1 = static_cast<double> (4 * beat);
    CHECK (midifx::nextGridLine (map, bar1 + 5.5 * eighth, eighth, 0.75, false)
           == bar1 + 5.5 * eighth); // the swung sixth
    CHECK (midifx::nextGridLine (map, bar1 + 5.6 * eighth, eighth, 0.75, false) == bar1 + 6 * eighth);
    CHECK (midifx::nextGridLine (map, bar1 + 6 * eighth, eighth, 0.75, true) == bar1 + 7 * eighth);
}

TEST_CASE ("derived note ids never meet live ones or each other's", "[midifx]")
{
    CHECK (midifx::derivedNoteId (1, 0) < -(ObjectId { 1 } << 52) + 1);
    CHECK (midifx::derivedNoteId (1, 0) != midifx::derivedNoteId (2, 0));
    CHECK (midifx::derivedNoteId (1, 1) != midifx::derivedNoteId (1, 0));
}

// ---------------------------------------------------------------------------------------------------------
// Scale Lock.

TEST_CASE ("scale lock moves notes into the scale and keeps a bend's shape", "[midifx][scaleLock]")
{
    Rig rig ({ nodetype::scaleLock },
             { note (0, beat, 60), bentNote (beat, beat, 61.25, 2.0), note (2 * beat, beat, 66) });
    rig.run (4 * samplesPerSixteenth * 4);

    const auto ons = rig.of (NoteEventType::noteOn);
    REQUIRE (ons.size() == 3);
    CHECK (ons[0].id > 0); // in the scale: passes as itself
    CHECK (ons[0].pitch == 60.0);
    CHECK (ons[1].id < 0);
    CHECK_THAT (ons[1].pitch, WithinAbs (60.25, 1.0e-9)); // the detune is kept
    CHECK (ons[2].pitch == 65.0);
    CHECK (ons[1].sample == 24000);

    // The bend is the source's: half way, a semitone up.
    const auto& map = constantTempo();
    CHECK_THAT (ons[1].note->pitchOffset (static_cast<double> (beat + beat / 2), map), WithinAbs (1.0, 1.0e-6));
    CHECK (rig.sounding().empty());
}

TEST_CASE ("scale lock's filter drops notes outside the scale", "[midifx][scaleLock]")
{
    Rig rig ({ nodetype::scaleLock }, { note (0, beat, 61), note (beat, beat, 62) });
    rig.set (0, "mode", 3.0f);
    rig.run (3 * 24000);
    const auto ons = rig.of (NoteEventType::noteOn);
    REQUIRE (ons.size() == 1);
    CHECK (ons[0].pitch == 62.0);
    CHECK (rig.of (NoteEventType::noteOff).size() == 1);
}

TEST_CASE ("scale lock follows the project key while one is set", "[midifx][scaleLock][key]")
{
    // C sharp: outside C major, in D major.
    const auto lockedPitch = [] (std::optional<ProjectKey> key, float follow)
    {
        Rig rig ({ nodetype::scaleLock }, { note (0, beat, 61) });
        rig.set (0, "follow", follow);

        if (key.has_value())
            rig.effects[0]->setKeys (std::make_shared<const KeyTimeline> (KeyTimeline { { { 0.0, *key } } }));

        rig.run (24000);
        const auto ons = rig.of (NoteEventType::noteOn);
        REQUIRE (ons.size() == 1);
        return ons[0].pitch;
    };

    const ProjectKey dMajor { true, 2, 0 };
    CHECK (lockedPitch (dMajor, 1.0f) == 61.0);
    CHECK (lockedPitch (dMajor, 0.0f) == 60.0);                     // its own C major
    CHECK (lockedPitch (ProjectKey { false, 2, 0 }, 1.0f) == 60.0); // no key set: its own
    CHECK (lockedPitch (std::nullopt, 1.0f) == 60.0);

    // A new instance follows.
    const auto fresh = NodeRegistry::builtin().create (nodetype::scaleLock);
    const auto& descriptor = fresh->descriptor();
    CHECK (descriptor.params[static_cast<size_t> (descriptor.indexOfParam ("follow"))].defaultValue == 1.0f);
}

TEST_CASE ("scale lock follows the key where each note plays", "[midifx][scaleLock][key]")
{
    // C sharp twice: in D major for the first beat, then in C major.
    Rig rig ({ nodetype::scaleLock }, { note (0, beat, 61), note (2 * beat, beat, 61) });
    rig.effects[0]->setKeys (std::make_shared<const KeyTimeline> (KeyTimeline {
        { { 0.0, ProjectKey { true, 2, 0 } }, { static_cast<double> (beat), ProjectKey { true, 0, 0 } } } }));
    rig.run (4 * 24000);
    const auto ons = rig.of (NoteEventType::noteOn);
    REQUIRE (ons.size() == 2);
    CHECK (ons[0].pitch == 61.0);
    CHECK (ons[1].pitch == 60.0);
}

TEST_CASE ("key changes are commands, saved, and change nothing that needs a recompile", "[midifx][key][commands]")
{
    auto setup = projectWithClip();
    auto project =
        apply (setup.project, "insertNode", { { "trackId", setup.trackId }, { "type", nodetype::scaleLock } });
    REQUIRE (project.keys.size() == 1);
    CHECK_FALSE (project.keys[0].key.enabled);

    GraphCompiler compiler;
    REQUIRE (compiler.compile (project, spec) != nullptr);

    project = apply (project, "setKey", { { "enabled", true }, { "root", 9 }, { "scale", 1 } });
    CHECK (project.keys[0].key == ProjectKey { true, 9, 1 });
    CHECK (compiler.update (project, spec) == nullptr);

    // A change on bar 4 starts from the key in effect; arguments left out keep their values.
    project = apply (project, "setKey", { { "bar", 4 }, { "root", 2 } });
    REQUIRE (project.keys.size() == 2);
    CHECK (project.keys[1] == KeyChange { 4, ProjectKey { true, 2, 1 } });
    CHECK (project.keyAtBar (3) == ProjectKey { true, 9, 1 });
    CHECK (project.keyAtBar (7) == ProjectKey { true, 2, 1 });
    CHECK (compiler.update (project, spec) == nullptr);

    // In ticks, by the tempo map (4/4: bar 4 starts on beat 16).
    const auto timeline = project.keyTimeline();
    CHECK (timeline.at (16.0 * ticksPerBeat - 1.0).root == 9);
    CHECK (timeline.at (16.0 * ticksPerBeat).root == 2);

    project = apply (project, "moveKey", { { "from", 4 }, { "to", 2 } });
    CHECK (project.keys[1].bar == 2);
    CHECK_THROWS_AS (apply (project, "moveKey", { { "from", 0 }, { "to", 3 } }), CommandError);
    CHECK_THROWS_AS (apply (project, "removeKey", { { "bar", 0 } }), CommandError);

    // Out-of-range values are clamped.
    project = apply (project, "setKey", { { "bar", 2 }, { "root", 14 }, { "scale", 99 } });
    CHECK (project.keys[1].key == ProjectKey { true, 11, static_cast<int> (scales.size()) - 1 });

    const auto folder =
        juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("dawg-key-" + juce::Uuid().toString());
    REQUIRE (folder.createDirectory());
    const auto file = folder.getChildFile (juce::String ("Song") + projectfile::extension);
    REQUIRE (projectfile::write (project, { "id", "0.1.0" }, file).isEmpty());
    const auto loaded = projectfile::read (file).project;
    std::ignore = folder.deleteRecursively();
    CHECK (loaded.keys == project.keys);

    project = apply (project, "removeKey", { { "bar", 2 } });
    CHECK (project.keys.size() == 1);
}

TEST_CASE ("linked notes stay linked through scale lock", "[midifx][scaleLock][links]")
{
    Rig rig ({ nodetype::scaleLock }, { note (0, beat, 61), note (beat, beat, 64) });
    const auto& notes = rig.project.tracks[0]->clips[0]->notes;
    rig.setNotes (applyCommand (rig.project, "setNoteLinks",
                                makeObject ({ { "clipId", rig.clipId },
                                              { "links", list ({ makeObject ({ { "from", notes[0]->id },
                                                                               { "to", notes[1]->id },
                                                                               { "glideMs", 50.0 } }) }) } })));
    rig.run (3 * 24000);

    const auto ons = rig.of (NoteEventType::noteOn);
    const auto legatos = rig.of (NoteEventType::legato);
    REQUIRE (ons.size() == 1);
    REQUIRE (legatos.size() == 1);
    CHECK (ons[0].pitch == 60.0);
    CHECK (legatos[0].from == ons[0].id);
    CHECK (legatos[0].pitch == 64.0);
    CHECK (rig.sounding().empty());
}

// ---------------------------------------------------------------------------------------------------------
// Chord.

TEST_CASE ("a chord from one note, sharing its curves", "[midifx][chord]")
{
    Rig rig ({ nodetype::chord }, { bentNote (0, beat, 60, 2.0) });
    rig.set (0, "velocity", 50.0f);
    rig.run (2 * 24000);

    const auto ons = rig.of (NoteEventType::noteOn);
    REQUIRE (ons.size() == 3);
    std::vector<double> pitches;

    for (const auto& on : ons)
    {
        pitches.push_back (on.pitch);
        CHECK (on.sample == 0);
        CHECK (on.id < 0);
        CHECK_THAT (on.note->pitchOffset (static_cast<double> (beat / 2), constantTempo()), WithinAbs (1.0, 1.0e-6));
    }

    CHECK (pitches == std::vector { 60.0, 64.0, 67.0 });
    CHECK_THAT (ons[0].velocity, WithinAbs (0.8, 1.0e-6));
    CHECK_THAT (ons[1].velocity, WithinAbs (0.4, 1.0e-6));

    const auto offs = rig.of (NoteEventType::noteOff);
    REQUIRE (offs.size() == 3);
    CHECK (offs[0].sample == 24000);
    CHECK (rig.sounding().empty());
}

TEST_CASE ("a strummed chord, block size or not", "[midifx][chord]")
{
    for (const auto block : { 1, 17, 64, 512 })
    {
        Rig rig ({ nodetype::chord }, { note (0, beat, 60) });
        rig.set (0, "shape", 7.0f); // 7
        rig.set (0, "strum", 10.0f);
        rig.set (0, "strumOrder", 1.0f); // down
        rig.run (30000, block);

        const auto ons = rig.of (NoteEventType::noteOn);
        REQUIRE (ons.size() == 4);
        CHECK (ons[0].sample == 0);
        CHECK (ons[0].pitch == 70.0);
        CHECK (ons[1].sample == 480);
        CHECK (ons[1].pitch == 67.0);
        CHECK (ons[3].sample == 1440);
        CHECK (ons[3].pitch == 60.0);
        CHECK (rig.sounding().empty());
    }
}

TEST_CASE ("a note let go before its strum ends starts no more notes", "[midifx][chord]")
{
    Rig rig ({ nodetype::chord }, { note (0, 100, 60) }); // about 156 samples
    rig.set (0, "strum", 100.0f);
    rig.run (24000);
    CHECK (rig.of (NoteEventType::noteOn).size() == 1);
    CHECK (rig.sounding().empty());
}

TEST_CASE ("linked chords continue voice by voice", "[midifx][chord][links]")
{
    Rig rig ({ nodetype::chord }, { note (0, beat, 60), note (beat, beat, 62) });
    const auto& notes = rig.project.tracks[0]->clips[0]->notes;
    rig.setNotes (applyCommand (rig.project, "setNoteLinks",
                                makeObject ({ { "clipId", rig.clipId },
                                              { "links", list ({ makeObject ({ { "from", notes[0]->id },
                                                                               { "to", notes[1]->id },
                                                                               { "glideMs", 50.0 } }) }) } })));
    rig.run (3 * 24000);

    const auto ons = rig.of (NoteEventType::noteOn);
    const auto legatos = rig.of (NoteEventType::legato);
    REQUIRE (ons.size() == 3);
    REQUIRE (legatos.size() == 3);

    for (size_t i = 0; i < 3; ++i)
    {
        CHECK (legatos[i].from == ons[i].id);
        CHECK (legatos[i].pitch == ons[i].pitch + 2.0);
    }

    CHECK (rig.sounding().empty());
}

TEST_CASE ("live expression reaches every chord note", "[midifx][chord][live]")
{
    Rig rig ({ nodetype::chord });
    rig.playing = false;
    rig.press (-5, 60.0);
    rig.run (512);
    rig.live.push_back ({ 0, NoteEventType::expression, -5, nullptr, static_cast<int> (Lane::pressure), 0.7f });
    rig.run (512);

    const auto expressions = rig.of (NoteEventType::expression);
    REQUIRE (expressions.size() == 3);
    CHECK (std::ranges::all_of (expressions, [] (const Heard& h) { return h.id < 0 && h.id != -5; }));

    rig.release (-5);
    rig.run (512);
    CHECK (rig.sounding().empty());
}

// ---------------------------------------------------------------------------------------------------------
// Arpeggiator.

TEST_CASE ("an arpeggio on the grid, at any block size", "[midifx][arp]")
{
    for (const auto block : { 1, 17, 64, 512 })
    {
        Rig rig ({ nodetype::arpeggiator }, { note (0, beat, 60), note (0, beat, 64), note (0, beat, 67) });
        rig.run (2 * 24000, block);

        const auto ons = rig.of (NoteEventType::noteOn);
        REQUIRE (ons.size() == 4);

        for (size_t i = 0; i < ons.size(); ++i)
            CHECK (ons[i].sample == static_cast<juce::int64> (i) * samplesPerSixteenth);

        CHECK (ons[0].pitch == 60.0);
        CHECK (ons[1].pitch == 64.0);
        CHECK (ons[2].pitch == 67.0);
        CHECK (ons[3].pitch == 60.0);

        const auto offs = rig.of (NoteEventType::noteOff);
        REQUIRE (offs.size() == 4);
        CHECK (offs[0].sample == samplesPerSixteenth / 2); // gate 50 %
        CHECK (rig.sounding().empty());
    }
}

TEST_CASE ("an arpeggio swings and follows a tempo ramp", "[midifx][arp][swing]")
{
    Rig swung ({ nodetype::arpeggiator }, { note (0, beat, 60) });
    swung.set (0, "swing", 75.0f);
    swung.run (24000);
    const auto ons = swung.of (NoteEventType::noteOn);
    REQUIRE (ons.size() == 4);
    CHECK (ons[1].sample == 9000); // 75 % of the pair of 16ths
    CHECK (ons[2].sample == 12000);
    CHECK (ons[3].sample == 21000);

    const TempoMap ramp ({ { 0, 60.0, true }, { 4 * beat, 180.0, false } }, {});
    Rig rig ({ nodetype::arpeggiator }, { note (0, 4 * beat, 60) });
    rig.timeMap = TimeMap (ramp, sampleRate);
    rig.run (static_cast<juce::int64> (ramp.secondsAt (static_cast<double> (4 * beat)) * sampleRate));
    const auto steps = rig.of (NoteEventType::noteOn);
    REQUIRE (steps.size() == 16);

    for (size_t i = 0; i < steps.size(); ++i)
        CHECK (steps[i].sample
               == std::llround (rig.timeMap.tickToSample (static_cast<double> (static_cast<Ticks> (i) * sixteenth))));
}

TEST_CASE ("arpeggiated notes follow the held note's bend as it is when they play", "[midifx][arp]")
{
    Rig rig ({ nodetype::arpeggiator }, { bentNote (0, beat, 60, 4.0) });
    rig.run (24000);
    const auto ons = rig.of (NoteEventType::noteOn);
    REQUIRE (ons.size() == 4);
    // The third step plays at half a beat, where the bend has reached 2 semitones.
    CHECK_THAT (ons[2].note->pitchOffset (static_cast<double> (beat / 2), constantTempo()), WithinAbs (2.0, 1.0e-6));
}

TEST_CASE ("latch keeps the last keys playing until new ones come", "[midifx][arp][live]")
{
    Rig rig ({ nodetype::arpeggiator });
    rig.set (0, "latch", 1.0f);
    rig.playing = false;
    rig.press (-1, 60.0);
    rig.run (512);
    rig.release (-1);
    rig.run (4 * samplesPerSixteenth);
    CHECK (rig.of (NoteEventType::noteOn).size() == 5); // at 0, 6000, ... 24000

    rig.press (-2, 72.0);
    rig.run (2 * samplesPerSixteenth);
    const auto ons = rig.of (NoteEventType::noteOn);
    CHECK (ons.back().pitch == 72.0);
    CHECK (std::ranges::count_if (ons, [] (const Heard& h) { return h.pitch == 72.0; }) == 2);
    CHECK (ons[ons.size() - 2].pitch == 72.0); // the old key went

    rig.set (0, "latch", 0.0f);
    rig.release (-2);
    rig.run (4 * samplesPerSixteenth);
    CHECK (rig.of (NoteEventType::noteOn).size() == ons.size());
    CHECK (rig.sounding().empty());
}

TEST_CASE ("with the transport stopped the arpeggiator keeps its own time", "[midifx][arp][live]")
{
    Rig rig ({ nodetype::arpeggiator });
    rig.playing = false;
    rig.run (1000);
    rig.press (-1, 60.0);
    rig.press (-2, 63.0);
    rig.run (3 * samplesPerSixteenth, 100);
    const auto ons = rig.of (NoteEventType::noteOn);
    REQUIRE (ons.size() == 3);
    CHECK (ons[0].sample == 1000);
    CHECK (ons[1].sample == 1000 + samplesPerSixteenth);
    CHECK (ons[1].pitch == 63.0);
    rig.release (-1);
    rig.release (-2);
    rig.run (samplesPerSixteenth);
    CHECK (rig.sounding().empty());
}

// ---------------------------------------------------------------------------------------------------------
// Note Repeat.

TEST_CASE ("note repeat hits at the note's start, then on the grid", "[midifx][repeat]")
{
    for (const auto block : { 1, 64, 512 })
    {
        Rig rig ({ nodetype::noteRepeat }, { note (100, beat - 100, 36), note (0, beat / 2, 42) });
        rig.run (2 * 24000, block);
        std::vector<juce::int64> kick;
        std::vector<juce::int64> hat;

        for (const auto& on : rig.of (NoteEventType::noteOn))
            (on.pitch == 36.0 ? kick : hat).push_back (on.sample);

        CHECK (hat == std::vector<juce::int64> { 0, 6000 });
        REQUIRE (kick.size() == 4);
        CHECK (kick[0] == std::llround (rig.timeMap.tickToSample (100.0)));
        CHECK (kick[1] == 6000);
        CHECK (kick[3] == 18000);
        CHECK (rig.sounding().empty());
    }
}

// ---------------------------------------------------------------------------------------------------------
// No note hangs.

TEST_CASE ("no MIDI effect leaves a note sounding when the transport stops", "[midifx][hung]")
{
    for (const auto* type : { nodetype::arpeggiator, nodetype::chord, nodetype::scaleLock, nodetype::noteRepeat })
    {
        Rig rig ({ type }, { note (0, 8 * beat, 61), note (0, 8 * beat, 64) });
        rig.set (0, "gate", 200.0f);
        rig.run (30000);
        CHECK_FALSE (rig.sounding().empty());
        rig.playing = false;
        rig.run (512);
        INFO (type);
        CHECK (rig.sounding().empty());
    }
}

TEST_CASE ("no MIDI effect leaves a note sounding when bypassed, and held notes carry on", "[midifx][hung]")
{
    for (const auto* type : { nodetype::arpeggiator, nodetype::chord, nodetype::scaleLock, nodetype::noteRepeat })
    {
        Rig rig ({ type }, { note (0, 4 * beat, 61) });
        rig.run (10000);
        rig.bypass (0, true);
        rig.run (512);
        INFO (type);
        const auto now = rig.sounding();
        REQUIRE (now.size() == 1);
        CHECK (*now.begin() > 0); // the held note plays as itself

        rig.bypass (0, false);
        rig.run (30000);
        CHECK (std::ranges::none_of (rig.sounding(), [] (ObjectId id) { return id > 0; }));
        rig.run (4 * 24000);
        CHECK (rig.sounding().empty());
    }
}

TEST_CASE ("no MIDI effect leaves a note sounding when its source note is deleted", "[midifx][hung]")
{
    for (const auto* type : { nodetype::arpeggiator, nodetype::chord, nodetype::scaleLock, nodetype::noteRepeat })
    {
        Rig rig ({ type }, { note (0, 4 * beat, 61) });
        rig.set (0, "gate", 200.0f);
        rig.run (10000);
        const auto id = rig.project.tracks[0]->clips[0]->notes[0]->id;
        rig.setNotes (applyCommand (rig.project, "removeNotes",
                                    makeObject ({ { "clipId", rig.clipId }, { "ids", list ({ id }) } })));
        rig.run (512);
        INFO (type);
        CHECK (std::ranges::none_of (rig.sounding(), [] (ObjectId i) { return i < 0; }));
    }
}

TEST_CASE ("MIDI effects follow a sequence edit without restarting", "[midifx]")
{
    Rig rig ({ nodetype::chord }, { bentNote (0, 4 * beat, 60, 0.0) });
    rig.run (10000);
    const auto ons = rig.of (NoteEventType::noteOn);
    REQUIRE (ons.size() == 3);
    const auto id = rig.project.tracks[0]->clips[0]->notes[0]->id;
    rig.setNotes (applyCommand (
        rig.project, "updateNotes",
        makeObject ({ { "clipId", rig.clipId },
                      { "notes",
                        list ({ makeObject (
                            { { "id", id },
                              { "expression", makeObject ({ { "pitch", list ({ list ({ 0, 3.0 }) }) } }) } }) }) } })));
    rig.run (512);
    CHECK (rig.of (NoteEventType::noteOn).size() == 3);
    CHECK_THAT (ons[1].note->pitchOffset (static_cast<double> (beat), constantTempo()), WithinAbs (3.0, 1.0e-6));
}

TEST_CASE ("a source change makes voices forget their notes", "[midifx][hung][voices]")
{
    CompiledNote derived;
    derived.id = midifx::derivedNoteId (99, 1);
    derived.pitch = 60.0;
    VoiceAllocator voices;
    std::ignore = voices.handle ({ 0, NoteEventType::noteOn, derived.id, &derived });
    std::ignore = voices.handle ({ 0, NoteEventType::noteOn, -3, &derived });
    std::ignore = voices.handle ({ 0, NoteEventType::sourceChanged, 0, nullptr });

    for (const auto& voice : voices.voices())
    {
        if (voice.state == VoiceState::free)
            continue;

        CHECK (voice.state == VoiceState::choking);
        CHECK (voice.note == nullptr);
    }
}

TEST_CASE ("MIDI effects don't allocate while playing", "[midifx][realtime]")
{
    for (const auto* type : { nodetype::arpeggiator, nodetype::chord, nodetype::scaleLock, nodetype::noteRepeat })
    {
        Rig rig ({ type }, { bentNote (0, beat, 61, 1.0), note (0, beat, 64), note (beat, beat, 66) });
        const auto before = allocationViolationCount();
        rig.run (3 * 24000, 128, true);
        rig.bypass (0, true);
        rig.run (512, 128, true);
        rig.bypass (0, false);
        rig.run (512, 128, true);
        rig.playing = false;
        rig.run (512, 128, true);
        INFO (type);
        CHECK (allocationViolationCount() == before);
    }
}

// ---------------------------------------------------------------------------------------------------------
// The engine and the chain.

namespace
{
[[nodiscard]] std::vector<juce::String> chainTypes (const Project& project)
{
    // Walk the note and audio connections from the sequencer.
    const auto& graph = project.tracks[0]->graph;
    std::vector<juce::String> result;
    const auto find = [&] (const char* type)
    { return std::ranges::find_if (graph.nodes, [type] (const GraphNode& n) { return n.type == type; })->id; };

    for (auto at = find (nodetype::sequencer); at != 0;)
    {
        const auto& node = *std::ranges::find_if (graph.nodes, [at] (const GraphNode& n) { return n.id == at; });
        result.push_back (node.type);
        const auto next = std::ranges::find_if (graph.connections, [at] (const Connection& c)
                                                { return c.fromNode == at && c.fromPort == 0 && c.toPort == 0; });
        at = next != graph.connections.end() ? next->toNode : 0;
    }

    return result;
}

[[nodiscard]] ObjectId idOf (const Project& project, const char* type)
{
    const auto& nodes = project.tracks[0]->graph.nodes;
    return std::ranges::find_if (nodes, [type] (const GraphNode& n) { return n.type == type; })->id;
}
} // namespace

TEST_CASE ("MIDI effects go between the sequencer and the instrument", "[midifx][commands]")
{
    auto setup = projectWithClip();
    const auto track = setup.trackId;
    auto project = apply (setup.project, "insertNode", { { "trackId", track }, { "type", nodetype::chord } });
    project = apply (project, "insertNode", { { "trackId", track }, { "type", nodetype::arpeggiator } });
    auto types = chainTypes (project);
    REQUIRE (types.size() >= 4);
    CHECK (types[0] == nodetype::sequencer);
    CHECK (types[1] == nodetype::chord);
    CHECK (types[2] == nodetype::arpeggiator);
    CHECK (types[3] == nodetype::mpeSynth);

    project = apply (project, "reorderNode",
                     { { "trackId", track }, { "nodeId", idOf (project, nodetype::arpeggiator) }, { "index", 0 } });
    types = chainTypes (project);
    CHECK (types[1] == nodetype::arpeggiator);
    CHECK (types[2] == nodetype::chord);

    // A new instrument keeps them in front of it.
    project = apply (project, "insertNode", { { "trackId", track }, { "type", nodetype::sampler } });
    types = chainTypes (project);
    CHECK (types[3] == nodetype::sampler);

    project =
        apply (project, "removeNode", { { "trackId", track }, { "nodeId", idOf (project, nodetype::arpeggiator) } });
    types = chainTypes (project);
    CHECK (types[1] == nodetype::chord);
    CHECK (types[2] == nodetype::sampler);

    // Without an instrument they stay hung off the sequencer.
    project = apply (project, "removeNode", { { "trackId", track }, { "nodeId", idOf (project, nodetype::sampler) } });
    types = chainTypes (project);
    REQUIRE (types.size() == 2);
    CHECK (types[1] == nodetype::chord);
    project = apply (project, "insertNode", { { "trackId", track }, { "type", nodetype::mpeSynth } });
    CHECK (chainTypes (project)[2] == nodetype::mpeSynth);
}

TEST_CASE ("MIDI effects stay out of voice graphs and audio tracks", "[midifx][commands]")
{
    auto setup = projectWithClip();
    auto project =
        apply (setup.project, "insertNode", { { "trackId", setup.trackId }, { "type", nodetype::voiceInstrument } });
    const auto voiceInstrument = idOf (project, nodetype::voiceInstrument);
    CHECK_THROWS_AS (
        apply (project, "addNode",
               { { "trackId", setup.trackId }, { "voiceNodeId", voiceInstrument }, { "type", nodetype::arpeggiator } }),
        CommandError);

    project = apply (project, "addTrack", { { "kind", "audio" } });
    CHECK_THROWS_AS (
        apply (project, "insertNode", { { "trackId", project.tracks[1]->id }, { "type", nodetype::chord } }),
        CommandError);
}

TEST_CASE ("removing a MIDI effect mid-chord silences its notes", "[midifx][hung][engine]")
{
    auto setup = projectWithClip();
    auto project = apply (setup.project, "addNotes",
                          { { "clipId", setup.clipId }, { "notes", list ({ noteArgs (0, 16 * beat, 60) }) } });
    project = apply (project, "insertNode", { { "trackId", setup.trackId }, { "type", nodetype::chord } });

    GraphCompiler compiler;
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, spec));
    engine.play();
    const auto before = render (engine, 24000).left();
    CHECK (std::ranges::any_of (before, [] (float s) { return std::abs (s) > 1.0e-3f; }));

    project =
        apply (project, "removeNode", { { "trackId", setup.trackId }, { "nodeId", idOf (project, nodetype::chord) } });
    engine.setEngine (compiler.compile (project, spec));
    std::ignore = render (engine, 4800); // the old chord fades out
    const auto after = render (engine, 24000).left();

    // Only the played note sounds now: the chord's notes are gone, and nothing replays the held source.
    CHECK (std::ranges::all_of (after, [] (float s) { return std::abs (s) < 1.0e-4f; }));
}

TEST_CASE ("render MIDI effects to the clip", "[midifx][render][commands]")
{
    auto setup = projectWithClip();
    auto project = apply (setup.project, "addNotes",
                          { { "clipId", setup.clipId }, { "notes", list ({ bentNote (0, beat, 60, 4.0) }) } });
    project = apply (project, "insertNode", { { "trackId", setup.trackId }, { "type", nodetype::arpeggiator } });
    project =
        apply (project, "insertNode", { { "trackId", setup.trackId }, { "type", nodetype::chord }, { "index", 0 } });
    project = apply (project, "renderMidiEffects", { { "clipId", setup.clipId } });

    const auto& clip = *project.tracks[0]->clips[0];
    // A major chord arpeggiated in 16ths over one beat: four notes, C E G C.
    REQUIRE (clip.notes.size() == 4);
    CHECK (clip.notes[0]->start == 0);
    CHECK (clip.notes[1]->start == sixteenth);
    CHECK (clip.notes[1]->pitch == 64.0);
    CHECK (clip.notes[3]->pitch == 60.0);
    CHECK (clip.notes[0]->length == sixteenth / 2);

    // The third starts at half a beat, where the bend had reached 2 semitones, and goes on from there.
    const auto& curve = clip.notes[2]->expression[static_cast<size_t> (Lane::pitch)];
    REQUIRE (curve.size() >= 2);
    CHECK_THAT (curve.front().value, WithinAbs (2.0, 1.0e-6));
    CHECK_THAT (evaluateCurve (curve, static_cast<double> (sixteenth / 2), 0.0), WithinAbs (2.5, 1.0e-6));

    for (const auto& node : project.tracks[0]->graph.nodes)
        if (nodetype::isMidiEffect (node.type))
            CHECK (node.bypassed);

    // Nothing to render any more.
    CHECK_THROWS_AS (apply (project, "renderMidiEffects", { { "clipId", setup.clipId } }), CommandError);
}

TEST_CASE ("rendered chords keep their links", "[midifx][render][links]")
{
    auto setup = projectWithClip();
    auto project = apply (
        setup.project, "addNotes",
        { { "clipId", setup.clipId }, { "notes", list ({ noteArgs (0, beat, 60), noteArgs (beat, beat, 62) }) } });
    const auto& notes = project.tracks[0]->clips[0]->notes;
    project = apply (
        project, "setNoteLinks",
        { { "clipId", setup.clipId },
          { "links",
            list ({ makeObject ({ { "from", notes[0]->id }, { "to", notes[1]->id }, { "glideMs", 40.0 } }) }) } });
    project = apply (project, "insertNode", { { "trackId", setup.trackId }, { "type", nodetype::chord } });
    project = apply (project, "renderMidiEffects", { { "clipId", setup.clipId } });

    const auto& rendered = project.tracks[0]->clips[0]->notes;
    REQUIRE (rendered.size() == 6);
    const auto links = std::ranges::count_if (rendered, [] (const NotePtr& n) { return n->link.has_value(); });
    CHECK (links == 3);

    for (const auto& n : rendered)
        if (n->link.has_value())
            CHECK_THAT (n->link->glideMs, WithinAbs (40.0, 1.0e-9));
}

// ---------------------------------------------------------------------------------------------------------
// Step sequencer (docs/design/step-sequencer.md)

namespace
{
constexpr auto stepTicks16 = ticksPerBeat / 4;

// A sequence of one row (pitch 36) and one pattern with the given steps on.
[[nodiscard]] StepSequence oneRow (std::initializer_list<int> on, int length = 16)
{
    StepSequence steps;
    steps.rows.push_back ({ 36, "Kick" });
    steps = projectfile::normalisedSteps (std::move (steps));
    steps.patterns[0].rows[0].length = length;

    for (const auto step : on)
        steps.patterns[0].rows[0].cells[static_cast<size_t> (step)].on = true;

    return steps;
}

[[nodiscard]] std::vector<stepseq::Hit> hitsOf (const StepSequence& steps, const stepseq::Span& span)
{
    std::vector<stepseq::Hit> hits;
    stepseq::forEachStepHit (steps, span, static_cast<double> (span.start), static_cast<double> (span.end),
                             [&] (const stepseq::Hit& hit) { hits.push_back (hit); });
    return hits;
}

[[nodiscard]] const GraphNode& nodeById (const Project& project, ObjectId nodeId)
{
    const auto& nodes = project.tracks[0]->graph.nodes;
    const auto it = std::ranges::find (nodes, nodeId, &GraphNode::id);
    REQUIRE (it != nodes.end());
    return *it;
}
} // namespace

TEST_CASE ("steps start at the clip's start, or at its start step", "[steps]")
{
    const auto steps = oneRow ({ 0, 4 }, 8);
    const stepseq::Span span { 4 * beat, 8 * beat, 4 * beat, 0, 0, 1 };
    auto hits = hitsOf (steps, span);
    REQUIRE (hits.size() == 4); // steps 0 and 4 of an 8-step row, twice in 4 beats
    CHECK (hits[0].start == 4.0 * beat);
    CHECK (hits[1].start == 4.0 * beat + 4.0 * stepTicks16);
    CHECK (hits[2].start == 6.0 * beat);
    CHECK (hits[0].end == hits[0].start + stepTicks16);
    CHECK (hits[0].pitch == 36);

    // Starting on step 2: step 4 comes two steps after the clip's start.
    auto from2 = span;
    from2.startStep = 2;
    hits = hitsOf (steps, from2);
    REQUIRE_FALSE (hits.empty());
    CHECK (hits[0].start == 4.0 * beat + 2.0 * stepTicks16);

    // A trimmed left edge keeps the steps where they were: the origin stays.
    auto trimmed = span;
    trimmed.start = 4 * beat + stepTicks16;
    hits = hitsOf (steps, trimmed);
    REQUIRE_FALSE (hits.empty());
    CHECK (hits[0].start == 4.0 * beat + 4.0 * stepTicks16);
}

TEST_CASE ("rows of different lengths drift against each other, notes are cut at the clip's end", "[steps]")
{
    auto steps = oneRow ({ 0 }, 3);
    steps.rows.push_back ({ 38, "Snare" });
    steps = projectfile::normalisedSteps (std::move (steps));
    steps.patterns[0].rows[1].length = 4;
    steps.patterns[0].rows[1].cells[0].on = true;
    steps.patterns[0].rows[1].cells[0].length = 16.0f; // ties far past the clip

    const stepseq::Span span { 0, 12 * stepTicks16, 0, 0, 0, 1 };
    std::vector<double> kicks;
    std::vector<double> snares;

    for (const auto& hit : hitsOf (steps, span))
        (hit.pitch == 36 ? kicks : snares).push_back (hit.start / stepTicks16);

    CHECK (kicks == std::vector<double> { 0, 3, 6, 9 });
    CHECK (snares == std::vector<double> { 0, 4, 8 });

    for (const auto& hit : hitsOf (steps, span))
        CHECK (hit.end <= 12.0 * stepTicks16);
}

TEST_CASE ("ratchets split a step, swing moves every second step, chance repeats", "[steps]")
{
    auto steps = oneRow ({ 1 });
    steps.patterns[0].rows[0].cells[1].ratchet = 3;
    steps.patterns[0].swing = 0.75f;
    auto hits = hitsOf (steps, { 0, 16 * stepTicks16, 0, 0, 0, 1 });
    REQUIRE (hits.size() == 3);
    // Step 1 swung to 75 % of the pair: 1.5 sixteenths in; it lasts until step 2, half a stepTicks16.
    CHECK_THAT (hits[0].start, WithinAbs (1.5 * stepTicks16, 1.0e-6));
    CHECK_THAT (hits[1].start - hits[0].start, WithinAbs (0.5 * stepTicks16 / 3.0, 1.0e-6));

    auto chancy = oneRow ({ 0 }, 1);
    chancy.patterns[0].rows[0].cells[0].chance = 0.5f;
    const stepseq::Span span { 0, 400 * stepTicks16, 0, 0, 0, 7 };
    const auto first = hitsOf (chancy, span);
    const auto again = hitsOf (chancy, span);
    CHECK (first.size() == again.size());
    CHECK (first.size() > 150);
    CHECK (first.size() < 250);
    auto other = span;
    other.clipId = 8;
    const auto otherHits = hitsOf (chancy, other);
    const auto sameStarts =
        std::ranges::equal (first, otherHits, [] (const auto& a, const auto& b) { return a.start == b.start; });
    CHECK_FALSE (sameStarts); // another clip, another draw
}

TEST_CASE ("the step sequencer plays inside its clips, passes notes through and leaves nothing hanging",
           "[steps][midifx]")
{
    Rig rig ({ nodetype::stepSequencer }, { note (0, beat, 60) });
    const auto trackId = rig.project.tracks[0]->id;
    rig.setNotes (apply (rig.project, "addClip",
                         { { "trackId", trackId },
                           { "start", beat },
                           { "length", beat },
                           { "sequencer", makeObject ({ { "pattern", 0 } }) } }));

    GraphNode node;
    node.type = nodetype::stepSequencer;
    node.steps = std::make_shared<const StepSequence> (oneRow ({ 0, 2 }));
    rig.effects[0]->configure (node);
    rig.run (3 * 24000, 333);

    const auto ons = rig.of (NoteEventType::noteOn);
    REQUIRE (ons.size() == 3);
    CHECK (ons[0].pitch == 60.0); // the clip's own note, passed through
    CHECK (ons[0].sample == 0);
    CHECK (ons[1].pitch == 36.0);
    CHECK (ons[1].sample == 24000);
    CHECK (ons[2].sample == 24000 + 2 * samplesPerSixteenth);
    CHECK (rig.of (NoteEventType::noteOff).size() == 3);
    CHECK (rig.sounding().empty());

    // Stopping mid-note ends it.
    Rig held ({ nodetype::stepSequencer });
    held.setNotes (apply (held.project, "addClip",
                          { { "trackId", held.project.tracks[0]->id },
                            { "start", 0 },
                            { "length", 4 * beat },
                            { "sequencer", makeObject ({ { "pattern", 0 } }) } }));
    auto longNote = oneRow ({ 0 });
    longNote.patterns[0].rows[0].cells[0].length = 16.0f;
    node.steps = std::make_shared<const StepSequence> (longNote);
    held.effects[0]->configure (node);
    held.run (6000);
    CHECK (held.sounding().size() == 1);
    held.playing = false;
    held.run (512);
    CHECK (held.sounding().empty());

    // While stopped, a preview plays the pattern on its own clock, and stopping the preview ends its notes.
    Rig preview ({ nodetype::stepSequencer });
    preview.playing = false;
    node.steps = std::make_shared<const StepSequence> (oneRow ({ 0, 4 }));
    preview.effects[0]->configure (node);
    preview.effects[0]->setStepPreview (0);
    preview.run (24000 + 100);
    const auto previewed = preview.of (NoteEventType::noteOn);
    REQUIRE (previewed.size() == 2);
    CHECK (previewed[1].sample - previewed[0].sample == 4 * samplesPerSixteenth);
    preview.effects[0]->setStepPreview (-1);
    preview.run (512);
    CHECK (preview.sounding().empty());
}

TEST_CASE ("the step sequencer doesn't allocate while playing", "[steps][realtime]")
{
    Rig rig ({ nodetype::stepSequencer });
    const auto before = allocationViolationCount();
    rig.setNotes (apply (rig.project, "addClip",
                         { { "trackId", rig.project.tracks[0]->id },
                           { "start", 0 },
                           { "length", 4 * beat },
                           { "sequencer", makeObject ({ { "pattern", 0 } }) } }));
    GraphNode node;
    node.type = nodetype::stepSequencer;
    auto steps = oneRow ({ 0, 1, 2, 3, 5, 7 });
    steps.patterns[0].rows[0].cells[1].ratchet = 4;
    node.steps = std::make_shared<const StepSequence> (steps);
    rig.effects[0]->configure (node);
    rig.run (512);
    rig.run (4 * 24000, blockSize, true);
    CHECK_FALSE (rig.of (NoteEventType::noteOn).empty());
    CHECK (allocationViolationCount() == before);
}

TEST_CASE ("a step sequencer added in the graph or pasted has its steps", "[steps][commands]")
{
    auto setup = projectWithClip();
    const auto track = setup.trackId;
    // The graph view adds nodes with addNode: the sequencer still gets its rows and a pattern to show.
    auto project = apply (setup.project, "addNode",
                          { { "trackId", track }, { "type", nodetype::stepSequencer }, { "x", 10 }, { "y", 10 } });
    const auto nodeId = idOf (project, nodetype::stepSequencer);
    REQUIRE (nodeById (project, nodeId).steps != nullptr);
    CHECK (nodeById (project, nodeId).steps->rows.size() == 7);
    CHECK (nodeById (project, nodeId).steps->patterns.size() == 1);
    CHECK_THROWS_AS (apply (project, "addNode", { { "trackId", track }, { "type", nodetype::stepSequencer } }),
                     CommandError);

    // Pasted onto another track, it keeps what it plays.
    project = applyCommand (project, "setStepCell",
                            makeObject ({ { "trackId", track },
                                          { "nodeId", nodeId },
                                          { "pattern", 0 },
                                          { "row", 2 },
                                          { "step", 3 },
                                          { "on", true } }));
    // What the UI's clipboard holds: the node as the model shows it.
    const auto model = toVar (project);
    const auto copied = model["tracks"][juce::Identifier (juce::String (track))]["graph"]["nodes"]
                             [juce::Identifier (juce::String (nodeId))]["steps"];

    REQUIRE (copied.isObject());
    project = apply (project, "addTrack", { { "kind", "instrument" } });
    const auto other = project.tracks.back()->id;
    juce::Array<juce::var> nodes { makeObject ({ { "type", nodetype::stepSequencer }, { "steps", copied } }) };
    project = applyCommand (project, "addNodes", makeObject ({ { "trackId", other }, { "nodes", nodes } }));
    const auto& graph = project.findTrack (other)->graph;
    const auto pasted = std::ranges::find (graph.nodes, juce::String (nodetype::stepSequencer), &GraphNode::type);
    REQUIRE (pasted != graph.nodes.end());
    REQUIRE (pasted->steps != nullptr);
    CHECK (pasted->steps->patterns[0].rows[2].cells[3].on);
}

TEST_CASE ("a step sequencer is inserted first, once per track, and its steps are commands", "[steps][commands]")
{
    auto setup = projectWithClip();
    const auto track = setup.trackId;
    auto project = apply (setup.project, "insertNode", { { "trackId", track }, { "type", nodetype::chord } });
    project = apply (project, "insertNode", { { "trackId", track }, { "type", nodetype::stepSequencer } });
    const auto types = chainTypes (project);
    REQUIRE (types.size() >= 4);
    CHECK (types[1] == nodetype::stepSequencer);
    CHECK (types[2] == nodetype::chord);
    CHECK_THROWS_AS (apply (project, "insertNode", { { "trackId", track }, { "type", nodetype::stepSequencer } }),
                     CommandError);

    const auto nodeId = idOf (project, nodetype::stepSequencer);
    const auto stepsOf = [&] (const Project& p) { return *nodeById (p, nodeId).steps; };
    REQUIRE (stepsOf (project).rows.size() == 7); // a General MIDI kit without a Sampler
    CHECK (stepsOf (project).rows.back().pitch == 36);

    const auto edit =
        [&] (const Project& p, const char* command, std::initializer_list<std::pair<const char*, juce::var>> extra)
    {
        auto object = makeObject ({ { "trackId", track }, { "nodeId", nodeId } });

        for (const auto& [name, value] : extra)
            object.getDynamicObject()->setProperty (name, value);

        return applyCommand (p, command, object);
    };

    project = edit (project, "setStepCell",
                    { { "pattern", 0 }, { "row", 6 }, { "step", 4 }, { "on", true }, { "ratchet", 9 } });
    CHECK (stepsOf (project).patterns[0].rows[6].cells[4].on);
    CHECK (stepsOf (project).patterns[0].rows[6].cells[4].ratchet == 4);
    project = edit (project, "setStepRow", { { "row", 6 }, { "pattern", 0 }, { "length", 12 }, { "pitch", 35 } });
    CHECK (stepsOf (project).patterns[0].rows[6].length == 12);
    CHECK (stepsOf (project).rows[6].pitch == 35);
    project = edit (project, "copyStepPattern", { { "from", 0 }, { "to", 1 } });
    REQUIRE (stepsOf (project).patterns.size() == 2);
    CHECK (stepsOf (project).patterns[1] == stepsOf (project).patterns[0]);
    project = edit (project, "addStepRow", { { "pitch", 60 }, { "name", "C" } });
    CHECK (stepsOf (project).patterns[1].rows.size() == 8);
    project = edit (project, "removeStepRow", { { "row", 7 } });
    CHECK (stepsOf (project).rows.size() == 7);
    project = edit (project, "clearStepPattern", { { "pattern", 1 } });
    CHECK_FALSE (stepsOf (project).patterns[1].rows[6].cells[4].on);
    CHECK (stepsOf (project).patterns[0].rows[6].cells[4].on);

    // A sequencer clip, its pattern and start step, saved and read back.
    project = apply (project, "addClip",
                     { { "trackId", track },
                       { "start", 16 * beat },
                       { "length", 4 * beat },
                       { "sequencer", makeObject ({ { "pattern", 1 }, { "startStep", 2 } }) } });
    const auto clipIt =
        std::ranges::find_if (project.tracks[0]->clips, [] (const ClipPtr& c) { return c->isSequencer(); });
    REQUIRE (clipIt != project.tracks[0]->clips.end());
    const auto clipId = (*clipIt)->id;
    CHECK ((*clipIt)->sequencerPattern == 1);
    CHECK ((*clipIt)->sequencerStartStep == 2);
    project = apply (project, "setClipSequencer", { { "clipId", clipId }, { "pattern", 0 }, { "startStep", 0 } });

    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("dawg-steps-" + juce::Uuid().toString());
    REQUIRE (folder.createDirectory());
    const auto file = folder.getChildFile (juce::String ("Song") + projectfile::extension);
    REQUIRE (projectfile::write (project, { "id", "0.1.0" }, file).isEmpty());
    const auto loaded = projectfile::read (file).project;
    std::ignore = folder.deleteRecursively();
    CHECK (stepsOf (loaded) == stepsOf (project));
    CHECK (std::ranges::any_of (loaded.tracks[0]->clips, [] (const ClipPtr& c) { return c->sequencerPattern == 0; }));

    // Rendering: the clip becomes a clip of the notes it plays: step 4 of a 12-step row, ratcheted four times, in the
    // clip's 16 steps (step 16 would be its end).
    project = apply (project, "renderStepClip", { { "clipId", clipId } });
    const auto renderedIt =
        std::ranges::find (project.tracks[0]->clips, clipId, [] (const ClipPtr& c) { return c->id; });
    REQUIRE (renderedIt != project.tracks[0]->clips.end());
    const auto& rendered = **renderedIt;
    CHECK_FALSE (rendered.isSequencer());
    REQUIRE (rendered.notes.size() == 4);
    CHECK (rendered.notes[0]->start == 4 * stepTicks16);
    CHECK (rendered.notes[0]->pitch == 35.0);
    CHECK (rendered.notes[1]->start == 4 * stepTicks16 + stepTicks16 / 4);
    CHECK (rendered.notes[3]->length == stepTicks16 / 4);
}

TEST_CASE ("a step sequencer on a Sampler track takes a row per pad, named after its sample", "[steps][commands]")
{
    auto setup = projectWithClip();
    auto project = apply (setup.project, "insertNode", { { "trackId", setup.trackId }, { "type", nodetype::sampler } });
    const auto samplerId = idOf (project, nodetype::sampler);
    juce::Array<juce::var> files;

    for (const auto* name : { "Kick 01.wav", "Snare.wav" })
        files.add (makeObject ({ { "path", juce::String ("Audio/") + name },
                                 { "name", name },
                                 { "sampleRate", 48000.0 },
                                 { "channels", 1 },
                                 { "frames", 4800 } }));

    project = applyCommand (
        project, "addSampleZones",
        makeObject ({ { "trackId", setup.trackId }, { "nodeId", samplerId }, { "files", files }, { "key", 36 } }));
    project = apply (project, "insertNode", { { "trackId", setup.trackId }, { "type", nodetype::stepSequencer } });
    const auto& rows = nodeById (project, idOf (project, nodetype::stepSequencer)).steps->rows;
    REQUIRE (rows.size() == 2);
    CHECK (rows[0].pitch == 37);
    CHECK (rows[0].name == "Snare");
    CHECK (rows[1].pitch == 36);
    CHECK (rows[1].name == "Kick 01");
}

TEST_CASE ("while stopped, a held key plays the shown pattern until it is let go", "[steps][midifx][live]")
{
    Rig rig ({ nodetype::stepSequencer });
    rig.playing = false;
    GraphNode node;
    node.type = nodetype::stepSequencer;
    auto steps = oneRow ({ 0, 4 });
    steps.patterns.push_back (steps.patterns[0]);
    steps.patterns[1].rows[0].cells[4].on = false;
    node.steps = std::make_shared<const StepSequence> (steps);
    rig.effects[0]->configure (node);
    rig.effects[0]->setStepAudition (1);

    rig.run (1024);
    CHECK (rig.of (NoteEventType::noteOn).empty());
    rig.press (-5, 60.0);
    rig.run (24000);
    const auto ons = rig.of (NoteEventType::noteOn);
    // The held key itself, then pattern B's one step, from the block after the press.
    REQUIRE (ons.size() == 2);
    CHECK (ons[0].id == -5);
    CHECK (ons[1].pitch == 36.0);
    rig.release (-5);
    rig.run (24000);
    CHECK (rig.of (NoteEventType::noteOn).size() == 2);
    CHECK (rig.sounding().empty());
}
