import { X } from 'lucide-react';
import { useMemo, useState } from 'react';
import { NODE_TYPES } from '../bridge/protocol';
import { addAsNewTrack, addToTrack, NODE_DRAG_TYPE, setDraggedPaletteNode } from './paletteDrag';
import { useUi } from '../store/uiStore';
import { ConfirmDialog } from './ConfirmDiscard';
import { call } from '../bridge/bridge';
import { actions, refreshSavedComponents, reportError, useDawg } from '../store/store';
import { builtinItems, componentItems, scriptItems, type PickedNode } from './PluginPicker';
import { Button } from '../ui/controls';

/** Lets a row be dragged onto the graph canvas, a track or empty arrangement space (paletteDrag.ts), and a double-click
    add it to the selected track (or a new one, when there is none). */
const dragNode = (picked: PickedNode) => ({
  draggable: true,
  onDoubleClick: () => {
    const project = useDawg.getState().project;
    // The track the inspector shows: the selected one, or the first.
    const id = useUi.getState().selectedTrackId ?? project?.trackOrder[0] ?? null;
    const track = id === null ? undefined : project?.master?.id === id ? project.master : project?.tracks[String(id)];
    if (track) addToTrack(track, picked);
    else addAsNewTrack(picked);
  },
  onDragStart: (e: React.DragEvent) => {
    e.dataTransfer.setData(NODE_DRAG_TYPE, JSON.stringify(picked));
    setDraggedPaletteNode(picked);
    e.dataTransfer.effectAllowed = 'copy';
  },
  onDragEnd: () => setDraggedPaletteNode(null),
});

export function PluginBrowser() {
  const options = useDawg((s) => s.pluginOptions);
  const state = useDawg((s) => s.plugins);
  const nodeTypes = useDawg((s) => s.nodeTypes);
  const faustScripts = useDawg((s) => s.faustScripts);
  const savedComponents = useDawg((s) => s.savedComponents);
  const [filter, setFilter] = useState('');
  // A saved component asked to be deleted: its file is gone for good, so this asks first.
  const [deleting, setDeleting] = useState<{ file: string; name: string } | null>(null);
  const components = useMemo(() => {
    const f = filter.trim().toLowerCase();
    return componentItems(savedComponents, 'any').filter((c) => !f || `${c.name} ${c.vendor}`.toLowerCase().includes(f));
  }, [savedComponents, filter]);
  const builtins = useMemo(() => {
    const f = filter.trim().toLowerCase();
    return builtinItems(nodeTypes, 'any').filter((b) => !f || b.name.toLowerCase().includes(f));
  }, [nodeTypes, filter]);
  const scripts = useMemo(() => {
    const f = filter.trim().toLowerCase();
    return scriptItems(faustScripts, 'any').filter((s) => !f || `${s.name} ${s.vendor}`.toLowerCase().includes(f));
  }, [faustScripts, filter]);

  const visible = useMemo(() => {
    const f = filter.trim().toLowerCase();
    return state.plugins
      .filter((p) => !f || `${p.name} ${p.manufacturer} ${p.category}`.toLowerCase().includes(f))
      .sort((a, b) => a.name.localeCompare(b.name));
  }, [state.plugins, filter]);

  return (
    <>
      <div className="panel-toolbar">
        <span>{state.plugins.length} plugins</span>
        {options.available && (
          <label className="plugin-processes" title="A plugin that crashes then can't take DAWg with it. Its sound doesn't change.">
            <input
              type="checkbox"
              checked={options.outOfProcess}
              onChange={(e) => actions.setPluginsOutOfProcess(e.target.checked)}
            />
            Separate processes
          </label>
        )}
        <Button disabled={state.scanning} title="Each plugin is scanned in a separate process" onClick={actions.scanPlugins}>
          {state.scanning ? 'Scanning…' : state.plugins.length ? 'Rescan' : 'Scan'}
        </Button>
      </div>

      {state.scanning && (
        <div className="scan-progress">
          <div className="progress">
            <div className="progress-fill" style={{ transform: `scaleX(${state.progress})` }} />
          </div>
          <span className="scan-current">{state.current}</span>
        </div>
      )}

      <input
        className="search"
        placeholder={`Filter ${state.plugins.length} plugins`}
        value={filter}
        onChange={(e) => setFilter(e.target.value)}
      />

      <div className="panel-body">
        {components.map((c) => (
          <div className="plugin-row" key={c.key} title={`${c.vendor} · drag onto a track or the graph, double-click to add it to the selected track`} {...dragNode({ type: c.type, patch: c.patch, saved: c.saved })}>
            <span className={`badge ${c.format === 'Patch' ? 'instrument' : 'effect'}`}>{c.format === 'Patch' ? 'INS' : 'BOX'}</span>
            <span className="plugin-name">{c.name}</span>
            <span className="plugin-vendor">{c.vendor}</span>
            {c.saved ? (
              <button
                className="plugin-format component-delete"
                title="Delete this saved component"
                aria-label={`Delete saved component ${c.name}`}
                onClick={() => setDeleting({ file: c.saved!, name: c.name })}
              >
                <X size={11} />
              </button>
            ) : (
              <span className="plugin-format">{c.format}</span>
            )}
          </div>
        ))}
        {builtins.map((b) => {
          const category = nodeTypes[b.type]?.category;
          return (
            <div className="plugin-row" key={b.key} title="Built-in · drag onto a track or the graph, double-click to add it to the selected track" {...dragNode({ type: b.type })}>
              <span className={`badge ${category === 'instrument' ? 'instrument' : 'effect'}`}>
                {category === 'instrument' ? 'INS' : category === 'utility' ? 'UTIL' : 'FX'}
              </span>
              <span className="plugin-name">{b.name}</span>
              <span className="plugin-vendor">DAWg</span>
              <span className="plugin-format">Built-in</span>
            </div>
          );
        })}
        {scripts.map((s) => (
          <div className="plugin-row" key={s.key} title="Faust script · drag onto a track or the graph, double-click to add it to the selected track" {...dragNode({ type: s.type, code: s.code })}>
            <span className="badge effect">DSP</span>
            <span className="plugin-name">{s.name}</span>
            <span className="plugin-vendor">{s.vendor}</span>
            <span className="plugin-format">Faust</span>
          </div>
        ))}
        {visible.map((p) => (
          <div
            className="plugin-row"
            key={p.id}
            title={`${p.file} · drag onto a track or the graph, double-click to add it to the selected track`}
            {...dragNode({
              type: p.isInstrument ? NODE_TYPES.pluginInstrument : NODE_TYPES.pluginEffect,
              plugin: { identifier: p.id, name: p.name, manufacturer: p.manufacturer, format: p.format },
            })}
          >
            <span className={`badge ${p.isInstrument ? 'instrument' : 'effect'}`}>{p.isInstrument ? 'INS' : 'FX'}</span>
            <span className="plugin-name">{p.name}</span>
            <span className="plugin-vendor">{p.manufacturer}</span>
            <span className="plugin-format">{p.format}</span>
          </div>
        ))}
        {!state.scanning && state.plugins.length === 0 && (
          <p className="empty">No plugins scanned yet.</p>
        )}
        {deleting && (
          <ConfirmDialog
            title="Delete saved component"
            message={`Delete “${deleting.name}”? Its file is deleted, and this can't be undone. Tracks that use it keep their copy.`}
            confirmLabel="Delete"
            onDone={(confirmed) => {
              const file = deleting.file;
              setDeleting(null);
              if (confirmed) void call('components.delete', { file }).then(refreshSavedComponents).catch(reportError);
            }}
          />
        )}
        {state.failed.length > 0 && (
          <details className="failed">
            <summary>{state.failed.length} failed to scan</summary>
            {state.failed.map((f) => (
              <div key={f}>{f}</div>
            ))}
          </details>
        )}
      </div>
    </>
  );
}
