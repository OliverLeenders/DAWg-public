import { Circle, Metronome, Pause, Piano, Play, Redo2, Repeat, Rows3, SlidersVertical, Square, SquareDashedBottom, Undo2, Workflow } from 'lucide-react';
import { ui, useUi } from '../store/uiStore';
import { actions, dispatch, useDawg } from '../store/store';
import { DragNumber } from './DragNumber';
import { FileMenu, ProjectName } from './FileMenu';
import { ViewMenu } from './ViewMenu';
import { EditMenu } from './EditMenu';
import { HelpMenu } from './HelpMenu';
import { Meter } from './Meter';
import { MidiSettings } from './MidiSettings';
import { useWindowDrag, WindowControls } from './WindowControls';
import { BrandMark } from './BrandMark';
import { useLayout } from '../store/layoutStore';
import { TICKS_PER_BEAT } from '../bridge/protocol';
import { tempoMapOf, type TempoMap } from '../time/tempoMap';
import { SignaturePrompt } from '../arrangement/SignatureStrip';
import { useEffect, useState } from 'react';
import * as bridge from '../bridge/bridge';
import { logTaper } from './dragScale';
import { Button, IconButton } from '../ui/controls';
import { Bar } from '../ui/Bar';

// The tempo drags along a log curve, 2 px a BPM around 120 (dragScale.ts).
const TEMPO_TAPER = logTaper(20, 999);

/** The pre-roll choices, in bars. */
const PRE_ROLLS = [0, 1, 2, 4];

/** m:ss.mmm of real time through the tempo map (docs/design/tempo-map.md, TM13). */
function formatTime(beats: number, map: TempoMap) {
  const seconds = Math.max(0, map.secondsAt(Math.max(0, beats) * TICKS_PER_BEAT));
  const minutes = Math.floor(seconds / 60);
  const rest = seconds - minutes * 60;
  return `${minutes}:${rest.toFixed(3).padStart(6, '0')}`;
}

/** bar.beat.ticks (1-based, beats in the signature's unit, 960 ticks per beat as shown). */
function formatPosition(beats: number, map: TempoMap) {
  const signature = map.signatureAt(Math.max(0, beats) * TICKS_PER_BEAT);
  const position = map.barAt(Math.max(0, beats) * TICKS_PER_BEAT);
  const ticks = Math.floor((position.tickInBeat / ((TICKS_PER_BEAT * 4) / signature.denominator)) * 960);
  return `${position.bar + 1}.${position.beat + 1}.${String(ticks).padStart(3, '0')}`;
}

export function TopBar() {
  const project = useDawg((s) => s.project);
  const history = useDawg((s) => s.history);
  const [preRoll, setPreRoll] = useState(1);
  useEffect(() => {
    bridge
      .call('record.preRoll', {})
      .then((bars) => setPreRoll(bars))
      .catch(() => {});
  }, []);
  // Narrow selectors: the whole telemetry object is replaced 30 times a second, and subscribing to it re-rendered
  // the top bar even when nothing it shows had changed.
  const playing = useDawg((s) => s.telemetry.playing);
  const recording = useDawg((s) => s.telemetry.recording);
  const positionBeats = useDawg((s) => s.telemetry.positionBeats);
  const peakLeft = useDawg((s) => s.telemetry.peakLeft);
  const peakRight = useDawg((s) => s.telemetry.peakRight);
  const cpuPercent = useDawg((s) => Math.round(s.telemetry.cpu * 100));
  const latencySamples = useDawg((s) => s.telemetry.latencySamples);
  const allocationViolations = useDawg((s) => s.telemetry.allocationViolations);
  // Allocation violations only happen in Debug builds (the guard is compiled out otherwise): mention them only then.
  const performanceTitle =
    `Audio CPU load ${cpuPercent}%, delay compensation ${latencySamples} samples` +
    (allocationViolations > 0 ? `, ${allocationViolations} audio-thread allocations (Debug)` : '') +
    (Date.now() - useDawg.getState().latePlugins.at < 10000
      ? `. Too slow just now, so some of their sound was lost: ${useDawg.getState().latePlugins.names.join(', ')}`
      : '');
  const late = useDawg((s) => s.latePlugins);
  const lateNow = Date.now() - late.at < 10000;
  const masterId = project?.master?.id;
  const masterLeft = useDawg((s) => (masterId === undefined ? 0 : (s.telemetry.meters?.[String(masterId)]?.[0] ?? 0)));
  const masterRight = useDawg((s) => (masterId === undefined ? 0 : (s.telemetry.meters?.[String(masterId)]?.[1] ?? 0)));
  const audio = useDawg((s) => s.audio);
  const projectOpen = useDawg((s) => s.projectState.open);
  const centre = useUi((s) => s.centre);
  const mixerOpen = useDawg((s) => s.mixerOpen);
  const editorOpen = useDawg((s) => s.editorOpen);
  const timeDisplay = useLayout((s) => s.timeDisplay);

  const startWindowDrag = useWindowDrag();
  const [editingSignature, setEditingSignature] = useState(false);

  if (!project) return null;

  // The BPM and signature fields show and edit the values at the playhead (docs/design/tempo-map.md, TM11).
  const map = tempoMapOf(project);
  const playhead = Math.max(0, Math.round(positionBeats * TICKS_PER_BEAT));
  const signature = map.signatureAt(playhead);

  return (
    // One row that never wraps: when it is too narrow, whole groups fold into a "⋯" menu, the status first
    // (design-system.md, DS14). The project's controls aren't shown on the welcome screen (docs/design/ux-review.md, U4).
    <Bar
      className="topbar"
      onPointerDown={startWindowDrag}
      items={[
        {
          key: 'brand',
          node: (
            <>
              <div className="brand">
                <BrandMark size={22} />
                <span className="brand-name">
                  DAW<span>g</span>
                </span>
              </div>
              <ProjectName />
            </>
          ),
        },
        {
          key: 'menus',
          node: (
            <div className="group">
              <FileMenu />
              <EditMenu />
              <ViewMenu />
              <HelpMenu />
            </div>
          ),
        },
        projectOpen && {
          key: 'history',
          fold: 4,
          node: (
            <div className="group">
              <IconButton
                size="l"
                title={history.canUndo ? `Undo ${history.undoLabel} (Ctrl+Z)` : 'Nothing to undo'}
                disabled={!history.canUndo}
                onClick={actions.undo}
              >
                <Undo2 size={16} />
              </IconButton>
              <IconButton
                size="l"
                title={history.canRedo ? `Redo ${history.redoLabel} (Ctrl+Shift+Z)` : 'Nothing to redo'}
                disabled={!history.canRedo}
                onClick={actions.redo}
              >
                <Redo2 size={16} />
              </IconButton>
            </div>
          ),
        },
        // Play, Stop and Record together, Play first, then the loop and punch (docs/ui-review-2026-11-10.md, 3.2).
        projectOpen && {
          key: 'transport',
          fold: 1,
          node: (
            <div className="group transport">
              <IconButton
                size="l"
                className={`play ${playing ? 'active' : ''}`}
                title={playing ? 'Pause (Space)' : 'Play (Space)'}
                onClick={actions.togglePlay}
              >
                {playing ? <Pause size={16} /> : <Play size={16} />}
              </IconButton>
              <IconButton size="l" title="Stop (returns to where playback started; press again for the start)" onClick={actions.stop}>
                <Square size={14} />
              </IconButton>
              <IconButton
                size="l"
                className={`record ${recording ? 'active' : ''}`}
                title={`Record armed tracks (R; Shift+R with ${preRoll === 0 ? 'no' : `a ${preRoll}-bar`} pre-roll)`}
                onClick={(e) => actions.record(e.shiftKey)}
              >
                <Circle size={14} />
              </IconButton>
              <IconButton
                size="l"
                className={`loop ${project.loop.enabled ? 'active' : ''}`}
                title={`Loop ${project.loop.enabled ? 'on' : 'off'}: playback repeats the loop range (also the ruler's upper half: click, or drag to set it)`}
                aria-pressed={project.loop.enabled}
                onClick={() => dispatch({ command: 'setLoop', args: { enabled: !project.loop.enabled } })}
              >
                <Repeat size={14} />
              </IconButton>
              {/* Punch and pre-roll (docs/design/audio-features.md, AU10, AU12). */}
              <IconButton
                size="l"
                className={`punch ${project.punch?.enabled ? 'active' : ''}`}
                title="Punch in and out (P): recordings keep only the punch range. Alt + drag in the ruler sets it"
                onClick={() => dispatch({ command: 'setPunch', args: { enabled: !project.punch?.enabled } })}
              >
                <SquareDashedBottom size={14} />
              </IconButton>
              <Button
                className="pre-roll"
                title="Pre-roll before recording with Shift+R (click to change)"
                onClick={() => {
                  const next = PRE_ROLLS[(PRE_ROLLS.indexOf(preRoll) + 1) % PRE_ROLLS.length];
                  setPreRoll(next);
                  bridge.call('record.preRoll', { bars: next }).catch(() => {});
                }}
              >
                {preRoll === 0 ? 'No pre-roll' : `Pre-roll ${preRoll}`}
              </Button>
              <div
                className="lcd position clickable"
                title={timeDisplay === 'bars' ? 'Bars and beats (click for minutes and seconds)' : 'Minutes and seconds (click for bars and beats)'}
                onClick={() => useLayout.setState({ timeDisplay: timeDisplay === 'bars' ? 'time' : 'bars' })}
              >
                {timeDisplay === 'bars' ? formatPosition(positionBeats, map) : formatTime(positionBeats, map)}
              </div>
            </div>
          ),
        },
        projectOpen && {
          key: 'tempo',
          fold: 2,
          node: (
            <div className="group">
              <label className="field">
                <span>BPM</span>
                <DragNumber
                  className="lcd"
                  value={map.bpmAt(playhead)}
                  min={20}
                  max={999}
                  quantum={0.01}
                  taper={TEMPO_TAPER}
                  rangePixels={1000}
                  format={(v) => v.toFixed(2)}
                  onChange={(bpm, gesture) => dispatch({ command: 'setTempo', args: { bpm, at: playhead } }, gesture)}
                />
                {map.hasTempoChanges && (
                  <span className="badge" title="The song has tempo changes: this is the tempo at the playhead">
                    MAP
                  </span>
                )}
              </label>
              <label className="field">
                <span>Sig</span>
                {/* The signature at the playhead, edited in the signature strip's dialog: beats per bar and note value. */}
                <button className="lcd signature-button" title="The time signature at the playhead: click to change it" onClick={() => setEditingSignature(true)}>
                  {signature.numerator}/{signature.denominator}
                </button>
              </label>
              {editingSignature && (
                // The top bar drags the window from its empty parts: the dialog's presses stay in the dialog.
                <div onPointerDown={(e) => e.stopPropagation()}>
                  <SignaturePrompt
                    signature={signature}
                    isNew={false}
                    onDone={(result) => {
                      setEditingSignature(false);
                      if (result === 'remove') dispatch({ command: 'removeSignature', args: { bar: signature.bar } });
                      else if (result) dispatch({ command: 'setSignature', args: result });
                    }}
                  />
                </div>
              )}
              <IconButton
                size="l"
                className={`${project.metronome ? 'active' : ''}`}
                title="Metronome"
                onClick={() => dispatch({ command: 'setMetronome', args: { enabled: !project.metronome } })}
              >
                <Metronome size={16} />
              </IconButton>
            </div>
          ),
        },
        projectOpen && {
          key: 'views',
          fold: 3,
          node: (
            <div className="group view-switch" title="Switch views (Tab); the piano roll opens in its own window">
              <IconButton
                size="l"
                className={`${centre === 'arrangement' ? 'active' : ''}`}
                title="Arrangement (Tab)"
                onClick={() => ui.setCentre('arrangement')}
              >
                <Rows3 size={16} />
              </IconButton>
              <IconButton
                size="l"
                className={`${editorOpen ? 'active' : ''}`}
                title={editorOpen ? 'Close the piano roll window' : 'Open the piano roll in its own window'}
                onClick={() => actions.showEditor(!editorOpen)}
              >
                <Piano size={16} />
              </IconButton>
              <IconButton
                size="l"
                className={`${centre === 'graph' ? 'active' : ''}`}
                title="The selected track's node graph, beside the track headers (Tab)"
                onClick={() => ui.setCentre('graph')}
              >
                <Workflow size={16} />
              </IconButton>
            </div>
          ),
        },
        projectOpen && {
          key: 'mixer',
          fold: 3,
          node: (
            <div className="group">
              <IconButton
                size="l"
                className={`${mixerOpen ? 'active' : ''}`}
                title={mixerOpen ? 'Close the mixer window (Ctrl+M)' : 'Open the mixer in its own window (Ctrl+M)'}
                onClick={() => actions.showMixer(!mixerOpen)}
              >
                <SlidersVertical size={16} />
              </IconButton>
            </div>
          ),
        },
        { key: 'spacer', grow: true, node: <div className="spacer" /> },
        {
          key: 'status',
          fold: 5,
          node: (
            <div className="group status">
              {/* The master track's output (docs/design/mixing-basics.md, M5); the device output without one. */}
              {project.master ? (
                <Meter left={masterLeft} right={masterRight} title="Master output peak" />
              ) : (
                <Meter left={peakLeft} right={peakRight} />
              )}
              <MidiSettings />
              <span className={`stat cpu ${lateNow ? 'late' : ''}`} title={performanceTitle} aria-label={performanceTitle}>
                CPU {cpuPercent}%
              </span>
              <Button onClick={actions.openAudioSettings} title="Audio settings">
                {audio.open ? `${audio.name} · ${audio.sampleRate} Hz · ${audio.bufferSize} smp` : 'No audio device'}
              </Button>
            </div>
          ),
        },
        { key: 'window', end: true, node: <WindowControls /> },
      ]}
    />
  );
}
