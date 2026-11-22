#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <optional>
#include <vector>

namespace dawg
{

/**
    What a sample's file name says about it (docs/design/sampler-mapping.md, SM5).
*/
struct SampleName
{
    /**
        The named note as a MIDI key, counting C4 as 60, or -1.
    */
    int key = -1;

    /**
        By how much a MIDI number beside the note says to move the key (12 where the name counts C3 as 60), or 0.
    */
    int octaveHint = 0;

    /**
        A loudness from a dynamic marking or a named velocity, 1..127, or -1.
    */
    int velocity = -1;

    /**
        A named velocity range's lowest velocity, or -1.
    */
    int lowVelocity = -1;

    /**
        Its highest velocity, or -1.
    */
    int highVelocity = -1;

    /**
        The side a file of a stereo pair holds: -1 for left (L, Left), 1 for right (R, Right), 0 for neither (TL4).
    */
    int side = 0;

    /**
        The microphone a file was recorded with (a token ending in Mic, or OH, OHL, OHR, Room or Close), or empty.
    */
    juce::String microphone;

    /**
        Whether the name numbers a take (a number with a leading zero, as 01 or 001).
    */
    bool take = false;

    /**
        The name without its note, loudness, take, microphone and side: what a drum kit's files of one piece share
        (TL5).
    */
    juce::String piece;
};

/**
    Reads a note, a loudness or a velocity range from a sample's file name (SM5).

    @param name  The file name, without its extension.
    @return      What it names.
*/
[[nodiscard]] SampleName parseSampleName (const juce::String& name);

/**
    One sound in a sample file: a slot of a file holding several notes, or the whole file (SM3, SM4).
*/
struct SampleSlice
{
    /**
        Its first frame; 0, with an end of 0, for the whole file.
    */
    juce::int64 start = 0;

    /**
        The frame after its last; 0 for the whole file.
    */
    juce::int64 end = 0;

    /**
        Its pitch in MIDI semitones (fractional), or 0 if none was found.
    */
    double pitch = 0.0;
};

/**
    Cuts a file into equal slots where every slot starts with a sound, and finds each one's pitch; a file without such
    slots is one slice (SM3, SM4).

    @param audio       The file's audio.
    @param sampleRate  Its sample rate.
    @return            The slices, in order.
*/
[[nodiscard]] std::vector<SampleSlice> analyseSample (const juce::AudioBuffer<float>& audio, double sampleRate);

/**
    The pitch of a sound: YIN over a second of it, from 50 ms after its start (SM4); a zone's Detect pitch.

    @param audio       The audio.
    @param sampleRate  Its sample rate.
    @param start       The sound's first frame.
    @param end         The frame after its last.
    @return            Its pitch in MIDI semitones (fractional), or 0 if none was found.
*/
[[nodiscard]] double soundPitch (const juce::AudioBuffer<float>& audio, double sampleRate, juce::int64 start,
                                 juce::int64 end);

/**
    A file to map: its name and its slices.
*/
struct SampleToMap
{
    /// The file's name, without its extension.
    juce::String name;
    /// Its sounds, from analyseSample().
    std::vector<SampleSlice> slices;
    /// The root key the file stores (a WAV file's smpl chunk), or -1: exact, before anything its name says (SM5a).
    int storedKey = -1;
    /// The title the file stores (a WAV file's INAM), which may name its note.
    juce::String title;
    /// The loop the file stores (a WAV file's smpl chunk), start inclusive, end exclusive; -1 for none.
    juce::int64 loopStart = -1;
    /// The end of that loop.
    juce::int64 loopEnd = -1;
};

/**
    A zone the mapping makes: its file, its frames (both 0 for the whole file), its root key and its ranges.
*/
struct PlannedZone
{
    /// The file, by its index among the files mapped.
    size_t file = 0;
    /// Its first frame (0, with an end of 0, for the whole file).
    juce::int64 start = 0;
    /// The frame after its last (0 for the whole file).
    juce::int64 end = 0;
    /// The key it plays at its own pitch.
    int rootKey = 60;
    /// The lowest key it plays.
    int lowKey = 0;
    /// The highest key it plays.
    int highKey = 127;
    /// The lowest velocity it plays.
    int lowVelocity = 0;
    /// The highest velocity it plays.
    int highVelocity = 127;
    /// Its layer, 1 to 8 (TL4).
    int layer = 1;
    /// Its pan, -1 to 1.
    double pan = 0.0;
    /// The loop its file stores, or -1 for both: the zone loops between them.
    juce::int64 loopStart = -1;
    /// The end of that loop.
    juce::int64 loopEnd = -1;

    /// Whether two zones are the same in every field.
    [[nodiscard]] bool operator== (const PlannedZone&) const = default;
};

/**
    Maps files to keys by their notes, named or found, spreading each across the keys to the next and its loudness layers
    across the velocities (SM5 to SM8); or, for drums, a key to each piece from a kick's (TL5). Microphones and sides
    go in layers of their own (TL4).

    @param files  The files, with their slices.
    @return       The zones, or nothing for a single file with neither a note nor a pitch, which covers the keyboard.
*/
[[nodiscard]] std::optional<std::vector<PlannedZone>> planSampleZones (const std::vector<SampleToMap>& files);

} // namespace dawg
