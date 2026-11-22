import { Expand, LocateFixed, Pencil, Plus, Trash2, ZoomIn, ZoomOut } from 'lucide-react';
import { useEffect, useMemo, useRef, useState } from 'react';
import { TICKS_PER_BEAT, type Marker } from '../bridge/protocol';
import { TextPrompt } from '../components/ConfirmDiscard';
import { drawRuler, formatLoopPosition } from '../pianoroll/draw';
import { alpha, readTheme, type Size } from '../canvas/theme';
import { tickToX, xToTick, type View } from '../pianoroll/geometry';
import { tempoMapOf } from '../time/tempoMap';
import { KeyStrip } from './KeyStrip';
import { CornerAdd, playheadTick, SignatureStrip } from './SignatureStrip';
import { formatBpm, TempoLane } from './TempoLane';
import { useCanvas } from '../canvas/useCanvas';
import { actions, dispatch, newGestureId, projectActions, useDawg } from '../store/store';
import { importDrop, isAudioDrag } from './importAudio';
import { ui, useUi } from '../store/uiStore';
import { clipEnd } from '../timeline/clips';
import { useRulerInput } from '../timeline/useRulerInput';
import { usePeaksVersion } from '../timeline/peaks';
import { drawAutomation, drawBox, drawClip, drawCompRange, drawCovered, drawLanes, drawLoopRange, drawMarkerRow, drawPlayedValues } from './draw';
import { clipBox, clipLanes, hitClip, laneAt, laneOfTrack, lanesOf, NAME_BAR, totalHeight } from './layout';
import { ClipMenu, LaneMenu, type SelectedClip } from './ClipMenu';
import { MasterHeader, TrackHeaders } from './TrackHeaders';
import { AutomationHeader } from './AutomationHeaders';
import { Splitter } from '../components/Splitter';
import { layout, useLayout } from '../store/layoutStore';
import { useArrangementEditing } from './useArrangementEditing';
import { useAutomationEditing } from './useAutomationEditing';
import { manualPosition } from '../automation/automation';
import { PlayheadOverlay } from '../components/PlayheadOverlay';
import { GlobalSwitches } from '../components/GlobalSwitches';
import { GraphView } from '../graph/GraphView';
import { HelpButton } from '../components/HelpMenu';
import { MenuItem } from '../components/MenuItem';
import { addAsNewTrack, addToTrack, chainProblem, draggedPaletteNode, isPaletteDrag } from '../components/paletteDrag';
import type { TempoMap } from '../time/tempoMap';
import { Button, IconButton, Toggle } from '../ui/controls';
import { Menu } from '../ui/overlays';

// The arrangement view (docs/design/phase6-arrangement.md): track headers, a ruler with the loop zone, a
// marker row and the clip lanes. In the graph view the selected track's node graph takes the timeline's place, and
// the track headers stay to pick the track (phase8-node-graph.md, section 5).

const MIN_PX_PER_BEAT = 1;
const MAX_PX_PER_BEAT = 400;
const FOLLOW_FRACTION = 1 / 3;
const MARKER_HIT_PX = 6;
const MASTER_HEIGHT = 64; // the standard track header layout with its meter

const SNAP_CHOICES = [
  { label: 'Auto', beats: null },
  { label: 'Bar', beats: -1 },
  { label: 'Beat', beats: 1 },
  { label: '1/8', beats: 1 / 2 },
  { label: '1/16', beats: 1 / 4 },
  { label: 'Off', beats: 0 },
];

const AUTO_SNAP_BEATS = [1 / 32, 1 / 16, 1 / 8, 1 / 4, 1 / 2, 1, 2, 4, 8];
const AUTO_SNAP_MIN_PX = 10;

/** The bar grid: TempoMap.snap snaps to bar lines with a grid at least a bar long. */
const BAR_GRID = 1e12;

const timeView = (startTick: number, pxPerTick: number): View => ({ startTick, pxPerTick, topPitch: 0, keyHeight: 1 });

/** The tempo under the playhead. It updates on its own, without re-rendering the whole arrangement. */
function TempoNow({ map }: { map: TempoMap }) {
  const beats = useDawg((s) => s.telemetry.positionBeats);
  return <>{formatBpm(Math.round(map.bpmAt(beats * TICKS_PER_BEAT) * 100) / 100)} BPM</>;
}

/** The arrangement, or with `graph` the selected track's node graph beside the track headers. */
export function Arrangement({ graph = false }: { graph?: boolean }) {
  const project = useDawg((s) => s.project);
  const master = project?.master ?? null;
  // The master track's automation rows sit under its pinned header (polish after phase 10).
  const masterLane = useMemo(() => (master ? laneOfTrack(master, 0, 0, MASTER_HEIGHT) : null), [master]);
  const masterCanvas = useCanvas();
  // Only the fields that change the rendered output: subscribing to the whole telemetry object re-rendered (and
  // fully redrew) the arrangement 30 times a second, even when nothing moved. The playhead itself lives on its own
  // overlay (components/PlayheadOverlay.tsx), so it is not subscribed here.
  const playing = useDawg((s) => s.telemetry.playing);
  const recording = useDawg((s) => s.telemetry.recording);
  const recordStartBeats = useDawg((s) => s.telemetry.recordStartBeats);
  // While recording, the take grows with the playhead, so the lanes redraw at the telemetry rate after all.
  const recordingBeats = useDawg((s) => (s.telemetry.recording ? s.telemetry.positionBeats : 0));
  const selectedTrackId = useUi((s) => s.selectedTrackId);
  const selectedClips = useUi((s) => s.selectedClips);
  const follow = useUi((s) => s.follow);
  const tempoLaneChoice = useUi((s) => s.tempoLane);
  const headerWidth = useLayout((s) => s.headerWidth);

  const [startTick, setStartTick] = useState(0);
  const [pxPerTick, setPxPerTick] = useState(24 / TICKS_PER_BEAT);
  const [scrollY, setScrollY] = useState(0);
  const [snapChoice, setSnapChoice] = useState(SNAP_CHOICES[0]);
  const [markerDrag, setMarkerDrag] = useState<{ marker: Marker; position: number; moved: boolean } | null>(null);
  // A marker selected by a click on it: Delete removes it, Escape lets go of it.
  const [selectedMarker, setSelectedMarker] = useState<number | null>(null);
  const [renaming, setRenaming] = useState<Marker | null>(null);
  const [menu, setMenu] = useState<{ x: number; y: number; tick: number; lane: number; onClip: boolean } | null>(null);
  const [renamingClips, setRenamingClips] = useState<number[] | null>(null);
  const [trackMenu, setTrackMenu] = useState<DOMRect | null>(null);
  // A marker's right-click menu (docs/design/ux-review.md, U15).
  const [markerMenu, setMarkerMenu] = useState<{ marker: Marker; x: number; y: number } | null>(null);
  // A browser item dragged over the tracks: the track it would go to, or a new track below them.
  const [deviceDrop, setDeviceDrop] = useState<{ top: number; height: number; label: string; ok: boolean } | null>(null);
  const [loopTooltip, setLoopTooltip] = useState<{ edge: 'start' | 'end'; x: number; y: number } | null>(null);
  const missingAudio = useDawg((s) => s.missingAudio);
  const armed = useDawg((s) => s.armed);
  const renameCancelled = useRef(false);
  const theme = useMemo(readTheme, []);

  const lanesCanvas = useCanvas();
  const ruler = useCanvas();
  const markerRow = useCanvas();

  const view = timeView(startTick, pxPerTick);
  // Bars and the grid come from the tempo map (docs/design/tempo-map.md, TM14); the bar grid snaps to bar lines.
  const map = tempoMapOf(project);
  const tempoLaneShown = tempoLaneChoice ?? map.hasTempoChanges;
  const autoSnapBeats =
    AUTO_SNAP_BEATS.find((beats) => beats * TICKS_PER_BEAT * pxPerTick >= AUTO_SNAP_MIN_PX) ??
    AUTO_SNAP_BEATS[AUTO_SNAP_BEATS.length - 1];
  const gridTicks =
    snapChoice.beats === null
      ? Math.round(autoSnapBeats * TICKS_PER_BEAT)
      : snapChoice.beats < 0
        ? BAR_GRID
        : Math.round(snapChoice.beats * TICKS_PER_BEAT);
  const lanes = useMemo(() => (project ? lanesOf(project) : []), [project]);
  const peaksVersion = usePeaksVersion((s) => s.version); // redraw when waveforms arrive
  const missingAudioList = useDawg((s) => s.missingAudio);
  const stretchPending = useDawg((s) => s.stretchPending);
  const audioContext = useMemo(
    () => ({
      files: project?.audioFiles ?? {},
      map,
      missing: new Set(missingAudioList.map((m) => m.id)),
      stretchPending,
    }),
    [project, map, missingAudioList, stretchPending],
  );
  void peaksVersion;
  const markers = useMemo(() => Object.values(project?.markers ?? {}).sort((a, b) => a.position - b.position), [project]);
  const playheadNow = () => useDawg.getState().telemetry.positionBeats * TICKS_PER_BEAT;
  const maxScroll = Math.max(0, totalHeight(lanes) - lanesCanvas.size.height + 40);
  // The selected track's place among the lanes that hold clips (paste goes there).
  const selectedLane = Math.max(0, lanes.find((l) => l.track.id === selectedTrackId && l.clipIndex >= 0)?.clipIndex ?? 0);

  const editing = useArrangementEditing({ project, lanes, view, scrollY, gridTicks, map, selected: selectedClips });
  const rulerInput = useRulerInput({ view, gridTicks, map, loop: project?.loop, punch: project?.punch });
  const automationEditing = useAutomationEditing({ lanes, view, scrollY, gridTicks, map });
  const masterEditing = useAutomationEditing({ lanes: masterLane ? [masterLane] : [], view, scrollY: 0, gridTicks, map });
  const nodeTypes = useDawg((s) => s.nodeTypes);

  // Keep the scroll position valid when tracks are removed or lanes shrink.
  useEffect(() => {
    if (scrollY > maxScroll) setScrollY(maxScroll);
  }, [scrollY, maxScroll]);

  // Continuous follow while playing, as in the piano roll. The playhead overlay calls this at the telemetry rate;
  // the view only re-renders (and the lanes only redraw) when the playhead actually moves it.
  const followTick = (tick: number) => {
    if (!follow || !playing || lanesCanvas.size.width === 0) return;
    const visible = lanesCanvas.size.width / pxPerTick;
    const next = Math.max(0, tick - visible * FOLLOW_FRACTION);
    if (tick < startTick || tick > startTick + visible * 0.9) setStartTick(next);
  };

  // The played value of each active automation lane at the playhead. These move every tick, so they are painted on
  // the overlay too; the lanes under it stay as they are.
  const paintPlayed = (ctx: CanvasRenderingContext2D, size: Size, tick: number) =>
    drawPlayedValues(
      ctx,
      size,
      view,
      theme,
      lanes.flatMap((lane) => lane.automation),
      useDawg.getState().telemetry.automation,
      scrollY,
      tick,
    );
  const paintMasterPlayed = (ctx: CanvasRenderingContext2D, size: Size, tick: number) =>
    drawPlayedValues(
      ctx,
      size,
      view,
      theme,
      masterLane?.automation ?? [],
      useDawg.getState().telemetry.automation,
      0,
      tick,
    );

  // Redraw once per frame when anything changes.
  const frame = useRef(0);
  useEffect(() => {
    cancelAnimationFrame(frame.current);
    frame.current = requestAnimationFrame(() => {
      const ctx = lanesCanvas.context();
      if (ctx) {
        drawLanes(ctx, lanesCanvas.size, view, theme, lanes, scrollY, selectedTrackId, map, gridTicks);
        if (project?.loop.enabled) drawLoopRange(ctx, lanesCanvas.size, view, theme, project.loop.start, project.loop.end);
        // Clips linked with a selected clip get a dashed outline.
        const selectedLinks = new Set(
          lanes.flatMap((l) => Object.values(l.track.clips)).filter((c) => selectedClips.has(c.id) && c.linkId !== 0).map((c) => c.linkId),
        );
        for (const item of editing.displayClips()) {
          drawClip(
            ctx,
            lanesCanvas.size,
            view,
            theme,
            item,
            scrollY,
            selectedClips.has(item.clip.id) && !item.faded,
            !item.faded && selectedLinks.has(item.clip.linkId),
            audioContext,
            item.showGain,
          );
          if (item.covered?.length) drawCovered(ctx, lanesCanvas.size, view, theme, clipBox(view, item.lane, item.clip, scrollY), item.covered);
        }
        if (editing.comp)
          drawCompRange(ctx, theme, clipBox(view, editing.comp.lane, editing.comp.clip, scrollY), editing.comp.x0, editing.comp.x1);
        if (recording && recordStartBeats >= 0) {
          // The take grows on armed audio tracks while recording.
          const x0 = (recordStartBeats * TICKS_PER_BEAT - startTick) * pxPerTick;
          const x1 = (recordingBeats * TICKS_PER_BEAT - startTick) * pxPerTick;
          ctx.fillStyle = alpha(theme.danger, 0.28);
          for (const lane of lanes)
            if (armed.has(lane.track.id) && lane.track.kind === 'audio')
              ctx.fillRect(x0, lane.top - scrollY + 2, Math.max(0, x1 - x0), lane.height - 4);
        }
        for (const lane of lanes)
          for (const row of lane.automation)
            drawAutomation(ctx, lanesCanvas.size, view, theme, row, automationEditing.pointsOf(row), scrollY, lane.track.colour, {
              neutral: manualPosition(row.lane, row.track, nodeTypes),
              selected: automationEditing.selectedOf(row),
            });
        if (automationEditing.box)
          drawBox(
            ctx,
            theme,
            automationEditing.box.x0,
            automationEditing.box.y0,
            automationEditing.box.x1,
            automationEditing.box.y1,
          );

        const masterCtx = masterCanvas.context();
        if (masterCtx && masterLane) {
          drawLanes(masterCtx, masterCanvas.size, view, theme, [masterLane], 0, null, map, gridTicks);
          for (const row of masterLane.automation)
            drawAutomation(masterCtx, masterCanvas.size, view, theme, row, masterEditing.pointsOf(row), 0, theme.textDim, {
              neutral: manualPosition(row.lane, row.track, nodeTypes),
              selected: masterEditing.selectedOf(row),
            });
          if (masterEditing.box)
            drawBox(
              masterCtx,
              theme,
              masterEditing.box.x0,
              masterEditing.box.y0,
              masterEditing.box.x1,
              masterEditing.box.y1,
            );
        }
        if (editing.box) drawBox(ctx, theme, editing.box.x0, editing.box.y0, editing.box.x1, editing.box.y1);
      }

      const rulerCtx = ruler.context();
      if (rulerCtx) drawRuler(rulerCtx, ruler.size, view, theme, map, rulerInput.loop ?? undefined, [], false, rulerInput.punch ?? undefined);

      const markerCtx = markerRow.context();
      if (markerCtx) {
        const shown = markerDrag
          ? markers.map((m) => (m.id === markerDrag.marker.id ? { ...m, position: markerDrag.position } : m))
          : markers;
        drawMarkerRow(markerCtx, markerRow.size, view, theme, shown, markerDrag?.marker.id ?? selectedMarker);
      }
    });
  });

  // Wheel: scroll lanes; Shift scrolls time; Ctrl/Cmd zooms time; Alt scales all lane heights.
  const bodyRef = useRef<HTMLDivElement>(null);
  const onWheelRef = useRef<(e: WheelEvent, x: number) => void>(() => {});
  const heightGesture = useRef({ id: '', at: 0 });
  useEffect(() => {
    const element = bodyRef.current;
    if (!element) return;
    const listener = (e: WheelEvent) => {
      // The graph zooms and pans itself.
      if (e.target instanceof Element && e.target.closest('.arrangement-graph')) return;
      e.preventDefault();
      const rect = lanesCanvas.ref.current?.getBoundingClientRect();
      onWheelRef.current(e, rect ? e.clientX - rect.left : 0);
    };
    element.addEventListener('wheel', listener, { passive: false });
    return () => element.removeEventListener('wheel', listener);
  }, [lanesCanvas.ref]);

  onWheelRef.current = (e, x) => {
    if (e.ctrlKey || e.metaKey) {
      const anchor = xToTick(view, x);
      const next = clampZoom(pxPerTick * Math.exp(-e.deltaY * 0.0015));
      setPxPerTick(next);
      setStartTick(Math.max(0, anchor - x / next));
    } else if (e.altKey && project) {
      // One undo step per wheel gesture (events less than a second apart).
      const now = Date.now();
      if (now - heightGesture.current.at > 1000) heightGesture.current.id = newGestureId();
      heightGesture.current.at = now;
      const factor = Math.exp(-e.deltaY * 0.002);
      dispatch(
        {
          command: 'batch',
          args: {
            label: 'Resize tracks',
            commands: lanes.map((l) => ({
              command: 'setTrackHeight' as const,
              args: { id: l.track.id, height: Math.round(l.height * factor) },
            })),
          },
        },
        heightGesture.current.id,
      );
    } else if (e.shiftKey || Math.abs(e.deltaX) > Math.abs(e.deltaY)) {
      const delta = e.shiftKey ? e.deltaY : e.deltaX;
      setStartTick((s) => Math.max(0, s + delta / pxPerTick));
    } else {
      setScrollY((s) => Math.min(maxScroll, Math.max(0, s + e.deltaY)));
    }
  };

  const clampZoom = (p: number) => Math.min(MAX_PX_PER_BEAT, Math.max(MIN_PX_PER_BEAT, p * TICKS_PER_BEAT)) / TICKS_PER_BEAT;

  const zoom = (factor: number) => {
    const width = lanesCanvas.size.width;
    const centre = xToTick(view, width / 2);
    const next = clampZoom(pxPerTick * factor);
    setPxPerTick(next);
    setStartTick(Math.max(0, centre - width / 2 / next));
  };

  const zoomToFit = () => {
    const clips = lanes.flatMap((l) => Object.values(l.track.clips));
    const end = Math.max(map.tickOfBar(4), ...clips.map(clipEnd));
    setPxPerTick(clampZoom((lanesCanvas.size.width * 0.95) / end));
    setStartTick(0);
  };

  const seekTo = (x: number, free: boolean) => {
    const tick = Math.max(0, xToTick(view, x));
    actions.seek((free || gridTicks <= 0 ? tick : map.snap(tick, gridTicks, true)) / TICKS_PER_BEAT);
  };

  // Marker row: double-click adds (or renames), drag moves, right-click removes.
  const markerAt = (x: number) => markers.find((m) => Math.abs(x - (m.position - startTick) * pxPerTick) <= MARKER_HIT_PX);
  const snapped = (x: number, free: boolean) => {
    const tick = Math.max(0, xToTick(view, x));
    return free ? Math.round(tick) : map.snap(tick, gridTicks, true);
  };

  const chosenClips = (): SelectedClip[] =>
    lanes.flatMap((l) => Object.values(l.track.clips).filter((c) => selectedClips.has(c.id)).map((clip) => ({ clip, track: l.track })));

  const startRename = (ids: number[]) => {
    renameCancelled.current = false;
    setRenamingClips(ids.length > 0 ? ids : null);
  };

  // Where the inline name field goes: over the name bar of the first clip being renamed.
  const renameBox = (() => {
    if (!renamingClips) return null;
    for (const lane of lanes) {
      const clip = lane.track.clips[String(renamingClips[0])];
      if (clip) return { box: clipBox(view, lane, clip, scrollY), name: clip.name || lane.track.name };
    }
    return null;
  })();

  const commitRename = (name: string) => {
    const ids = renamingClips ?? [];
    setRenamingClips(null);
    if (renameCancelled.current || ids.length === 0) return;
    dispatch({
      command: 'batch',
      args: { label: 'Rename clips', commands: ids.map((id) => ({ command: 'setClipProps' as const, args: { id, name: name.trim() } })) },
    });
  };

  if (!project) return null;

  // Browser items dropped on a track header or lane go into that track's chain, and below the tracks they make a new
  // track (paletteDrag.ts). The lanes and the header column share the same vertical layout.
  const deviceTarget = (e: React.DragEvent<HTMLElement>) => {
    const y = e.clientY - e.currentTarget.getBoundingClientRect().top;
    return { lane: laneAt(lanes, y, scrollY), y };
  };
  const deviceDropHandlers = {
    onDragOver: (e: React.DragEvent<HTMLElement>) => {
      const picked = draggedPaletteNode();
      if (!isPaletteDrag(e.dataTransfer) || !picked) return;
      e.preventDefault();
      const { lane } = deviceTarget(e);
      const end = totalHeight(lanes) - scrollY;
      if (lane) {
        const problem = chainProblem(lane.track, picked);
        e.dataTransfer.dropEffect = problem ? 'none' : 'copy';
        setDeviceDrop({ top: lane.top - scrollY, height: lane.height, label: problem ?? `Add to ${lane.track.name}`, ok: problem === null });
      } else {
        e.dataTransfer.dropEffect = 'copy';
        setDeviceDrop({ top: Math.max(0, end), height: 28, label: 'New track', ok: true });
      }
    },
    onDragLeave: (e: React.DragEvent<HTMLElement>) => {
      if (!e.currentTarget.contains(e.relatedTarget as Node)) setDeviceDrop(null);
    },
    onDrop: (e: React.DragEvent<HTMLElement>) => {
      const picked = draggedPaletteNode();
      if (!isPaletteDrag(e.dataTransfer) || !picked) return;
      e.preventDefault();
      setDeviceDrop(null);
      const { lane } = deviceTarget(e);
      if (lane) addToTrack(lane.track, picked);
      else addAsNewTrack(picked);
    },
  };
  const deviceDropMark = deviceDrop && (
    <div className={`device-drop-mark ${deviceDrop.ok ? '' : 'refused'}`} style={{ top: deviceDrop.top, height: deviceDrop.height }}>
      <span>{deviceDrop.label}</span>
    </div>
  );

  return (
    <div className="arrangement">
      {!graph && (
      <div className="roll-toolbar">
        <span className="roll-title">Arrangement</span>
        <label className="field">
          <span>Snap</span>
          <select
            value={snapChoice.label}
            onChange={(e) => setSnapChoice(SNAP_CHOICES.find((c) => c.label === e.target.value) ?? SNAP_CHOICES[0])}
          >
            {SNAP_CHOICES.map((c) => (
              <option key={c.label}>{c.label}</option>
            ))}
          </select>
        </label>
        <div className="spacer" />
        <HelpButton area="Arrangement" />
        <IconButton size="l" className={`${follow ? 'active' : ''}`} title="Follow playhead" onClick={() => ui.setFollow(!follow)}>
          <LocateFixed size={16} />
        </IconButton>
        <IconButton size="l" title="Zoom out" onClick={() => zoom(1 / 1.5)}>
          <ZoomOut size={16} />
        </IconButton>
        <IconButton size="l" title="Zoom in" onClick={() => zoom(1.5)}>
          <ZoomIn size={16} />
        </IconButton>
        <IconButton size="l" title="Zoom to fit" onClick={zoomToFit}>
          <Expand size={16} />
        </IconButton>
      </div>
      )}

      {missingAudio.length > 0 && (
        <div className="missing-banner">
          <span>
            {missingAudio.length === 1 ? 'An audio file is missing' : `${missingAudio.length} audio files are missing`}:{' '}
            {missingAudio
              .slice(0, 3)
              .map((m) => m.name)
              .join(', ')}
            {missingAudio.length > 3 ? '…' : ''}
          </span>
          <Button onClick={() => projectActions.locateAudio(missingAudio[0].id)}>
            Locate…
          </Button>
        </div>
      )}

      <div
        ref={bodyRef}
        className={`arrangement-grid ${graph ? 'graph-mode' : ''}`}
        style={{
          gridTemplateColumns: `${headerWidth}px minmax(0, 1fr)`,
          gridTemplateRows: graph
            ? `26px minmax(0, 1fr)${masterLane ? ` ${Math.min(masterLane.fullHeight, 320)}px` : ''}`
            : `26px 16px 16px 18px${tempoLaneShown ? ' 64px' : ''} minmax(0, 1fr)${masterLane ? ` ${Math.min(masterLane.fullHeight, 320)}px` : ''}`,
        }}
      >
        <div className="arrangement-corner graph-corner">
          <Button size="s" onClick={(e) => setTrackMenu(e.currentTarget.getBoundingClientRect())}>
            <Plus size={12} /> Track
          </Button>
          {trackMenu && (
            <>
              <Menu onClose={() => setTrackMenu(null)} at={{ x: trackMenu.left, y: trackMenu.bottom + 4 }}>
                <button
                  className="menu-item"
                  onClick={() => {
                    setTrackMenu(null);
                    dispatch({ command: 'addTrack', args: {} });
                  }}
                >
                  Instrument track
                </button>
                <button
                  className="menu-item"
                  onClick={() => {
                    setTrackMenu(null);
                    dispatch({ command: 'addTrack', args: { kind: 'audio' } });
                  }}
                >
                  Audio track
                </button>
                <button
                  className="menu-item"
                  onClick={() => {
                    setTrackMenu(null);
                    dispatch({ command: 'addTrack', args: { kind: 'return' } });
                  }}
                >
                  Return track
                </button>
              </Menu>
            </>
          )}
          <span className="spacer" />
          <GlobalSwitches />
        </div>
        <div className="canvas-stack">
          <canvas
            ref={ruler.ref}
            className="roll-ruler"
            title="Upper half: drag to set the loop range, drag inside it to move it, click to toggle it. Lower half: move the playhead (Ctrl: off the grid)"
            onPointerDown={(e) => {
              e.currentTarget.setPointerCapture(e.pointerId);
              if (!rulerInput.down(e, ruler.size.height)) seekTo(e.nativeEvent.offsetX, e.ctrlKey || e.metaKey);
            }}
            onPointerMove={(e) => {
              if (e.buttons !== 1) {
                e.currentTarget.style.cursor = rulerInput.cursorAt(e.nativeEvent.offsetX, e.nativeEvent.offsetY, ruler.size.height, e.altKey);
                const activeLoop = rulerInput.loop;
                if (activeLoop?.enabled && e.nativeEvent.offsetY <= ruler.size.height * 0.42) {
                  const startX = tickToX(view, activeLoop.start);
                  const endX = tickToX(view, activeLoop.end);
                  const edge =
                    Math.abs(e.nativeEvent.offsetX - startX) <= 8
                      ? 'start'
                      : Math.abs(e.nativeEvent.offsetX - endX) <= 8
                        ? 'end'
                        : null;
                  setLoopTooltip(edge ? { edge, x: e.nativeEvent.offsetX, y: e.nativeEvent.offsetY } : null);
                } else {
                  setLoopTooltip(null);
                }
                return;
              }
              if (!rulerInput.move(e)) seekTo(e.nativeEvent.offsetX, e.ctrlKey || e.metaKey);
            }}
            onPointerUp={rulerInput.up}
            onPointerLeave={() => setLoopTooltip(null)}
          />
          {loopTooltip && rulerInput.loop && (
            <div
              className="loop-tooltip"
              style={{ left: loopTooltip.x, top: loopTooltip.y }}
              role="tooltip"
            >
              {loopTooltip.edge === 'start' ? 'Start' : 'End'}{' '}
              {formatLoopPosition(
                loopTooltip.edge === 'start' ? rulerInput.loop.start : rulerInput.loop.end,
                map,
              )}
            </div>
          )}
          <PlayheadOverlay view={view} theme={theme} ruler />
        </div>

        <SignatureStrip
          view={view}
          map={map}
          corner={
            <>
              <span>Signature</span>
              <span className="spacer" />
              <Toggle
                size="s"
                className="corner-toggle"
                on={tempoLaneShown}
                title={tempoLaneShown ? 'Hide the tempo lane' : 'Show the tempo lane'}
                onChange={ui.setTempoLane}
              >
                Tempo
              </Toggle>
            </>
          }
        />

        <KeyStrip view={view} map={map} keys={project?.keys ?? []} />

        <div className="arrangement-corner marker-label">
          <span>Markers</span>
          <span className="spacer" />
          <CornerAdd
            title="Add a marker at the playhead (or double-click the strip)"
            onClick={() => dispatch({ command: 'addMarker', args: { position: playheadTick() } })}
          />
        </div>
        <canvas
          ref={markerRow.ref}
          className="marker-row"
          title="Double-click: add a marker (or rename one) · click: select · drag: move · right-click: rename or delete"
          tabIndex={0}
          onPointerDown={(e) => {
            if (e.button !== 0) return;
            const marker = markerAt(e.nativeEvent.offsetX);
            setSelectedMarker(marker?.id ?? null);
            if (!marker) return;
            e.currentTarget.focus({ preventScroll: true });
            e.currentTarget.setPointerCapture(e.pointerId);
            setMarkerDrag({ marker, position: marker.position, moved: false });
          }}
          onKeyDown={(e) => {
            if (selectedMarker === null) return;
            if (e.key === 'Delete') {
              e.preventDefault();
              dispatch({ command: 'removeMarker', args: { id: selectedMarker } });
              setSelectedMarker(null);
            } else if (e.key === 'Escape') setSelectedMarker(null);
          }}
          onBlur={() => setSelectedMarker(null)}
          onPointerMove={(e) => {
            if (!markerDrag || e.buttons !== 1) return;
            setMarkerDrag({ ...markerDrag, position: snapped(e.nativeEvent.offsetX, e.ctrlKey || e.metaKey), moved: true });
          }}
          onPointerUp={() => {
            if (markerDrag?.moved && markerDrag.position !== markerDrag.marker.position)
              dispatch({ command: 'moveMarker', args: { id: markerDrag.marker.id, position: markerDrag.position } });
            setMarkerDrag(null);
          }}
          onDoubleClick={(e) => {
            const marker = markerAt(e.nativeEvent.offsetX);
            if (marker) setRenaming(marker);
            else dispatch({ command: 'addMarker', args: { position: snapped(e.nativeEvent.offsetX, e.ctrlKey || e.metaKey) } });
          }}
          onContextMenu={(e) => {
            e.preventDefault();
            const marker = markerAt(e.nativeEvent.offsetX);
            if (marker) {
              setSelectedMarker(marker.id);
              setMarkerMenu({ marker, x: e.clientX, y: e.clientY });
            }
          }}
        />

        {markerMenu && (
          <>
            <Menu onClose={() => setMarkerMenu(null)} at={{ x: markerMenu.x, y: markerMenu.y }} onClick={() => setMarkerMenu(null)}>
              <div className="menu-heading">{markerMenu.marker.name || 'Marker'}</div>
              <MenuItem icon={Pencil} label="Rename…" onClick={() => setRenaming(markerMenu.marker)} />
              <MenuItem icon={Trash2} label="Delete" shortcut="Del" danger onClick={() => dispatch({ command: 'removeMarker', args: { id: markerMenu.marker.id } })} />
            </Menu>
          </>
        )}

        {tempoLaneShown && (
          <>
            <div className="arrangement-corner tempo-corner">
              <span className="marker-label">Tempo</span>
              <span className="tempo-now" title="The tempo at the playhead">
                <TempoNow map={map} />
              </span>
            </div>
            <TempoLane view={view} map={map} gridTicks={gridTicks} />
          </>
        )}

        <div className="track-header-column" {...deviceDropHandlers}>
          {deviceDropMark}
          <TrackHeaders lanes={lanes} scrollY={scrollY} selectedTrackId={selectedTrackId ?? lanes[0]?.track.id ?? null} />
          <Splitter
            className="header-splitter"
            title="Drag to resize the track headers"
            width={headerWidth}
            direction={1}
            onResize={layout.setHeaderWidth}
          />
        </div>
        <div
          className="arrangement-lanes"
          onDragLeave={deviceDropHandlers.onDragLeave}
          onDragOver={(e) => {
            deviceDropHandlers.onDragOver(e);
            if (!isAudioDrag(e.dataTransfer)) return;
            e.preventDefault();
            e.dataTransfer.dropEffect = 'copy';
          }}
          onDrop={(e) => {
            deviceDropHandlers.onDrop(e);
            if (!isAudioDrag(e.dataTransfer)) return;
            e.preventDefault();
            const rect = e.currentTarget.getBoundingClientRect();
            const lane = laneAt(lanes, e.clientY - rect.top, scrollY);
            const tick = map.snap(Math.max(0, xToTick(view, e.clientX - rect.left)), gridTicks, false);
            // Audio onto an audio track, MIDI onto an instrument track; anything else gets new tracks.
            importDrop(
              e.dataTransfer,
              lane?.track.kind === 'audio' ? lane.track.id : undefined,
              tick,
              lane?.track.kind === 'instrument' ? lane.track.id : undefined,
            );
          }}
        >
          {deviceDropMark}
          <canvas
            ref={lanesCanvas.ref}
            className="arrangement-canvas"
            tabIndex={0}
            style={{ cursor: editing.cursor }}
            onPointerDown={(e) => {
              if (!automationEditing.onPointerDown(e)) {
                automationEditing.clearSelection();
                editing.onPointerDown(e);
              }
            }}
            onPointerMove={(e) => {
              if (!automationEditing.onPointerMove(e)) editing.onPointerMove(e);
            }}
            onPointerUp={() => {
              if (!automationEditing.onPointerUp()) editing.onPointerUp();
            }}
            onPointerCancel={() => {
              if (!automationEditing.onPointerUp()) editing.onPointerUp();
            }}
            onDoubleClick={(e) => editing.onDoubleClick(e, (id) => startRename([id]))}
            onKeyDown={(e) => {
              if (!automationEditing.onKeyDown(e)) editing.onKeyDown(e, playheadNow(), selectedLane, () => startRename([...selectedClips]));
            }}
            onContextMenu={(e) => {
              e.preventDefault();
              const { offsetX: x, offsetY: y } = e.nativeEvent;
              const hit = hitClip(view, lanes, x, y, scrollY, map);
              if (hit && !selectedClips.has(hit.clip.id)) ui.selectClips([hit.clip.id]);
              const lane = laneAt(lanes, y, scrollY);
              if (lane) ui.selectTrack(lane.track.id);
              setMenu({
                x: e.clientX,
                y: e.clientY,
                tick: Math.max(0, xToTick(view, x)),
                // Among the lanes that hold clips; -1 on a group's lane.
                lane: lane ? lane.clipIndex : Math.max(0, clipLanes(lanes).length - 1),
                onClip: hit !== undefined,
              });
            }}
          />
          <PlayheadOverlay view={view} theme={theme} onTick={followTick} onPaint={paintPlayed} />
          {renameBox && (
            <input
              className="clip-rename"
              autoFocus
              defaultValue={renameBox.name}
              style={{
                left: renameBox.box.x0,
                top: renameBox.box.y0,
                width: Math.max(140, renameBox.box.x1 - renameBox.box.x0),
                height: NAME_BAR + 4,
              }}
              onFocus={(e) => e.target.select()}
              onBlur={(e) => commitRename(e.target.value)}
              onKeyDown={(e) => {
                e.stopPropagation();
                if (e.key === 'Enter') e.currentTarget.blur();
                else if (e.key === 'Escape') {
                  renameCancelled.current = true;
                  setRenamingClips(null);
                }
              }}
            />
          )}
        </div>

        {master && (
          <>
            <div className="master-column">
              <MasterHeader master={master} selected={selectedTrackId === master.id} />
              {masterLane?.automation.map((row) => <AutomationHeader key={row.lane.id} row={row} top={row.top} />)}
            </div>
            <div className="master-lane">
              <canvas
                ref={masterCanvas.ref}
                className="arrangement-canvas"
                tabIndex={0}
                onPointerDown={(e) => {
                  ui.selectTrack(master.id);
                  masterEditing.onPointerDown(e);
                }}
                onPointerMove={(e) => masterEditing.onPointerMove(e)}
                onPointerUp={() => masterEditing.onPointerUp()}
                onPointerCancel={() => masterEditing.onPointerUp()}
                onKeyDown={(e) => masterEditing.onKeyDown(e)}
              />
              <PlayheadOverlay view={view} theme={theme} onPaint={paintMasterPlayed} />
            </div>
          </>
        )}

        {graph && (
          <div className="arrangement-graph">
            <GraphView />
          </div>
        )}
      </div>

      {menu?.onClip && chosenClips().length > 0 && (
        <ClipMenu
          x={menu.x}
          y={menu.y}
          clips={chosenClips()}
          onClose={() => setMenu(null)}
          onRename={() => startRename([...selectedClips])}
          onCopy={() => editing.copySelection()}
          onCut={() => {
            if (editing.copySelection()) dispatch({ command: 'deleteClips', args: { ids: [...selectedClips] } });
          }}
          onPaste={() => editing.paste(playheadNow(), Math.max(0, selectedLane))}
          onDuplicate={(linked) => editing.duplicate([...selectedClips], linked)}
        />
      )}

      {menu && !menu.onClip && (
        <LaneMenu
          x={menu.x}
          y={menu.y}
          canPaste={editing.hasClipboard()}
          onClose={() => setMenu(null)}
          onPaste={() => editing.paste(map.snap(menu.tick, gridTicks, false), Math.max(0, menu.lane))}
          onNewClip={() => {
            const lane = clipLanes(lanes)[menu.lane];
            if (lane)
              dispatch({
                command: 'addClip',
                args: { trackId: lane.track.id, start: map.barFloor(menu.tick), length: map.barLengthAt(menu.tick) },
              });
          }}
          onAddMarker={() => dispatch({ command: 'addMarker', args: { position: map.snap(menu.tick, gridTicks, true) } })}
        />
      )}

      {renaming && (
        <TextPrompt
          title="Rename marker"
          label="Name"
          initial={renaming.name}
          confirmLabel="Rename"
          onDone={(name) => {
            if (name) dispatch({ command: 'renameMarker', args: { id: renaming.id, name } });
            setRenaming(null);
          }}
        />
      )}
    </div>
  );
}
