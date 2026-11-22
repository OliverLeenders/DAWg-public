# Design: group tracks and effect sends

Status: **implemented**, in two steps: groups (step 1, notes in section 10), then returns and sends (step 2, notes in
section 11). The first part of
phase 13 (routing). The decisions are numbered R1–R13 and summarised in section 7; the owner's answers are in
section 8.

## 1. Goals and non-goals

Goals:

* **Group tracks** (Ctrl+G): a folder of tracks whose main outputs are summed on the group's bus, which has its own
  inserts, fader, pan, meter, mute and solo. Groups fold away in the arrangement.
* **Effect sends**: any track (or group) sends an adjustable share of its signal to a **return track**, a bus with
  effects (a reverb, a delay) whose output goes to the master.
* Both behave like the tracks and the master track do today: automation, meters, latency compensation, undo, and
  the project file.

Non-goals (later in phase 13 or after):

* Sidechain inputs and arbitrary routing of a track's output to any bus. Groups and sends cover the common cases,
  and the model below leaves room for both.
* A mixer view with channel strips. Groups and returns are designed so a mixer can show them as strips later.
* Running buses on several threads.

## 2. What already exists

* The master track (mixing-basics.md, M4): a `Track` of kind `master` whose graph starts with a `Mix Input` node.
  The engine sums every track into one mix buffer, with fader and pan, then runs the master track on it.
* Latency compensation at the master: each track is delayed to the slowest one.
* Tracks are a flat list (`Project::tracks`). There is no command to reorder them yet, and the UI selects one
  track at a time.

## 3. Model

**(R1) Buses are tracks.** Recommended. Two new track kinds join `instrument`, `audio` and `master`:

* `group`: a bus fed by its member tracks.
* `return`: a bus fed by sends.

Both have no clips and the master track's graph: `Mix Input → [effects…] → Gain → Track Output`. Everything that
works on the master track (inserts, the graph view, gain and pan, automation, meters, the inspector) works on them
unchanged, and a mixer can later treat all four kinds as strips.

**(R2) Group membership is a parent id on each track; the track list stays flat.** Recommended.

* `Track` gains `parent` (`ObjectId`, 0 = top level). A group's members are the tracks whose `parent` is the
  group's id.
* `Project::tracks` keeps one flat order, with the invariant that a group's members follow it directly, in a
  contiguous block (like folders in a list). Commands keep the invariant; reading a file that breaks it repairs it.
* The flat order keeps the arrangement, the lanes, the file format and every existing command as they are; only
  moves and grouping need to know about parents.

**(R3) Groups nest.** Approved. A group's parent may be another group, so a "Drums" group can hold a "Toms"
group. With parent ids this costs nothing extra in the model, and the engine handles any depth (R6).

**(R4) Sends belong to the sending track.** Recommended.

```
Send { id, target (a track id), targetNode (0 = the track's Mix Input), levelDb, preFader, active }
```

* `Track` gains `sends`, a list. Instrument, audio, group and return tracks can send; the master can't.
* **Post-fader by default**: the send taps the signal after the track's fader and pan, so pulling a fader down
  also pulls its reverb down. Pre-fader taps before the fader (for headphone mixes or effects that should stay
  when the dry track is muted).
* A send targets a **return** track only, for now. Groups get their signal from membership, never from sends.
* **Sidechains later (R13).** Once nodes have sidechain inputs, a send may also feed the sidechain input of a node
  on another track (a compressor ducking under the kick). `targetNode` names that node; 0 means the target track's
  `Mix Input`, which is all step 2 allows. The engine treats such a send as one more edge into the target track
  (R6), so the ordering, the cycle check and latency compensation (R7) already cover it.
* A return may send to another return (a delay into a reverb), but never in a loop: `addSend` rejects a send that
  would close a cycle, as `connect` does inside a graph.

**(R5) Mute and solo follow the routing.** Recommended.

* Muting a group silences everything in it. Muting a member silences only that member.
* Soloing a member keeps its groups and the returns it sends to audible (they are on its path to the master).
  Soloing a group solos all its members. Soloing a return keeps every track that sends to it audible
  ("solo-safe" return behaviour, Cubase's default).

## 4. Engine

**(R6) A project-level bus graph, compiled like a track graph.** Recommended.

* The compiler builds a small graph over the project: each track, group and return is a node, plus the master.
  Edges are main outputs (a track into its group, a group into its parent group or the master, a return into the
  master) and sends (a track into a return).
* It sorts the graph topologically: members before their group, senders before their returns. Each group and
  return gets its own two-channel mix buffer for one block; the master keeps today's.
* `renderChunk` runs the nodes in that order. A track runs its graph as today, then adds its output to its
  parent's buffer (with fader and pan) and to each send target's buffer (scaled by the send level, taken before or
  after the fader). A bus starts from its mix buffer through its `Mix Input` node.
* With no groups and no sends this is exactly today's engine: every track's parent is the master.

**(R7) Latency compensation at every bus input.** Recommended.

* Each node's output arrives at its bus with a latency: its own plugin latency plus the latency at its input.
* Where signals of different latency meet at a bus (a group's members, a return's senders), each is delayed to the
  latest one, with a delay line on that edge. Today's alignment at the master is the same rule applied once.
* A send from a track with latency is delayed like that track's main output, so the dry signal and its reverb stay
  aligned.

**(R8) Meters and automation come for free.** Groups and returns have `TrackState`, so their meters, clip
indicators and gain and pan automation work as on tracks. Send levels become a new automation target
(`kind: "send"` with the send's id), so a send can be ridden like a fader.

## 5. Commands

| Command | What it does |
|---|---|
| `groupTracks { ids, name? }` | Ctrl+G. Creates a group at the position of the first selected track and makes the selected tracks its members, in their current order. Members already in a group move into the new one (which joins that group, so nesting follows). |
| `ungroup { id }` | Ctrl+Shift+G. Moves the members up to the group's parent and removes the group (its inserts go with it; undo brings them back). |
| `moveTracks { ids, to, parent }` | Reorders tracks and changes their group: drag in the headers. Keeps groups contiguous. |
| `removeTrack { id }` on a group | Removes the group and all its tracks (nested groups included). The UI asks first: "Delete the group "Drums" and its 5 tracks?" |
| `addTrack { kind: "return" }` | Adds a return track (with an empty chain; a reverb is one insert away). |
| `addSend { trackId, target, levelDb?, preFader? }` | Adds a send. Rejects cycles and targets that aren't returns. |
| `setSend { trackId, sendId, levelDb?, preFader?, active? }` | Changes a send; drags merge like faders. |
| `removeSend { trackId, sendId }` | Removes a send. |
| `setTrackFolded { id, folded }` | Folds or unfolds a group in the arrangement (view state, kept in the file like track height). |

Removing a return removes the sends that target it. `duplicateTrack` on a group duplicates its members too; on a
track it copies its sends.

## 6. UI

**(R9) A group is drawn as a "C" around its members.** As requested by the owner:

```
 ┌──────────────────────────────┐
 │▌▾ Drums                 M  S │  top arm: fold toggle, name, mute, solo
 │▌ ┌────────────────────────┐  │
 │▌ │ Kick       M S ●       │  │  member headers, indented by the spine
 │▌ │ Snare      M S ●       │  │
 │▌ │ ▸ Toms (nested group)  │  │
 │▌ └────────────────────────┘  │
 │▌ −2.0 dB  C  ▸ R  ▬▬▬▬▬▬○    │  bottom arm: gain, pan, automation, meter
 └──────────────────────────────┘
```

* The group's colour forms the "C": a top bar, a spine down the left edge of its members, and a bottom bar.
  Member headers sit inside it, indented by the width of the spine (each nesting level adds a spine).
* **Top arm** (fixed, 26 px): fold toggle, name (double-click renames), mute and solo.
* **Bottom arm** (fixed, 40 px): gain, pan, the automation toggle and mode, and the group's meter along the bottom.
  The group's automation lanes open below the bottom arm.
* In the arrangement, the top arm's lane shows a thin summary of the members' clips in the group's colour, and the
  bottom arm's lane is empty (it lines up with the header).
* **Folded**, the "C" closes into one ordinary header (the layouts of the new track headers: compact, standard or
  tall by height), and its lane shows the members' clips as a summary strip. Folding hides the member lanes only;
  everything keeps playing.
* Selecting the group (click the top or bottom arm) shows its inserts in the inspector and its graph in the graph
  view, like the master track.

**(R10) Return tracks live in a fixed Returns section.** Approved (owner's choice). Returns are listed in a
section of their own at the bottom of the arrangement, between the tracks and the master track, with a "Returns"
heading. Their headers are like track headers (a send icon before the name, no arm switch) and their lanes stay
empty. They can be reordered within the section, but tracks and returns never mix. In the model they stay in
`Project::tracks`, after every other track (an invariant like the one for groups).

**(R11) Sends live in the inspector.** The selected track's inspector gets a **Sends** section under the inserts:

* one row per send: on/off, the target's name, a level knob (dB), and a pre/post toggle;
* an "Add send" menu listing the return tracks, plus "New return track…".

Later the mixer view shows the same sends on each strip.

**(R12) Selecting several tracks.** Ctrl+G needs a multi-track selection, which the headers don't have yet:
Ctrl+click toggles a track and Shift+click selects a range, as with clips. The inspector shows the last-clicked
track. Dragging headers (one or a selection) reorders tracks and moves them into or out of groups, with a drop line
showing where they land (`moveTracks`).

## 7. Decisions to review

| # | Decision | Recommendation |
|---|---|---|
| R1 | Groups and returns are tracks (`kind` group / return) with the master track's graph | Yes |
| R2 | Membership by a `parent` id; the track list stays flat, with contiguous groups | Yes |
| R3 | Groups nest | Approved |
| R4 | Sends belong to the sending track, target returns only (for now), post-fader by default, no cycles | Approved |
| R5 | Mute and solo follow the routing; returns are solo-safe | Yes |
| R6 | A project-level bus graph, sorted and compiled; one mix buffer per bus | Yes |
| R7 | Latency compensation at every bus input, sends included | Yes |
| R8 | Meters and gain/pan automation as on tracks; send levels automatable | Yes |
| R9 | A group drawn as a "C": top arm name, mute and solo, bottom arm gain, pan and meter; folds into one header | Approved (owner's idea) |
| R10 | Return tracks in a fixed Returns section above the master | Approved |
| R11 | Sends in the inspector, under the inserts | Yes |
| R12 | Multi-track selection and drag reordering in the headers (needed for Ctrl+G) | Yes |
| R13 | A send names a target node, so sends can feed sidechain inputs once nodes have them | Approved (owner's note) |

## 8. The owner's answers

1. **Nesting:** groups may contain groups (R3).
2. **Removing a group** removes it with all its tracks, after asking: "Delete the group "…" and its n tracks?"
   (n counts every track inside, nested groups included). "Ungroup" keeps the tracks.
3. **Returns** go in a fixed Returns section above the master (R10).
4. **Sends** default to post-fader (R4).
5. **Mute and solo** sit on the top arm, as drawn (R9).
6. **Two steps:** groups first, then returns and sends.
7. **Sidechains:** sends may later connect to sidechain inputs of other tracks (R13).

## 9. Implementation outline

1. Track reordering and multi-selection (`moveTracks`, R12): useful on its own.
2. Groups: model and file (`parent`, `kind: group`, `folded`), the bus graph in the compiler and engine (R6, R7),
   `groupTracks` / `ungroup`, mute and solo rules (R5), and the "C" in the headers and lanes (R9).
3. Returns and sends: `kind: return`, `sends`, the send commands, send taps in the engine, send automation (R8),
   and the inspector section (R11).
4. Tests for each: the bus order and summing, latency alignment at buses and through sends, mute and solo
   propagation, cycle rejection, the grouping invariants, and file round trips.

## 10. Implementation notes (step 1: groups)

* **Model.** `TrackKind::group`, `Track::parent` and `Track::folded`, serialised as `kind: "group"`, `parent` and
  `folded`. `normaliseGroups()` (Model.h) restores the invariants: parents that aren't existing groups, or that
  would make a track contain itself, become 0, and every group's members follow it as one block. The grouping
  commands and `projectfile::read()` call it. `subtreeEnd()` finds the end of a track's block.
* **Commands.** `groupTracks`, `ungroup`, `moveTracks` and `setTrackFolded` are new. `addTrack` takes `kind: "group"`
  and `parent`; `removeTrack` and `duplicateTrack` take a group's whole block (copies of nested groups hold the copies
  of their members). A group's graph is the master track's: `Mix Input → Gain → Track Output`. `moveClips` also takes
  `trackIds` (a destination per clip id), which the arrangement uses because groups and folded tracks lie between
  its lanes; clips can't move onto a group (different kind).
* **Compiler.** Tracks are planned as before, then aligned at every bus: walking the track list backwards (members
  come after their group), each track reaches its bus at its own latency plus the latency at its input, and is
  delayed to the latest arrival there. `CompiledTrack` gains `bus` (a group's input bus), `outputBus` (its group's,
  or -1 for the master mix), `aheadSamples` (how far ahead of the playhead it runs: the engine latency minus the
  latency at its input) and `enclosing` / `inside` (indices of the groups around it and the tracks inside it).
  `CompiledEngine::tracks` is in mixing order (members before their group), with `busMemory` holding two channels per
  group. A change of `parent` or `kind` counts as structural.
* **Engine.** `renderChunk` clears the group buses, then runs the tracks in mixing order: a group's `Mix Input`
  reads its bus, and each track is summed (with fader and pan) into its group's bus or the master mix. A track is
  heard when it isn't muted and, while anything is soloed, when it, a group around it or a track inside it is soloed;
  a muted group silences its members by not passing them on. A stem export of a group renders it with its members.
* **UI.** `lanesOf()` gives lanes a `role` (`track`, `groupTop`, `groupBottom`), the `enclosing` groups and a
  `clipIndex` among the lanes that hold clips (clip drags, paste and the lane menu count in those). An open group's
  top lane shows a strip where its members have clips; a folded group's lane shows faint blocks. In the headers,
  `GroupArm` draws the arms, `Spines` the groups' colours down the left, and folded groups get a fold button.
  `uiStore` has `selectedTracks` (Ctrl+click toggles, Shift+click selects a range); dragging headers shows a drop line
  and sends `moveTracks` (`groups.ts`: `dropSlots`, `nearestSlot`). Ctrl+G groups the selected tracks and Ctrl+Shift+G
  ungroups the selected group (App.tsx); the header menu has Group, Ungroup, Fold, Duplicate and Delete. Deleting a
  group asks first (`ConfirmDialog`). The inspector shows a group's inserts like the master track's.
* **Tests.** tests/GroupTests.cpp: grouping (nesting included), ungrouping, moves and their limits, removal and
  duplication of blocks, adding into groups, folding, normalising, clip moves past groups, the file round trip, bus
  summing with nested groups and meters, and mute and solo. PluginTests.cpp checks latency alignment at a group's bus
  and through its chain. ui/src/arrangement/groups.test.ts covers the lanes, the helpers and the drop slots.

## 11. Implementation notes (step 2: returns and sends)

* **Model.** `TrackKind::returnTrack` (`kind: "return"`), `Send { id, target, targetNode, levelDb, preFader, active }`
  and `Track::sends`, serialised as an object keyed by send id (ids grow, so that is the order they were added).
  `AutomationTargetKind::send` (`kind: "send"`, `param` = the send's id) automates a send level. `normaliseGroups()`
  also keeps returns at the top level after every other track and drops sends that don't go to another, existing
  return. `limits::maxSends` is 16.
* **Commands.** `addTrack { kind: "return", sendFrom? }` adds a return after every track (with `sendFrom`, also the
  send to it, as one step); new other tracks go before the returns. `addSend`, `setSend` and `removeSend` are new:
  sends go to returns only, once per target, and never in a loop (`sendsReach`). Removing a return removes the sends
  to it; removing a send removes its automation lanes. `duplicateTrack` copies sends with new ids and re-points the
  lanes of their levels; a duplicated return stays in the returns section. Returns can't be grouped, and `moveTracks`
  keeps returns and other tracks apart.
* **Compiler.** Returns get buses like groups and run last, ordered so a return comes after the returns that send to
  it (a loop from a file is broken and that send left out). A send leaves its track at the track's own output, before
  that is aligned at the track's bus; each return's input latency is the latest of its senders', and a send that
  arrives earlier gets its own delay step, so a dry track and its reverb stay in step. `CompiledSend` holds the
  return's bus and index, the (delayed) source and a `SendState` (level, on/off, pre/post), which changes without
  recompiling. A send's routing (id, target) counts as structural.
* **Engine.** After a track is summed into its bus, each active send adds its source to the return's bus: at the send
  level before the fader, or after the fader and pan scaled by the send level. Send automation ramps the level per
  chunk like the fader. Returns are solo-safe, a soloed return keeps the tracks that send to it audible, and a stem
  export renders its track dry.
* **App.** `AutomationControl` records send levels like the track gain (the same fader curve).
* **UI.** `lanesOf()` adds a `returnsHeading` lane above the returns ("Returns", with a + to add one); returns have a
  return icon and no arm switch; drops keep returns and other tracks apart. The inspector's Sends section
  (`SendsInspector`) lists each send with on/off, the return, the level (drag, double-click for 0 dB; it follows
  automation while that plays), Pre/Post and remove, and adds sends to existing returns or to a new one. The add-track
  menu has Return track; send levels appear in the automation picker as "Send → name".
* **Tests.** tests/SendTests.cpp: the returns section, send rules and loops, automation lanes and their cleanup,
  duplication, the file round trip and repair, send levels before and after the fader, returns feeding returns, and
  solo and mute. PluginTests.cpp checks that a send is delayed to its return's latest input.
  ui/src/arrangement/groups.test.ts covers the returns heading and drops.
