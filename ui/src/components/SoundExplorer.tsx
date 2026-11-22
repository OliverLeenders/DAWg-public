import { ChevronDown, ChevronRight, FileAudio, Folder, Plus, Search, Star, Volume2 } from 'lucide-react';
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import * as bridge from '../bridge/bridge';
import { FOLDER_MIME, PATHS_MIME, isSfz } from '../arrangement/importAudio';
import { alpha, readTheme } from '../canvas/theme';
import { useDawg } from '../store/store';
import { getPeaks, usePeaksVersion } from '../timeline/peaks';
import { drawWave } from '../timeline/waveform';
import { Button } from '../ui/controls';

// The sound explorer (docs/design/phase7-audio-tracks.md, section 9, item 8): favourite folders and the
// project's Audio folder, a file tree with search, and previewing. Selecting a file plays it (auto-play can
// be switched off); Up/Down cycle through files, Space/Enter toggle playback, Escape stops, Left/Right
// collapse/expand folders. Files are dragged onto arrangement lanes to import them.

interface Entry {
  name: string;
  path: string;
  dir: boolean;
}

interface Root {
  name: string;
  path: string;
  project: boolean;
}

interface Row extends Entry {
  depth: number;
}

const PREFS_KEY = 'dawg.explorer';

function loadPrefs(): { autoPlay: boolean; volume: number; root: string | null } {
  try {
    return { autoPlay: true, volume: 0.7, root: null, ...JSON.parse(localStorage.getItem(PREFS_KEY) ?? '{}') };
  } catch {
    return { autoPlay: true, volume: 0.7, root: null };
  }
}

function savePrefs(prefs: ReturnType<typeof loadPrefs>) {
  try {
    localStorage.setItem(PREFS_KEY, JSON.stringify(prefs));
  } catch {
    // not remembered
  }
}

const formatDuration = (seconds: number | undefined) =>
  seconds === undefined ? '' : `${Math.floor(seconds / 60)}:${String(Math.floor(seconds % 60)).padStart(2, '0')}`;

export function SoundExplorer() {
  const [roots, setRoots] = useState<Root[]>([]);
  const [prefs, setPrefs] = useState(loadPrefs);
  const [listings, setListings] = useState<Record<string, Entry[]>>({});
  const [expanded, setExpanded] = useState<ReadonlySet<string>>(new Set());
  const [durations, setDurations] = useState<Record<string, number>>({});
  const [query, setQuery] = useState('');
  const [results, setResults] = useState<Entry[] | null>(null);
  const [selected, setSelected] = useState<string | null>(null);
  const [playing, setPlaying] = useState<string | null>(null);
  const projectPath = useDawg((s) => s.projectState.path);
  const previewSeconds = useDawg((s) => s.telemetry.previewSeconds);
  const listRef = useRef<HTMLDivElement>(null);

  const updatePrefs = (next: Partial<ReturnType<typeof loadPrefs>>) =>
    setPrefs((p) => {
      const merged = { ...p, ...next };
      savePrefs(merged);
      return merged;
    });

  const root = roots.find((r) => r.path === prefs.root) ?? roots[0];

  // Roots: favourites + the project's Audio folder; refreshed when the project changes.
  useEffect(() => {
    bridge.call('files.roots').then(setRoots).catch(() => {});
    const offRoots = bridge.on('dawg_roots', setRoots);
    const offDurations = bridge.on('dawg_durations', (d) => setDurations((all) => ({ ...all, ...d })));
    return () => {
      offRoots?.();
      offDurations?.();
    };
  }, [projectPath]);

  const list = useCallback(
    (path: string) => {
      if (listings[path]) return;
      bridge
        .call('files.list', { path })
        .then((r) => setListings((all) => ({ ...all, [r.path]: r.entries })))
        .catch(() => {});
    },
    [listings],
  );

  useEffect(() => {
    if (root) list(root.path);
  }, [root, list]);

  useEffect(() => {
    bridge.call('preview.setVolume', { gain: prefs.volume }).catch(() => {});
  }, [prefs.volume]);

  // Search below the current root (debounced).
  useEffect(() => {
    if (!root || query.trim() === '') {
      setResults(null);
      return;
    }
    const timer = setTimeout(() => {
      bridge
        .call('files.search', { path: root.path, query })
        .then((r) => setResults(r.entries))
        .catch(() => {});
    }, 200);
    return () => clearTimeout(timer);
  }, [query, root]);

  // The visible rows: search results, or the tree with expanded folders.
  const rows = useMemo<Row[]>(() => {
    if (results) return results.map((e) => ({ ...e, depth: 0 }));
    if (!root) return [];
    const out: Row[] = [];
    const walk = (path: string, depth: number) => {
      for (const entry of listings[path] ?? []) {
        out.push({ ...entry, depth });
        if (entry.dir && expanded.has(entry.path)) walk(entry.path, depth + 1);
      }
    };
    walk(root.path, 0);
    return out;
  }, [results, root, listings, expanded]);

  const play = (path: string) => {
    bridge
      .call('preview.play', { path })
      .then(() => setPlaying(path))
      .catch((e) => useDawg.setState({ lastError: String(e) }));
  };
  const stop = () => {
    bridge.call('preview.stop').catch(() => {});
    setPlaying(null);
  };

  const select = (row: Row) => {
    setSelected(row.path);
    if (!row.dir && !isSfz(row.path) && prefs.autoPlay) play(row.path);
  };

  const toggleFolder = (row: Row, open?: boolean) => {
    const next = new Set(expanded);
    const shouldOpen = open ?? !next.has(row.path);
    if (shouldOpen) {
      next.add(row.path);
      list(row.path);
    } else next.delete(row.path);
    setExpanded(next);
  };

  // Keep the selected row in view while cycling.
  useEffect(() => {
    listRef.current?.querySelector('.explorer-row.selected')?.scrollIntoView({ block: 'nearest' });
  }, [selected]);

  const onKeyDown = (e: React.KeyboardEvent) => {
    const index = rows.findIndex((r) => r.path === selected);
    const row = rows[index];

    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      const next = rows[Math.max(0, Math.min(rows.length - 1, index + (e.key === 'ArrowDown' ? 1 : -1)))];
      if (next) select(next);
    } else if ((e.key === ' ' || e.key === 'Enter') && row) {
      e.preventDefault();
      if (row.dir) toggleFolder(row);
      else if (playing === row.path) stop();
      else if (!isSfz(row.path)) play(row.path);
    } else if (e.key === 'Escape') {
      stop();
    } else if (e.key === 'ArrowRight' && row?.dir) {
      toggleFolder(row, true);
    } else if (e.key === 'ArrowLeft' && row?.dir) {
      toggleFolder(row, false);
    } else {
      return;
    }
    e.stopPropagation(); // keep Space from starting the transport
  };

  const selectedFile = selected && !rows.find((r) => r.path === selected)?.dir && !isSfz(selected) ? selected : null;

  return (
    <div className="explorer">
      <div className="explorer-roots">
        {roots.map((r) => (
          <button
            key={r.path}
            className={`root-chip ${r.path === root?.path ? 'active' : ''}`}
            title={r.project ? r.path : `${r.path} (right-click to remove)`}
            onClick={() => {
              updatePrefs({ root: r.path });
              setQuery('');
            }}
            onContextMenu={(e) => {
              e.preventDefault();
              if (!r.project) bridge.call('files.removeRoot', { path: r.path }).then(setRoots).catch(() => {});
            }}
          >
            {r.project ? <FileAudio size={11} /> : <Star size={11} />} {r.name}
          </button>
        ))}
        <button className="root-chip add" title="Add a favourite folder" onClick={() => bridge.call('files.addRoot').catch(() => {})}>
          <Plus size={11} />
        </button>
      </div>

      <label className="explorer-search">
        <Search size={12} />
        <input placeholder={root ? `Search ${root.name}` : 'Search'} disabled={!root} value={query} onChange={(e) => setQuery(e.target.value)} />
      </label>

      <div className="explorer-list" ref={listRef} tabIndex={0} onKeyDown={onKeyDown}>
        {rows.map((row) => (
          <div
            key={row.path}
            className={`explorer-row ${row.path === selected ? 'selected' : ''} ${row.path === playing ? 'playing' : ''}`}
            style={{ paddingLeft: 6 + row.depth * 14 }}
            title={row.path}
            draggable
            onDragStart={(e) => {
              // A folder or an SFZ patch goes to a Sampler, which maps it to keys; a file goes anywhere audio does.
              e.dataTransfer.setData(row.dir || isSfz(row.path) ? FOLDER_MIME : PATHS_MIME, JSON.stringify([row.path]));
              e.dataTransfer.effectAllowed = 'copy';
            }}
            onClick={() => {
              listRef.current?.focus();
              if (row.dir) toggleFolder(row);
              select(row);
            }}
          >
            {row.dir ? (
              <>
                {expanded.has(row.path) ? <ChevronDown size={12} /> : <ChevronRight size={12} />}
                <Folder size={12} />
              </>
            ) : (
              <FileAudio size={12} className="file-icon" />
            )}
            <span className="explorer-name">{row.name}</span>
            {!row.dir && !isSfz(row.path) && <span className="explorer-duration">{formatDuration(durations[row.path])}</span>}
          </div>
        ))}
        {rows.length === 0 && (
          // With no folder, adding one is a button here rather than the search field's placeholder (docs/ui-review-2026-11-10.md, 3.5).
          <div className="empty">
            {root ? (
              results ? 'Nothing matches.' : 'No audio files here.'
            ) : (
              <>
                <p>No favourite folders yet.</p>
                <Button onClick={() => bridge.call('files.addRoot').catch(() => {})}>
                  <Plus size={12} /> Add a folder
                </Button>
              </>
            )}
          </div>
        )}
      </div>

      <div className="explorer-preview">
        <PreviewWave path={selectedFile} position={playing === selectedFile ? previewSeconds : -1} />
        <div className="explorer-controls">
          <label className="explorer-toggle" title="Play files when they are selected">
            <input type="checkbox" checked={prefs.autoPlay} onChange={(e) => updatePrefs({ autoPlay: e.target.checked })} />
            Auto-play
          </label>
          <Volume2 size={13} />
          <input
            type="range"
            min={0}
            max={1}
            step={0.01}
            value={prefs.volume}
            onChange={(e) => updatePrefs({ volume: Number(e.target.value) })}
          />
        </div>
      </div>
    </div>
  );
}

/** The selected file's waveform with the preview playhead. */
function PreviewWave({ path, position }: { path: string | null; position: number }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  usePeaksVersion((s) => s.version);

  useEffect(() => {
    const element = canvas.current;
    const ctx = element?.getContext('2d');
    if (!element || !ctx) return;
    const width = element.clientWidth;
    const height = element.clientHeight;
    const dpr = window.devicePixelRatio || 1;
    element.width = width * dpr;
    element.height = height * dpr;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, width, height);
    if (!path || width === 0) return;
    const theme = readTheme();

    // A coarse level first; the frame count comes with it.
    const coarse = getPeaks({ path }, 4);
    if (!coarse) return;
    // Drawn as the arrangement draws a clip's wave (timeline/waveform.ts).
    const framesPerPixel = coarse.frames / width;
    const spec = { source: { path }, frames: coarse.frames, x0: 0, x1: width, frameAt: (x: number) => x * framesPerPixel };
    drawWave(ctx, { ...spec, mid: height / 2, half: height / 2 - 1, top: 0, bottom: height }, alpha(theme.signalAudio, 0.75));

    if (position >= 0) {
      ctx.fillStyle = theme.text;
      ctx.fillRect(Math.round((position * coarse.sampleRate) / framesPerPixel), 0, 1, height);
    }
  });

  return <canvas ref={canvas} className="preview-wave" />;
}
