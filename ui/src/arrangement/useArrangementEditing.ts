import { useEffect, useRef, useState } from 'react';
import { sequencerOf } from '../dock/stepGrid';

/** On a track with a step sequencer, new clips are its clips, playing pattern A (docs/design/step-sequencer.md, SS2). */
const sequencerArgs = (track: Track) => (sequencerOf(track) ? { sequencer: { pattern: 0 } } : {});
import { type Clip, type Command, type Project, type Track } from '../bridge/protocol';
import { xToTick, type View } from '../pianoroll/geometry';
import type { TempoMap } from '../time/tempoMap';
import { audioTime, isMusicalClip } from '../timeline/audioTime';
import { dispatch, useDawg } from '../store/store';
import { ui } from '../store/uiStore';
import * as bridge from '../bridge/bridge';
import { clipEnd, isAudio, isLooped, resizedClip } from '../timeline/clips';
import type { ClipDraw } from './draw';
import { clipBox, clipLanes, clipsInRect, hitClip, hitTake, laneAt, NAME_BAR, rowAsLane, takeRowAt, type Lane } from './layout';
import { crossfadeTails } from '../timeline/crossfades';
import { selectionAfterDelete } from './clipSelection';
import { coveredSpans, playedClipsOf } from '../timeline/takes';

// Clip editing in the arrangement, the same Cubase-style scheme as the piano roll
// (docs/design/phase6-arrangement.md, section 7):
//   click clip             select (Shift toggles)
//   drag clip              move the selection, also to other tracks (Ctrl/Cmd places it off the grid; Shift locks
//                          the direction)
//   Alt+drag clip          copy the selection (Alt+Shift: linked copies)
//   Alt+click clip         split it at the pointer
//   drag clip edge         resize (the start edge keeps the content in place)
//   drag top-right corner  loop: extend the clip by repeating its content
//   drag empty space       box selection (Shift adds)
//   Alt+click empty space  create a one-bar clip (inside an active loop: the loop range); keep dragging right to set its
//                          length
//   double-click clip      open it in the piano roll (on empty space: create a one-bar clip)
// Edits are previewed locally and committed as one command on release.

const DRAG_PX = 4;

type Drag =
  | {
      kind: 'move';
      ids: number[];
      startX: number;
      startY: number;
      originTick: number;
      /** The grabbed clip's start: the move snaps where it lands, so clips stay on the grid across bar lengths. */
      grabbedStart: number;
      originLane: number;
      copy: boolean;
      linked: boolean;
      moved: boolean;
      split: { clip: Clip; at: number } | null;
      delta: number;
      trackDelta: number;
    }
  | { kind: 'resize'; clip: Clip; edge: 'start' | 'end'; to: number }
  /** Alt + drag of a musical clip's right edge: the audio stretches to the new length (time-stretching.md, TS5). */
  | { kind: 'stretch'; clip: Clip; to: number }
  | { kind: 'loop'; clip: Clip; to: number }
  | { kind: 'fade'; clip: Clip; edge: 'in' | 'out'; length: number }
  | { kind: 'gain'; clip: Clip; startY: number; gainDb: number }
  | { kind: 'box'; x0: number; y0: number; x1: number; y1: number; additive: boolean; before: ReadonlySet<number> }
  | { kind: 'create'; lane: Lane; start: number; length: number; startX: number; sized: boolean }
  /** Picking a take in a takes row (T6): a click picks the whole clip, a drag across it the part dragged over. */
  | { kind: 'comp'; clip: Clip; lane: Lane; x0: number; x1: number; moved: boolean };

interface Options {
  project: Project | null;
  lanes: Lane[];
  view: View;
  scrollY: number;
  gridTicks: number;
  map: TempoMap;
  selected: ReadonlySet<number>;
}

/** Clips copied with Ctrl+C / Ctrl+X: full data, so pasting works after the originals are gone. */
let clipboard: { clips: Array<{ laneIndex: number; clip: Clip }>; start: number; firstLane: number } | null = null;

/** Deletes clips and selects the next clip on each track that lost one (docs/design/phase6-arrangement.md, 7.3). */
export function deleteClips(ids: number[]) {
  const project = useDawg.getState().project;
  if (ids.length === 0 || project === null) return;
  ui.selectClips(selectionAfterDelete(Object.values(project.tracks), ids));
  dispatch({ command: 'deleteClips', args: { ids } });
}

export function useArrangementEditing({ project, lanes, view, scrollY, gridTicks, map, selected }: Options) {
  const rev = useDawg((s) => s.rev);
  const [drag, setDragState] = useState<Drag | null>(null);
  const dragRef = useRef<Drag | null>(null);
  const setDrag = (d: Drag | null) => {
    dragRef.current = d;
    setDragState(d ? { ...d } : null);
  };
  const [cursor, setCursor] = useState('default');
  const selectNewAfter = useRef<{ ids: ReadonlySet<number>; rev: number } | null>(null);

  useEffect(() => {
    const pending = selectNewAfter.current;
    if (!pending || rev <= pending.rev) return;
    selectNewAfter.current = null;
    const created = lanes
      .flatMap((lane) => Object.values(lane.track.clips))
      .filter((clip) => !pending.ids.has(clip.id));
    if (created.length > 0) ui.selectClips(created.map((clip) => clip.id));
  }, [rev, lanes]);

  const duplicate = (ids: number[], linked = false) => {
    if (ids.length === 0) return;
    selectNewAfter.current = {
      ids: new Set(lanes.flatMap((lane) => Object.values(lane.track.clips)).map((clip) => clip.id)),
      rev: useDawg.getState().rev,
    };
    dispatch({ command: 'duplicateClips', args: { ids, linked } });
  };

  /** Where a new clip goes for a click at `tick`: the loop range, when it's on and the click is inside it; else the
      bar under the pointer. */
  const newClipAt = (tick: number) => {
    const loop = project?.loop;
    if (loop?.enabled && tick >= loop.start && tick < loop.end) return { start: loop.start, length: loop.end - loop.start };
    const start = map.barFloor(tick);
    return { start, length: map.barLengthAt(start) };
  };

  const snapTick = (tick: number, free: boolean) => (free ? Math.round(tick) : map.snap(tick, gridTicks, true));

  const onPointerDown = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (e.button !== 0 || !project) return;
    e.currentTarget.setPointerCapture(e.pointerId);
    e.currentTarget.focus({ preventScroll: true });
    const { offsetX: x, offsetY: y } = e.nativeEvent;
    const tick = Math.max(0, xToTick(view, x));
    // A takes row: a press picks the take under it (docs/design/takes-lanes.md, T6).
    const take = hitTake(view, lanes, x, y, scrollY);
    if (take) {
      ui.selectTrack(take.row.track.id);
      ui.selectClips([take.clip.id]);
      setDrag({ kind: 'comp', clip: take.clip, lane: rowAsLane(take.lane, take.row), x0: x, x1: x, moved: false });
      return;
    }
    if (takeRowAt(lanes, y, scrollY)) return;

    const hit = hitClip(view, lanes, x, y, scrollY, map);
    const lane = laneAt(lanes, y, scrollY);
    if (lane) ui.selectTrack(lane.track.id);

    if (!hit) {
      if (e.altKey && lane && lane.clipIndex >= 0) {
        // Alt+click on empty space: a clip (newClipAt), or as long as the drag that follows.
        const { start, length } = newClipAt(tick);
        setDrag({ kind: 'create', lane, start, length, startX: x, sized: false });
        return;
      }
      setDrag({ kind: 'box', x0: x, y0: y, x1: x, y1: y, additive: e.shiftKey, before: selected });
      if (!e.shiftKey) ui.selectClips([]);
      return;
    }

    if (hit.zone === 'end' && e.altKey && hit.clip.fileId !== 0) {
      ui.selectClips([hit.clip.id]);
      // Only musical files stretch; nothing is made musical by itself.
      if (isMusicalClip(hit.clip)) setDrag({ kind: 'stretch', clip: hit.clip, to: clipEnd(hit.clip) });
      else useDawg.setState({ lastHint: 'Only musical files stretch: make the file musical first (right-click the clip, Make musical).' });
      return;
    }

    if (hit.zone === 'start' || hit.zone === 'end') {
      ui.selectClips([hit.clip.id]);
      setDrag({ kind: 'resize', clip: hit.clip, edge: hit.zone, to: hit.zone === 'start' ? hit.clip.start : clipEnd(hit.clip) });
      return;
    }

    if (hit.zone === 'fadeIn' || hit.zone === 'fadeOut') {
      ui.selectClips([hit.clip.id]);
      const edge = hit.zone === 'fadeIn' ? 'in' : 'out';
      setDrag({ kind: 'fade', clip: hit.clip, edge, length: edge === 'in' ? hit.clip.fadeIn : hit.clip.fadeOut });
      return;
    }

    if (hit.zone === 'gain') {
      ui.selectClips([hit.clip.id]);
      setDrag({ kind: 'gain', clip: hit.clip, startY: y, gainDb: hit.clip.gainDb });
      return;
    }

    if (hit.zone === 'loop') {
      ui.selectClips([hit.clip.id]);
      setDrag({ kind: 'loop', clip: hit.clip, to: clipEnd(hit.clip) });
      return;
    }

    // Body: select, then move (or copy with Alt; a plain Alt+click splits).
    let ids: number[];
    if (e.shiftKey && !e.altKey) {
      const next = new Set(selected);
      if (next.has(hit.clip.id)) next.delete(hit.clip.id);
      else next.add(hit.clip.id);
      ui.selectClips(next);
      ids = [...next];
    } else {
      ids = selected.has(hit.clip.id) ? [...selected] : [hit.clip.id];
      if (!selected.has(hit.clip.id)) ui.selectClips(ids);
    }

    setDrag({
      kind: 'move',
      ids,
      startX: x,
      startY: y,
      originTick: tick,
      grabbedStart: hit.clip.start,
      originLane: hit.lane.clipIndex,
      copy: e.altKey,
      linked: e.altKey && e.shiftKey,
      moved: false,
      split: e.altKey ? { clip: hit.clip, at: snapTick(tick, e.ctrlKey || e.metaKey) } : null,
      delta: 0,
      trackDelta: 0,
    });
  };

  const onPointerMove = (e: React.PointerEvent<HTMLCanvasElement>) => {
    const { offsetX: x, offsetY: y } = e.nativeEvent;
    const tick = Math.max(0, xToTick(view, x));
    const d = dragRef.current;

    if (!d) {
      if (hitTake(view, lanes, x, y, scrollY)) return setCursor('pointer');
      const hit = hitClip(view, lanes, x, y, scrollY, map);
      const cursors: Record<string, string> = { loop: 'alias', body: 'grab', gain: 'ns-resize' };
      if (hit?.zone === 'end' && e.altKey && hit.clip.fileId !== 0) return setCursor(isMusicalClip(hit.clip) ? 'col-resize' : 'not-allowed');
      setCursor(!hit ? 'default' : (cursors[hit.zone] ?? 'ew-resize'));
      return;
    }

    switch (d.kind) {
      case 'comp':
        setDrag({ ...d, x1: x, moved: d.moved || Math.abs(x - d.x0) >= DRAG_PX });
        break;

      case 'create': {
        if (!d.sized && Math.abs(x - d.startX) < DRAG_PX) return;
        const end = snapTick(tick, e.ctrlKey || e.metaKey);
        setDrag({ ...d, sized: true, length: Math.max(gridTicks, end - d.start) });
        break;
      }

      case 'box':
        setDrag({ ...d, x1: x, y1: y });
        ui.selectClips([...(d.additive ? d.before : []), ...clipsInRect(view, lanes, scrollY, d.x0, d.y0, x, y)]);
        break;

      case 'resize': {
        const to = snapTick(tick, e.ctrlKey || e.metaKey);
        setDrag({ ...d, to: d.edge === 'start' ? Math.min(to, clipEnd(d.clip) - 1) : Math.max(to, d.clip.start + 1) });
        break;
      }

      case 'loop':
        setDrag({ ...d, to: Math.max(d.clip.start + 1, snapTick(tick, e.ctrlKey || e.metaKey)) });
        break;

      case 'stretch':
        setDrag({ ...d, to: Math.max(d.clip.start + 1, snapTick(tick, e.ctrlKey || e.metaKey)) });
        break;

      case 'fade': {
        // Fades are free (no grid), never overlap each other, and are content ticks (timeline/audioTime.ts).
        const time = audioTime(d.clip, map);
        const other = d.edge === 'in' ? d.clip.fadeOut : d.clip.fadeIn;
        const raw = d.edge === 'in' ? time.localAt(tick) : time.localLength - time.localAt(tick);
        setDrag({ ...d, length: Math.round(Math.max(0, Math.min(time.localLength - other, raw))) });
        break;
      }

      case 'gain': {
        const db = d.clip.gainDb + (d.startY - y) * (e.shiftKey ? 0.05 : 0.25);
        setDrag({ ...d, gainDb: Math.round(Math.max(-60, Math.min(12, db)) * 10) / 10 });
        break;
      }

      case 'move': {
        if (!d.moved && Math.hypot(x - d.startX, y - d.startY) < DRAG_PX) return;
        const lane = laneAt(lanes, y, scrollY);
        let delta = snapTick(d.grabbedStart + tick - d.originTick, e.ctrlKey || e.metaKey) - d.grabbedStart;
        // Tracks are counted in lanes that hold clips, so groups in between are stepped over.
        let trackDelta = lane && lane.clipIndex >= 0 ? lane.clipIndex - d.originLane : d.trackDelta;

        // Shift locks the drag to time or to tracks, whichever moved more.
        if (e.shiftKey && !e.altKey) {
          if (Math.abs(x - d.startX) > Math.abs(y - d.startY)) trackDelta = 0;
          else delta = 0;
        }

        // Keep every moved clip on an existing track and after the timeline start.
        const moving = movingClips(d.ids);
        const minLane = Math.min(...moving.map((m) => m.lane.clipIndex));
        const maxLane = Math.max(...moving.map((m) => m.lane.clipIndex));
        trackDelta = Math.max(-minLane, Math.min(clipLanes(lanes).length - 1 - maxLane, trackDelta));
        delta = Math.max(-Math.min(...moving.map((m) => m.clip.start)), delta);
        setDrag({ ...d, moved: true, delta, trackDelta });
        break;
      }
    }
  };

  const onPointerUp = () => {
    const d = dragRef.current;
    setDrag(null);
    if (!d) return;

    switch (d.kind) {
      case 'comp': {
        if (!d.moved) {
          dispatch({ command: 'bringToTop', args: { id: d.clip.id } });
          break;
        }
        // The part dragged over, on the grid (it can't reach past the clip).
        const [a, b] = [Math.min(d.x0, d.x1), Math.max(d.x0, d.x1)];
        const from = Math.max(d.clip.start, snapTick(Math.max(0, xToTick(view, a)), false));
        const to = Math.min(clipEnd(d.clip), snapTick(Math.max(0, xToTick(view, b)), false));
        if (to > from) dispatch({ command: 'bringToTop', args: { id: d.clip.id, from, to } });
        break;
      }

      case 'create':
        dispatch({ command: 'addClip', args: { trackId: d.lane.track.id, start: d.start, length: d.length, ...sequencerArgs(d.lane.track) } });
        break;

      case 'resize':
        if (d.to !== (d.edge === 'start' ? d.clip.start : clipEnd(d.clip)))
          dispatch({ command: 'resizeClip', args: { id: d.clip.id, edge: d.edge, to: d.to } });
        break;

      case 'stretch':
        if (d.to !== clipEnd(d.clip)) dispatch({ command: 'stretchClip', args: { clipId: d.clip.id, length: d.to - d.clip.start } });
        break;

      case 'loop': {
        if (d.to === clipEnd(d.clip)) break;
        const commands: Command[] = [];
        if (!isLooped(d.clip)) commands.push({ command: 'setClipLoop', args: { id: d.clip.id, loopLength: d.clip.length } });
        commands.push({ command: 'resizeClip', args: { id: d.clip.id, edge: 'end', to: d.to } });
        dispatch({ command: 'batch', args: { label: 'Loop clip', commands } });
        break;
      }

      case 'move':
        if (!d.moved) {
          if (d.split && d.split.at > d.split.clip.start && d.split.at < clipEnd(d.split.clip))
            dispatch({ command: 'splitClips', args: { ids: [d.split.clip.id], at: d.split.at } });
        } else if (d.delta !== 0 || d.trackDelta !== 0 || d.copy) {
          // Name each clip's destination track: groups and folded tracks lie between the lanes.
          const targets = clipLanes(lanes);
          const trackIds = Object.fromEntries(
            movingClips(d.ids).map(({ clip, lane }) => [String(clip.id), targets[lane.clipIndex + d.trackDelta].track.id]),
          );
          if (d.copy)
            selectNewAfter.current = {
              ids: new Set(lanes.flatMap((lane) => Object.values(lane.track.clips)).map((clip) => clip.id)),
              rev: useDawg.getState().rev,
            };
          dispatch({
            command: 'moveClips',
            args: { ids: d.ids, delta: d.delta, trackIds, copy: d.copy, linked: d.linked },
          });
        }
        break;

      case 'fade':
        if (d.length !== (d.edge === 'in' ? d.clip.fadeIn : d.clip.fadeOut))
          dispatch({ command: 'setClipProps', args: { id: d.clip.id, [d.edge === 'in' ? 'fadeIn' : 'fadeOut']: d.length } });
        break;

      case 'gain':
        if (d.gainDb !== d.clip.gainDb) dispatch({ command: 'setClipProps', args: { id: d.clip.id, gainDb: d.gainDb } });
        break;

      case 'box':
        break;
    }
  };

  // Double-click: on the name bar renames, elsewhere opens the clip in the piano roll, and on empty space makes a clip.
  const onDoubleClick = (e: React.MouseEvent<HTMLCanvasElement>, onRename: (clipId: number) => void) => {
    const { offsetX: x, offsetY: y } = e.nativeEvent;
    // A take opens like a clip.
    const take = hitTake(view, lanes, x, y, scrollY);
    if (take) {
      if (take.clip.fileId !== 0) openAudioEditor(take.clip.id);
      else ui.editClip(take.row.track.id, take.clip.id);
      return;
    }
    const hit = hitClip(view, lanes, x, y, scrollY, map);
    if (!hit) {
      // On empty space of an instrument track, as Alt+click: a clip (newClipAt).
      const lane = laneAt(lanes, y, scrollY);
      if (lane && lane.clipIndex >= 0 && lane.track.kind === 'instrument')
        dispatch({ command: 'addClip', args: { trackId: lane.track.id, ...newClipAt(Math.max(0, xToTick(view, x))), ...sequencerArgs(lane.track) } });
      return;
    }
    if (y <= clipBox(view, hit.lane, hit.clip, scrollY).y0 + NAME_BAR) onRename(hit.clip.id);
    else if (hit.clip.fileId !== 0) openAudioEditor(hit.clip.id);
    // A sequencer clip is edited in its track's step sequencer, in the device strip (docs/design/step-sequencer.md, SS9).
    else if (hit.clip.sequencerPattern >= 0) ui.selectTrack(hit.lane.track.id);
    else ui.editClip(hit.lane.track.id, hit.clip.id);
  };

  function movingClips(ids: number[]) {
    const result: Array<{ clip: Clip; lane: Lane }> = [];
    for (const lane of lanes)
      for (const clip of Object.values(lane.track.clips)) if (ids.includes(clip.id)) result.push({ clip, lane });
    return result;
  }

  /** What to draw: clips with the drag preview applied. */
  const displayClips = (): ClipDraw[] => {
    const d = drag;
    const result: ClipDraw[] = [];

    for (const lane of lanes) {
      // The track's row shows what plays (docs/design/takes-lanes.md, T8); a clip being edited shows whole.
      const edited = new Set<number>();
      for (const part of playedClipsOf(lane.track)) {
        const whole = lane.track.clips[String(part.id)] ?? part;
        const editing = d && d.kind !== 'move' && d.kind !== 'box' && d.kind !== 'create' && d.kind !== 'comp' && d.clip.id === part.id;
        if (editing && edited.has(part.id)) continue;
        if (editing) edited.add(part.id);
        const clip = editing ? whole : part;
        if (d?.kind === 'move' && d.moved && d.ids.includes(clip.id)) {
          if (d.copy) result.push({ clip, lane });
          else result.push({ clip, lane, faded: true });
          continue;
        }
        if (d?.kind === 'resize' && d.clip.id === clip.id) {
          result.push({ clip: resizedClip(clip, d.edge, d.to), lane });
          continue;
        }
        if (d?.kind === 'stretch' && d.clip.id === clip.id) {
          const ratio = (d.to - clip.start) / clip.length;
          const sourceBpm = project?.audioFiles[String(clip.fileId)]?.timing.sourceBpm ?? 120;
          result.push({ clip: { ...clip, length: d.to - clip.start }, lane, readout: `×${ratio.toFixed(3)} · ${(sourceBpm * ratio).toFixed(2)} BPM` });
          continue;
        }
        if (d?.kind === 'fade' && d.clip.id === clip.id) {
          const faded = { ...clip, [d.edge === 'in' ? 'fadeIn' : 'fadeOut']: d.length };
          result.push({ clip: faded, lane, crossfades: crossfadeTails({ ...lane.track, clips: { ...lane.track.clips, [String(clip.id)]: faded } }, faded) });
          continue;
        }
        if (d?.kind === 'gain' && d.clip.id === clip.id) {
          result.push({ clip: { ...clip, gainDb: d.gainDb }, lane, showGain: true });
          continue;
        }
        if (d?.kind === 'loop' && d.clip.id === clip.id) {
          const looped = isLooped(clip) ? clip : { ...clip, loopStart: clip.offset, loopLength: clip.length };
          result.push({ clip: { ...looped, length: d.to - clip.start }, lane });
          continue;
        }
        // A frozen track's clips are drawn faded: they don't play (docs/design/audio-features.md, AU15).
        result.push({ clip, lane, faded: lane.track.freeze ? true : undefined, crossfades: isAudio(clip) ? crossfadeTails(lane.track, clip) : undefined });
      }

      // Its takes rows: every clip of each lane, with what's covered dimmed.
      for (const row of lane.takes) {
        const rowLane = rowAsLane(lane, row);
        for (const clip of Object.values(lane.track.clips))
          if (clip.lane === row.lane) result.push({ clip, lane: rowLane, covered: coveredSpans(lane.track, clip) });
      }
    }

    if (d?.kind === 'create') result.push({ clip: { ...NEW_CLIP, start: d.start, length: d.length }, lane: d.lane });

    if (d?.kind === 'move' && d.moved)
      for (const { clip, lane } of movingClips(d.ids))
        result.push({ clip: { ...clip, start: clip.start + d.delta }, lane: clipLanes(lanes)[lane.clipIndex + d.trackDelta] });

    return result;
  };

  // ---------------------------------------------------------------------------------------------------
  // Keyboard

  const selectedClips = () =>
    clipLanes(lanes).flatMap((lane) =>
      Object.values(lane.track.clips)
        .filter((c) => selected.has(c.id))
        .map((clip) => ({ clip, laneIndex: lane.clipIndex })),
    );

  const copySelection = () => {
    const items = selectedClips();
    if (items.length === 0) return false;
    clipboard = {
      clips: items,
      start: Math.min(...items.map((i) => i.clip.start)),
      firstLane: Math.min(...items.map((i) => i.laneIndex)),
    };
    return true;
  };

  const paste = (playheadTick: number, targetLane: number) => {
    if (!clipboard) return;
    const commands: Command[] = clipboard.clips.flatMap(({ clip, laneIndex }) => {
      const lane = clipLanes(lanes)[targetLane + laneIndex - clipboard!.firstLane];
      if (!lane) return [];
      return [
        {
          command: 'addClip',
          args: {
            trackId: lane.track.id,
            start: Math.round(playheadTick + clip.start - clipboard!.start),
            length: clip.length,
            name: clip.name,
            colour: clip.colour,
            offset: clip.offset,
            ...(clip.sequencerPattern >= 0 ? { sequencer: { pattern: clip.sequencerPattern, startStep: clip.sequencerStartStep } } : {}),
            loopStart: clip.loopStart,
            loopLength: clip.loopLength,
            notes: Object.values(clip.notes).map((n) => ({
              start: n.start,
              length: n.length,
              pitch: n.pitch,
              velocity: n.velocity,
              releaseVelocity: n.releaseVelocity,
              expression: n.expression,
              vibrato: n.vibrato,
            })),
          },
        } as Command,
      ];
    });
    if (commands.length > 0) dispatch({ command: 'batch', args: { label: 'Paste clips', commands } });
  };

  const onKeyDown = (e: React.KeyboardEvent, playheadTick: number, selectedLane: number, onRename: () => void) => {
    const mod = e.ctrlKey || e.metaKey;
    const ids = [...selected];
    const key = e.key.toLowerCase();
    const chosen = selectedClips();

    if ((e.key === 'Delete' || e.key === 'Backspace') && ids.length > 0) {
      deleteClips(ids);
    } else if (mod && !e.shiftKey && key === 'd' && ids.length > 0) {
      // Ctrl+D copies, Ctrl+Alt+D makes linked copies (Ctrl+Shift+D duplicates the track: App.tsx).
      duplicate(ids, e.altKey);
    } else if (mod && key === 'l' && chosen.length > 0) {
      // Toggle looping: loop the current length, or remove the loop when every selected clip loops.
      const allLooped = chosen.every((c) => isLooped(c.clip));
      dispatch({
        command: 'batch',
        args: {
          label: allLooped ? 'Unloop clips' : 'Loop clips',
          commands: chosen.map(({ clip }) => ({
            command: 'setClipLoop' as const,
            args: { id: clip.id, loopLength: allLooped ? 0 : isLooped(clip) ? clip.loopLength : clip.length },
          })),
        },
      });
    } else if (e.key === 'F2' && ids.length > 0) {
      onRename();
    } else if (e.key === 'Enter' && chosen.length === 1) {
      if (chosen[0].clip.fileId !== 0) openAudioEditor(chosen[0].clip.id);
      else ui.editClip(clipLanes(lanes)[chosen[0].laneIndex].track.id, chosen[0].clip.id);
    } else if (mod && key === 'j' && ids.length > 1) {
      dispatch({ command: 'glueClips', args: { ids } });
    } else if (mod && key === 'a') {
      ui.selectClips(lanes.flatMap((l) => Object.values(l.track.clips).map((c) => c.id)));
    } else if (mod && key === 'c') {
      copySelection();
    } else if (mod && key === 'x') {
      if (copySelection()) dispatch({ command: 'deleteClips', args: { ids } });
    } else if (mod && key === 'v') {
      paste(playheadTick, Math.max(0, selectedLane));
    } else if (!mod && key === 's' && ids.length > 0) {
      // At the playhead, as the clip menu's Split at playhead does; Alt+click splits at the pointer.
      dispatch({ command: 'splitClips', args: { ids, at: Math.round(playheadTick) } });
    } else if (e.key === 'Escape') {
      ui.selectClips([]);
    } else {
      return;
    }
    e.preventDefault();
  };

  const box = drag?.kind === 'box' ? drag : null;
  const comp = drag?.kind === 'comp' && drag.moved ? drag : null;
  return {
    onPointerDown,
    onPointerMove,
    onPointerUp,
    onDoubleClick,
    onKeyDown,
    displayClips,
    cursor,
    box,
    comp,
    copySelection,
    duplicate,
    paste,
    hasClipboard: () => clipboard !== null,
  };
}

/** What a clip being created is drawn as, until the engine makes it. */
const NEW_CLIP: Clip = {
  id: -1,
  start: 0,
  length: 0,
  offset: 0,
  loopStart: 0,
  loopLength: 0,
  name: '',
  colour: '',
  linkId: 0,
  fileId: 0,
  gainDb: 0,
  fadeIn: 0,
  fadeOut: 0,
  fadeInShape: 0,
  fadeOutShape: 0,
  transpose: 0,
  lane: 0,
  order: 0,
  sequencerPattern: -1,
  sequencerStartStep: 0,
  controllers: {},
  notes: {},
};

/** Double-click on an audio clip: selects it (the inspector shows it) and opens it in the audio editor window
    (docs/design/time-stretching.md, TS13). */
function openAudioEditor(clipId: number) {
  ui.selectClips([clipId]);
  bridge.call('audioEditor.show', { open: true, clip: clipId }).catch(() => {});
}
