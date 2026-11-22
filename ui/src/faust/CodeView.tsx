import Editor, { type OnMount } from '@monaco-editor/react';
import { BookOpen, CircleAlert, CircleCheck, FolderOpen, LoaderCircle, Play, Plus, Save, X } from 'lucide-react';
import { useEffect, useMemo, useRef, useState } from 'react';
import * as bridge from '../bridge/bridge';
import { NODE_TYPES, type FaustScript, type GraphNode, type Track } from '../bridge/protocol';
import { nodeName } from '../components/chain';
import { dispatch, refreshFaustScripts, useDawg } from '../store/store';
import { ui, useUi } from '../store/uiStore';
import { findNode } from '../graph/voice';
import { parseErrors } from './errors';
import { FAUST_LANGUAGE, FAUST_THEME, monaco, registerFaust } from './monaco';
import { Button, IconButton } from '../ui/controls';
import { Menu, Popover } from '../ui/overlays';

// The code view (docs/design/phase11-faust-panels.md, section 5): edits a Faust node's script. Typing compiles about
// 600 ms after it stops, Ctrl+Enter at once; errors show inline while the old DSP keeps playing.

const COMPILE_DELAY_MS = 600;

interface FaustEntry {
  track: Track;
  node: GraphNode;
}

function useFaustNodes(): FaustEntry[] {
  const project = useDawg((s) => s.project);
  return useMemo(() => {
    if (!project) return [];
    const tracks = project.trackOrder.map((id) => project.tracks[String(id)]).filter((t): t is Track => !!t);
    if (project.master) tracks.push(project.master);
    return tracks.flatMap((track) =>
      Object.values(track.graph.nodes)
        .filter((node) => node.type === NODE_TYPES.faust)
        .map((node) => ({ track, node })),
    );
  }, [project]);
}

export function CodeView() {
  const entries = useFaustNodes();
  const codeNode = useUi((s) => s.codeNode);
  const selectedTrackId = useUi((s) => s.selectedTrackId);
  const project = useDawg((s) => s.project);
  const current =
    entries.find((e) => e.track.id === codeNode?.trackId && e.node.id === codeNode?.nodeId) ?? entries[0] ?? null;

  // The dock shows the node being edited.
  useEffect(() => {
    if (current && (codeNode?.trackId !== current.track.id || codeNode?.nodeId !== current.node.id))
      ui.setCodeNode({ trackId: current.track.id, nodeId: current.node.id });
  }, [current, codeNode]);

  if (!current) {
    const trackId = selectedTrackId ?? project?.trackOrder[0];
    return (
      <div className="code-view empty-view">
        <p>No Faust nodes in this project yet.</p>
        <CloseCode />
        {trackId !== undefined && (
          <Button
            onClick={() => dispatch({ command: 'insertNode', args: { trackId, type: NODE_TYPES.faust } })}
          >
            <Plus size={13} /> Add a Faust node to the selected track
          </Button>
        )}
      </div>
    );
  }

  return <ScriptEditor key={`${current.track.id}:${current.node.id}`} entry={current} entries={entries} />;
}

function ScriptEditor({ entry, entries }: { entry: FaustEntry; entries: FaustEntry[] }) {
  const { track, node } = entry;
  const types = useDawg((s) => s.nodeTypes);
  const status = useDawg((s) => s.nodes[`${track.id}:${node.id}`]);
  const editor = useRef<Parameters<OnMount>[0] | null>(null);
  const timer = useRef<number | null>(null);
  const [menu, setMenu] = useState<'examples' | 'save' | null>(null);
  const code = node.code ?? '';
  const errors = status?.status === 'failed' ? parseErrors(status.error) : [];

  const commit = () => {
    if (timer.current !== null) window.clearTimeout(timer.current);
    timer.current = null;
    const value = editor.current?.getValue();
    if (value !== undefined && value !== currentCode(track.id, node.id))
      dispatch({ command: 'setNodeCode', args: { trackId: track.id, nodeId: node.id, code: value } });
  };
  const commitRef = useRef(commit);
  commitRef.current = commit;

  // Pending edits are kept when switching nodes or views.
  useEffect(() => () => commitRef.current(), []);

  // Undo, redo and other windows change the code: follow unless the user is typing.
  useEffect(() => {
    const instance = editor.current;
    const model = instance?.getModel();
    if (!instance || !model || timer.current !== null || model.getValue() === code) return;
    instance.executeEdits('dawg', [{ range: model.getFullModelRange(), text: code }]);
  }, [code]);

  // Errors as markers, while the previous program keeps running.
  useEffect(() => {
    const model = editor.current?.getModel();
    if (!model) return;
    monaco.editor.setModelMarkers(
      model,
      'faust',
      errors.map((e) => ({
        severity: monaco.MarkerSeverity.Error,
        message: e.message,
        startLineNumber: e.line,
        endLineNumber: e.line,
        startColumn: 1,
        endColumn: model.getLineMaxColumn(Math.min(e.line, model.getLineCount())),
      })),
    );
  }, [status?.status, status?.error]); // eslint-disable-line react-hooks/exhaustive-deps

  const onMount: OnMount = (instance, api) => {
    editor.current = instance;
    instance.addCommand(api.KeyMod.CtrlCmd | api.KeyCode.Enter, () => commitRef.current());
    instance.focus();
  };

  const replaceCode = (text: string) => {
    const instance = editor.current;
    const model = instance?.getModel();
    if (!instance || !model) return;
    instance.executeEdits('dawg', [{ range: model.getFullModelRange(), text }]);
    commitRef.current();
  };

  return (
    <div className="code-view">
      <div className="code-toolbar">
        <select
          className="code-node-picker"
          value={`${track.id}:${node.id}`}
          title="The Faust node to edit"
          onChange={(e) => {
            const [trackId, nodeId] = e.target.value.split(':').map(Number);
            ui.setCodeNode({ trackId, nodeId });
          }}
        >
          {entries.map((e) => (
            <option key={`${e.track.id}:${e.node.id}`} value={`${e.track.id}:${e.node.id}`}>
              {e.track.kind === 'master' ? 'Master' : e.track.name} · {nodeName(e.node, types)}
            </option>
          ))}
        </select>
        <CompileState status={status?.status} errors={errors.length} />
        <div className="spacer" />
        <IconButton size="l" title="Compile now (Ctrl+Enter)" onClick={commit}>
          <Play size={14} />
        </IconButton>
        <div className="code-menu-anchor">
          <IconButton
            size="l"
            className={`${menu === 'examples' ? 'active' : ''}`}
            title="Load an example or a script from your library"
            onClick={() => setMenu(menu === 'examples' ? null : 'examples')}
          >
            <BookOpen size={14} />
          </IconButton>
          {menu === 'examples' && (
            <ScriptMenu
              onPick={(script) => {
                setMenu(null);
                replaceCode(script.code);
              }}
              onClose={() => setMenu(null)}
            />
          )}
        </div>
        <div className="code-menu-anchor">
          <IconButton
            size="l"
            className={`${menu === 'save' ? 'active' : ''}`}
            title="Save to library"
            onClick={() => setMenu(menu === 'save' ? null : 'save')}
          >
            <Save size={14} />
          </IconButton>
          {menu === 'save' && (
            <SaveScript
              defaultName={nodeName(node, types)}
              code={() => editor.current?.getValue() ?? code}
              onClose={() => setMenu(null)}
            />
          )}
        </div>
        <IconButton
          size="l"
          title="Show the script library folder"
          onClick={() => bridge.call('faust.revealLibrary').catch(() => {})}
        >
          <FolderOpen size={14} />
        </IconButton>
        <CloseCode />
      </div>
      <div className="code-editor">
        <Editor
          defaultValue={code}
          language={FAUST_LANGUAGE}
          theme={FAUST_THEME}
          beforeMount={registerFaust}
          onMount={onMount}
          onChange={() => {
            if (timer.current !== null) window.clearTimeout(timer.current);
            timer.current = window.setTimeout(() => commitRef.current(), COMPILE_DELAY_MS);
          }}
          options={{
            fontFamily: "'IBM Plex Mono', ui-monospace, monospace",
            fontSize: 13,
            minimap: { enabled: false },
            scrollBeyondLastLine: false,
            automaticLayout: true,
            tabSize: 4,
            renderLineHighlight: 'line',
            contextmenu: false,
          }}
        />
      </div>
      {errors.length > 0 && (
        <div className="code-errors">
          {errors.map((e, i) => (
            <button
              key={i}
              className="code-error"
              onClick={() => {
                editor.current?.revealLineInCenter(e.line);
                editor.current?.setPosition({ lineNumber: e.line, column: 1 });
                editor.current?.focus();
              }}
            >
              <span className="code-error-line">Line {e.line}</span>
              <span>{e.message}</span>
            </button>
          ))}
          <span className="code-errors-note">The previous version keeps playing.</span>
        </div>
      )}
    </div>
  );
}

/** Closes the code view, back to the view it was opened from (it opens only from a Faust node). */
function CloseCode() {
  const back = useUi((s) => s.codeReturn);
  const label = back === 'graph' ? 'the graph' : 'the arrangement';
  return (
    <IconButton size="l" title={`Close the code editor, back to ${label}`} onClick={ui.closeCode}>
      <X size={14} />
    </IconButton>
  );
}

// The node's code in the model right now (the editor's closure may hold an older one).
function currentCode(trackId: number, nodeId: number) {
  const project = useDawg.getState().project;
  const track = project?.master?.id === trackId ? project.master : project?.tracks[String(trackId)];
  return track ? findNode(track, nodeId)?.code : undefined;
}

function CompileState({ status, errors }: { status: string | undefined; errors: number }) {
  if (status === 'compiling')
    return (
      <span className="compile-state">
        <LoaderCircle size={13} className="spin" /> Compiling
      </span>
    );
  if (status === 'failed')
    return (
      <span className="compile-state failed">
        <CircleAlert size={13} /> {errors === 1 ? '1 error' : `${errors} errors`}
      </span>
    );
  if (status === 'ready')
    return (
      <span className="compile-state ok">
        <CircleCheck size={13} /> Running
      </span>
    );
  return null;
}

function ScriptMenu({ onPick, onClose }: { onPick: (script: FaustScript) => void; onClose: () => void }) {
  const [scripts, setScripts] = useState<FaustScript[] | null>(null);
  useEffect(() => {
    bridge
      .call('faust.scripts')
      .then(setScripts)
      .catch(() => setScripts([]));
  }, []);

  const groups: Array<[string, FaustScript[]]> = [
    ['Examples', (scripts ?? []).filter((s) => s.source === 'example')],
    ['Your library', (scripts ?? []).filter((s) => s.source === 'library')],
  ];

  return (
    <>
      <Menu onClose={onClose} className="code-menu">
        {scripts === null && <div className="menu-note">Loading…</div>}
        {groups.map(([label, list]) =>
          list.length === 0 ? null : (
            <div key={label}>
              <div className="menu-heading">{label}</div>
              {list.map((script) => (
                <button key={`${script.source}/${script.file}`} className="menu-item" onClick={() => onPick(script)}>
                  {script.name}
                </button>
              ))}
            </div>
          ),
        )}
        {scripts !== null && scripts.every((s) => s.source !== 'library') && (
          <div className="menu-note">Save scripts to build your library.</div>
        )}
      </Menu>
    </>
  );
}

function SaveScript({ defaultName, code, onClose }: { defaultName: string; code: () => string; onClose: () => void }) {
  const [name, setName] = useState(defaultName);
  const [error, setError] = useState<string | null>(null);
  const save = () =>
    bridge
      .call('faust.saveScript', { name, code: code() })
      .then(() => {
        refreshFaustScripts();
        onClose();
      })
      .catch((e) => setError(String(e)));

  return (
    <>
      <Popover
        onClose={onClose}
        className="context-menu code-menu code-save"
        onSubmit={(e) => {
          e.preventDefault();
          save();
        }}
      >
        <label className="menu-heading" htmlFor="faust-save-name">
          Save to library as
        </label>
        <input id="faust-save-name" autoFocus value={name} onChange={(e) => setName(e.target.value)} />
        {error && <div className="menu-note failed">{error}</div>}
        <Button type="submit" disabled={!name.trim()}>
          Save
        </Button>
      </Popover>
    </>
  );
}
