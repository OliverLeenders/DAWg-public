#pragma once

#include "dawg/Model.h"

#include <stdexcept>

namespace dawg
{

/**
    Thrown by a command when it is unknown or its arguments are invalid.

    The message is shown to the user.
*/
class CommandError final : public std::runtime_error
{
public:
    /**
        Creates an error with a user-facing message.

        @param message  What went wrong, phrased for the user.
    */
    explicit CommandError (const juce::String& message);
};

/**
    Applies a named command to a project and returns the new project.

    Commands are pure functions of (project, args), which keeps them easy to test, replay and undo.
    Out-of-range numbers are clamped where a sensible value exists and rejected otherwise. Positions and
    lengths are in ticks (see Time.h).

    Project:
    - setTempo         { bpm, at? }                    (the tempo in force at tick at (default 0): the last tempo
                                                        point at or before it; docs/design/tempo-map.md, TM11)
    - addTempoPoint    { tick, bpm, ramp? }            (replaces a point on the same tick)
    - setTempoPoint    { index, tick?, bpm?, ramp? }   (stays between its neighbours; the first stays at tick 0)
    - removeTempoPoint { index }                       (not the first)
    - setSignature     { bar, numerator, denominator } (0-based bar; replaces a signature there; denominator 2, 4,
                                                        8 or 16)
    - moveSignature    { from, to }                    (bars; not the one on bar 0)
    - removeSignature  { bar }                         (not bar 0)

    Every command keeps audio clips' real-time length when the tempo map changes or they move (TM8).
    - setMetronome     { enabled }
    - setKey           { bar? (default 0), enabled?, root? (0..11), scale? (index into dawg::scales) } (edits the key
                       change on the bar, or adds one from the key in effect)
    - moveKey          { from, to }                    (bars; not the one on bar 0)
    - removeKey        { bar }                         (not bar 0)

    Tracks (groups: docs/design/groups-and-sends.md):
    - addTrack         { name?, kind?, parent? }       (kind "instrument" (default), "audio", "group" or "return";
                                                        creates the default graph, with an EQ and a bypassed compressor
                                                        before the gain on all but the master (channel-strip.md, C2);
                                                        parent: a group to add it to, at its
                                                        end; returns go after every other track; a return with
                                                        sendFrom also gets a send from that track)
    - setTrackSwitches { id, mute?, solo?, monitor?, inputChannel?, inputStereo? }
    - removeTrack      { id }                          (a group goes with everything inside it; sends to a removed
                                                        return go too)
    - groupTracks      { ids, name? }                  (a new group where the first track is, holding the tracks)
    - ungroup          { id }                          (the members move up a level; the group goes)
    - moveTracks       { ids, parent, before? }        (into a group or the top level (0), before one of its
                                                        tracks or at its end)
    - setTrackFolded   { id, folded }                  (groups)
    - addSend          { trackId, target, targetNode?, levelDb?, preFader?, active? }  (to a return track; no loops,
                                                        one per target. With targetNode: a key send to a keyable node on
                                                        another track, pre-fader by default, once per node, no loops;
                                                        docs/design/sidechain.md, K2. Removing the node removes it.)
    - setSend          { trackId, sendId, levelDb?, preFader?, active? }
    - removeSend       { trackId, sendId }
    - duplicateTrack   { id }                          (a copy right below: new ids, clips not linked to the
                                                        originals, plugins from the same saved state)
    - renameTrack      { id, name }
    - setTrackGain     { id, gainDb }                  (id may be the master track's)
    - setTrackPan      { id, pan }                     (-1 left ... +1 right; id may be the master track's)

    Clips (docs/design/phase6-arrangement.md). Clips stack (docs/design/takes-lanes.md): a placed clip goes on top,
    into a takes lane where it overlaps nothing (its own, else the first free one, else a new lane on top), and only
    the top clip plays where clips overlap. Lanes left empty go.
    - addClip          { trackId, start, length, notes?, controllers?, name?, colour?, offset?, loopStart?, loopLength? }
                       (notes as in addNotes)
    - removeClip       { id }
    - deleteClips      { ids }
    - moveClip         { id, start, trackId? }
    - moveClips        { ids, delta, trackDelta?, trackIds?, copy?, linked? } (trackDelta in track-order steps;
                       trackIds: { clipId: trackId } names a clip's destination instead; linked makes linked copies)
    - resizeClip       { id, edge: "start" | "end", to }    (or { id, length }: the end edge)
    - duplicateClips   { ids, linked? }                     (copies right after the selection)
    - makeUnique       { ids }                              (unlinks clips; they get their own notes)
    - splitClips       { ids, at }
    - setClipLoop      { id, loopLength, loopStart? }       (loopLength 0 = no loop)
    - setClipProps     { id, name?, colour? }
    - glueClips        { ids }                              (one track; one clip unrolls its loop)
    - bringToTop       { id, from?, to? }                   (the clip, or that part of it split off, plays on top)
    - flattenTakes     { trackId }                          (keeps what plays, in one lane)
    - removeLane       { trackId, lane }                    (the lane and its clips; the lanes below move up)
    - setTrackLanesExpanded { id, expanded }                (shows the takes lanes; layout only)
    - setTrackHeight   { id, height }
    - setTrackColour   { id, colour }

    Audio (docs/design/phase7-audio-tracks.md):
    - addAudioClip     { trackId | newTrack: true, start, fileId | file: { path, name, sampleRate, channels, frames },
                         offset?, length? }      (offset: content ticks skipped at the start)
                       (adds the file to the pool unless a file with that path is there)
    - relinkAudioFile  { id, path }

    Sampler (docs/design/sampler-midi-delay-limiter.md, section 4):
    - addSampleZones   { trackId, nodeId, files: [{ path, name, sampleRate, channels, frames }], key?, replaceZone?,
                         zones?: [{ file, ...setSampleZone's fields }] }
                       (zones: planned from the files or an SFZ patch, docs/design/sampler-mapping.md, SM9, SF1)
    - setSampleZone    { trackId, nodeId, zoneId, lowKey?, highKey?, rootKey?, lowVelocity?, highVelocity?, start?, end?,
                         tune?, gainDb?, pan?, mode?, loopStart?, loopEnd?, crossfade?, chokeGroup?, layer?, attackMs?,
                         decayMs?, sustain?, releaseMs?, attackHandleTime?, attackHandleLevel?, decayHandleTime?,
                         decayHandleLevel?, releaseHandleTime?, releaseHandleLevel?, output?, fadeInLowVelocity?,
                         fadeInHighVelocity?, fadeOutLowVelocity?, fadeOutHighVelocity? }
                       (mode: "oneShot", "gate" or "loop"; handles: a segment's tension point, 0..1 of its rectangle)
    - removeSampleZone { trackId, nodeId, zoneId }

    Drum Synth (docs/design/drum-synth.md):
    - setDrumPad        { trackId, nodeId, pad, name?, model?, output?, chokeGroup?, mono?, locked? }
                        (pad 0..15; model "off", "kick", "snare", "tom", "hat", "cymbal", "clap" or "rim")
    - copyDrumPad       { trackId, nodeId, from, to }  (name, model, choke, mono and every parameter; not the output)
    - randomiseDrumPads { trackId, nodeId, mode?: "randomise" | "vary", pads?, seed? }
                        (within each parameter's safe range; without pads, every pad that plays and isn't locked)

    Multi-output instruments (docs/design/multi-output-instruments.md):
    - addTrack           { kind: "output", source, name? }  (an output track of an instrument or audio track, MO4)
    - setNodeTarget      { trackId, nodeId, target?, newTrack? }
                         (an Output to track node's output track; newTrack: a name, for a new one in the same step)
    - splitInstrumentOutputs { trackId, nodeId, outputs: [{ name, zoneIds | pads }] }
                         (each group of a Sampler's zones or a Drum Synth's pads to the next free output, an output
                         track and a node wired to it, MO5)
    - setPluginOutputs   { trackId, nodeId, all }  (an instrument plugin's output buses all on, or as it starts, MO1)
    - splitPluginOutputs { trackId, nodeId, outputs: [{ port, name }] }
                         (all buses on; each port not yet wired to a new output track, through a new node, MO5)

    Split nodes (docs/design/split-nodes.md):
    - setBandCount       { trackId, nodeId, bands }  (2-4; cables from bands that go away are removed, SP1)
    - setStereoMode      { trackId, nodeId, mode: "midSide" | "leftRight" }  (a Stereo Split's or Merge's parts, SP4)

    Mono ports (docs/design/mono-ports.md):
    - setPluginChannels  { trackId, nodeId, channels: "auto" | "monoToStereo" | "stereo" }
                         (the main bus layouts an effect plugin may take, MP4)
    convertToVoiceGraph takes a Sampler too: its zones move to a Sample Voice in the voice graph.

    - addAudioClip and addClip also take recorded: true (a recording goes into the top takes lane if it's free there,
      else a new lane on top; docs/design/takes-lanes.md, T3)
      `take` (0-based) of such a clip (docs/design/phase9-export-midi-input.md, M6)

    MIDI learn (docs/design/phase9-export-midi-input.md, M8; param is "gain" or "pan" without nodeId):
    - setMidiMapping   { device?, channel, controller, trackId, nodeId?, param }
    - removeMidiMapping { trackId, nodeId?, param }
    - setClipProps also takes gainDb, fadeIn, fadeOut, fadeInShape, fadeOutShape for audio clips; setTempo
      rescales audio clips so their audio keeps its duration

    Note edits (addNotes, updateNotes, removeNotes, splitNotes, setCurve) on a linked clip apply to all clips linked
    with it.

    Markers and loop range:
    - addMarker        { position, name? }
    - moveMarker       { id, position }
    - renameMarker     { id, name }
    - removeMarker     { id }
    - setLoop          { enabled?, start?, end? }

    Notes (curves are arrays of [time, value, shape?, tension?]; vibrato is { depth?, rateHz?, syncBeats?,
    shape?, delay?, fadeIn? } and merges into the note's current vibrato):
    - addNotes         { clipId, notes: [{ start, length, pitch, velocity?, releaseVelocity?, expression?, vibrato? }] }
    - updateNotes      { clipId, notes: [{ id, ...fields to change }] }
    - removeNotes      { clipId, ids: [...] }
    - splitNotes       { clipId, ids: [...], at }          (at in clip ticks; notes it isn't inside are left alone,
                                                          fails if that is all of them)
    - setCurve         { clipId, noteId, lane, points }
    - addNotes also takes link: { toIndex, glideMs } on a note, naming another note of the call (pasting)
    - setNoteLinks     { clipId, links: [{ from, to | null, glideMs? }] }   (docs/design/linked-notes.md: `to` continues
                       `from`'s voice; a note links to one note and is linked from one, so linking moves older links)

    Graph (docs/design/phase8-node-graph.md; fixed nodes can't be added or removed, instruments only go on
    instrument tracks, plugin types need plugin):
    - addNode          { trackId, type, x?, y?, plugin? }
    - addNodes         { trackId, nodes: [{ type, x?, y?, plugin?, params?, bypassed? }],
                         connections?: [{ from, fromPort, to, toPort }] }   (from and to index into nodes)
    - removeNode       { trackId, nodeId }      (in a chain, the neighbours are joined up again)
    - removeNodes      { trackId, nodeIds }     (no rejoining)
    - connect          { trackId, fromNode, fromPort, toNode, toPort }
    - disconnect       { trackId, fromNode, fromPort, toNode, toPort }
    - setConnectionBends { trackId, fromNode, fromPort, toNode, toPort, bends?: [{ x, y }] }  (a cable's bends on
                                                        the canvas, along and across the flow; none clears them)
    - setModulation    { trackId, nodeId, port, param, depth?, active? }  (param empty removes the slot; depth
                                                        -1..1, scaled by the target parameter's range)
    - setModulationShape { trackId, nodeId, points }   (LFO or Waveshaper; times 0..ticksPerBeat, values -1..1)
    - setNodeParam     { trackId, nodeId, param, value }
    - setNodePosition  { trackId, nodeId, x, y }
    - setNodeBypass    { trackId, nodeId, bypassed }

    Chain editing (phase 4: Sequencer -> [instrument] -> [effects...] -> Gain -> Track Output). removeNode
    joins the neighbours of a removed instrument or effect up again:
    - insertNode       { trackId, type, plugin?, index? }   (an instrument replaces the current one; an effect
                                                             is inserted at index, default last. plugin is
                                                             { identifier, name, manufacturer, format } and
                                                             required for plugin types)
    - reorderNode      { trackId, nodeId, index }            (effects only)
    - setPluginMidi    { trackId, nodeId, mpe?, bendRange? } (instrument plugins only)

    The graph and chain commands also accept the master track's id as trackId (docs/design/mixing-basics.md,
    M4); it takes effects only.

    Automation (docs/design/phase10-automation.md; trackId may be the master track's; values are 0..1; target is
    { kind: "gain" | "pan" | "node" | "plugin", nodeId?, param?, name? }):
    - addAutomationLane   { trackId, target, points? }     (one lane per parameter; expands the track)
    - removeAutomationLane { trackId, laneId }
    - setAutomationLane   { trackId, laneId, active?, height? }
    - setAutomationPoints { trackId, laneId, points }      (points at absolute ticks)
    - writeAutomation     { trackId, laneId, from, to, points }  (replaces the range from..to)
    - setTrackAutomation  { id, mode?, expanded? }        (mode: 0 off, 1 read, 2 touch, 3 latch, 4 write)
    - setClipController   { clipId, controller, points }  (controller 0-127, 128 pitch bend, 129 pressure;
                                                           content ticks; empty points remove it)

    Grouping:
    - batch            { label?, commands: [{ command, args }] }   (one undo step; no nesting)

    @param project  The project to start from. It is not modified.
    @param name     The command name.
    @param args     The command arguments as a JSON object.
    @return         The resulting project.
    @throws CommandError  If the command is unknown or an argument is missing or invalid.
*/
[[nodiscard]] Project applyCommand (const Project& project, const juce::String& name, const juce::var& args);

/**
    Returns the project with a master track, creating the default one (Mix Input -> Gain -> Track Output) if it
    has none (docs/design/mixing-basics.md, M4). Not a command: new and loaded projects get their master track
    outside the undo history.

    @param project  The project to start from. It is not modified.
    @return         The project, with a master track.
*/
[[nodiscard]] Project withMaster (const Project& project);

/**
    Parses a curve: [[time, value, shape?, tension?], ...], sorted by time with distinct times.

    @param value     The points.
    @param minValue  Values are clamped to this...
    @param maxValue  ...and this.
    @return          The curve.
    @throws CommandError  If the points are malformed.
*/
[[nodiscard]] Curve curveFromVar (const juce::var& value, double minValue, double maxValue);

/**
    Parses a pool file's pitch edits as the model writes them (docs/design/pitch-correction.md, PC5).

    @param value       { analyser, notes: [{ id, leadStart, start, end, detectedCentre, targetCentre, detectedShape,
                       shape, pivots, vibrato, transitionMs }] }; anything else gives no notes.
    @param fileFrames  The file's length, which notes must stay inside (0: unchecked).
    @return            The edits.
    @throws CommandError  If a note is malformed or the notes overlap.
*/
[[nodiscard]] FilePitch filePitchFromVar (const juce::var& value, juce::int64 fileFrames);

/**
    Parses a link's mapping (docs/design/parameter-linking.md, L2): { kind, amount?, centre?, inMin?, inMax?, outMin?,
    outMax?, curve? }.

    @param value  The mapping.
    @return       The mapping; missing fields take their defaults.
    @throws CommandError  If the kind is unknown or the curve is malformed.
*/
[[nodiscard]] LinkMapping linkMappingFromVar (const juce::var& value);

/**
    Parses a parameter link (docs/design/parameter-linking.md, L1): { id, source, target, mapping?, active? }, each
    end { trackId, nodeId, param }.

    @param value  The link.
    @return       The link.
    @throws CommandError  If a field is missing or invalid.
*/
[[nodiscard]] ParamLink paramLinkFromVar (const juce::var& value);

/**
    Drops the links whose node is gone, or whose built-in parameter its node no longer has (docs/design/
    parameter-linking.md, L12). A plugin parameter isn't known without the plugin, so its links stay.

    @param project  The project to prune.
*/
void pruneParamLinks (Project& project);

/**
    Parses a note from its JSON form (as produced by toVar (const Note&), or a command's note argument).

    @param fields  { id?, start, length, pitch, velocity?, releaseVelocity?, expression?, vibrato? }
    @return        The note, with id 0 unless fields has one.
    @throws CommandError  If a field is missing or invalid.
*/
[[nodiscard]] Note noteFromVar (const juce::var& fields);

/**
    Returns a project in which one plugin node carries the given state. Not a command: plugin state never comes
    from the UI; the app refreshes it from live instances with Session::amend().

    @param project  The project to start from. It is not modified.
    @param trackId  The track holding the node.
    @param nodeId   The plugin node.
    @param state    The state blob (may be null).
    @return         The resulting project.
    @throws CommandError  If the track or node doesn't exist or isn't a plugin node.
*/
[[nodiscard]] Project withPluginState (const Project& project, ObjectId trackId, ObjectId nodeId,
                                       std::shared_ptr<const juce::MemoryBlock> state);

/**
    Returns a human-readable label for the undo history.

    @param name  The command name.
    @param args  The command arguments (a batch may carry its own label).
    @return      A label such as "Set tempo", or the name itself for unknown commands.
*/
[[nodiscard]] juce::String describeCommand (const juce::String& name, const juce::var& args);

} // namespace dawg
