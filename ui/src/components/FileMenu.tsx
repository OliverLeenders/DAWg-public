import { ChevronRight } from 'lucide-react';
import { useEffect, useMemo, useState } from 'react';
import * as bridge from '../bridge/bridge';
import { TICKS_PER_BEAT } from '../bridge/protocol';
import { projectActions, useDawg } from '../store/store';
import { ui, useUi } from '../store/uiStore';
import { TextPrompt } from './ConfirmDiscard';
import { Button } from '../ui/controls';
import { Menu, below, type Point } from '../ui/overlays';

// The "File" menu in the top bar (docs/design/phase5-project-workflow.md, section 8).

function Item({
  label,
  shortcut,
  disabled,
  onClick,
}: {
  label: string;
  shortcut?: string;
  disabled?: boolean;
  onClick: () => void;
}) {
  return (
    <button className="menu-item with-shortcut" disabled={disabled} onClick={onClick}>
      <span>{label}</span>
      {shortcut && <kbd>{shortcut}</kbd>}
    </button>
  );
}

const formatBytes = (bytes: number) =>
  bytes >= 1e9 ? `${(bytes / 1e9).toFixed(1)} GB` : bytes >= 1e6 ? `${(bytes / 1e6).toFixed(0)} MB` : `${Math.round(bytes / 1e3)} kB`;

export function FileMenu() {
  const [open, setOpen] = useState<Point | null>(null);
  const [recentOpen, setRecentOpen] = useState(false);
  const [templatePrompt, setTemplatePrompt] = useState(false);
  const [stretchCache, setStretchCache] = useState<{ size: number; limit: number } | null>(null);
  useEffect(() => {
    if (open) bridge.call('audio.stretchCache').then(setStretchCache).catch(() => setStretchCache(null));
  }, [open]);
  const projectOpen = useDawg((s) => s.projectState.open);
  const name = useDawg((s) => s.projectState.name);
  const allRecent = useDawg((s) => s.recent);
  const recent = useMemo(() => allRecent.filter((p) => !p.missing).slice(0, 10), [allRecent]);

  const run = (action: () => void) => () => {
    setOpen(null);
    setRecentOpen(false);
    action();
  };

  return (
    <div className="file-menu">
      <Button className={`${open ? 'active' : ''}`} onClick={(e) => setOpen(open ? null : below(e.currentTarget))}>
        File
      </Button>

      {open && (
        <>
          <Menu at={open} onClose={() => setOpen(null)} className="file-menu-popup">
            <Item label="New" shortcut="Ctrl+N" onClick={run(() => projectActions.newProject())} />
            <Item label="Open…" shortcut="Ctrl+O" onClick={run(() => projectActions.open())} />
            <div
              className="submenu"
              onPointerEnter={() => setRecentOpen(true)}
              onPointerLeave={() => setRecentOpen(false)}
            >
              <button className="menu-item with-shortcut" disabled={recent.length === 0}>
                <span>Open recent</span>
                <ChevronRight size={12} />
              </button>
              {recentOpen && recent.length > 0 && (
                <div className="context-menu submenu-popup">
                  {recent.map((p) => (
                    <button
                      key={p.path}
                      className="menu-item"
                      title={p.path}
                      onClick={run(() => projectActions.open(p.path))}
                    >
                      {p.name}
                    </button>
                  ))}
                </div>
              )}
            </div>
            <Item
              label="Import audio…"
              disabled={!projectOpen}
              onClick={run(() => {
                const s = useDawg.getState();
                const trackId = useUi.getState().selectedTrackId ?? undefined;
                const track = trackId !== undefined ? s.project?.tracks[String(trackId)] : undefined;
                projectActions.importAudio(
                  track?.kind === 'audio' ? track.id : undefined,
                  Math.round(s.telemetry.positionBeats * TICKS_PER_BEAT),
                );
              })}
            />
            <Item label="Import MIDI file…" disabled={!projectOpen} onClick={run(() => projectActions.importMidi())} />
            <Item label="Export audio…" shortcut="Ctrl+E" disabled={!projectOpen} onClick={run(() => ui.setExportOpen(true))} />
            <Item label="Export MIDI file…" disabled={!projectOpen} onClick={run(() => projectActions.exportMidi())} />
            <div className="menu-separator" />
            <Item label="Save" shortcut="Ctrl+S" disabled={!projectOpen} onClick={run(() => projectActions.save())} />
            <Item
              label="Save as…"
              shortcut="Ctrl+Shift+S"
              disabled={!projectOpen}
              onClick={run(() => projectActions.save(true))}
            />
            <Item label="Save as template…" disabled={!projectOpen} onClick={run(() => setTemplatePrompt(true))} />
            <div className="menu-separator" />
            {/* Renders of stretched audio are made again when needed (docs/design/time-stretching.md, TS8). */}
            <Item
              label={`Clear stretch cache${stretchCache ? ` (${formatBytes(stretchCache.size)})` : ''}`}
              onClick={run(() => bridge.call('audio.clearStretchCache').catch(() => {}))}
            />
            <div className="menu-separator" />
            <Item label="Close" shortcut="Ctrl+W" disabled={!projectOpen} onClick={run(() => projectActions.close())} />
          </Menu>
        </>
      )}

      {templatePrompt && (
        <TextPrompt
          title="Save as template"
          label="Template name"
          initial={name}
          confirmLabel="Save template"
          onDone={(value) => {
            setTemplatePrompt(false);
            if (value) projectActions.saveAsTemplate(value);
          }}
        />
      )}
    </div>
  );
}

/** The project name with a dot while there are unsaved changes. */
export function ProjectName() {
  const state = useDawg((s) => s.projectState);
  if (!state.open) return null;

  return (
    <span className="project-name" title={state.untitled ? 'Not saved yet' : state.path}>
      {state.name}
      {state.dirty && <span className="dirty-dot" title="Unsaved changes" />}
    </span>
  );
}
