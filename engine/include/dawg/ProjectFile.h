#pragma once

#include "dawg/Model.h"

#include <functional>
#include <map>

namespace dawg
{

/**
    Project files. See docs/design/phase5-project-workflow.md.

    A project file (".dawg") is a zip holding project.json (the model in its UI JSON form plus metadata) and one
    binary entry per plugin state (plugins/{trackId}-{nodeId}.bin). Files are versioned; versions from
    oldestVersion on are migrated as JSON before parsing, and older ones are refused.
*/
namespace projectfile
{
/**
    Identifies the format in project.json's "format" field.
*/
inline constexpr const char* formatName = "dawg-project";

/**
    The format version written by write().
*/
inline constexpr int version = 17;

/**
    The oldest format version read() opens; older files are refused.
*/
inline constexpr int oldestVersion = 5;

/**
    The file name extension, including the dot.
*/
inline constexpr const char* extension = ".dawg";

/**
    How many previous versions write() keeps in the Backups folder next to the file.
*/
inline constexpr int backupsKept = 5;

/**
    Data stored with a project that is not part of the model.
*/
struct Metadata
{
    /**
        The project's UUID (F7). Empty in files from before version 1; readers then assign one.
    */
    juce::String id;

    /**
        The DAWg version that wrote the file.
    */
    juce::String appVersion;
};

/**
    A project read from a file.
*/
struct Loaded
{
    /**
        The model.
    */
    Project project;

    /**
        The file's metadata.
    */
    Metadata metadata;
};

/**
    A few facts about a project for lists (cached in the recent-projects list).
*/
struct Summary
{
    /**
        The starting tempo in BPM.
    */
    double tempo = 120.0;

    /**
        Number of tracks.
    */
    int tracks = 0;

    /**
        End of the last clip, in seconds through the tempo map.
    */
    double lengthSeconds = 0.0;
};

/**
    Looks up a plugin state entry by its path in the zip, e.g. "plugins/3-7.bin". Returns null if missing.
*/
using StateSource = std::function<std::shared_ptr<const juce::MemoryBlock> (const juce::String& path)>;

/**
    Serialises a project to project.json's form and collects the plugin states to store next to it.

    @param project   The project.
    @param metadata  Its metadata.
    @param states    Receives the plugin states by entry path (nodes refer to them as "stateFile").
    @return          { format, version, id, appVersion, nextId, project }
*/
[[nodiscard]] juce::var toVar (const Project& project, const Metadata& metadata,
                               std::map<juce::String, std::shared_ptr<const juce::MemoryBlock>>& states);

/**
    Brings a project.json up to the current version.

    @param json  The parsed file.
    @return      The migrated JSON.
    @throws CommandError  If it is not a DAWg project, or comes from a newer version or one before oldestVersion.
*/
[[nodiscard]] juce::var migrate (const juce::var& json);

/**
    Parses a current-version project.json.

    @param json    The migrated JSON.
    @param states  Resolves "stateFile" references. Version 0's inline base64 "state" is read directly.
    @return        The project and its metadata.
    @throws CommandError  If the data is not a valid project.
*/
[[nodiscard]] Loaded fromVar (const juce::var& json, const StateSource& states);

/**
    Reads a Voice Instrument's voice graph in the file's form, checked as strictly as in a file (pasting a Voice
    Instrument; docs/design/phase12-voice-scope.md, V16). Its ids are the source's; the caller gives new ones.

    @param json  The `voice` object: id-keyed nodes and connections.
    @return      The voice graph.
    @throws CommandError if it is malformed.
*/
[[nodiscard]] std::shared_ptr<const TrackGraph> voiceGraphFromVar (const juce::var& json);

/**
    Reads a modulation slot's macro knob (docs/design/macros-and-modulation-column.md, MC7) from its JSON form.

    @param fields  The slot's fields.
    @return        0..maxMacros - 1, or -1 for none.
    @throws        CommandError if it is out of range.
*/
[[nodiscard]] int macroOf (const juce::var& fields);

/**
    Reads a Macros device's knob names (MC5) from a node's JSON form.

    @param node  The node's fields.
    @return      The names, at most maxMacros.
*/
[[nodiscard]] std::vector<juce::String> macroNamesOf (const juce::var& node);

/**
    Parses what's in a component from its JSON form, with the checks a file gets (docs/design/component-nodes.md, N3).

    @param json  The component's "component" field.
    @return      The graph and ports inside.
    @throws      CommandError if it isn't valid.
*/
[[nodiscard]] std::shared_ptr<const ComponentGraph> componentFromVar (const juce::var& json);

/**
    What a component saved for reuse holds (docs/design/component-nodes.md, N9): its JSON form, with its plugins' state inline, which
    componentFromVar() reads back.

    @param node  A component node.
    @return      The JSON.
*/
[[nodiscard]] juce::var savedComponentVar (const GraphNode& node);

/**
    Applies the fields a var holds to a sample zone, each kept in its range. Its id and file are the caller's.

    @param fields  The fields to change; others stay.
    @param zone    The zone.
    @throws CommandError if a field is not a number.
*/
void applySampleZoneFields (const juce::var& fields, SampleZone& zone);

/**
    Reads a Sampler's zones.

    @param value  The `zones` array, or anything else for none.
    @return       The zones.
    @throws CommandError if one is malformed.
*/
[[nodiscard]] std::vector<SampleZone> sampleZonesFromVar (const juce::var& value);

/**
    Reads a Drum Synth's pads (docs/design/drum-synth.md, DR15), outputs and choke groups clamped to their ranges.

    @param value  The `pads` array of drumPadCount pads, or anything else for none (the default kit).
    @return       The pads, or empty.
    @throws CommandError if the array has the wrong length or a pad an unknown model.
*/
[[nodiscard]] std::vector<DrumPad> drumPadsFromVar (const juce::var& value);

/**
    Reads a step sequencer's rows and patterns (docs/design/step-sequencer.md), each pattern made to hold one row of
    StepSequence::maxSteps cells per row, and values clamped to their ranges.

    @param value  The `steps` object.
    @return       The rows and patterns; at least one pattern.
*/
[[nodiscard]] std::shared_ptr<const StepSequence> stepsFromVar (const juce::var& value);

/**
    Makes a step sequence whole: one pattern at least, each with a row per sequence row and every row maxSteps
    cells long, values in range.
*/
[[nodiscard]] StepSequence normalisedSteps (StepSequence steps);

/**
    Writes a project file safely: to a temporary file next to the target first, then the previous version
    (if any) moves to Backups/ (keeping backupsKept), then the temporary file replaces the target (F2, F3).

    @param project   The project.
    @param metadata  Its metadata.
    @param target    The .dawg file to write. Its folder must exist.
    @param backups   Whether to keep the previous version in Backups/ (false for autosaves).
    @return          An empty string on success, otherwise what went wrong.
*/
[[nodiscard]] juce::String write (const Project& project, const Metadata& metadata, const juce::File& target,
                                  bool backups = true);

/**
    Reads a project file.

    @param file  The file.
    @return      The project and its metadata (with a new id if the file had none).
    @throws CommandError  If the file cannot be read or is not a valid project.
*/
[[nodiscard]] Loaded read (const juce::File& file);

/**
    Summarises a project for lists.

    @param project  The project.
    @return         Its tempo, track count and length.
*/
[[nodiscard]] Summary summarise (const Project& project);
} // namespace projectfile

} // namespace dawg
