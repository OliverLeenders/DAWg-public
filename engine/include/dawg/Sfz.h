#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <vector>

namespace dawg
{

/**
    A region of an SFZ patch, as a Sampler zone (docs/design/sampler-mapping.md, SF3 to SF5).
*/
struct SfzRegion
{
    /**
        The sample it plays.
    */
    juce::File sample;

    /**
        Its zone's fields, as setSampleZone takes them (keys, velocities, frames, tune, level, pan, mode, loop, envelope,
        choke group and layer).
    */
    juce::var fields;
};

/**
    What an SFZ patch maps.
*/
struct SfzPatch
{
    /**
        The regions to make zones of, in the order the zones go in.
    */
    std::vector<SfzRegion> regions;

    /**
        Whether its takes alternate at random (lorand and hirand) rather than in turn (SF5).
    */
    bool randomTakes = false;

    /**
        How much velocity changes the level, in percent (amp_veltrack), or -1 where the patch doesn't say (SF7).
    */
    double velocityTracking = -1.0;

    /**
        The opcodes it uses that a Sampler zone has nothing for (SF6).
    */
    juce::StringArray ignored;

    /**
        Regions left out: release triggers, and samples that aren't there (SF6).
    */
    int skipped = 0;
};

/**
    Reads an SFZ patch (SF2).

    @param text         The patch's text.
    @param folder       The folder it is in, which its sample paths and includes are relative to.
    @param readInclude  Returns the text of a file an include line names (empty if it can't be read).
    @param exists       Whether a sample file is there; regions whose sample isn't are skipped.
    @return             The patch.
*/
[[nodiscard]] SfzPatch readSfz (const juce::String& text, const juce::File& folder,
                                const std::function<juce::String (const juce::File&)>& readInclude,
                                const std::function<bool (const juce::File&)>& exists);

/**
    Reads an SFZ patch from its file, includes and samples from the disk.

    @param file  The .sfz file.
    @return      The patch.
*/
[[nodiscard]] SfzPatch readSfzFile (const juce::File& file);

} // namespace dawg
