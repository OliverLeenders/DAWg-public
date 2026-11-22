import { ChevronDown, ChevronRight, FileMusic, Folder, Search, Square } from 'lucide-react';
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import * as bridge from '../bridge/bridge';
import { MIDI_PATH_MIME } from '../arrangement/importAudio';
import { reportError, useDawg } from '../store/store';
import { useUi } from '../store/uiStore';
import { tempoMapOf } from '../time/tempoMap';
import { MidiPreview } from './midiPreview';
import { IconButton } from '../ui/controls';

// The MIDI files that ship with DAWg, or join them later (docs/design/sampler-midi-delay-limiter.md, section 3): a tree of
// folders with search. Selecting a file plays it through the selected instrument track; dragging it onto an instrument
// track adds a clip there, and anywhere else a new track. Up and Down step through the files, Left and Right fold folders,
// Space plays or stops and Escape stops.

interface Entry {
  name: string;
  path: string;
  dir: boolean;
}

interface Row extends Entry {
  depth: number;
}

export function MidiFilesBrowser() {
  const [root, setRoot] = useState<string | null>(null);
  const [listings, setListings] = useState<Record<string, Entry[]>>({});
  const [expanded, setExpanded] = useState<ReadonlySet<string>>(new Set());
  const [query, setQuery] = useState('');
  const [results, setResults] = useState<Entry[] | null>(null);
  const [selected, setSelected] = useState<string | null>(null);
  const [playing, setPlaying] = useState<string | null>(null);
  const preview = useRef(new MidiPreview());
  const listRef = useRef<HTMLDivElement>(null);

  const load = useCallback((path?: string) => {
    bridge
      .call('midiFiles.list', { path })
      .then((listing) => {
        setRoot((current) => current ?? listing.path);
        setListings((all) => ({ ...all, [listing.path]: listing.entries }));
      })
      .catch(reportError);
  }, []);

  useEffect(load, [load]);

  useEffect(() => {
    const player = preview.current;
    player.onEnd = () => setPlaying(null);
    return () => player.stop();
  }, []);

  useEffect(() => {
    if (query.trim() === '') {
      setResults(null);
      return;
    }
    // Waits for a pause in the typing: every search holds up the message thread for a moment.
    let current = true;
    const timer = setTimeout(() => {
      bridge
        .call('midiFiles.search', { query })
        .then((found) => {
          if (current) setResults(found);
        })
        .catch(reportError);
    }, 150);
    return () => {
      current = false;
      clearTimeout(timer);
    };
  }, [query]);

  const rows = useMemo<Row[]>(() => {
    if (results) return results.map((entry) => ({ ...entry, depth: 0 }));
    const out: Row[] = [];
    const walk = (path: string, depth: number) => {
      for (const entry of listings[path] ?? []) {
        out.push({ ...entry, depth });
        if (entry.dir && expanded.has(entry.path)) walk(entry.path, depth + 1);
      }
    };
    if (root) walk(root, 0);
    return out;
  }, [results, listings, expanded, root]);

  const previewTrack = () => {
    const { project } = useDawg.getState();
    if (!project) return undefined;
    const isInstrument = (id: number | null | undefined) => id != null && project.tracks[String(id)]?.kind === 'instrument';
    const chosen = useUi.getState().selectedTrackId;
    return isInstrument(chosen) ? chosen! : project.trackOrder.find((id) => isInstrument(id));
  };

  const play = (path: string) => {
    const trackId = previewTrack();
    preview.current.stop();
    if (trackId === undefined) {
      setPlaying(null);
      return;
    }
    setPlaying(path);
    bridge
      .call('midi.parse', { path })
      .then((file) => {
        // Only the newest selection plays.
        if (preview.current.wanted(path)) {
          const project = useDawg.getState().project;
          preview.current.start(file, trackId, tempoMapOf(project).bpmAt(0));
        }
      })
      .catch(reportError);
    preview.current.want(path);
  };

  const stop = () => {
    preview.current.stop();
    setPlaying(null);
  };

  const toggle = (path: string) => {
    setExpanded((current) => {
      const next = new Set(current);
      if (next.has(path)) next.delete(path);
      else next.add(path);
      return next;
    });
    if (!listings[path]) load(path);
  };

  const choose = (row: Row) => {
    setSelected(row.path);
    if (row.dir) {
      stop();
      return;
    }
    play(row.path);
  };

  const onKeyDown = (e: React.KeyboardEvent) => {
    const index = rows.findIndex((row) => row.path === selected);
    const row = rows[index];
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      const next = rows[Math.min(rows.length - 1, Math.max(0, index + (e.key === 'ArrowDown' ? 1 : -1)))];
      if (next) choose(next);
    } else if (e.key === 'ArrowRight' && row?.dir && !expanded.has(row.path)) toggle(row.path);
    else if (e.key === 'ArrowLeft' && row?.dir && expanded.has(row.path)) toggle(row.path);
    else if (e.key === 'Escape') stop();
    else if ((e.key === ' ' || e.key === 'Enter') && row) {
      if (row.dir) toggle(row.path);
      else if (playing === row.path) stop();
      else play(row.path);
    } else return;
    e.preventDefault();
    e.stopPropagation();
  };

  return (
    <div className="explorer">
      <label className="explorer-search">
        <Search size={12} />
        <input
          value={query}
          placeholder="Search: key, type or mood"
          onChange={(e) => setQuery(e.target.value)}
          onKeyDown={(e) => e.stopPropagation()}
        />
      </label>
      <div className="explorer-list" ref={listRef} tabIndex={0} onKeyDown={onKeyDown}>
        {rows.map((row) => (
          <div
            key={row.path}
            className={`explorer-row ${row.path === selected ? 'selected' : ''} ${row.path === playing ? 'playing' : ''}`}
            style={{ paddingLeft: 6 + row.depth * 12 }}
            title={row.name}
            draggable={!row.dir}
            onDragStart={(e) => {
              e.dataTransfer.setData(MIDI_PATH_MIME, row.path);
              e.dataTransfer.effectAllowed = 'copy';
            }}
            onClick={() => {
              listRef.current?.focus();
              if (row.dir) toggle(row.path);
              choose(row);
            }}
          >
            {row.dir ? (
              <>
                {expanded.has(row.path) ? <ChevronDown size={12} /> : <ChevronRight size={12} />}
                <Folder size={12} className="file-icon" />
              </>
            ) : (
              <FileMusic size={12} className="file-icon" />
            )}
            <span className="explorer-name">{row.name.replace(/\.midi?$/i, '')}</span>
          </div>
        ))}
        {rows.length === 0 && <p className="empty">{results ? 'Nothing matches.' : 'No MIDI files here.'}</p>}
      </div>
      <div className="explorer-preview">
        <div className="explorer-controls">
          <IconButton title="Stop the preview" disabled={playing === null} onClick={stop}>
            <Square size={12} />
          </IconButton>
          <span>{previewTrack() === undefined ? 'Add an instrument track to hear files' : 'Plays through the selected instrument'}</span>
        </div>
      </div>
    </div>
  );
}
