import { useEffect, useMemo, useRef, useState } from 'react';
import { NODE_TYPES, type ComponentGraph, type FaustScript, type NodeTypeInfo, type PluginRef, type SavedComponent } from '../bridge/protocol';
import { PATCHES, patchComponent } from '../graph/patches';
import { call } from '../bridge/bridge';
import { useDawg } from '../store/store';
import { allowedInVoiceGraph } from '../graph/voice';
import { builtinChainRole } from './chain';
import { Menu, Submenu } from '../ui/overlays';
import { builtinFolders, intoFolders, PATCH_FOLDERS, pluginFolders, type FolderKey } from './addFolders';

/** What the picker returns: a node type and, for plugins, which plugin; for Faust scripts, the script. */
export interface PickedNode {
  /** A factory patch rather than one node (graph/patches.ts). */
  patch?: string;
  /** A component saved for reuse: its file. */
  saved?: string;
  type: string;
  plugin?: PluginRef;
  code?: string;
  /** A Stereo Split or Merge in left/right: "Left/right split" (docs/design/split-nodes.md, SP12). */
  mode?: 'leftRight';
}

export interface Item extends PickedNode {
  key: string;
  name: string;
  vendor: string;
  format: string;
  /** The add menu's folders it sits in (addFolders.ts). */
  folders: FolderKey[];
}

/** What a picker offers: instruments, effects (chain inserts), or any node (graph view, with or without
    instruments). */
export type PickerKind = 'instrument' | 'midiEffect' | 'effect' | 'any';

/** Built-in node types a picker offers, for drag-and-drop palettes too. In a voice graph, only the types that can run
    per voice (docs/design/phase12-voice-scope.md, V6). */
export function builtinItems(types: Record<string, NodeTypeInfo>, kind: PickerKind, instruments = true, voice = false) {
  return Object.values(types)
    .filter((t) => t.userEditable && t.type !== NODE_TYPES.pluginInstrument && t.type !== NODE_TYPES.pluginEffect)
    .filter((t) => !voice || allowedInVoiceGraph(t.type))
    .filter((t) => voice || t.type !== NODE_TYPES.sampleVoice)
    .filter((t) => {
      // A chain takes what builtinChainRole() says goes there; a graph takes everything, instruments only on an
      // instrument track, and MIDI effects (docs/design/midi-effects.md, ME3) only outside a voice graph.
      if (kind !== 'any') return builtinChainRole(t) === kind;
      if (t.category === 'instrument') return instruments;
      if (t.category === 'midiEffect') return instruments && !voice;
      return true;
    })
    .flatMap((t): Item[] => {
      const item: Item = { key: `builtin:${t.type}`, type: t.type, name: t.name, vendor: 'DAWg', format: 'Built-in', folders: builtinFolders(t) };
      // The stereo nodes are offered in both modes, by what they do (SP4, SP12).
      if (t.type === NODE_TYPES.stereoSplit || t.type === NODE_TYPES.stereoMerge) {
        const what = t.type === NODE_TYPES.stereoSplit ? 'split' : 'merge';
        return [
          { ...item, name: `Mid/side ${what}` },
          { ...item, key: `${item.key}:leftRight`, name: `Left/right ${what}`, mode: 'leftRight' },
        ];
      }
      return [t.type === NODE_TYPES.bandSplit ? { ...item, name: 'Band split' } : item];
    })
    .sort((a, b) => a.name.localeCompare(b.name));
}

/** What's in a picked component, factory or saved, to go with its command. */
export async function pickedComponent(picked: PickedNode): Promise<ComponentGraph | undefined> {
  if (picked.saved) return call('components.load', { file: picked.saved });
  const patch = PATCHES.find((p) => p.key === picked.patch);
  return patch && patchComponent(patch);
}

/** Factory patches and saved components. As a chain's effect, only those with one audio input and output. */
export function componentItems(saved: SavedComponent[], kind: PickerKind, instruments = true): Item[] {
  if (kind === 'instrument' || kind === 'midiEffect') return [];
  const isEffect = (c: { inputs: { type: string }[]; outputs: { type: string }[] }) =>
    c.inputs.length === 1 && c.inputs[0].type === 'audio' && c.outputs.length === 1 && c.outputs[0].type === 'audio';

  return [
    ...PATCHES.filter((p) => (kind === 'any' ? p.box !== undefined || instruments : p.box !== undefined)).map(
      (p): Item => ({
        key: `patch:${p.key}`,
        patch: p.key,
        type: p.box ? NODE_TYPES.component : '',
        name: p.name,
        vendor: p.hint,
        format: p.box ? 'Component' : 'Patch',
        folders: PATCH_FOLDERS[p.key] ?? ['builtin'],
      }),
    ),
    ...saved
      .filter((c) => kind === 'any' || isEffect(c))
      .map((c): Item => ({ key: `saved:${c.file}`, saved: c.file, type: NODE_TYPES.component, name: c.name, vendor: 'Your components', format: 'Component', folders: ['components'] })),
  ];
}

/** Faust examples and library scripts as effects (docs/design/phase11-faust-panels.md, F6). */
export function scriptItems(scripts: FaustScript[], kind: PickerKind): Item[] {
  if (kind === 'instrument' || kind === 'midiEffect') return [];
  return scripts.map((s) => ({
    key: `faust:${s.source}/${s.file}`,
    type: NODE_TYPES.faust,
    code: s.code,
    name: s.name,
    vendor: s.source === 'example' ? 'Faust example' : 'Your library',
    format: 'Faust',
    folders: ['faust'],
  }));
}

/**
 * The menu that adds an instrument, an effect or any node (docs/design/phase4-plugin-hosting.md, section 10;
 * phase8-node-graph.md, section 5): folders by what things are (addFolders.ts), Built in first, each a submenu; typing
 * searches them all instead. Arrow keys move (right opens a folder, left goes back), Enter picks, Escape closes.
 */
export function PluginPicker({
  kind,
  x,
  y,
  onPick,
  onClose,
  instruments = true,
  voice = false,
}: {
  kind: PickerKind;
  /** For kind 'any': whether instruments are offered (only instrument tracks hold them). */
  instruments?: boolean;
  /** For kind 'any': nodes for a voice graph, which holds no plugins (docs/design/phase12-voice-scope.md, V6). */
  voice?: boolean;
  x: number;
  y: number;
  onPick: (picked: PickedNode) => void;
  onClose: () => void;
}) {
  const plugins = useDawg((s) => s.plugins.plugins);
  const nodeTypes = useDawg((s) => s.nodeTypes);
  const scripts = useDawg((s) => s.faustScripts);
  const saved = useDawg((s) => s.savedComponents);
  const [filter, setFilter] = useState('');
  const [active, setActive] = useState(0);
  const listRef = useRef<HTMLDivElement>(null);

  const all = useMemo(() => {
    const fromScan: Item[] = plugins
      .filter(() => !voice)
      .filter(() => kind !== 'midiEffect')
      .filter((p) => (kind === 'any' ? instruments || !p.isInstrument : p.isInstrument === (kind === 'instrument')))
      .map((p) => ({
        key: p.id,
        type: p.isInstrument ? NODE_TYPES.pluginInstrument : NODE_TYPES.pluginEffect,
        plugin: { identifier: p.id, name: p.name, manufacturer: p.manufacturer, format: p.format },
        name: p.name,
        vendor: p.manufacturer,
        format: p.format,
        folders: pluginFolders(p.category, p.isInstrument),
      }))
      .sort((a, b) => a.name.localeCompare(b.name));
    const patches = componentItems(saved, kind, instruments && !voice).filter((i) => !voice || i.patch !== 'plucked-string');
    return [...patches, ...builtinItems(nodeTypes, kind, instruments && !voice, voice), ...scriptItems(scripts, kind), ...fromScan];
  }, [plugins, nodeTypes, scripts, saved, kind, instruments, voice]);
  const folders = useMemo(() => intoFolders(all), [all]);
  const searching = filter.trim() !== '';
  const items = useMemo(() => {
    const f = filter.trim().toLowerCase();
    return f ? all.filter((i) => `${i.name} ${i.vendor} ${i.format}`.toLowerCase().includes(f)) : [];
  }, [all, filter]);

  useEffect(() => setActive(0), [filter]);
  useEffect(() => {
    listRef.current?.children[active]?.scrollIntoView({ block: 'nearest' });
  }, [active]);

  // Keep the popup on screen (the Menu moves it up where its folders would hang out).
  const left = Math.min(x, window.innerWidth - 340);
  const top = Math.min(y, window.innerHeight - 380);

  return (
    <Menu onClose={onClose} at={{ x: left, y: top }} className="plugin-picker">
      <input
        className="search"
        autoFocus
        placeholder={
          kind === 'instrument' ? 'Search instruments' : kind === 'effect' ? 'Search effects' : kind === 'midiEffect' ? 'Search MIDI effects' : 'Add a node'
        }
        value={filter}
        onChange={(e) => setFilter(e.target.value)}
        onKeyDown={(e) => {
          // Without a search the arrow keys go on to the menu, into its folders.
          if (!searching) return;
          if (e.key === 'ArrowDown') {
            e.preventDefault();
            setActive((a) => Math.min(items.length - 1, a + 1));
          } else if (e.key === 'ArrowUp') {
            e.preventDefault();
            setActive((a) => Math.max(0, a - 1));
          } else if (e.key === 'Enter' && items[active]) {
            onPick(items[active]);
          }
        }}
      />
      {searching ? (
        <div className="picker-list" ref={listRef}>
          {items.map((item, i) => (
            <button
              key={item.key}
              className={`picker-item ${i === active ? 'active' : ''}`}
              onPointerEnter={() => setActive(i)}
              onClick={() => onPick(item)}
            >
              <span className="plugin-name">{item.name}</span>
              <span className="plugin-vendor">{item.vendor}</span>
              <span className="plugin-format">{item.format}</span>
            </button>
          ))}
          {items.length === 0 && <p className="empty">Nothing matches.</p>}
        </div>
      ) : (
        <div className="picker-folders">
          {folders.map((folder) => (
            <Submenu key={folder.key} label={folder.name} detail={folder.items.length}>
              {folder.items.map((item) => (
                <button key={item.key} className="menu-item picker-entry" title={`${item.vendor} · ${item.format}`} onClick={() => onPick(item)}>
                  <span className="plugin-name">{item.name}</span>
                  <span className="plugin-vendor">{item.format === 'Built-in' ? '' : item.vendor}</span>
                </button>
              ))}
            </Submenu>
          ))}
          {plugins.length === 0 && !voice && kind !== 'midiEffect' && <p className="empty">No plugins scanned yet: use Scan in the plugin panel.</p>}
        </div>
      )}
    </Menu>
  );
}
