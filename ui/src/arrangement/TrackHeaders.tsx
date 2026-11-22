import { isInstrument, trackChain } from '../components/chain';
import { openInstrument } from '../components/instrumentWindow';
import { ChevronDown, ChevronRight, ChevronsDownUp, ChevronsUpDown, Circle, CopyPlus, CornerDownRight, Download, Group, Headphones, Layers, Merge, Pencil, Piano, Plus, Snowflake, Split, Trash2, TriangleAlert, Ungroup, X } from 'lucide-react';
import { memo, useEffect, useMemo, useRef, useState } from 'react';
import type { Project, Track } from '../bridge/protocol';
import { ConfirmDialog } from '../components/ConfirmDiscard';
import { DragNumber } from '../components/DragNumber';
import { Meter } from '../components/Meter';
import { MidiLearnable } from '../components/MidiLearn';
import { Swatches } from '../components/Swatches';
import { MenuItem } from '../components/MenuItem';
import * as bridge from '../bridge/bridge';
import { actions, dispatch, newGestureId, reportError, useDawg } from '../store/store';
import { ui, useUi } from '../store/uiStore';
import type { Lane, TakeRow } from './layout';
import { takeName } from '../timeline/takes';
import { dropSlots, nearestSlot, outputTracksOf, tracksInside, underSource, type DropSlot } from './groups';
import { indentStyle, SPINE, Spines } from './Spines';
import { laneFor, playedPosition, positionToGain } from '../automation/automation';
import { touchProps, trackTarget } from '../automation/touch';
import { AutomationHeader, AutomationModeButton, AutomationPicker } from './AutomationHeaders';
import { MeterGainKnob, useTrackValues } from '../dock/TrackFader';
import { gainTaper } from '../dock/TrackFader';
import { IconButton, TrackSwitch } from '../ui/controls';
import { Menu } from '../ui/overlays';

// Track headers aligned with the arrangement lanes (docs/design/phase6-arrangement.md, section 7): colour,
// name (double-click renames), gain, pan, switches, remove, a level meter, and a bottom edge that resizes the lane;
// the layout follows the lane height (see headerTier). An open group is drawn as a "C" around its members: a top arm
// with its name, mute and solo, its colour down their left edge, and a bottom arm with gain, pan and meter
// (docs/design/groups-and-sends.md, R9). Headers select with Ctrl/Shift+click and reorder by dragging (R12). The
// master track has a header of its own, pinned below them (docs/design/mixing-basics.md, section 3).


/** Pan as shown in headers: C, L40, R20. */
export function formatPan(pan: number) {
  const amount = Math.round(Math.abs(pan) * 100);
  return amount === 0 ? 'C' : `${pan < 0 ? 'L' : 'R'}${amount}`;
}

function GainAndPan({ track }: { track: Track }) {
  // While automation plays, the controls show the automated values (docs/design/phase10-automation.md, A4).
  // Select the played numbers, not the whole automation map: the map is a new object every 100 ms, and selecting
  // it re-rendered every track header 30 times a second.
  const gainLane = laneFor(track, trackTarget('gain'))?.id;
  const panLane = laneFor(track, trackTarget('pan'))?.id;
  const gainPlayed = useDawg((s) => playedPosition(track.automationMode === 0 ? undefined : s.telemetry.automation, gainLane));
  const panPlayed = useDawg((s) => playedPosition(track.automationMode === 0 ? undefined : s.telemetry.automation, panLane));

  return (
    <div className="track-mix">
      <MidiLearnable target={{ trackId: track.id, nodeId: 0, param: 'gain' }}>
      <DragNumber
        className="track-gain"
        value={gainPlayed !== undefined ? positionToGain(gainPlayed) : track.gainDb}
        min={-96}
        max={12}
        quantum={0.1}
        taper={gainTaper}
        format={(v) => `${v > 0 ? '+' : ''}${v.toFixed(1)} dB`}
        onChange={(gainDb, gesture) => dispatch({ command: 'setTrackGain', args: { id: track.id, gainDb } }, gesture)}
        {...touchProps(track.id, trackTarget('gain'))}
      />
      </MidiLearnable>
      <MidiLearnable target={{ trackId: track.id, nodeId: 0, param: 'pan' }}>
      <DragNumber
        className="track-pan"
        value={panPlayed !== undefined ? panPlayed * 2 - 1 : track.pan}
        min={-1}
        max={1}
        quantum={0.01}
        axis="x"
        resetValue={0}
        title="Pan: drag sideways, double-click to centre"
        format={formatPan}
        onChange={(pan, gesture) => dispatch({ command: 'setTrackPan', args: { id: track.id, pan } }, gesture)}
        {...touchProps(track.id, trackTarget('pan'))}
      />
      </MidiLearnable>
    </div>
  );
}

/** The track's level after fader and pan; armed audio tracks show their input level instead. A knob on it sets the
    track's volume. */
function TrackMeter({ track, input }: { track: Track; input: boolean }) {
  const { gainDb } = useTrackValues(track);
  // The three levels are primitives, so a silent track's header no longer re-renders for every telemetry tick.
  const id = String(track.id);
  const left = useDawg((s) => s.telemetry.meters?.[id]?.[0] ?? 0);
  const right = useDawg((s) => s.telemetry.meters?.[id]?.[1] ?? 0);
  const inputPeak = useDawg((s) => s.telemetry.meters?.[id]?.[2] ?? 0);
  const clipped = useDawg((s) => s.clipped.has(track.id));

  return (
    <Meter
      className={`track-meter ${input ? 'input' : ''}`}
      left={input ? inputPeak : left}
      right={input ? inputPeak : right}
      title={input ? 'Input level (armed)' : 'Track level'}
      clipped={clipped}
      onResetClip={() => actions.resetClip(track.id)}
      overlay={<MeterGainKnob track={track} gainDb={gainDb} />}
    />
  );
}

/** Header layouts by lane height, so every size shows what fits and nothing overlaps:
    compact  one row: name, mute, solo, arm (and monitoring), automation
    standard two rows: name and switches, then gain, pan and automation
    tall     three rows: name, then gain and pan, then switches and automation
    The meter runs along the bottom edge once there is room under the row; the right-click menu duplicates or
    removes the track at any size. */
type Tier = 'compact' | 'standard' | 'tall';
// Each layout's rows plus the meter below them (8 px, 4 px off the bottom, 3 px under the last row) must fit.
const STANDARD_HEIGHT = 64; // 5 + 20 + 4 + 20, then the meter
const TALL_HEIGHT = 94; // 7 + 20 + 6 + 20 + 6 + 20, then the meter
const METER_HEIGHT = 36; // a compact row centred above the meter

export const headerTier = (height: number): Tier =>
  height >= TALL_HEIGHT ? 'tall' : height >= STANDARD_HEIGHT ? 'standard' : 'compact';

/** A track's switches in fixed columns from the right edge (design-system.md, step 4): its own (arm, then monitoring or
    the instrument's editor), then mute and solo, which every track has, so they line up down the arrangement. */
function Switches({ track }: { track: Track }) {
  const armed = useDawg((s) => s.armed.has(track.id));
  const recordable = track.kind !== 'group' && track.kind !== 'return' && track.kind !== 'output';
  return (
    <>
      <span className="switch-group own">
        {recordable && (
          <TrackSwitch
            kind="arm"
            on={armed}
            title={track.kind === 'audio' ? 'Arm for recording' : 'Arm: record and play MIDI input'}
            onClick={(e) => {
              e.stopPropagation();
              actions.arm(track.id, !armed);
            }}
          >
            {/* Filled when armed, hollow when not, so the state isn't told by colour alone (3.10). */}
            <Circle size={10} fill={armed ? 'currentColor' : 'none'} strokeWidth={armed ? 0 : 2.5} />
          </TrackSwitch>
        )}
        {track.kind === 'instrument' && <InstrumentButton track={track} />}
        {track.kind === 'audio' && (
          <button
            className={`switch monitor m${track.monitor}`}
            title={`Monitoring: ${['off', 'automatic (while armed)', 'on'][track.monitor]} (click to change)`}
            onClick={(e) => {
              e.stopPropagation();
              dispatch({ command: 'setTrackSwitches', args: { id: track.id, monitor: (track.monitor + 1) % 3 } });
            }}
          >
            <Headphones size={12} />
          </button>
        )}
      </span>
      <span className="switch-group shared">
        <TrackSwitch
          kind="mute"
          on={track.mute}
          title="Mute"
          onClick={(e) => {
            e.stopPropagation();
            dispatch({ command: 'setTrackSwitches', args: { id: track.id, mute: !track.mute } });
          }}
        >
          M
        </TrackSwitch>
        <TrackSwitch
          kind="solo"
          on={track.solo}
          title="Solo"
          onClick={(e) => {
            e.stopPropagation();
            dispatch({ command: 'setTrackSwitches', args: { id: track.id, solo: !track.solo } });
          }}
        >
          S
        </TrackSwitch>
      </span>
    </>
  );
}

/** Opens the track's instrument in its own window, or a plugin's own window (docs/design/macros-and-modulation-column.md, MC14). */
function InstrumentButton({ track }: { track: Track }) {
  const instrument = trackChain(track)?.instrument ?? Object.values(track.graph.nodes).find(isInstrument);
  if (!instrument) return null;
  return (
    <button
      className="switch instrument"
      title="Open the instrument's editor"
      onClick={(e) => {
        e.stopPropagation();
        openInstrument(track, instrument);
      }}
    >
      <Piano size={12} />
    </button>
  );
}

/** Show/hide the automation lanes; when shown, add a lane and pick the read/write mode. */
function AutomationSwitches({ track, full, onPick }: { track: Track; full: boolean; onPick: (e: React.MouseEvent) => void }) {
  // Under mute and solo: the add button first, so the expander and the mode stay in their columns.
  return (
    <>
      {full && track.automationExpanded && (
        <button className="switch" title="Automate a parameter" onClick={onPick}>
          <Plus size={12} />
        </button>
      )}
      <button
        className="switch expand"
        title={track.automationExpanded ? 'Hide automation' : 'Show automation'}
        onClick={(e) => {
          e.stopPropagation();
          dispatch({ command: 'setTrackAutomation', args: { id: track.id, expanded: !track.automationExpanded } });
        }}
      >
        {track.automationExpanded ? <ChevronDown size={12} /> : <ChevronRight size={12} />}
      </button>
      {full && <AutomationModeButton track={track} />}
    </>
  );
}

/** Shows or hides a track's takes lanes (docs/design/takes-lanes.md, T5); only a track with several has one. */
function TakesButton({ track }: { track: Track }) {
  if (track.lanes < 2) return null;
  return (
    <button
      className={`switch takes ${track.lanesExpanded ? 'active' : ''}`}
      title={track.lanesExpanded ? 'Hide the takes' : `Show the ${track.lanes} takes`}
      onClick={(e) => {
        e.stopPropagation();
        dispatch({ command: 'setTrackLanesExpanded', args: { id: track.id, expanded: !track.lanesExpanded } });
      }}
    >
      <Layers size={12} />
      {track.lanes}
    </button>
  );
}

/** A takes row's header: the take's name and a way to delete it with its clips. */
function TakeHeader({ row, top, groups }: { row: TakeRow; top: number; groups: Track[] }) {
  const name = takeName(row.track, row.lane);
  return (
    <div className="take-header" style={{ top, height: row.height, ...indentStyle(groups) }}>
      <Spines groups={groups} />
      <span className="take-name" title="Click a take to play it, or drag across part of it">
        {name}
      </span>
      <IconButton
        size="s"
        title={`Delete ${name} and its clips`}
        onClick={() => dispatch({ command: 'removeLane', args: { trackId: row.track.id, lane: row.lane } })}
      >
        <X size={12} />
      </IconButton>
    </div>
  );
}

function FoldButton({ group }: { group: Track }) {
  return (
    <button
      className="switch expand fold"
      title={group.folded ? 'Unfold the group' : 'Fold the group'}
      onClick={(e) => {
        e.stopPropagation();
        dispatch({ command: 'setTrackFolded', args: { id: group.id, folded: !group.folded } });
      }}
    >
      {group.folded ? <ChevronRight size={11} /> : <ChevronDown size={11} />}
    </button>
  );
}

interface HeaderActions {
  /** A press on the header: selection, and maybe the start of a drag. */
  onPress: (e: React.PointerEvent, track: Track) => void;
  /** Remove the track (a group asks first). */
  onDelete: (track: Track) => void;
}

/** The right-click menu of a track or group header: rename, colour, grouping, duplicate and remove (the header has no
    remove button of its own, to keep it away from record arm: docs/design/ux-review.md, U10). */
function HeaderMenu({
  track,
  x,
  y,
  onDelete,
  onRename,
  onClose,
}: {
  track: Track;
  x: number;
  y: number;
  onDelete: (track: Track) => void;
  onRename: () => void;
  onClose: () => void;
}) {
  const selected = useUi((s) => s.selectedTracks);
  const run = (action: () => void) => () => {
    onClose();
    action();
  };
  const group = track.kind === 'group';
  const isReturn = track.kind === 'return';
  const tracks = useDawg.getState().project?.tracks ?? {};
  const groupable = (selected.has(track.id) ? [...selected] : [track.id]).filter((id) => tracks[String(id)]?.kind !== 'return');
  return (
    <>
      <Menu onClose={onClose} at={{ x: x, y: Math.min(y, window.innerHeight - 330) }}>
        <MenuItem icon={Pencil} label="Rename…" onClick={run(onRename)} />
        <div className="menu-heading">Colour</div>
        <Swatches
          current={track.colour}
          onPick={(colour) => {
            onClose();
            if (colour) dispatch({ command: 'setTrackColour', args: { id: track.id, colour } });
          }}
        />
        <div className="menu-separator" />
        {!isReturn && (
          <MenuItem
            icon={Group}
            label={groupable.length > 1 ? `Group ${groupable.length} tracks` : 'Group'}
            shortcut="Ctrl+G"
            onClick={run(() => dispatch({ command: 'groupTracks', args: { ids: groupable } }))}
          />
        )}
        {group && (
          <>
            <MenuItem icon={Ungroup} label="Ungroup" shortcut="Ctrl+Shift+G" onClick={run(() => dispatch({ command: 'ungroup', args: { id: track.id } }))} />
            <MenuItem
              icon={track.folded ? ChevronsUpDown : ChevronsDownUp}
              label={track.folded ? 'Unfold' : 'Fold'}
              onClick={run(() => dispatch({ command: 'setTrackFolded', args: { id: track.id, folded: !track.folded } }))}
            />
          </>
        )}
        {track.lanes > 1 && (
          <>
            <div className="menu-separator" />
            <MenuItem
              icon={Layers}
              label={track.lanesExpanded ? 'Hide takes' : 'Show takes'}
              onClick={run(() => dispatch({ command: 'setTrackLanesExpanded', args: { id: track.id, expanded: !track.lanesExpanded } }))}
            />
            <MenuItem icon={Merge} label="Flatten takes" onClick={run(() => dispatch({ command: 'flattenTakes', args: { trackId: track.id } }))} />
          </>
        )}
        {(track.kind === 'instrument' || track.kind === 'audio') && (
          <>
            <div className="menu-separator" />
            {/* Freeze (docs/design/audio-features.md, AU15). */}
            {/* Bounce in place and Render (audio-features.md, AU16, AU17). */}
            <MenuItem
              icon={Download}
              label="Bounce in place"
              disabled={!!track.freeze || Object.keys(track.clips).length === 0}
              onClick={run(() => bridge.call('audio.bounceTrack', { trackId: track.id }).catch(reportError))}
            />
            <MenuItem
              icon={Download}
              label={useUi.getState().selectedTracks.size > 1 ? 'Render selected tracks to audio' : 'Render to audio'}
              onClick={run(() => {
                const selected = useUi.getState().selectedTracks;
                const trackIds = selected.has(track.id) ? [...selected] : [track.id];
                bridge.call('audio.renderSelection', { trackIds, clipIds: [...useUi.getState().selectedClips] }).catch(reportError);
              })}
            />
            {track.freeze ? (
              <>
                {track.freeze.stale && (
                  <MenuItem icon={Snowflake} label="Refreeze" onClick={run(() => bridge.call('audio.freezeTrack', { trackId: track.id }).catch(reportError))} />
                )}
                <MenuItem icon={Snowflake} label="Unfreeze track" onClick={run(() => bridge.call('audio.unfreezeTrack', { trackId: track.id }).catch(reportError))} />
              </>
            ) : (
              <MenuItem
                icon={Snowflake}
                label="Freeze track"
                disabled={Object.keys(track.clips).length === 0}
                onClick={run(() => bridge.call('audio.freezeTrack', { trackId: track.id }).catch(reportError))}
              />
            )}
          </>
        )}
        {!isReturn && <div className="menu-separator" />}
        <MenuItem
          icon={CopyPlus}
          label={group ? 'Duplicate group' : isReturn ? 'Duplicate return' : 'Duplicate track'}
          shortcut="Ctrl+Shift+D"
          onClick={run(() => dispatch({ command: 'duplicateTrack', args: { id: track.id } }))}
        />
        <MenuItem icon={Trash2} label={group ? 'Delete group…' : isReturn ? 'Remove return' : 'Remove track'} shortcut="Del" danger={group} onClick={run(() => onDelete(track))} />
      </Menu>
    </>
  );
}

/** One arm of an open group's "C": the top arm (fold, name, mute, solo) or the bottom arm (gain, pan,
    automation and the group's meter). */
function GroupArm({ lane, top, selected, actions: { onPress, onDelete } }: { lane: Lane; top: number; selected: boolean; actions: HeaderActions }) {
  const group = lane.track;
  const isTop = lane.role === 'groupTop';
  const [renaming, setRenaming] = useState(false);
  const [menu, setMenu] = useState<{ x: number; y: number } | null>(null);
  const [picker, setPicker] = useState<{ x: number; y: number } | null>(null);

  return (
    <div
      className={`track-header group-arm ${isTop ? 'arm-top' : 'arm-bottom'} ${selected ? 'selected' : ''}`}
      style={{ top, height: lane.height, ['--group-colour' as string]: group.colour, ...indentStyle(lane.enclosing) }}
      onPointerDown={(e) => onPress(e, group)}
      onContextMenu={(e) => {
        e.preventDefault();
        if (!selected) ui.selectTrack(group.id);
        setMenu({ x: e.clientX, y: e.clientY });
      }}
    >
      <Spines groups={lane.enclosing} own={group} end={isTop ? 'top' : 'bottom'} />
      <div className="track-header-body">
        {isTop ? (
          <div className="header-row">
            <FoldButton group={group} />
            {renaming ? (
              <input
                className="track-name-input"
                autoFocus
                defaultValue={group.name}
                onBlur={() => setRenaming(false)}
                onKeyDown={(e) => {
                  e.stopPropagation();
                  if (e.key === 'Enter') {
                    const next = (e.target as HTMLInputElement).value.trim();
                    if (next && next !== group.name) dispatch({ command: 'renameTrack', args: { id: group.id, name: next } });
                    setRenaming(false);
                  } else if (e.key === 'Escape') setRenaming(false);
                }}
              />
            ) : (
              <span className="track-name group-name" onDoubleClick={() => setRenaming(true)} title={`${group.name} (double-click to rename)`}>
                {group.name}
              </span>
            )}
            <Switches track={group} />
          </div>
        ) : (
          <div className="header-row">
            <GainAndPan track={group} />
            <AutomationSwitches
              track={group}
              full
              onPick={(e) => {
                e.stopPropagation();
                const rect = e.currentTarget.getBoundingClientRect();
                setPicker({ x: rect.left, y: rect.bottom + 4 });
              }}
            />
          </div>
        )}
      </div>
      {!isTop && <TrackMeter track={group} input={false} />}
      {picker && <AutomationPicker track={group} x={picker.x} y={picker.y} onClose={() => setPicker(null)} />}
      {menu && <HeaderMenu track={group} x={menu.x} y={menu.y} onDelete={onDelete} onRename={() => setRenaming(true)} onClose={() => setMenu(null)} />}
    </div>
  );
}

type HeaderProps = { lane: Lane; top: number; selected: boolean; actions: HeaderActions };

/**
 * Whether a header shows the same: its track (patches keep an unchanged track's identity), place, selection and
 * groups. A drag on one track's controls patches the project many times a second; the other headers stay as they are.
 */
const sameHeader = (a: HeaderProps, b: HeaderProps) =>
  a.lane.track === b.lane.track &&
  a.lane.height === b.lane.height &&
  a.lane.enclosing.length === b.lane.enclosing.length &&
  a.lane.enclosing.every((group, i) => group === b.lane.enclosing[i]) &&
  a.top === b.top &&
  a.selected === b.selected &&
  a.actions === b.actions;

const Header = memo(function Header({ lane, top, selected, actions: { onPress, onDelete } }: HeaderProps) {
  const { track, height } = lane;
  const [renaming, setRenaming] = useState(false);
  const [palette, setPalette] = useState<{ x: number; y: number } | null>(null);
  const [picker, setPicker] = useState<{ x: number; y: number } | null>(null);
  const [menu, setMenu] = useState<{ x: number; y: number } | null>(null);
  const armed = useDawg((s) => s.armed.has(track.id));
  // An output track hangs off its track while right under it; elsewhere it names it (multi-output-instruments.md, MO9).
  const sourceName = useDawg((s) => (track.kind === 'output' ? s.project?.tracks[String(track.source)]?.name : undefined));
  const attached = useDawg((s) => underSource(s.project, track));
  // A track with output tracks is only their header: no fader, pan or meter of its own (MO12).
  const header = useDawg((s) => !!s.project && outputTracksOf(s.project, track.id) > 0);
  const fold = track.kind === 'group' ? <FoldButton group={track} /> : null;
  const resize = useRef<{ startY: number; startHeight: number; gesture: string } | null>(null);
  const tier = headerTier(height);

  const openPicker = (e: React.MouseEvent) => {
    e.stopPropagation();
    const rect = e.currentTarget.getBoundingClientRect();
    setPicker({ x: rect.left, y: rect.bottom + 4 });
  };

  const name = renaming ? (
    <input
      className="track-name-input"
      autoFocus
      defaultValue={track.name}
      onBlur={() => setRenaming(false)}
      onKeyDown={(e) => {
        e.stopPropagation();
        if (e.key === 'Enter') {
          const next = (e.target as HTMLInputElement).value.trim();
          if (next && next !== track.name) dispatch({ command: 'renameTrack', args: { id: track.id, name: next } });
          setRenaming(false);
        } else if (e.key === 'Escape') {
          setRenaming(false);
        }
      }}
    />
  ) : (
    <span className="track-name" onDoubleClick={() => setRenaming(true)} title={`${track.name} (double-click to rename)`}>
      {track.kind === 'return' && <CornerDownRight size={12} className="return-icon" aria-label="Return track" />}
      {track.kind === 'output' && <Split size={12} className="return-icon" aria-label={`Output track of ${sourceName ?? 'a removed track'}`} />}
      {track.freeze &&
        (track.freeze.stale ? (
          <TriangleAlert size={12} className="frozen-icon stale" aria-label="Frozen, stale: refreeze from the track menu" />
        ) : (
          <Snowflake size={12} className="frozen-icon" aria-label="Frozen: unfreeze from the track menu to edit" />
        ))}
      {track.name}
      {track.kind === 'output' && !attached && sourceName && <span className="track-source">{sourceName}</span>}
    </span>
  );

  return (
    <div
      className={`track-header ${tier} ${height >= METER_HEIGHT ? 'with-meter' : ''} ${selected ? 'selected' : ''} ${track.kind === 'output' ? 'output-header' : ''} ${attached ? 'attached' : ''} ${header ? 'outputs-header' : ''}`}
      style={{ top, height, ...indentStyle(lane.enclosing) }}
      onPointerDown={(e) => onPress(e, track)}
      onContextMenu={(e) => {
        e.preventDefault();
        if (!selected) ui.selectTrack(track.id);
        setMenu({ x: e.clientX, y: e.clientY });
      }}
    >
      <Spines groups={lane.enclosing} />
      <button
        className="track-colour"
        style={{ background: track.colour }}
        title="Change the track colour"
        onClick={(e) => {
          e.stopPropagation();
          const rect = e.currentTarget.getBoundingClientRect();
          setPalette({ x: rect.right + 4, y: rect.top });
        }}
      />
      {palette && (
        <>
          <Menu onClose={() => setPalette(null)} at={{ x: palette.x, y: Math.min(palette.y, window.innerHeight - 140) }}>
            <div className="menu-heading">Track colour</div>
            <Swatches
              current={track.colour}
              onPick={(colour) => {
                setPalette(null);
                if (colour) dispatch({ command: 'setTrackColour', args: { id: track.id, colour } });
              }}
            />
          </Menu>
        </>
      )}
      <div className="track-header-body">
        {tier === 'compact' && (
          <div className="header-row">
            {fold}
            {name}
            <TakesButton track={track} />
            <AutomationSwitches track={track} full={false} onPick={openPicker} />
            <Switches track={track} />
          </div>
        )}
        {tier === 'standard' && (
          <>
            <div className="header-row">
              {fold}
              {name}
              <TakesButton track={track} />
              <Switches track={track} />
            </div>
            <div className="header-row">
              {!header && <GainAndPan track={track} />}
              <AutomationSwitches track={track} full onPick={openPicker} />
            </div>
          </>
        )}
        {tier === 'tall' && (
          <>
            <div className="header-row">
              {fold}
              {name}
              <TakesButton track={track} />
              <Switches track={track} />
            </div>
            <div className="header-row">
              {!header && <GainAndPan track={track} />}
              <AutomationSwitches track={track} full onPick={openPicker} />
            </div>
          </>
        )}
      </div>
      {height >= METER_HEIGHT && !header && <TrackMeter track={track} input={armed && track.kind === 'audio'} />}
      {picker && <AutomationPicker track={track} x={picker.x} y={picker.y} onClose={() => setPicker(null)} />}
      {menu && <HeaderMenu track={track} x={menu.x} y={menu.y} onDelete={onDelete} onRename={() => setRenaming(true)} onClose={() => setMenu(null)} />}
      <div
        className="track-header-resize"
        title="Drag to resize the lane"
        onPointerDown={(e) => {
          e.stopPropagation();
          e.currentTarget.setPointerCapture(e.pointerId);
          resize.current = { startY: e.clientY, startHeight: track.height, gesture: newGestureId() };
        }}
        onPointerMove={(e) => {
          const r = resize.current;
          if (!r) return;
          const next = Math.round(r.startHeight + e.clientY - r.startY);
          if (next !== track.height) dispatch({ command: 'setTrackHeight', args: { id: track.id, height: next } }, r.gesture);
        }}
        onPointerUp={() => (resize.current = null)}
      />
    </div>
  );
}, sameHeader);

/** The master track's header, pinned below the track headers: fader, pan and meter. Selecting it shows its
    insert chain in the inspector. */
export function MasterHeader({ master, selected }: { master: Track; selected: boolean }) {
  const [picker, setPicker] = useState<{ x: number; y: number } | null>(null);
  return (
    <div className={`track-header standard master-header ${selected ? 'selected' : ''}`} onPointerDown={() => ui.selectTrack(master.id)}>
      <div className="track-colour" style={{ background: master.colour }} />
      <div className="track-header-body">
        <div className="header-row">
          <span className="track-name">Master</span>
        </div>
        <div className="header-row">
          <GainAndPan track={master} />
          <AutomationSwitches
            track={master}
            full
            onPick={(e) => {
              e.stopPropagation();
              const rect = e.currentTarget.getBoundingClientRect();
              setPicker({ x: rect.left, y: Math.max(8, rect.top - 330) });
            }}
          />
        </div>
      </div>
      <TrackMeter track={master} input={false} />
      {picker && <AutomationPicker track={master} x={picker.x} y={picker.y} onClose={() => setPicker(null)} />}
    </div>
  );
}

export function TrackHeaders({ lanes, scrollY, selectedTrackId }: { lanes: Lane[]; scrollY: number; selectedTrackId: number | null }) {
  const project = useDawg((s) => s.project);
  const selectedTracks = useUi((s) => s.selectedTracks);
  const [deleting, setDeleting] = useState<Track | null>(null);
  const [drop, setDrop] = useState<DropSlot | null>(null);
  const dropRef = useRef<DropSlot | null>(null);
  const container = useRef<HTMLDivElement>(null);
  const press = useRef<{ x: number; y: number; ids: number[]; track: Track; dragging: boolean } | null>(null);
  const isSelected = (id: number) => selectedTracks.has(id) || (selectedTracks.size === 0 && id === selectedTrackId);

  // The headers in order, for Shift+click ranges: tracks and groups (by their top arm).
  const order = lanes.filter((lane) => lane.role === 'track' || lane.role === 'groupTop').map((lane) => lane.track.id);

  const onPress = (e: React.PointerEvent, track: Track) => {
    if (e.button !== 0) return;
    const { selectedTracks: current, selectedTrackId: primary } = useUi.getState();

    if (e.ctrlKey || e.metaKey) {
      ui.toggleTrack(track.id);
      return;
    }
    if (e.shiftKey && primary !== null && order.includes(primary)) {
      const [a, b] = [order.indexOf(primary), order.indexOf(track.id)].sort((x, y) => x - y);
      ui.selectTracks(order.slice(a, b + 1), track.id);
      return;
    }
    if (!current.has(track.id)) ui.selectTrack(track.id);

    // Controls keep their own drags; elsewhere a drag moves the selected tracks (R12).
    if ((e.target as HTMLElement).closest('button, input, .drag-number, .track-header-resize, .meter')) return;
    const ids = current.has(track.id) ? [...current] : [track.id];
    press.current = { x: e.clientX, y: e.clientY, ids, track, dragging: false };
  };

  useEffect(() => {
    const move = (e: PointerEvent) => {
      const p = press.current;
      if (!p || !project || !container.current) return;
      if (!p.dragging && Math.hypot(e.clientX - p.x, e.clientY - p.y) < 4) return;
      p.dragging = true;
      const y = e.clientY - container.current.getBoundingClientRect().top + scrollY;
      dropRef.current = nearestSlot(dropSlots(project, lanes, p.ids), y);
      setDrop(dropRef.current);
    };
    const up = () => {
      const p = press.current;
      press.current = null;
      if (!p) return;
      if (!p.dragging) {
        // A plain click on one of several selected tracks selects just that one.
        if (useUi.getState().selectedTracks.size > 1) ui.selectTrack(p.track.id);
        return;
      }
      const slot = dropRef.current;
      dropRef.current = null;
      setDrop(null);
      if (slot) dispatch({ command: 'moveTracks', args: { ids: p.ids, parent: slot.parent, before: slot.before } });
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
    return () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
    };
  }, [project, lanes, scrollY]);

  const current: HeaderActions = {
    // Delete then acts on the headers (removeSelected), not on what had the focus before.
    onPress: (e, track) => {
      container.current?.focus({ preventScroll: true });
      onPress(e, track);
    },
    onDelete: (track) => {
      if (track.kind === 'group' || (project && outputTracksOf(project, track.id) > 0)) setDeleting(track);
      else dispatch({ command: 'removeTrack', args: { id: track.id } });
    },
  };
  // The same object on every render, calling the latest handlers, so memoised headers aren't redrawn for it.
  const latest = useRef(current);
  latest.current = current;
  const actions = useMemo<HeaderActions>(
    () => ({ onPress: (e, track) => latest.current.onPress(e, track), onDelete: (track) => latest.current.onDelete(track) }),
    [],
  );

  /** Delete on the headers: the selected tracks go, in one step. A group asks first, so groups go one at a time. */
  const removeSelected = (e: React.KeyboardEvent) => {
    if (e.key !== 'Delete' || (e.target as HTMLElement).tagName === 'INPUT' || !project) return;
    const chosen = [...useUi.getState().selectedTracks].map((id) => project.tracks[String(id)]).filter((t) => t !== undefined);
    if (chosen.length === 0) return;
    e.preventDefault();
    const groups = chosen.filter((t) => t.kind === 'group' || outputTracksOf(project, t.id) > 0);
    if (groups.length > 0) {
      if (chosen.length === 1) setDeleting(groups[0]);
      else reportError('A group is deleted on its own, with its tracks: select just the group');
      return;
    }
    dispatch(
      chosen.length === 1
        ? { command: 'removeTrack', args: { id: chosen[0].id } }
        : { command: 'batch', args: { label: 'Remove tracks', commands: chosen.map((t) => ({ command: 'removeTrack' as const, args: { id: t.id } })) } },
    );
    // Nothing selected then: the inspector falls back to the first track.
    ui.selectTrack(null);
  };

  return (
    <div className="track-headers" ref={container} tabIndex={-1} onKeyDown={removeSelected}>
      {lanes.map((lane) =>
        lane.role === 'returnsHeading' ? (
          <ReturnsHeading key="returns" top={lane.top - scrollY} height={lane.height} />
        ) : lane.role === 'track' ? (
          <Header key={lane.track.id} lane={lane} top={lane.top - scrollY} selected={isSelected(lane.track.id)} actions={actions} />
        ) : (
          <GroupArm
            key={`${lane.role}:${lane.track.id}`}
            lane={lane}
            top={lane.top - scrollY}
            selected={isSelected(lane.track.id)}
            actions={actions}
          />
        ),
      )}
      {lanes.flatMap((lane) =>
        lane.takes.map((row) => <TakeHeader key={`t${row.track.id}:${row.lane}`} row={row} top={row.top - scrollY} groups={lane.enclosing} />),
      )}
      {lanes.flatMap((lane) =>
        lane.automation.map((row) => (
          <AutomationHeader key={`a${row.lane.id}`} row={row} top={row.top - scrollY} groups={lane.enclosing} />
        )),
      )}
      {drop && <div className="track-drop-line" style={{ top: drop.y - scrollY - 1, left: drop.depth * SPINE }} />}
      {deleting && project && (
        <DeleteGroup
          project={project}
          group={deleting}
          onDone={(confirmed) => {
            if (confirmed) dispatch({ command: 'removeTrack', args: { id: deleting.id } });
            setDeleting(null);
          }}
        />
      )}
    </div>
  );
}

/** The heading of the returns section (docs/design/groups-and-sends.md, R10), with a button for a new return. */
function ReturnsHeading({ top, height }: { top: number; height: number }) {
  return (
    <div className="returns-heading" style={{ top, height }}>
      <span>Returns</span>
      <IconButton size="s" title="Add a return track" onClick={() => dispatch({ command: 'addTrack', args: { kind: 'return' } })}>
        <Plus size={12} />
      </IconButton>
    </div>
  );
}

function DeleteGroup({ project, group, onDone }: { project: Project; group: Track; onDone: (confirmed: boolean) => void }) {
  if (group.kind !== 'group') {
    // A track with output tracks takes them along (docs/design/multi-output-instruments.md, MO11).
    const outputs = outputTracksOf(project, group.id);
    return (
      <ConfirmDialog
        title={`Delete “${group.name}”?`}
        message={`Its ${outputs === 1 ? 'output track' : `${outputs} output tracks`} will be deleted with it.`}
        confirmLabel={`Delete track and ${outputs === 1 ? '1 output track' : `${outputs} output tracks`}`}
        onDone={onDone}
      />
    );
  }
  const count = tracksInside(project, group.id);
  return (
    <ConfirmDialog
      title={`Delete the group “${group.name}”?`}
      message={
        count === 0
          ? 'The group is empty.'
          : `Its ${count === 1 ? 'track' : `${count} tracks`} will be deleted with it. Ungroup first to keep ${count === 1 ? 'it' : 'them'}.`
      }
      confirmLabel={count === 0 ? 'Delete group' : `Delete group and ${count === 1 ? '1 track' : `${count} tracks`}`}
      onDone={onDone}
    />
  );
}
