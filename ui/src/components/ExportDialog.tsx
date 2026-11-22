import { FolderOpen } from 'lucide-react';
import { useEffect, useState } from 'react';
import * as bridge from '../bridge/bridge';
import { TICKS_PER_BEAT, type ExportOptions, type Mp3Quality, type Mp3Tags } from '../bridge/protocol';
import { growingLoop } from '../graph/validate';
import { useDawg } from '../store/store';
import { tempoMapOf } from '../time/tempoMap';
import { Button, IconButton } from '../ui/controls';
import { Dialog } from '../ui/overlays';

// File › Export audio (docs/design/phase9-export-midi-input.md, section 5): one page with range, tail, format,
// normalisation, the mix and a stem checklist; then progress, then the loudness report.

type Range = 'song' | 'loop' | 'custom';

const FORMATS: Array<{ value: ExportOptions['format']; label: string }> = [
  { value: 'wav24', label: 'WAV 24-bit' },
  { value: 'wav16', label: 'WAV 16-bit (dithered)' },
  { value: 'wav32', label: 'WAV 32-bit float' },
  { value: 'flac24', label: 'FLAC 24-bit' },
  { value: 'flac16', label: 'FLAC 16-bit (dithered)' },
  { value: 'mp3', label: 'MP3' },
];

const MP3_QUALITIES: Array<{ value: Mp3Quality; label: string }> = [
  { value: 'cbr320', label: '320 kbps' },
  { value: 'cbr256', label: '256 kbps' },
  { value: 'cbr192', label: '192 kbps' },
  { value: 'cbr128', label: '128 kbps' },
  { value: 'v0', label: 'Variable, V0 (best)' },
  { value: 'v2', label: 'Variable, V2' },
];

const TAG_FIELDS: Array<{ key: keyof Mp3Tags; label: string; placeholder?: string }> = [
  { key: 'title', label: 'Title', placeholder: 'The file name' },
  { key: 'artist', label: 'Artist' },
  { key: 'album', label: 'Album' },
  { key: 'year', label: 'Year' },
  { key: 'track', label: 'Track' },
  { key: 'genre', label: 'Genre' },
  { key: 'comment', label: 'Comment' },
];

const EMPTY_TAGS: Mp3Tags = { title: '', artist: '', album: '', year: '', track: '', genre: '', comment: '' };

// The MP3 options are remembered between exports on this computer (X6): an artist rarely changes.
const MP3_KEY = 'dawg.exportMp3';

export function loadMp3Options(): ExportOptions['mp3'] {
  try {
    const stored = JSON.parse(localStorage.getItem(MP3_KEY) ?? 'null') as Partial<ExportOptions['mp3']> | null;
    const quality = MP3_QUALITIES.some((q) => q.value === stored?.quality) ? stored!.quality! : 'cbr320';
    return { quality, tags: { ...EMPTY_TAGS, ...(stored?.tags ?? {}) } };
  } catch {
    return { quality: 'cbr320', tags: { ...EMPTY_TAGS } };
  }
}

function saveMp3Options(mp3: ExportOptions['mp3']) {
  try {
    // The title belongs to one song, so it isn't kept.
    localStorage.setItem(MP3_KEY, JSON.stringify({ quality: mp3.quality, tags: { ...mp3.tags, title: '' } }));
  } catch {
    // Storage may be unavailable; the options then start from their defaults next time.
  }
}

const formatRate = (rate: number) => `${(rate / 1000).toLocaleString('en', { maximumFractionDigits: 1 })} kHz`;

const formatDb = (db: number) => (Number.isFinite(db) ? `${db.toFixed(1)}` : '−∞');

export function ExportDialog({ onClose }: { onClose: () => void }) {
  const project = useDawg((s) => s.project);
  const state = useDawg((s) => s.exportState);
  const [range, setRange] = useState<Range>('song');
  const [customBars, setCustomBars] = useState({ from: 1, to: 9 });
  const [songEndBeats, setSongEndBeats] = useState(0);
  const [rates, setRates] = useState({ sampleRate: 48000, mp3SampleRate: 48000 });
  // Normalising was turned on or off by hand: choosing MP3 no longer switches it on (X6).
  const [normaliseTouched, setNormaliseTouched] = useState(false);
  const [options, setOptions] = useState<Omit<ExportOptions, 'startBeats' | 'endBeats'>>({
    tail: 'auto',
    tailSeconds: 2,
    format: 'wav24',
    mp3: loadMp3Options(),
    normalise: false,
    normaliseDb: -1,
    mix: true,
    stems: [],
    folder: '',
    name: '',
    realtime: false,
  });
  const set = (fields: Partial<typeof options>) => setOptions((o) => ({ ...o, ...fields }));

  useEffect(() => {
    useDawg.setState({ exportState: null });
    bridge
      .call('export.defaults')
      .then((d) => {
        setSongEndBeats(d.songEndBeats);
        setRates({ sampleRate: d.sampleRate, mp3SampleRate: d.mp3SampleRate });
        setOptions((o) => ({ ...o, folder: d.folder, name: d.name || 'Export' }));
      })
      .catch((e) => useDawg.setState({ lastError: String(e) }));
    return bridge.on('dawg_exportFolder', (folder) => setOptions((o) => ({ ...o, folder })));
  }, []);

  if (!project) return null;

  const map = tempoMapOf(project);
  const barBeats = (bar: number) => map.tickOfBar(bar - 1) / TICKS_PER_BEAT;
  const loop = project.loop;
  const [startBeats, endBeats] =
    range === 'song'
      ? [0, songEndBeats]
      : range === 'loop'
        ? [loop.start / TICKS_PER_BEAT, loop.end / TICKS_PER_BEAT]
        : [barBeats(customBars.from), barBeats(customBars.to)];
  const tracks = project.trackOrder.map((id) => project.tracks[String(id)]).filter((t) => t && !t.mute);
  const running = state?.state === 'stretching' || state?.state === 'rendering' || state?.state === 'writing';
  const finished = state?.state === 'done' || state?.state === 'failed' || state?.state === 'cancelled';
  const canExport = endBeats > startBeats && (options.mix || options.stems.length > 0) && options.folder !== '';

  const isMp3 = options.format === 'mp3';
  const setMp3 = (mp3: Partial<ExportOptions['mp3']>) => set({ mp3: { ...options.mp3, ...mp3 } });
  // Encoding can push peaks over the source's, so MP3 normalises to −1 dBFS unless the user decided otherwise.
  const chooseFormat = (format: ExportOptions['format']) =>
    set(format === 'mp3' && !normaliseTouched ? { format, normalise: true, normaliseDb: Math.min(options.normaliseDb, -1) } : { format });

  const start = () => {
    if (isMp3) saveMp3Options(options.mp3);
    return bridge
      .call('export.start', { ...options, startBeats, endBeats })
      .then(() => useDawg.setState({ exportState: { state: 'rendering', progress: 0 } }))
      .catch((e) => useDawg.setState({ lastError: e instanceof Error ? e.message : String(e) }));
  };

  return (
    <Dialog onClose={() => !running && onClose()} className="export-dialog">
      <h3>Export audio</h3>

      {!running && !finished && (
        <>
          <div className="export-grid">
            <span>Range</span>
            <div className="export-row">
              <select value={range} onChange={(e) => setRange(e.target.value as Range)}>
                <option value="song">Song (bar 1 to the last clip)</option>
                <option value="loop">Loop range</option>
                <option value="custom">Bars…</option>
              </select>
              {range === 'custom' && (
                <>
                  <input
                    type="number"
                    min={1}
                    value={customBars.from}
                    onChange={(e) => setCustomBars({ ...customBars, from: Math.max(1, Number(e.target.value)) })}
                  />
                  <span>to</span>
                  <input
                    type="number"
                    min={customBars.from + 1}
                    value={customBars.to}
                    onChange={(e) => setCustomBars({ ...customBars, to: Math.max(customBars.from + 1, Number(e.target.value)) })}
                  />
                </>
              )}
            </div>

            <span>Tail</span>
            <div className="export-row">
              <select value={options.tail} onChange={(e) => set({ tail: e.target.value as ExportOptions['tail'] })}>
                <option value="auto">Until silence (up to 30 s)</option>
                <option value="fixed">Fixed</option>
                <option value="none">None</option>
              </select>
              {options.tail === 'fixed' && (
                <>
                  <input
                    type="number"
                    min={0}
                    step={0.5}
                    value={options.tailSeconds}
                    onChange={(e) => set({ tailSeconds: Math.max(0, Number(e.target.value)) })}
                  />
                  <span>s</span>
                </>
              )}
            </div>

            {options.tail === 'auto' && growingLoop(project) && (
              <p style={{ gridColumn: '1 / -1', margin: 0, color: 'var(--warn)' }}>
                A Feedback node here is set above 100 %, so its loop may ring on: the tail may run the full 30 s.
              </p>
            )}

            <span>Format</span>
            <div className="export-row">
              <select value={options.format} onChange={(e) => chooseFormat(e.target.value as ExportOptions['format'])}>
                {FORMATS.map((f) => (
                  <option key={f.value} value={f.value}>
                    {f.label}
                  </option>
                ))}
              </select>
              {isMp3 && (
                <select aria-label="MP3 quality" value={options.mp3.quality} onChange={(e) => setMp3({ quality: e.target.value as Mp3Quality })}>
                  {MP3_QUALITIES.map((q) => (
                    <option key={q.value} value={q.value}>
                      {q.label}
                    </option>
                  ))}
                </select>
              )}
            </div>

            {isMp3 && rates.mp3SampleRate !== rates.sampleRate && (
              <p className="export-note">
                MP3 holds at most 48 kHz: the {formatRate(rates.sampleRate)} render is converted to {formatRate(rates.mp3SampleRate)}.
              </p>
            )}

            {isMp3 && (
              <>
                <span>Tags</span>
                <div className="export-tags">
                  {TAG_FIELDS.map((field) => (
                    <label key={field.key} className={field.key === 'comment' || field.key === 'title' ? 'wide' : undefined}>
                      <span>{field.label}</span>
                      <input
                        className="search"
                        value={options.mp3.tags[field.key]}
                        placeholder={field.placeholder}
                        onChange={(e) => setMp3({ tags: { ...options.mp3.tags, [field.key]: e.target.value } })}
                      />
                    </label>
                  ))}
                </div>
              </>
            )}

            <span>Normalise</span>
            <div className="export-row">
              <label className="export-check">
                <input
                  type="checkbox"
                  checked={options.normalise}
                  onChange={(e) => {
                    setNormaliseTouched(true);
                    set({ normalise: e.target.checked });
                  }}
                />
                Peak to
              </label>
              <input
                type="number"
                max={0}
                step={0.1}
                disabled={!options.normalise}
                value={options.normaliseDb}
                onChange={(e) => set({ normaliseDb: Math.min(0, Number(e.target.value)) })}
              />
              <span>dBFS</span>
            </div>

            <span>Files</span>
            <div className="export-files">
              <label className="export-check">
                <input type="checkbox" checked={options.mix} onChange={(e) => set({ mix: e.target.checked })} />
                Master mix
              </label>
              {tracks.map((t) => (
                <label key={t.id} className="export-check" title={options.realtime ? 'Real-time export renders the mix only' : undefined}>
                  <input
                    type="checkbox"
                    disabled={options.realtime}
                    checked={options.stems.includes(t.id)}
                    onChange={(e) =>
                      set({ stems: e.target.checked ? [...options.stems, t.id] : options.stems.filter((id) => id !== t.id) })
                    }
                  />
                  <span className="swatch small" style={{ background: t.colour }} />
                  Stem: {t.name}
                </label>
              ))}
            </div>

            <span>Name</span>
            <input className="search" value={options.name} onChange={(e) => set({ name: e.target.value })} />

            <span>Folder</span>
            <div className="export-row">
              <input className="search" value={options.folder} onChange={(e) => set({ folder: e.target.value })} />
              <IconButton
                title="Choose a folder"
                onClick={() => bridge.call('export.chooseFolder', { folder: options.folder }).catch(() => {})}
              >
                <FolderOpen size={14} />
              </IconButton>
            </div>

            {/* How it renders, as two choices side by side (docs/ui-review-2026-11-10.md, 3.16). */}
            <span>Render</span>
            <div className="export-row">
              <div className="segmented" role="radiogroup" aria-label="Render">
                <button role="radio" aria-checked={!options.realtime} className={options.realtime ? '' : 'active'} title="As fast as the computer can" onClick={() => set({ realtime: false })}>
                  Offline
                </button>
                <button
                  role="radio"
                  aria-checked={options.realtime}
                  className={options.realtime ? 'active' : ''}
                  title="At playback speed, for plugins that don't render offline; the mix only"
                  onClick={() => set({ realtime: true })}
                >
                  Real time
                </button>
              </div>
              {options.realtime && <span className="hint">The mix only</span>}
            </div>
          </div>

          <div className="modal-buttons">
            <span className="hint">
              {endBeats > startBeats ? `${(map.barsAt(endBeats * TICKS_PER_BEAT) - map.barsAt(startBeats * TICKS_PER_BEAT)).toFixed(1)} bars` : 'The range is empty'}
            </span>
            <span className="spacer" />
            <Button onClick={onClose}>
              Cancel
            </Button>
            <Button className="primary" disabled={!canExport} onClick={start}>
              Export
            </Button>
          </div>
        </>
      )}

      {running && state && (
        <>
          <p>
            {state.state === 'writing'
              ? 'Writing files…'
              : state.state === 'stretching'
                ? 'Rendering stretched audio…'
                : `Rendering ${(state.passes ?? 1) > 1 ? `file ${(state.pass ?? 0) + 1} of ${state.passes}` : ''}…`}
          </p>
          <div className="progress">
            <div className="progress-fill" style={{ transform: `scaleX(${state.state === 'writing' ? 1 : (state.progress ?? 0)})` }} />
          </div>
          <div className="modal-buttons">
            <span className="spacer" />
            <Button onClick={() => bridge.call('export.cancel').catch(() => {})}>
              Cancel export
            </Button>
          </div>
        </>
      )}

      {finished && state && (
        <>
          {state.state === 'done' && (
            <table className="export-report">
              <thead>
                <tr>
                  <th>File</th>
                  <th>Peak (dBFS)</th>
                  <th>Loudness (LUFS)</th>
                </tr>
              </thead>
              <tbody>
                {(state.files ?? []).map((f) => (
                  <tr key={f.path} title={f.path}>
                    <td>{f.name}</td>
                    <td className={f.peakDb > 0 ? 'export-over' : undefined} title={f.peakDb > 0 ? 'Above full scale: it may clip when played' : undefined}>
                      {formatDb(f.peakDb)}
                    </td>
                    <td>{formatDb(f.lufs)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
          {state.state === 'done' && (state.files ?? []).some((f) => f.peakDb > 0) && (
            <p className="export-note warn">
              A file peaks above 0 dBFS and may clip when played. Export again with Normalise on, or to a lower peak.
            </p>
          )}
          {state.state === 'failed' && <p className="error-text">Export failed: {state.error}</p>}
          {state.state === 'cancelled' && <p>Export cancelled.</p>}
          <div className="modal-buttons">
            <span className="spacer" />
            <Button className="primary" autoFocus onClick={onClose}>
              Close
            </Button>
          </div>
        </>
      )}
    </Dialog>
  );
}
