import { Pause, Play } from 'lucide-react';
import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import * as bridge from '../bridge/bridge';
import { TICKS_PER_BEAT, type AudioFile, type Clip, type Command, type CurvePoint, type PitchAnalysisReply, type PitchNote } from '../bridge/protocol';
import { TimingSection } from '../components/TimingSection';
import { useWindowDrag, WindowControls } from '../components/WindowControls';
import { readTheme } from '../canvas/theme';
import { tickToX, xToTick, type View } from '../pianoroll/geometry';
import { useCanvas } from '../canvas/useCanvas';
import { actions, dispatch, newGestureId, useDawg } from '../store/store';
import { tempoMapOf } from '../time/tempoMap';
import { usePeaksVersion } from '../timeline/peaks';
import { holdRenders } from '../timeline/stretchHold';
import { initialAudioClip } from '../window';
import { clipRangeOn, clipsOfFile, editorAxis, snapToOnset } from './axis';
import { CLIPS_STRIP, drawEditor, gridFor, MARKER_HIT_PX, markerX, RULER_HEIGHT } from './draw';
import { drawPitchView, pitchLayout, tiltHandles, TILT_HANDLE_PX, VIBRATO_HANDLE_H, VIBRATO_HANDLE_W, vibratoHandle, viewNotes } from './drawPitch';
import { pivotsOf, tiltShape, tiltSpan, withNodeAt } from './pitchShapes';
import { deletePoints, hitCurve, movePoints, type CurveMapping } from '../pianoroll/curveEditing';
import { measureCentre, noteEdited, pitchCorrection, type Track } from './pitchCurve';
import { getOnsets, useOnsetsVersion } from './onsets';
import { getPitchAnalysis, usePitchVersion } from './pitch';
import { Button, IconButton, Segmented } from '../ui/controls';

// The audio editor (docs/design/time-stretching.md, TS13): a clip's whole file drawn large in the arrangement's bars,
// with its transients and its timing markers, which are placed and dragged here. Its Pitch view
// shows the notes found in the file's pitch (docs/design/pitch-correction.md), read-only for now.

type Mode = 'timing' | 'pitch';

type Drag =
  | { kind: 'markers'; indices: number[]; ticks: number[]; grabTick: number; pointerTick: number; gesture: string }
  | { kind: 'slide'; index: number; frame: number; rate: number; pointerTick: number; gesture: string }
  | { kind: 'box'; fromX: number; toX: number; additive: boolean }
  | { kind: 'notes'; ids: number[]; targets: number[]; fromY: number; gesture: string }
  | { kind: 'node'; noteId: number; points: CurvePoint[]; centre: number; index: number; gesture: string }
  | { kind: 'edge'; frame: number; content: number; grabTick: number; needsMarker: boolean; gesture: string }
  | { kind: 'tilt'; noteId: number; points: CurvePoint[]; centre: number; fixed: number; moving: number; fromY: number; gesture: string }
  | { kind: 'vibrato'; ids: number[]; from: number[]; fromY: number; gesture: string }
  | { kind: 'scrub' };

const SNAP_PX = 6;
/** How near a note's edge a press moves its timing rather than its pitch. */
const EDGE_PX = 4;
/** How far the pointer moves before a press on a note becomes a drag. */
const DRAG_PX = 4;
/** How far a vibrato handle is dragged for 100 % of vibrato. */
const VIBRATO_PX = 80;

/** The clip the editor shows: the window's clip, then whichever the main window opens here next. */
function useEditedClip() {
  const [clipId, setClipId] = useState<number | null>(initialAudioClip);
  useEffect(() => bridge.on('dawg_audioEditorClip', ({ clip }) => setClipId(clip)), []);
  const project = useDawg((s) => s.project);
  let found: Clip | null = null;
  for (const track of Object.values(project?.tracks ?? {})) if (clipId !== null && track.clips[String(clipId)]) found = track.clips[String(clipId)];
  return found;
}

export function AudioEditor() {
  const clip = useEditedClip();
  const project = useDawg((s) => s.project);
  const playing = useDawg((s) => s.telemetry.playing);
  const positionBeats = useDawg((s) => s.telemetry.positionBeats);
  const file = clip ? project?.audioFiles[String(clip.fileId)] : undefined;
  const map = useMemo(() => tempoMapOf(project), [project]);
  const canvas = useCanvas();
  const startWindowDrag = useWindowDrag();
  useOnsetsVersion((s) => s.version);
  usePitchVersion((s) => s.version);
  usePeaksVersion((s) => s.version);
  const [mode, setMode] = useState<Mode>('timing');
  const [audition, setAudition] = useState(true);
  // What a press would do where the pointer is, shown over the canvas.
  const [hint, setHint] = useState('');

  const [view, setView] = useState<View>({ startTick: 0, pxPerTick: 0.01, topPitch: 0, keyHeight: 1 });
  const [selected, setSelected] = useState<ReadonlySet<number>>(new Set());
  const [selectedNotes, setSelectedNotes] = useState<ReadonlySet<number>>(new Set());
  const drag = useRef<Drag | null>(null);
  // Where the press began, and whether it has moved far enough to be a drag rather than a click.
  const press = useRef({ x: 0, y: 0, moved: false });
  const [, redraw] = useState(0);
  const fitted = useRef<number | null>(null);

  const axis = useMemo(() => (clip && file ? editorAxis(clip, file, map) : null), [clip, file, map]);
  const onsets = file ? getOnsets(file.id) : undefined;
  // The analysis runs the first time the Pitch view asks for it, never on import (section 3.6).
  const pitch = file && mode === 'pitch' ? getPitchAnalysis(file.id) : undefined;

  // Notes that went away (merged into a neighbour, or the file re-seeded) leave the selection.
  useEffect(() => {
    if (!file) return;
    setSelectedNotes((current) => {
      const kept = [...current].filter((id) => file.pitch.notes.some((n) => n.id === id));
      return kept.length === current.size ? current : new Set(kept);
    });
  }, [file]);

  // The analysis' pitch as a track, for the corrected line and for measuring split notes' centres.
  const track = useMemo<Track | undefined>(
    () => (pitch?.pitch ? { start: pitch.start ?? 0, hop: pitch.hop ?? 0.02, pitch: pitch.pitch } : undefined),
    [pitch],
  );
  // The corrected pitch (section 4.3, the TypeScript twin), redrawn live as notes move.
  const corrected = useMemo(() => {
    if (!file || !track || !pitch?.sampleRate || !file.pitch.notes.some(noteEdited))
      return null;
    // One pass over the whole track: the values are a whole number of frames apart.
    const rate = pitch.sampleRate;
    const delta = pitchCorrection(file.pitch.notes, rate, Math.round(track.start * rate), track.pitch.length, Math.round(track.hop * rate), track);
    return track.pitch.map((p, i) => (p === null ? null : p + delta[i]));
  }, [file, track, pitch]);

  // The first time the analysis is there, its notes become the file's (section 4.2), so they can be edited.
  const seededFile = useRef<number | null>(null);
  useEffect(() => {
    if (!file || !pitch?.notes || pitch.error || file.pitch.notes.length > 0 || seededFile.current === file.id) return;
    seededFile.current = file.id;
    dispatch({
      command: 'seedPitchNotes',
      args: {
        fileId: file.id,
        analyser: pitch.analyser ?? '',
        notes: pitch.notes.map(({ leadStart, start, end, centre, shape }) => ({ leadStart, start, end, centre, shape })),
      },
    });
  }, [file, pitch]);

  // Show the whole file around the clip when it first opens.
  useLayoutEffect(() => {
    if (!clip || !file || !axis || canvas.size.width === 0 || fitted.current === clip.id) return;
    fitted.current = clip.id;
    const first = Math.min(clip.start, axis.frameToTick(0));
    const last = Math.max(clip.start + clip.length, axis.frameToTick(file.frames));
    const pad = (last - first) * 0.04;
    setView((v) => ({ ...v, startTick: first - pad, pxPerTick: canvas.size.width / Math.max(1, last - first + 2 * pad) }));
    setSelected(new Set());
  }, [clip, file, axis, canvas.size.width]);

  useEffect(() => {
    const ctx = canvas.context();
    if (!ctx || !clip || !file || !axis) return;
    const d = drag.current;
    if (mode === 'pitch') {
      const playheadTick = positionBeats * TICKS_PER_BEAT;
      drawPitchView(ctx, canvas.size, {
        view,
        map,
        theme: readTheme(),
        file,
        clip,
        axis,
        playheadTick,
        gridTicks: gridFor(view),
        analysis: pitch,
        notes: viewNotes(file, pitch),
        selected: selectedNotes,
        corrected,
        box: d?.kind === 'box' ? [d.fromX, d.toX] : null,
        adjustingVibrato: d?.kind === 'vibrato',
      });
      return;
    }
    drawEditor(ctx, canvas.size, {
      view,
      map,
      theme: readTheme(),
      file,
      clip,
      axis,
      others: clipsOfFile(project, file.id)
        .filter((c) => c.clip.id !== clip.id)
        .map((c) => clipRangeOn(axis, c.clip, map)),
      onsets,
      selected,
      playheadTick: positionBeats * TICKS_PER_BEAT,
      box: d?.kind === 'box' ? [d.fromX, d.toX] : null,
      gridTicks: gridFor(view),
    });
  });

  // Wheel: Ctrl zooms around the pointer, otherwise it scrolls. A listener of its own, not React's onWheel, which is
  // passive: Ctrl + wheel would zoom the whole page instead.
  const showsCanvas = clip !== null && file !== undefined && axis !== null;
  useEffect(() => {
    const element = canvas.ref.current;
    if (!showsCanvas || !element) return;
    const listener = (e: WheelEvent) => {
      e.preventDefault();
      const x = e.offsetX;
      if (e.ctrlKey || e.metaKey) {
        const factor = Math.exp(-e.deltaY * 0.0015);
        setView((v) => {
          // In as far as about 40 px per frame (at 120 BPM and 48 kHz), where each frame shows as a dot.
          const pxPerTick = Math.min(64, Math.max(1e-5, v.pxPerTick * factor));
          const anchor = xToTick(v, x);
          return { ...v, pxPerTick, startTick: anchor - x / pxPerTick };
        });
      } else {
        const dx = Math.abs(e.deltaX) > Math.abs(e.deltaY) ? e.deltaX : e.deltaY;
        setView((v) => ({ ...v, startTick: v.startTick + dx / v.pxPerTick }));
      }
    };
    element.addEventListener('wheel', listener, { passive: false });
    return () => element.removeEventListener('wheel', listener);
  }, [showsCanvas, canvas.ref]);

  // Delete removes the selected markers.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (!file || mode !== 'timing' || selected.size === 0 || (e.key !== 'Delete' && e.key !== 'Backspace')) return;
      if (e.target instanceof HTMLInputElement) return;
      e.preventDefault();
      const commands: Command[] = [...selected]
        .sort((a, b) => b - a)
        .map((index) => ({ command: 'removeTimingMarker', args: { fileId: file.id, index } }));
      dispatch({ command: 'batch', args: { label: 'Remove timing markers', commands } });
      setSelected(new Set());
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [file, mode, selected]);

  if (!clip || !file || !axis) {
    return (
      <>
        <div className="roll-toolbar" onPointerDown={startWindowDrag}>
          <span className="spacer" />
          <WindowControls />
        </div>
        <div className="editor-placeholder centred">Double-click an audio clip to edit it here.</div>
      </>
    );
  }

  const musical = axis.musical;
  const markers = file.timing.markers;
  const snapTick = (tick: number, ctrl: boolean) => (ctrl ? tick : map.snap(tick, gridFor(view), true));
  const markerAt = (x: number) => {
    let best = -1;
    markers.forEach((m, i) => {
      const d = Math.abs(markerX({ view, axis }, m) - x);
      if (d <= MARKER_HIT_PX && (best < 0 || d < Math.abs(markerX({ view, axis }, markers[best]) - x))) best = i;
    });
    return best;
  };

  // A note edge (its start or end frame) within a few pixels of a point on the note's bar, if any.
  const edgeAt = (x: number, y: number) => {
    const { y: rowY, blobHeight } = layout();
    for (const n of file.pitch.notes) {
      if (Math.abs(y - rowY(n.targetCentre)) > Math.max(6, blobHeight / 2)) continue;
      for (const frame of [n.start, n.end])
        if (Math.abs(tickToX(view, axis.frameToTick(frame)) - x) <= EDGE_PX) return frame;
    }
    return null;
  };

  // The seeded note under a point of the Pitch view, if any.
  const layout = () => pitchLayout(canvas.size, viewNotes(file, pitch), pitch);
  const noteAt = (x: number, y: number) => {
    if (file.pitch.notes.length === 0) return undefined;
    const { y: rowY, blobHeight } = layout();
    const frame = axis.tickToFrame(xToTick(view, x));
    return file.pitch.notes.find((n) => frame >= n.start && frame < n.end && Math.abs(y - rowY(n.targetCentre)) <= Math.max(6, blobHeight / 2));
  };

  // A note's shape mapped to the canvas, for the shared curve editing (section 4.4, PC7): times are frames from the
  // note's start, values semitones from its target centre, snapped to 10 cents (Ctrl: freely).
  const shapeMapping = (note: PitchNote, centre = note.targetCentre): CurveMapping => {
    const { y: rowY, pitchAt } = layout();
    return {
      toX: (time) => tickToX(view, axis.frameToTick(note.start + time)),
      toY: (value) => rowY(centre + value),
      fromX: (x) => axis.tickToFrame(xToTick(view, x)) - note.start,
      fromY: (y) => pitchAt(y) - centre,
      min: -48,
      max: 48,
      maxTime: note.end - note.start,
      neutral: 0,
      snapValue: (value) => Math.round(value * 10) / 10,
    };
  };
  const frameX = (frame: number) => tickToX(view, axis.frameToTick(frame));
  // A node of any note's shape near a point.
  const nodeAt = (x: number, y: number) => {
    for (const note of file.pitch.notes) {
      if (frameX(note.end) < x - 8 || frameX(note.start) > x + 8) continue;
      const hit = hitCurve(note.shape, x, y, shapeMapping(note));
      if (hit?.kind === 'point') return { note, index: hit.index };
    }
    return null;
  };
  // A selected note's tilt handle near a point: which note, and which side.
  const tiltHandleAt = (x: number, y: number) => {
    const l = layout();
    for (const note of file.pitch.notes) {
      if (!selectedNotes.has(note.id)) continue;
      const handles = tiltHandles({ start: note.start, end: note.end, centre: note.targetCentre }, l, frameX);
      for (const [side, handle] of [
        ['left', handles.left],
        ['right', handles.right],
      ] as const)
        if (Math.abs(handle.x - x) <= TILT_HANDLE_PX + 2 && Math.abs(handle.y - y) <= TILT_HANDLE_PX + 2) return { note, side };
    }
    return null;
  };
  // A selected note's vibrato handle near a point.
  const vibratoHandleAt = (x: number, y: number) => {
    const l = layout();
    return file.pitch.notes.find((note) => {
      if (!selectedNotes.has(note.id)) return false;
      const bar = vibratoHandle({ start: note.start, end: note.end, centre: note.targetCentre }, l, frameX);
      return Math.abs(bar.x - x) <= VIBRATO_HANDLE_W + 2 && Math.abs(bar.y - y) <= VIBRATO_HANDLE_H + 3;
    });
  };
  // The selected notes that still exist, for the handles that act on the whole selection.
  const selection = () => file.pitch.notes.filter((n) => selectedNotes.has(n.id));
  // A shape's end nodes stay on the note's edges (the engine's normalisePitchShape): dragging one only changes its value.
  const pinEnds = (shape: CurvePoint[], note: PitchNote) =>
    shape.map((p, i): CurvePoint => (i === 0 ? [0, ...p.slice(1)] as CurvePoint : i === shape.length - 1 ? [note.end - note.start, ...p.slice(1)] as CurvePoint : p));
  // shape is relative to centre; the engine then recentres the note where the edit put its pitch (section 4.4).
  const setShape = (note: PitchNote, shape: CurvePoint[], gesture?: string, pivots?: number[], centre = note.targetCentre) =>
    dispatch(
      { command: 'setNoteShape', args: { fileId: file.id, noteId: note.id, shape, centre, ...(pivots ? { pivots } : {}) } },
      gesture,
      gesture ? `shape-${file.id}-${note.id}` : undefined,
    );

  // The cursor and the hint for what a press would do at a point.
  const hover = (x: number, y: number, e: { altKey: boolean; shiftKey: boolean }): [string, string] => {
    if (y < RULER_HEIGHT) return ['default', 'Click or drag: move the playhead'];
    if (mode === 'timing') {
      if (markerAt(x) >= 0)
        return [e.shiftKey ? 'grab' : 'ew-resize', 'Drag: move this point of the audio (Ctrl: off the grid). Shift + drag: slide the audio under it'];
      return ['default', 'Double-click: add a marker. Drag: select markers'];
    }
    if (tiltHandleAt(x, y)) return ['ns-resize', 'Drag up or down: tilt this side around its pivot'];
    if (vibratoHandleAt(x, y)) return ['ns-resize', 'Drag up: more vibrato, down: less. Double-click: as sung'];
    const node = nodeAt(x, y);
    if (node) {
      const end = node.index === 0 || node.index === node.note.shape.length - 1;
      return ['grab', end ? 'Drag up or down: move this end of the shape (Ctrl: freely)' : 'Drag: reshape (Ctrl: freely). Double-click: remove the dot'];
    }
    if (edgeAt(x, y) !== null) return ['ew-resize', 'Drag sideways: move the note’s timing (Ctrl: off the grid)'];
    if (noteAt(x, y))
      return [
        e.altKey ? (e.shiftKey ? 'copy' : 'col-resize') : 'ns-resize',
        'Drag: move in semitones (Ctrl: freely). Double-click: add a dot. Alt + click: split. Shift + Alt + click: set a pivot',
      ];
    return ['default', 'Drag: select notes (Ctrl: add to the selection)'];
  };

  const onPointerDown = (e: React.PointerEvent<HTMLCanvasElement>) => {
    const { offsetX: x, offsetY: y } = e.nativeEvent;
    e.currentTarget.setPointerCapture(e.pointerId);
    const tick = xToTick(view, x);
    press.current = { x, y, moved: false };

    if (y < RULER_HEIGHT) {
      actions.seek(Math.max(0, tick) / TICKS_PER_BEAT);
      drag.current = { kind: 'scrub' };
      return;
    }

    // The Pitch view has one mode: what is under the pointer decides (section 4). In order: a selected note's tilt
    // handle, a shape's node, a note's edge, a note's bar, empty space.
    if (mode === 'pitch') {
      const handle = tiltHandleAt(x, y);
      if (handle) {
        // Tilts that side of the note around the pivot next to it (PC8).
        const { note, side } = handle;
        const length = note.end - note.start;
        const span = tiltSpan(pivotsOf(note.pivots, length), length, side === 'left' ? 1 : length - 1);
        holdRenders(true);
        drag.current = { kind: 'tilt', noteId: note.id, points: note.shape, centre: note.targetCentre, ...span, fromY: y, gesture: newGestureId() };
        return;
      }
      if (vibratoHandleAt(x, y)) {
        // Changes the selected notes' vibrato together, each from its own amount (section 4.4).
        const notes = selection();
        holdRenders(true);
        drag.current = { kind: 'vibrato', ids: notes.map((n) => n.id), from: notes.map((n) => n.vibrato), fromY: y, gesture: newGestureId() };
        return;
      }
      const node = nodeAt(x, y);
      if (node) {
        // A node: drag to reshape (10-cent steps, Ctrl freely); double-click removes it.
        holdRenders(true);
        drag.current = { kind: 'node', noteId: node.note.id, points: node.note.shape, centre: node.note.targetCentre, index: node.index, gesture: newGestureId() };
        return;
      }

      // A note's start or end edge moves its timing through a timing marker there (PC9).
      const edge = edgeAt(x, y);
      if (edge !== null) {
        const content = axis.contentAt(axis.frameToTick(edge));
        // The marker is added once the edge is dragged, so a click adds none.
        const needsMarker = !file.timing.markers.some((m) => Math.abs(m.frame - edge) < 1);
        if (needsMarker && (content <= 0 || edge <= 0 || edge >= file.frames)) return;
        holdRenders(true);
        drag.current = { kind: 'edge', frame: edge, content, grabTick: axis.tickOfContent(content), needsMarker, gesture: newGestureId() };
        return;
      }
      const note = noteAt(x, y);
      if (!note) {
        drag.current = { kind: 'box', fromX: x, toX: x, additive: e.ctrlKey || e.metaKey };
        return;
      }
      // Shift + Alt + click sets a tilt pivot there, with a node so the curve doesn't change (PC8).
      if (e.altKey && e.shiftKey) {
        const time = Math.round(axis.tickToFrame(tick)) - note.start;
        if (time > 0 && time < note.end - note.start) setShape(note, withNodeAt(note.shape, time), undefined, [...note.pivots, time]);
        return;
      }
      // Alt + click splits the note there, each half measuring its own centre (section 4.2).
      if (e.altKey) {
        const frame = Math.round(axis.tickToFrame(tick));
        const rate = pitch?.sampleRate ?? file.sampleRate;
        const left = track ? measureCentre(track, rate, note.start, frame) : null;
        const right = track ? measureCentre(track, rate, frame, note.end) : null;
        if (frame > note.start && frame < note.end)
          dispatch({
            command: 'splitPitchNote',
            args: { fileId: file.id, noteId: note.id, frame, ...(left !== null && right !== null ? { centres: [left, right] } : {}) },
          });
        return;
      }
      const additive = e.ctrlKey || e.metaKey;
      const next = new Set(selectedNotes.has(note.id) || additive ? selectedNotes : []);
      if (additive && selectedNotes.has(note.id)) next.delete(note.id);
      else next.add(note.id);
      setSelectedNotes(next);
      // Clicking a note plays it (the preview player), unless turned off in the side panel.
      if (audition) bridge.call('audio.auditionNote', { fileId: file.id, noteId: note.id }).catch(() => undefined);
      const ids = [...next].filter((id) => file.pitch.notes.some((n) => n.id === id));
      holdRenders(true); // rendered once the drag ends
      drag.current = {
        kind: 'notes',
        ids,
        targets: ids.map((id) => file.pitch.notes.find((n) => n.id === id)?.targetCentre ?? 0),
        fromY: y,
        gesture: newGestureId(),
      };
      return;
    }

    const hit = markerAt(x);
    if (hit >= 0) {
      holdRenders(true); // rendered once the drag ends
      const additive = e.ctrlKey || e.metaKey;
      const next = new Set(selected.has(hit) || additive ? selected : []);
      if (additive && selected.has(hit)) next.delete(hit);
      else next.add(hit);
      setSelected(next);
      if (e.shiftKey) {
        // Slide the audio under the marker: its frame moves, its tick stays.
        drag.current = { kind: 'slide', index: hit, frame: markers[hit].frame, rate: axis.timing.rateAt(markers[hit].tick), pointerTick: tick, gesture: newGestureId() };
      } else {
        const indices = [...next].sort((a, b) => a - b);
        drag.current = {
          kind: 'markers',
          indices,
          ticks: indices.map((i) => markers[i].tick),
          grabTick: axis.tickOfContent(markers[hit].tick),
          pointerTick: tick,
          gesture: newGestureId(),
        };
      }
      return;
    }

    drag.current = { kind: 'box', fromX: x, toX: x, additive: e.ctrlKey || e.metaKey };
  };

  const onPointerMove = (e: React.PointerEvent<HTMLCanvasElement>) => {
    const d = drag.current;
    const { offsetX: x } = e.nativeEvent;
    const tick = xToTick(view, x);
    if (!d) {
      const [cursor, text] = hover(x, e.nativeEvent.offsetY, e);
      e.currentTarget.style.cursor = cursor;
      setHint(text);
      return;
    }

    // Edits start only once the pointer has moved: a click that wobbles a pixel only selects (and doesn't snap).
    if (d.kind === 'edge' || d.kind === 'node' || d.kind === 'tilt' || d.kind === 'vibrato' || d.kind === 'notes') {
      if (!press.current.moved && Math.hypot(x - press.current.x, e.nativeEvent.offsetY - press.current.y) < DRAG_PX) return;
      press.current.moved = true;
    }

    if (d.kind === 'edge') {
      if (d.needsMarker) {
        d.needsMarker = false;
        dispatch({ command: 'addTimingMarker', args: { fileId: file.id, tick: d.content, frame: d.frame } });
        return;
      }
      // Grid snapping on where the edge lands; Ctrl turns it off.
      const landed = snapTick(tick, e.ctrlKey || e.metaKey);
      const index = file.timing.markers.findIndex((m) => Math.abs(m.frame - d.frame) < 1);
      if (index >= 0)
        dispatch(
          { command: 'moveTimingMarker', args: { fileId: file.id, index, tick: axis.contentAt(landed) } },
          d.gesture,
          `markers-${file.id}`,
        );
    } else if (d.kind === 'node' || d.kind === 'tilt') {
      const note = file.pitch.notes.find((n) => n.id === d.noteId);
      if (!note) return;
      const shape =
        d.kind === 'node'
          ? pinEnds(movePoints(d.points, [d.index], d.index, x, e.nativeEvent.offsetY, shapeMapping(note, d.centre), e.ctrlKey || e.metaKey).points, note)
          : tiltShape(d.points, d.fixed, d.moving, (d.fromY - e.nativeEvent.offsetY) / layout().rowHeight);
      // Against the centre the drag began with: the engine recentres the note on every step.
      setShape(note, shape, d.gesture, undefined, d.centre);
    } else if (d.kind === 'vibrato') {
      // Up deepens the vibrato, down flattens it, 0 to 200 %.
      const by = (d.fromY - e.nativeEvent.offsetY) / VIBRATO_PX;
      const commands: Command[] = d.ids.map((id, k) => ({
        command: 'setNotePitch',
        args: { fileId: file.id, noteIds: [id], vibrato: Math.round(Math.min(2, Math.max(0, d.from[k] + by)) * 100) / 100 },
      }));
      dispatch({ command: 'batch', args: { label: 'Change vibrato', commands } }, d.gesture, `vibrato-${file.id}`);
    } else if (d.kind === 'notes') {
      // Up and down in semitones from where the drag began, onto whole semitones unless Ctrl is held (section 4.6).
      const semitones = (d.fromY - e.nativeEvent.offsetY) / layout().rowHeight;
      const free = e.ctrlKey || e.metaKey;
      const commands: Command[] = d.ids.map((id, k) => ({
        command: 'setNotePitch',
        args: { fileId: file.id, noteIds: [id], target: free ? d.targets[k] + semitones : Math.round(d.targets[k] + semitones) },
      }));
      dispatch({ command: 'batch', args: { label: 'Move notes', commands } }, d.gesture, `pitch-${file.id}`);
    } else if (d.kind === 'scrub') {
      actions.seek(Math.max(0, tick) / TICKS_PER_BEAT);
    } else if (d.kind === 'box') {
      d.toX = x;
      redraw((n) => n + 1);
    } else if (d.kind === 'markers') {
      // Grid snapping on where the grabbed marker lands; Ctrl turns it off.
      const landed = snapTick(d.grabTick + tick - d.pointerTick, e.ctrlKey || e.metaKey);
      const delta = landed - d.grabTick;
      const order = delta > 0 ? [...d.indices.keys()].reverse() : [...d.indices.keys()];
      // Through the timeline: a one-shot file's markers are in seconds, which a tick covers more or less of.
      const commands: Command[] = order.map((k) => ({
        command: 'moveTimingMarker',
        args: { fileId: file.id, index: d.indices[k], tick: axis.contentAt(axis.tickOfContent(d.ticks[k]) + delta) },
      }));
      dispatch({ command: 'batch', args: { label: 'Move timing markers', commands } }, d.gesture, `markers-${file.id}`);
    } else if (d.kind === 'slide') {
      const frame = Math.max(1, d.frame - (axis.contentAt(tick) - axis.contentAt(d.pointerTick)) * d.rate);
      dispatch({ command: 'moveTimingMarker', args: { fileId: file.id, index: d.index, frame } }, d.gesture, `markers-${file.id}`);
    }
  };

  const onPointerUp = () => {
    const d = drag.current;
    drag.current = null;
    if (d?.kind === 'markers' || d?.kind === 'slide' || d?.kind === 'notes' || d?.kind === 'node' || d?.kind === 'tilt' || d?.kind === 'vibrato' || d?.kind === 'edge') holdRenders(false);
    if (d?.kind === 'vibrato') redraw((n) => n + 1); // the amounts hide again
    if (d?.kind !== 'box') return;
    if (mode === 'pitch') {
      const [a, b] = [Math.min(d.fromX, d.toX), Math.max(d.fromX, d.toX)];
      const frameAt = (x: number) => axis.tickToFrame(xToTick(view, x));
      const inBox = file.pitch.notes.filter((n) => n.end > frameAt(a) && n.start < frameAt(b)).map((n) => n.id);
      setSelectedNotes(new Set(d.additive ? [...selectedNotes, ...inBox] : inBox));
      redraw((n) => n + 1);
      return;
    }
    const [a, b] = [Math.min(d.fromX, d.toX), Math.max(d.fromX, d.toX)];
    const inBox = markers.map((m, i) => [markerX({ view, axis }, m), i] as const).filter(([mx]) => mx >= a && mx <= b).map(([, i]) => i);
    setSelected(new Set(d.additive ? [...selected, ...inBox] : inBox));
    redraw((n) => n + 1);
  };

  // Double-click adds a marker where the audio plays now, snapped to the nearest transient within a few pixels.
  const onDoubleClick = (e: React.MouseEvent<HTMLCanvasElement>) => {
    // In the Pitch view a double-click removes a node, or adds one on a note's shape where it was clicked.
    if (mode === 'pitch') {
      const { offsetX: x, offsetY: y } = e.nativeEvent;
      // On a vibrato handle: the selection's vibrato back as sung.
      if (vibratoHandleAt(x, y)) {
        const commands: Command[] = selection().map((n) => ({ command: 'setNotePitch', args: { fileId: file.id, noteIds: [n.id], vibrato: 1 } }));
        dispatch({ command: 'batch', args: { label: 'Vibrato as sung', commands } });
        return;
      }
      const node = nodeAt(x, y);
      if (node) {
        // The end nodes stay: they hold the curve to the note's edges.
        if (node.index > 0 && node.index < node.note.shape.length - 1) setShape(node.note, deletePoints(node.note.shape, [node.index]));
        return;
      }
      const note = noteAt(x, y);
      if (note) {
        const time = Math.round(axis.tickToFrame(xToTick(view, x))) - note.start;
        if (time >= 0 && time <= note.end - note.start) setShape(note, withNodeAt(note.shape, time));
      }
      return;
    }
    if (mode !== 'timing' || e.nativeEvent.offsetY < RULER_HEIGHT + CLIPS_STRIP) return;
    const tick = xToTick(view, e.nativeEvent.offsetX);
    let frame = axis.tickToFrame(tick);
    const framesPerPx = Math.abs(axis.tickToFrame(tick + 1 / view.pxPerTick) - frame);
    const snapped = onsets ? snapToOnset(frame, onsets, SNAP_PX * framesPerPx) : null;
    if (snapped !== null) frame = snapped;
    const content = axis.contentAt(axis.frameToTick(frame));
    if (content <= 0 || frame <= 0 || frame >= file.frames) return;
    dispatch({ command: 'addTimingMarker', args: { fileId: file.id, tick: content, frame } });
  };

  const shared = clipsOfFile(project, file.id).length;

  return (
    <>
      <div className="roll-toolbar audio-editor-toolbar" onPointerDown={startWindowDrag}>
        <IconButton size="l" title={playing ? 'Stop (Space)' : 'Play (Space)'} onClick={() => actions.togglePlay()}>
          {playing ? <Pause size={14} /> : <Play size={14} />}
        </IconButton>
        <span className="audio-editor-title">{file.name}</span>
        <span className="dim">
          {musical ? `Musical, ${file.timing.musicalStyle}` : 'One-shot'}
          {shared > 1 ? ` · ${shared} clips` : ''}
        </span>
        <span className="spacer" />
        <WindowControls />
      </div>
      <div className="audio-editor-body">
        <aside className="audio-editor-side" onPointerDown={(e) => e.stopPropagation()}>
          {/* The view decides what the panel below it holds. */}
          <Segmented
            label="View"
            className="audio-editor-views"
            value={mode}
            onChange={(next) => {
              setMode(next);
              setSelected(new Set());
              setHint('');
            }}
            options={[
              { value: 'timing', label: 'Timing', title: 'The wave, its transients and timing markers' },
              { value: 'pitch', label: 'Pitch', title: 'The notes found in the audio’s pitch' },
            ]}
          />
          {mode === 'pitch' && (
            <PitchSummary
              analysis={pitch}
              file={file}
              selected={selectedNotes}
              audition={audition}
              onAudition={setAudition}
            />
          )}
          {mode === 'timing' && <TimingSection clip={clip} file={file} />}
          {mode === 'timing' && (
            <Gestures>
              <li>
                Double-click: add a marker (it snaps to transients).
                {musical ? ' Markers sit on the beat, so the audio follows the tempo.' : ' Markers sit in real time: the tempo doesn’t move them.'}
              </li>
              <li>Drag a marker: move that point of the audio. Shift + drag: slide the audio under it. Ctrl: off the grid.</li>
              <li>Drag across empty space: select markers. Delete: remove the selected ones.</li>
            </Gestures>
          )}
        </aside>
        {/* A working area, sunk below the panels around it (design-system.md, DS7). */}
        <div className="audio-editor-area working-area">
          <canvas
            ref={canvas.ref}
            className="audio-editor-canvas"
            onPointerDown={onPointerDown}
            onPointerMove={onPointerMove}
            onPointerUp={onPointerUp}
            onPointerCancel={onPointerUp}
            onPointerLeave={() => setHint('')}
            onDoubleClick={onDoubleClick}
          />
          {hint && <div className="audio-editor-status">{hint}</div>}
        </div>
      </div>
    </>
  );
}

/** The gestures of a view, folded away until asked for. */
function Gestures({ children }: { children: React.ReactNode }) {
  return (
    <details className="audio-editor-gestures">
      <summary>Gestures</summary>
      <ul className="inspector-note pitch-help">{children}</ul>
    </details>
  );
}

/** The Pitch view's side panel: the tools for the selection (or every note), grouped by what they change. */
function PitchSummary({
  analysis,
  file,
  selected,
  audition,
  onAudition,
}: {
  analysis: PitchAnalysisReply | undefined;
  file: AudioFile;
  selected: ReadonlySet<number>;
  /** Whether clicking a note plays it. */
  audition: boolean;
  onAudition: (on: boolean) => void;
}) {
  const [snapMode, setSnapMode] = useState<'semitone' | 'key'>('semitone');
  // A slider dragged with the pointer is one undo step, rendered when it is let go.
  const sliding = useRef<string | null>(null);
  const slider = {
    onPointerDown: () => {
      sliding.current = newGestureId();
      holdRenders(true);
    },
    onPointerUp: () => {
      sliding.current = null;
      holdRenders(false);
    },
  };
  const [snapAmount, setSnapAmount] = useState(100);
  const hasKey = useDawg((s) => s.project?.keys?.some((k) => k.enabled) ?? false);
  if (!analysis) return <p className="inspector-note">Finding the pitch… This runs once per file and is kept.</p>;
  if (analysis.error) return <p className="inspector-note">{analysis.error}</p>;
  const notes = file.pitch.notes.length > 0 ? file.pitch.notes : (analysis.notes ?? []);
  const seeded = file.pitch.notes.length > 0;
  // The tools act on the selection, or on every note when nothing is selected; the buttons say which.
  const targets = file.pitch.notes.filter((n) => selected.size === 0 || selected.has(n.id));
  const ids = targets.map((n) => n.id);
  const plural = (n: number) => `${n} ${n === 1 ? 'note' : 'notes'}`;
  const target = selected.size > 0 ? plural(targets.length) : `all ${plural(targets.length)}`;
  const snap = () =>
    dispatch({ command: 'snapNotePitch', args: { fileId: file.id, noteIds: ids, mode: snapMode, amount: snapAmount / 100 } });
  const vibrato = targets.length > 0 ? targets[0].vibrato : 1;
  const mixedVibrato = targets.some((n) => n.vibrato !== vibrato);
  // Merge needs two neighbours or more, all selected.
  const indices = file.pitch.notes.flatMap((n, i) => (selected.has(n.id) ? [i] : []));
  const canMerge = indices.length >= 2 && indices.every((index, k) => k === 0 || index === indices[k - 1] + 1);
  return (
    <div className="audio-editor-hint">
      <p className="inspector-note">
        {plural(notes.length)}
        {selected.size > 0 ? `, ${selected.size} selected` : ''}
      </p>
      {seeded && (
        <>
          <div className="timing-section">
            <div className="inspector-label">Pitch</div>
            <Segmented
              label="Snap to"
              value={snapMode}
              onChange={setSnapMode}
              options={[
                { value: 'semitone', label: 'Semitones' },
                { value: 'key', label: 'Key', title: hasKey ? 'The project’s key' : 'Set the project’s key first' },
              ]}
            />
            <label className="inspector-note">
              Amount {snapAmount} %
              <input type="range" min={0} max={100} step={5} value={snapAmount} onChange={(e) => setSnapAmount(Number(e.target.value))} />
            </label>
            <Button disabled={snapMode === 'key' && !hasKey} onClick={snap}>
              Snap {target}
            </Button>
          </div>
          <div className="timing-section">
            <div className="inspector-label">Shape</div>
            <label className="inspector-note">
              Vibrato {mixedVibrato ? 'mixed' : `${Math.round(vibrato * 100)} %`}
              <input
                type="range"
                min={0}
                max={200}
                step={5}
                value={Math.round(vibrato * 100)}
                {...slider}
                onPointerCancel={slider.onPointerUp}
                onChange={(e) =>
                  dispatch(
                    { command: 'setNotePitch', args: { fileId: file.id, noteIds: ids, vibrato: Number(e.target.value) / 100 } },
                    sliding.current ?? undefined,
                    sliding.current ? `vibrato-${file.id}` : undefined,
                  )
                }
              />
            </label>
            <Button
              title={`Takes the scoop out of the start of ${target}`}
              onClick={() => dispatch({ command: 'shapeNotes', args: { fileId: file.id, noteIds: ids, op: 'flattenOnset' } })}
            >
              Flatten onset
            </Button>
            <Button
              title={`Takes the fall out of the end of ${target}`}
              onClick={() => dispatch({ command: 'shapeNotes', args: { fileId: file.id, noteIds: ids, op: 'flattenRelease' } })}
            >
              Flatten release
            </Button>
            <p className="inspector-note">A selected note's vibrato also has a handle below the note.</p>
          </div>
          <div className="timing-section">
            <div className="inspector-label">Notes</div>
            <Button
              disabled={!canMerge}
              title={canMerge ? undefined : 'Select two neighbouring notes or more'}
              onClick={() => dispatch({ command: 'mergePitchNotes', args: { fileId: file.id, noteIds: [...selected] } })}
            >
              Merge {canMerge ? plural(indices.length) : ''}
            </Button>
            <Button
              title="Back as sung: pitch, shape, pivots and vibrato"
              onClick={() => dispatch({ command: 'resetPitchNotes', args: { fileId: file.id, noteIds: ids } })}
            >
              Reset {target}
            </Button>
          </div>
          <label className="inspector-note">
            <input type="checkbox" checked={audition} onChange={(e) => onAudition(e.target.checked)} /> Play notes on click
          </label>
          <Gestures>
            <li>Drag a note up or down: semitones (Ctrl: freely). Drag across empty space: select.</li>
            <li>Drag a dot on a note's shape: reshape it (Ctrl: freely). Double-click the note: add a dot; double-click a dot: remove it.</li>
            <li>Drag a selected note's corner handle (▸ ◂) up or down: tilt that side around its pivot (◆). Shift + Alt + click: set a pivot.</li>
            <li>Drag a selected note's bottom handle up or down: more or less vibrato. Double-click the handle: as sung.</li>
            <li>A note's bar follows its shape: after a reshape or a tilt it sits at the pitch the note now has.</li>
            <li>Alt + click a note: split it. Drag a note’s edge sideways: move its timing.</li>
            <li>The orange line is the corrected pitch.</li>
          </Gestures>
        </>
      )}
    </div>
  );
}
