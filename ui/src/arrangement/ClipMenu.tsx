import { AudioLines, ClipboardPaste, FlipHorizontal2, Gauge, Grid3x3, Copy, CopyPlus, FileSearch, Flag, Link, Merge, Pencil, Piano, Repeat, Scissors, Split, SquarePlus, Trash2, Unlink, type LucideIcon } from 'lucide-react';
import * as bridge from '../bridge/bridge';
import { TICKS_PER_BEAT, type Clip, type Command, type Track } from '../bridge/protocol';
import { Swatches } from '../components/Swatches';
import { MenuItem } from '../components/MenuItem';
import { dispatch, projectActions, reportError, useDawg } from '../store/store';
import { ui } from '../store/uiStore';
import { clipEnd, isLooped } from '../timeline/clips';
import { activeMidiEffects } from '../components/chain';
import { DragNumber } from '../components/DragNumber';
import { patternName, sequencerOf } from '../dock/stepGrid';
import { coveredEdges } from '../timeline/crossfades';
import { Button } from '../ui/controls';
import { Menu } from '../ui/overlays';
import { deleteClips } from './useArrangementEditing';

// Right-click menus of the arrangement: one for clips (acting on the selection), one for empty lane space.

function Item(props: { icon?: LucideIcon; label: string; shortcut?: string; onClick: () => void; disabled?: boolean; checked?: boolean }) {
  return <MenuItem {...props} />;
}

export interface SelectedClip {
  clip: Clip;
  track: Track;
}

export function ClipMenu({
  x,
  y,
  clips,
  onClose,
  onRename,
  onCopy,
  onCut,
  onPaste,
  onDuplicate,
}: {
  x: number;
  y: number;
  clips: SelectedClip[];
  onClose: () => void;
  onRename: () => void;
  onCopy: () => void;
  onCut: () => void;
  onPaste: () => void;
  onDuplicate: (linked: boolean) => void;
}) {
  // Subscribed here, not in the arrangement: only the open menu needs the playhead moving under it.
  const playheadTick = useDawg((s) => s.telemetry.positionBeats) * TICKS_PER_BEAT;
  const ids = clips.map((c) => c.clip.id);
  const anyLooped = clips.some((c) => isLooped(c.clip));
  const allLooped = clips.every((c) => isLooped(c.clip));
  const anyLinked = clips.some((c) => c.clip.linkId !== 0);
  const oneTrack = clips.every((c) => c.track.id === clips[0].track.id);
  const splittable = clips.some((c) => playheadTick > c.clip.start && playheadTick < clipEnd(c.clip));
  const first = clips[0];
  const anyAudio = clips.some((c) => c.clip.fileId !== 0);
  const audioClips = clips.filter((c) => c.clip.fileId !== 0);
  const missing = useDawg((s) => s.missingAudio);
  const missingClip = audioClips.find((c) => missing.some((m) => m.id === c.clip.fileId));

  const files = useDawg((s) => s.project?.audioFiles);
  const modeOf = (fileId: number) => files?.[String(fileId)]?.timing?.mode;
  const fileIdsOf = (list: SelectedClip[]) => [...new Set(list.map((c) => c.clip.fileId))];
  const crossfaded = audioClips.filter(({ clip, track }) => {
    const edges = coveredEdges(track, clip);
    return edges.start || edges.end;
  });

  const run = (action: () => void) => () => {
    onClose();
    action();
  };

  // Long with the audio tools: it scrolls where it doesn't fit below its top.
  const menuTop = Math.max(8, Math.min(y, window.innerHeight - 560));

  const batch = (label: string, commands: Command[]) => dispatch({ command: 'batch', args: { label, commands } });

  return (
    <>
      <Menu
        onClose={onClose}
        at={{ x: Math.min(x, window.innerWidth - 240), y: menuTop }}
        className="clip-menu"
        style={{ maxHeight: window.innerHeight - menuTop - 8, overflowY: 'auto' }}
      >
        <div className="menu-heading">{clips.length === 1 ? first.clip.name || first.track.name : `${clips.length} clips`}</div>
        {clips.length === 1 && !anyAudio && (
          <Item icon={Piano} label="Open in piano roll" shortcut="Enter" onClick={run(() => ui.editClip(first.track.id, first.clip.id))} />
        )}
        {missingClip && <Item icon={FileSearch} label="Locate file…" onClick={run(() => projectActions.locateAudio(missingClip.clip.fileId))} />}
        <Item icon={Pencil} label="Rename…" shortcut="F2" onClick={run(onRename)} />

        <div className="menu-heading">Colour</div>
        <Swatches
          current={clips.length === 1 ? first.clip.colour : '-'}
          reset={{ label: 'Use track colour', colour: first.track.colour }}
          onPick={(colour) => {
            onClose();
            batch(
              'Change clip colour',
              ids.map((id) => ({ command: 'setClipProps', args: { id, colour } })),
            );
          }}
        />
        <div className="menu-separator" />

        <Item
          icon={Repeat}
          checked={allLooped}
          label="Loop clip"
          shortcut="Ctrl+L"
          onClick={run(() =>
            batch(
              allLooped ? 'Unloop clips' : 'Loop clips',
              clips.map(({ clip }) => ({
                command: 'setClipLoop',
                args: { id: clip.id, loopLength: allLooped ? 0 : isLooped(clip) ? clip.loopLength : clip.length },
              })),
            ),
          )}
        />
        {anyLooped && (
          <Item
            icon={Repeat}
            label="Unroll loop"
            onClick={run(() =>
              batch(
                'Unroll loop',
                clips.filter((c) => isLooped(c.clip)).map((c) => ({ command: 'glueClips', args: { ids: [c.clip.id] } })),
              ),
            )}
          />
        )}
        <Item
          icon={Split}
          label="Split at playhead"
          shortcut="S"
          disabled={!splittable}
          onClick={run(() => dispatch({ command: 'splitClips', args: { ids, at: Math.round(playheadTick) } }))}
        />
        <div className="menu-separator" />

        <Item icon={CopyPlus} label="Duplicate" shortcut="Ctrl+D" onClick={run(() => onDuplicate(false))} />
        {audioClips.length > 0 &&
          (['in', 'out'] as const).map((edge) => (
            <div key={edge}>
              <div className="menu-heading">Fade {edge} shape</div>
              {['Linear', 'Equal power', 'Exponential', 'S-curve'].map((label, shape) => (
                <Item
                  key={label}
                  label={label}
                  checked={audioClips.every((c) => (edge === 'in' ? c.clip.fadeInShape : c.clip.fadeOutShape) === shape)}
                  onClick={run(() =>
                    batch(
                      'Change fade shape',
                      audioClips.map(({ clip }) => ({
                        command: 'setClipProps',
                        args: edge === 'in' ? { id: clip.id, fadeInShape: shape } : { id: clip.id, fadeOutShape: shape },
                      })),
                    ),
                  )}
                />
              ))}
            </div>
          ))}
        {audioClips.length > 0 && <div className="menu-separator" />}
        {audioClips.length > 0 && (
          <>
            {/* Timing (docs/design/time-stretching.md, TS12): per pool file, so it applies to every clip of the file. */}
            <Item
              label="Make musical"
              disabled={fileIdsOf(audioClips).every((id) => modeOf(id) === 'musical')}
              onClick={run(() =>
                batch(
                  'Make musical',
                  fileIdsOf(audioClips)
                    .filter((id) => modeOf(id) !== 'musical')
                    .map((fileId) => ({ command: 'setFileTiming', args: { fileId, mode: 'musical' } })),
                ),
              )}
            />
            <Item
              label="Make one-shot"
              disabled={fileIdsOf(audioClips).every((id) => modeOf(id) !== 'musical')}
              onClick={run(() =>
                batch(
                  'Make one-shot',
                  fileIdsOf(audioClips)
                    .filter((id) => modeOf(id) === 'musical')
                    .map((fileId) => ({ command: 'setFileTiming', args: { fileId, mode: 'oneShot' } })),
                ),
              )}
            />
            {audioClips.length === 1 && modeOf(audioClips[0].clip.fileId) === 'musical' && (
              <Item label="Make copy" onClick={run(() => dispatch({ command: 'copyPoolFile', args: { clipId: audioClips[0].clip.id } }))} />
            )}
            {/* Crossfades over audio below (docs/design/audio-features.md, AU8). */}
            {crossfaded.length > 0 && (
              <Item
                label={crossfaded.length === 1 ? 'Crossfade' : 'Crossfade selected'}
                onClick={run(() => dispatch({ command: 'crossfadeClips', args: { clipIds: crossfaded.map((c) => c.clip.id) } }))}
              />
            )}
            {/* Clip operations (AU1, AU2). */}
            <Item
              icon={FlipHorizontal2}
              label="Reverse"
              onClick={run(() => bridge.call('audio.reverseClips', { clipIds: audioClips.map((c) => c.clip.id) }).catch(reportError))}
            />
            <Item icon={Gauge} label="Normalise…" onClick={run(() => useDawg.setState({ normalise: audioClips.map((c) => c.clip.id) }))} />
            <Item
              icon={Grid3x3}
              label="Slice to Sampler…"
              disabled={audioClips.length !== 1 || isLooped(audioClips[0].clip)}
              onClick={run(() => useDawg.setState({ slice: audioClips[0].clip.id }))}
            />
            {/* Audio to MIDI (audio-features.md, AU4): onto a new instrument track after the clip's. */}
            {(
              [
                ['Drums to MIDI', { kind: 'drums', split: false }],
                ['Drums to MIDI (kick, snare, hats)', { kind: 'drums', split: true }],
                ['Melody to MIDI', { kind: 'pitch', curves: true }],
              ] as const
            ).map(([label, options]) => (
              <Item
                key={label}
                icon={Piano}
                label={label}
                disabled={audioClips.length !== 1 || isLooped(audioClips[0].clip)}
                onClick={run(() => bridge.call('audio.toMidi', { clipId: audioClips[0].clip.id, sensitivity: 0.75, ...options }).catch(reportError))}
              />
            ))}
            {/* Consolidate: what the clips play, without the track's chain, as one clip (audio-features.md, AU16). */}
            <Item
              icon={Merge}
              label="Consolidate"
              disabled={audioClips.length < 1 || !audioClips.every((c) => c.track.id === audioClips[0].track.id)}
              onClick={run(() => bridge.call('audio.consolidateClips', { clipIds: audioClips.map((c) => c.clip.id) }).catch(reportError))}
            />
            <Item
              icon={AudioLines}
              label="Open in audio editor"
              disabled={audioClips.length !== 1}
              onClick={run(() => bridge.call('audioEditor.show', { open: true, clip: audioClips[0].clip.id }).catch(() => {}))}
            />
            <div className="menu-separator" />
          </>
        )}
        {/* A sequencer clip: its pattern, the step it starts on, and its notes as a MIDI clip (docs/design/step-sequencer.md). */}
        {clips.length === 1 && first.clip.sequencerPattern >= 0 && (
          <>
            <div className="menu-row step-clip-patterns" title="The pattern this clip plays">
              <span className="menu-row-label">Pattern</span>
              {(sequencerOf(first.track)?.steps?.patterns ?? []).map((_, index) => (
                <Button
                  key={index}
                  className={`step-pattern ${index === first.clip.sequencerPattern ? 'active' : ''}`}
                  onClick={() => dispatch({ command: 'setClipSequencer', args: { clipId: first.clip.id, pattern: index } })}
                >
                  {patternName(index)}
                </Button>
              ))}
            </div>
            <div className="menu-row" title="The step the pattern starts on at the clip's start">
              <span className="menu-row-label">Start step</span>
              <DragNumber
                value={first.clip.sequencerStartStep + 1}
                min={1}
                max={64}
                integer
                resetValue={1}
                onChange={(v, gesture) => dispatch({ command: 'setClipSequencer', args: { clipId: first.clip.id, startStep: v - 1 } }, gesture)}
              />
            </div>
            <Item
              icon={Piano}
              label="Render steps to MIDI"
              onClick={run(() => dispatch({ command: 'renderStepClip', args: { clipId: first.clip.id } }))}
            />
            <div className="menu-separator" />
          </>
        )}
        {/* What the MIDI effects play, as notes (docs/design/midi-effects.md, ME17). */}
        {clips.length === 1 && !anyAudio && activeMidiEffects(first.track).length > 0 && (
          <Item
            icon={Piano}
            label="Render MIDI effects to clip"
            onClick={run(() => dispatch({ command: 'renderMidiEffects', args: { clipId: first.clip.id } }))}
          />
        )}
        {!anyAudio && <Item
          icon={Link}
          label="Linked duplicate"
          shortcut="Ctrl+Alt+D"
          onClick={run(() => onDuplicate(true))}
        />}
        {anyLinked && <Item icon={Unlink} label="Make unique" onClick={run(() => dispatch({ command: 'makeUnique', args: { ids } }))} />}
        <Item
          icon={Merge}
          label="Glue"
          shortcut="Ctrl+J"
          disabled={clips.length < 2 || !oneTrack || anyAudio}
          onClick={run(() => dispatch({ command: 'glueClips', args: { ids } }))}
        />
        <div className="menu-separator" />

        {/* Render selection (AU17): the clips' tracks over their span, or the loop range when it is on. */}
        <Item
          label="Render to new track"
          onClick={run(() =>
            bridge
              .call('audio.renderSelection', { trackIds: [...new Set(clips.map((c) => c.track.id))], clipIds: ids })
              .catch(reportError),
          )}
        />
        <div className="menu-separator" />
        <Item icon={Scissors} label="Cut" shortcut="Ctrl+X" onClick={run(onCut)} />
        <Item icon={Copy} label="Copy" shortcut="Ctrl+C" onClick={run(onCopy)} />
        <Item icon={ClipboardPaste} label="Paste" shortcut="Ctrl+V" onClick={run(onPaste)} />
        <Item icon={Trash2} label="Delete" shortcut="Del" onClick={run(() => deleteClips(ids))} />
      </Menu>
    </>
  );
}

export function LaneMenu({
  x,
  y,
  canPaste,
  onClose,
  onPaste,
  onNewClip,
  onAddMarker,
}: {
  x: number;
  y: number;
  canPaste: boolean;
  onClose: () => void;
  onPaste: () => void;
  onNewClip: () => void;
  onAddMarker: () => void;
}) {
  const run = (action: () => void) => () => {
    onClose();
    action();
  };

  return (
    <>
      <Menu onClose={onClose} at={{ x: Math.min(x, window.innerWidth - 220), y: Math.min(y, window.innerHeight - 140) }}>
        <Item icon={ClipboardPaste} label="Paste here" disabled={!canPaste} onClick={run(onPaste)} />
        <Item icon={SquarePlus} label="New clip here" onClick={run(onNewClip)} />
        <Item icon={Flag} label="Add marker here" onClick={run(onAddMarker)} />
      </Menu>
    </>
  );
}
