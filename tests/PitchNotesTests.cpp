// Pitch edits of pool files: the model, its commands and the correction they make (docs/design/pitch-correction.md,
// sections 4.1–4.3 and 5.2).

#include "TestUtils.h"
#include "dawg/PitchAnalysis.h"
#include "dawg/ProjectFile.h"
#include "dawg/Session.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double rate = 48000.0;

[[nodiscard]] Project projectWithFile()
{
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    return apply (project, "addAudioClip",
                  { { "trackId", project.tracks[0]->id },
                    { "start", 0 },
                    { "file", makeObject ({ { "path", "Audio/take.wav" },
                                            { "name", "take" },
                                            { "sampleRate", rate },
                                            { "channels", 1 },
                                            { "frames", 4 * 48000 } }) } });
}

// A detected note as audio.pitchAnalysis gives it: seconds, centre, and shape points [frame, semitones].
[[nodiscard]] juce::var detected (double lead, double start, double end, double centre,
                                  std::initializer_list<std::pair<double, double>> shape = {})
{
    juce::Array<juce::var> points;

    for (const auto& [t, v] : shape)
        points.add (juce::Array<juce::var> { t * rate, v, static_cast<int> (CurveShape::smooth) });

    return makeObject ({ { "leadStart", std::llround (lead * rate) },
                         { "start", std::llround (start * rate) },
                         { "end", std::llround (end * rate) },
                         { "centre", centre },
                         { "shape", points } });
}

[[nodiscard]] Project seeded (std::initializer_list<juce::var> notes)
{
    auto project = projectWithFile();
    return apply (project, "seedPitchNotes",
                  { { "fileId", project.audioFiles[0].id },
                    { "analyser", "1/test" },
                    { "notes", juce::Array<juce::var> (notes) } });
}

// The correction at a time in seconds.
[[nodiscard]] double correctionAt (const FilePitch& pitch, double seconds, const PitchTrack* track = nullptr)
{
    return pitchCorrection (pitch, rate, std::llround (seconds * rate), 1, 1, track)[0];
}
} // namespace

TEST_CASE ("pitch notes are seeded, edited, reset, undone and saved", "[pitch][pitchnotes]")
{
    auto project =
        seeded ({ detected (0.1, 0.2, 1.0, 60.3, { { 0.0, -0.5 }, { 0.8, 0.0 } }), detected (1.0, 1.0, 2.0, 62.0) });
    const auto fileId = project.audioFiles[0].id;
    const auto& pitch = [&]() -> const FilePitch& { return project.audioFiles[0].pitch; };

    REQUIRE (pitch().notes.size() == 2);
    CHECK (pitch().analyser == "1/test");
    CHECK (pitch().notes[0].id == 1);
    CHECK (pitch().notes[0].leadStart == std::llround (0.1 * rate));
    CHECK_THAT (pitch().notes[0].targetCentre, WithinAbs (60.3, 1e-9));
    CHECK (pitch().notes[0].shape.size() == 2);
    CHECK (! pitch().edited());

    // Seeding again needs replace.
    CHECK_THROWS_AS (apply (project, "seedPitchNotes", { { "fileId", fileId }, { "notes", juce::Array<juce::var>() } }),
                     CommandError);

    project = apply (project, "setNotePitch",
                     { { "fileId", fileId }, { "noteIds", juce::Array<juce::var> { 1 } }, { "target", 60.0 } });
    project = apply (
        project, "setNotePitch",
        { { "fileId", fileId }, { "noteIds", juce::Array<juce::var> { 2 } }, { "delta", -1.0 }, { "vibrato", 0.0 } });
    CHECK_THAT (pitch().notes[0].targetCentre, WithinAbs (60.0, 1e-9));
    CHECK_THAT (pitch().notes[1].targetCentre, WithinAbs (61.0, 1e-9));
    CHECK (pitch().notes[1].vibrato == 0.0);
    CHECK (pitch().edited());
    CHECK_THROWS_AS (apply (project, "setNotePitch",
                            { { "fileId", fileId }, { "noteIds", juce::Array<juce::var> { 9 } }, { "delta", 1.0 } }),
                     CommandError);

    // Saved and read back the same.
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("dawg-test-" + juce::Uuid().toString());
    REQUIRE (folder.createDirectory());
    const auto file = folder.getChildFile (juce::String ("Song") + projectfile::extension);
    REQUIRE (projectfile::write (project, { "id", "0" }, file).isEmpty());
    const auto loaded = projectfile::read (file);
    std::ignore = folder.deleteRecursively();
    CHECK (juce::JSON::toString (toVar (loaded.project)) == juce::JSON::toString (toVar (project)));
    CHECK (loaded.project.audioFiles[0].pitch.edited());
    CHECK (loaded.project.audioFiles[0].pitch.notes[1].vibrato == 0.0);

    project = apply (project, "resetPitchNotes", { { "fileId", fileId } });
    CHECK (! pitch().edited());

    // One undo step each, through the session.
    Session session;
    session.reset (seeded ({ detected (0.1, 0.2, 1.0, 60.3) }));
    std::ignore = session.apply (
        "setNotePitch",
        makeObject ({ { "fileId", fileId }, { "noteIds", juce::Array<juce::var> { 1 } }, { "target", 61.0 } }));
    CHECK (session.project()->audioFiles[0].pitch.edited());
    CHECK (session.undo().ok);
    CHECK (! session.project()->audioFiles[0].pitch.edited());
}

TEST_CASE ("pitch correction: the centre's change, ramped at voiced edges, none in lead-ins", "[pitch][pitchnotes]")
{
    auto project = seeded ({ detected (0.1, 0.2, 1.0, 60.3) });
    project = apply (project, "setNotePitch", { { "fileId", project.audioFiles[0].id }, { "target", 61.3 } });
    const auto& pitch = project.audioFiles[0].pitch;

    CHECK (correctionAt (pitch, 0.15) == 0.0);                     // the lead-in
    CHECK_THAT (correctionAt (pitch, 0.2), WithinAbs (0.0, 1e-6)); // the ramp's start
    CHECK_THAT (correctionAt (pitch, 0.205), WithinAbs (0.5, 0.01));
    CHECK_THAT (correctionAt (pitch, 0.6), WithinAbs (1.0, 1e-6));
    CHECK (correctionAt (pitch, 1.1) == 0.0);
}

TEST_CASE ("pitch correction: shape edits, and steps between touching notes eased", "[pitch][pitchnotes]")
{
    auto project = seeded ({ detected (0.0, 0.0, 1.0, 60.0, { { 0.0, -1.0 }, { 0.5, 0.0 }, { 1.0, 0.0 } }),
                             detected (1.0, 1.0, 2.0, 62.0) });
    const auto fileId = project.audioFiles[0].id;
    project = apply (project, "setNotePitch",
                     { { "fileId", fileId }, { "noteIds", juce::Array<juce::var> { 2 } }, { "delta", 2.0 } });

    // Flatten note 1's scoop by hand: its shape becomes 0 throughout.
    auto pitch = project.audioFiles[0].pitch;
    for (auto& point : pitch.notes[0].shape)
        point.value = 0.0;

    CHECK_THAT (correctionAt (pitch, 0.1),
                WithinAbs (-evaluateCurve (pitch.notes[0].detectedShape, 0.1 * rate, 0.0), 1e-6));
    // The detected scoop is rounded, so it passes its last node a little before settling; flattening takes that away.
    const auto detectedAt = [&] (double t) { return evaluateCurve (pitch.notes[0].detectedShape, t * rate, 0.0); };
    CHECK (detectedAt (0.75) > 0.0);
    CHECK (detectedAt (0.75) < 0.15);
    CHECK_THAT (correctionAt (pitch, 0.75), WithinAbs (-detectedAt (0.75), 1e-6));
    CHECK_THAT (correctionAt (pitch, 1.5), WithinAbs (2.0, 1e-6));
    // The join at 1 s eases from 0 to 2 over 20 ms, so it passes halfway there.
    CHECK_THAT (correctionAt (pitch, 1.0), WithinAbs (1.0, 0.01));
    CHECK_THAT (correctionAt (pitch, 0.989), WithinAbs (-detectedAt (0.989), 1e-6));
    CHECK_THAT (correctionAt (pitch, 1.011), WithinAbs (2.0, 1e-6));
}

TEST_CASE ("pitch correction: removing vibrato cancels it, keeping the centre", "[pitch][pitchnotes]")
{
    // A note at 60 with 6 Hz vibrato of 0.3 semitones.
    PitchTrack track;
    track.sampleRate = rate;
    track.startSeconds = 0.005;

    for (auto t = track.startSeconds; t < 2.0; t += track.hopSeconds)
    {
        track.pitch.push_back (
            static_cast<float> (60.0 + 0.3 * std::sin (juce::MathConstants<double>::twoPi * 6.0 * t)));
        track.voicedProb.push_back (0.9f);
        track.levelDb.push_back (-10.0f);
    }

    auto project = seeded ({ detected (0.0, 0.0, 2.0, 60.0) });
    project = apply (project, "setNotePitch", { { "fileId", project.audioFiles[0].id }, { "vibrato", 0.0 } });
    const auto& pitch = project.audioFiles[0].pitch;

    // Inside the note (away from the edges the averaging shortens), sung plus correction stays near 60.
    auto worst = 0.0;

    for (auto t = 0.4; t < 1.6; t += 0.01)
    {
        const auto sung = 60.0 + 0.3 * std::sin (juce::MathConstants<double>::twoPi * 6.0 * t);
        worst = std::max (worst, std::abs (sung + correctionAt (pitch, t, &track) - 60.0));
    }

    CHECK (worst < 0.1);                      // from 0.3
    CHECK (correctionAt (pitch, 1.0) == 0.0); // without the track, vibrato stays
}

TEST_CASE ("pitch phrases: edited notes with their close neighbours", "[pitch][pitchnotes]")
{
    // Notes at 0–0.5, 0.55–1 (50 ms gap: chained), 1.5–2 (alone), 2.5–3 (alone, untouched).
    auto project = seeded ({ detected (0.0, 0.0, 0.5, 60.0), detected (0.55, 0.55, 1.0, 62.0),
                             detected (1.5, 1.5, 2.0, 64.0), detected (2.5, 2.5, 3.0, 65.0) });
    const auto fileId = project.audioFiles[0].id;
    CHECK (pitchPhrases (project.audioFiles[0].pitch, rate, nullptr).empty());

    project = apply (project, "setNotePitch",
                     { { "fileId", fileId }, { "noteIds", juce::Array<juce::var> { 2, 3 } }, { "delta", 0.5 } });
    const auto phrases = pitchPhrases (project.audioFiles[0].pitch, rate, nullptr);
    REQUIRE (phrases.size() == 2);
    CHECK (phrases[0].start == 0);
    CHECK (phrases[0].end == std::llround (1.0 * rate));
    CHECK (phrases[1].start == std::llround (1.5 * rate));
    CHECK (phrases[1].end == std::llround (2.0 * rate));

    // A change under half a cent plays the file.
    project = apply (project, "resetPitchNotes", { { "fileId", fileId } });
    project = apply (project, "setNotePitch",
                     { { "fileId", fileId }, { "noteIds", juce::Array<juce::var> { 4 } }, { "delta", 0.004 } });
    CHECK (project.audioFiles[0].pitch.edited());
    CHECK (pitchPhrases (project.audioFiles[0].pitch, rate, nullptr).empty());
}

// The same table as ui/src/audioeditor/pitchCurve.test.ts, which tests the TypeScript twin (section 4.3).
TEST_CASE ("pitch correction table shared with the UI's twin", "[pitch][pitchnotes]")
{
    // At 48 kHz: A from 0 to 1 s, centre 60 moved to 61; B touching it from 1 s to 2 s, untouched at 62.
    FilePitch pitch;
    PitchNote a;
    a.id = 1;
    a.start = 0;
    a.end = 48000;
    a.detectedCentre = 60.0;
    a.targetCentre = 61.0;
    PitchNote b;
    b.id = 2;
    b.leadStart = b.start = 48000;
    b.end = 96000;
    b.detectedCentre = b.targetCentre = 62.0;
    pitch.notes = { a, b };

    const std::vector<std::pair<juce::int64, double>> table {
        { 0, 0.0 }, { 240, 0.5 }, { 24000, 1.0 }, { 47520, 1.0 }, { 48000, 0.5 }, { 48240, 0.1464466 }, { 60000, 0.0 },
    };

    for (const auto& [frame, expected] : table)
    {
        INFO ("frame " << frame);
        CHECK_THAT (pitchCorrection (pitch, rate, frame, 1, 1, nullptr)[0], WithinAbs (expected, 1e-6));
    }
}

TEST_CASE ("pitch notes split and merge without moving the pitch", "[pitch][pitchnotes]")
{
    // One note from 0.2 to 1.2 s at 60.3 with a rising shape, moved up a semitone.
    auto project = seeded ({ detected (0.1, 0.2, 1.2, 60.3, { { 0.0, -0.4 }, { 0.5, 0.0 }, { 1.0, 0.2 } }) });
    const auto fileId = project.audioFiles[0].id;
    project = apply (project, "setNotePitch", { { "fileId", fileId }, { "delta", 1.0 } });
    const auto before = project.audioFiles[0].pitch;
    const auto sounds = [] (const FilePitch& pitch, double seconds)
    {
        const auto frame = seconds * rate;
        for (const auto& n : pitch.notes)
            if (frame >= static_cast<double> (n.start) && frame < static_cast<double> (n.end))
                return std::pair {
                    n.detectedCentre + evaluateCurve (n.detectedShape, frame - static_cast<double> (n.start), 0.0),
                    n.targetCentre + evaluateCurve (n.shape, frame - static_cast<double> (n.start), 0.0)
                };
        return std::pair { 0.0, 0.0 };
    };

    // Split at 0.7 s with measured centres: the sung and the edited pitch stay where they were.
    project = apply (project, "splitPitchNote",
                     { { "fileId", fileId },
                       { "noteId", 1 },
                       { "frame", 0.7 * rate },
                       { "centres", juce::Array<juce::var> { 60.1, 60.45 } } });
    const auto& split = project.audioFiles[0].pitch;
    REQUIRE (split.notes.size() == 2);
    CHECK (split.notes[0].end == split.notes[1].start);
    CHECK (split.notes[1].leadStart == split.notes[1].start);
    CHECK (split.notes[1].id == 2);
    CHECK_THAT (split.notes[1].targetCentre, WithinAbs (61.45, 1e-9));

    for (const auto t : { 0.3, 0.6, 0.75, 1.1 })
    {
        INFO ("at " << t);
        // Cutting a rounded curve changes its slopes near the cut (each half is level at its ends), so the pitches
        // move by a few cents; what is heard, the correction (edited minus sung), stays exactly as it was.
        CHECK_THAT (sounds (split, t).first, WithinAbs (sounds (before, t).first, 0.05));
        CHECK_THAT (sounds (split, t).second - sounds (split, t).first,
                    WithinAbs (sounds (before, t).second - sounds (before, t).first, 1e-9));
    }

    // Each half's shapes span it, end nodes on its edges.
    for (const auto& n : split.notes)
        for (const auto* curve : { &n.shape, &n.detectedShape })
        {
            CHECK (curve->front().time == 0);
            CHECK (curve->back().time == n.end - n.start);
        }

    CHECK_THROWS_AS (
        apply (project, "splitPitchNote", { { "fileId", fileId }, { "noteId", 1 }, { "frame", 0.1 * rate } }),
        CommandError);

    // Merged back: the pitch still where it was.
    project =
        apply (project, "mergePitchNotes", { { "fileId", fileId }, { "noteIds", juce::Array<juce::var> { 1, 2 } } });
    const auto& merged = project.audioFiles[0].pitch;
    REQUIRE (merged.notes.size() == 1);
    CHECK (merged.notes[0].end == std::llround (1.2 * rate));

    for (const auto t : { 0.3, 0.6, 0.75, 1.1 })
    {
        INFO ("at " << t);
        CHECK_THAT (sounds (merged, t).first, WithinAbs (sounds (before, t).first, 0.03));
        CHECK_THAT (sounds (merged, t).second - sounds (merged, t).first,
                    WithinAbs (sounds (before, t).second - sounds (before, t).first, 1e-9));
    }
}

TEST_CASE ("pitch notes merge only neighbours, smooth across a gap", "[pitch][pitchnotes]")
{
    auto project =
        seeded ({ detected (0.0, 0.0, 0.5, 60.0), detected (0.5, 0.6, 1.0, 62.0, { { 0.0, 0.0 }, { 0.4, 0.5 } }),
                  detected (1.0, 1.0, 1.5, 64.0) });
    const auto fileId = project.audioFiles[0].id;
    CHECK_THROWS_AS (
        apply (project, "mergePitchNotes", { { "fileId", fileId }, { "noteIds", juce::Array<juce::var> { 1, 3 } } }),
        CommandError);
    CHECK_THROWS_AS (
        apply (project, "mergePitchNotes", { { "fileId", fileId }, { "noteIds", juce::Array<juce::var> { 1 } } }),
        CommandError);

    project = apply (project, "mergePitchNotes",
                     { { "fileId", fileId }, { "noteIds", juce::Array<juce::var> { 1, 2 } }, { "centre", 61.0 } });
    const auto& note = project.audioFiles[0].pitch.notes[0];
    CHECK (project.audioFiles[0].pitch.notes.size() == 2);
    CHECK_THAT (note.detectedCentre, WithinAbs (61.0, 1e-9));
    // The second note's shape keeps its pitch (62 + its shape), re-referenced to 61.
    CHECK_THAT (61.0 + evaluateCurve (note.detectedShape, 0.8 * rate, 0.0), WithinAbs (62.25, 0.05));
}

TEST_CASE ("note shapes are set and shaped by presets", "[pitch][pitchnotes]")
{
    // A scoop up into the note, a held middle, a fall at the end.
    auto project =
        seeded ({ detected (0.0, 0.0, 1.0, 60.0, { { 0.0, -0.8 }, { 0.2, 0.1 }, { 0.6, -0.1 }, { 1.0, -0.6 } }) });
    const auto fileId = project.audioFiles[0].id;
    const auto shape = [&] { return project.audioFiles[0].pitch.notes[0].shape; };
    // A node's pitch as it plays: the centre plus its value (presets recentre the note, which moves both).
    const auto heard = [&] (size_t i) { return project.audioFiles[0].pitch.notes[0].targetCentre + shape()[i].value; };

    project = apply (project, "shapeNotes", { { "fileId", fileId }, { "op", "flattenOnset" } });
    CHECK_THAT (heard (0), WithinAbs (60.1, 1e-9));
    CHECK_THAT (heard (1), WithinAbs (60.1, 1e-9));
    project = apply (project, "shapeNotes", { { "fileId", fileId }, { "op", "flattenRelease" } });
    CHECK_THAT (heard (3), WithinAbs (59.9, 1e-9));
    CHECK_THAT (heard (2), WithinAbs (59.9, 1e-9));
    CHECK (project.audioFiles[0].pitch.edited());

    // Set directly, with pivots; pivots must lie inside the note.
    project = apply (project, "setNoteShape",
                     { { "fileId", fileId },
                       { "noteId", 1 },
                       { "shape", juce::Array<juce::var> { juce::Array<juce::var> { 0, 0.0, 1 },
                                                           juce::Array<juce::var> { 48000, 1.0, 0 } } },
                       { "pivots", juce::Array<juce::var> { 24000, 12000, 24000 } } });
    CHECK (shape().size() == 2);
    CHECK (
        std::ranges::all_of (shape(), [] (const CurvePoint& p) { return p.shape == CurveShape::rounded; })); // no steps
    CHECK (shape().front().time == 0);
    CHECK (shape().back().time == 48000); // the end nodes sit on the note's edges
    CHECK (project.audioFiles[0].pitch.notes[0].pivots == std::vector<juce::int64> { 12000, 24000 });
    CHECK_THROWS_AS (apply (project, "setNoteShape",
                            { { "fileId", fileId },
                              { "noteId", 1 },
                              { "shape", juce::Array<juce::var>() },
                              { "pivots", juce::Array<juce::var> { 0 } } }),
                     CommandError);
    CHECK_THROWS_AS (apply (project, "setNoteShape",
                            { { "fileId", fileId },
                              { "noteId", 1 },
                              { "shape", juce::Array<juce::var> { juce::Array<juce::var> { 60000, 0.0 } } } }),
                     CommandError); // outside the note

    project = apply (project, "resetPitchNotes", { { "fileId", fileId } });
    CHECK (shape().size() == 4);
    CHECK (project.audioFiles[0].pitch.notes[0].pivots.empty());
}

TEST_CASE ("a note's centre follows its shape's edits", "[pitch][pitchnotes]")
{
    // A level note at 60 with a rounded shape of four nodes.
    auto project =
        seeded ({ detected (0.0, 0.0, 1.0, 60.0, { { 0.0, 0.0 }, { 0.3, 0.0 }, { 0.7, 0.0 }, { 1.0, 0.0 } }) });
    const auto fileId = project.audioFiles[0].id;
    const auto note = [&] { return project.audioFiles[0].pitch.notes[0]; };
    const auto setShape = [&] (double centre, std::initializer_list<std::pair<double, double>> nodes)
    {
        juce::Array<juce::var> points;

        for (const auto& [time, value] : nodes)
            points.add (juce::Array<juce::var> { time * rate, value, static_cast<int> (CurveShape::smooth) });

        project = apply (project, "setNoteShape",
                         { { "fileId", fileId }, { "noteId", 1 }, { "centre", centre }, { "shape", points } });
    };
    const auto before = project.audioFiles[0].pitch;

    // The same shape against the same centre: nothing moves, the note stays as sung.
    setShape (60.0, { { 0.0, 0.0 }, { 0.3, 0.0 }, { 0.7, 0.0 }, { 1.0, 0.0 } });
    CHECK_THAT (note().targetCentre, WithinAbs (60.0, 1e-9));
    CHECK_FALSE (project.audioFiles[0].pitch.edited());

    // The whole shape a semitone up: the centre goes up, the shape back to level, and what plays is the same.
    setShape (60.0, { { 0.0, 1.0 }, { 0.3, 1.0 }, { 0.7, 1.0 }, { 1.0, 1.0 } });
    CHECK_THAT (note().targetCentre, WithinAbs (61.0, 1e-9));
    for (const auto& point : note().shape)
        CHECK_THAT (point.value, WithinAbs (0.0, 1e-9));

    // A drag's later steps send the shape against the centre it began with: each step means the same.
    setShape (60.0, { { 0.0, 1.0 }, { 0.3, 1.0 }, { 0.7, 1.0 }, { 1.0, 1.0 } });
    CHECK_THAT (note().targetCentre, WithinAbs (61.0, 1e-9));

    // One raised end shifts the centre a little, and the nodes keep their pitch.
    setShape (60.0, { { 0.0, 0.0 }, { 0.3, 0.0 }, { 0.7, 0.0 }, { 1.0, 2.0 } });
    CHECK (note().targetCentre > 60.0);
    CHECK (note().targetCentre < 60.5);
    CHECK_THAT (note().targetCentre + note().shape[3].value, WithinAbs (62.0, 1e-9));
    CHECK_THAT (note().targetCentre + note().shape[1].value, WithinAbs (60.0, 1e-9));

    // Back to the sung shape: the centre comes back with it.
    setShape (60.0, { { 0.0, 0.0 }, { 0.3, 0.0 }, { 0.7, 0.0 }, { 1.0, 0.0 } });
    CHECK (project.audioFiles[0].pitch.soundsLike (before));
}

TEST_CASE ("notes snap to semitones or the key, by an amount", "[pitch][pitchnotes]")
{
    auto project = seeded ({ detected (0.0, 0.0, 0.5, 61.2), detected (0.5, 0.5, 1.0, 63.6) });
    const auto fileId = project.audioFiles[0].id;
    const auto centre = [&] (size_t i) { return project.audioFiles[0].pitch.notes[i].targetCentre; };

    auto half = apply (project, "snapNotePitch", { { "fileId", fileId }, { "mode", "semitone" }, { "amount", 0.5 } });
    CHECK_THAT (half.audioFiles[0].pitch.notes[0].targetCentre, WithinAbs (61.1, 1e-9));
    CHECK_THAT (half.audioFiles[0].pitch.notes[1].targetCentre, WithinAbs (63.8, 1e-9));

    // C major: 61.2 lies between 60 and 62 and goes to 62 (nearer); 63.6 between 62 and 64 goes to 64.
    CHECK_THROWS_AS (apply (project, "snapNotePitch", { { "fileId", fileId }, { "mode", "key" } }),
                     CommandError); // no key
    project = apply (project, "setKey", { { "enabled", true }, { "root", 0 }, { "scale", 0 } });
    project = apply (project, "snapNotePitch", { { "fileId", fileId }, { "mode", "key" } });
    CHECK_THAT (centre (0), WithinAbs (62.0, 1e-9));
    CHECK_THAT (centre (1), WithinAbs (64.0, 1e-9));
}
