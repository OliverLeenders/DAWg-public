#pragma once

#include "dawg/Model.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace dawg
{

/**
    Reads a standard MIDI file for the importMidi command (docs/design/sampler-midi-delay-limiter.md, section 3). Each MIDI
    track or channel with notes becomes one part; an MPE file's track stays one part, its channels being per note. Pitch
    bend and pressure become note curves, as when recording.

    @param data      The file's bytes.
    @param fileName  The file's name, without its extension: parts without a name of their own are called after it.
    @return          { name, tracks: [{ name, drums, length, notes, controllers }], tempo: [{ tick, bpm }],
                       signatures: [{ bar, numerator, denominator }] }
    @throws CommandError  If it isn't a MIDI file, or is timed in SMPTE frames rather than beats.
*/
[[nodiscard]] juce::var parseMidiFile (const juce::MemoryBlock& data, const juce::String& fileName);

/**
    Writes a project's instrument tracks as a type 1 MIDI file, with the tempo map on a track of its own. A track is plain
    MIDI unless a note has curves or a fractional pitch; then it is MPE, a member channel per note, so nothing is lost
    (docs/design/sampler-midi-delay-limiter.md, section 3).

    @param project  The project.
    @return         The file's bytes.
*/
[[nodiscard]] juce::MemoryBlock writeMidiFile (const Project& project);

} // namespace dawg
