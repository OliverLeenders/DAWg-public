import { Layers, Save, X } from 'lucide-react';
import { call } from '../bridge/bridge';
import { useState } from 'react';
import { exposedControls } from '../graph/nodeInfo';
import { dispatch, refreshSavedComponents, reportError, useDawg } from '../store/store';
import { ui } from '../store/uiStore';
import { Knob } from './Knob';
import type { PanelProps } from './panels';
import { Button } from '../ui/controls';

// A component in the device dock (docs/design/component-nodes.md, N6): its name, and the inner parameters shown on it. A knob here is the
// inner parameter itself, so it goes through the same command, undo, automation and MIDI learn.

export function ComponentPanel({ track, node }: PanelProps) {
  const types = useDawg((s) => s.nodeTypes);
  const statuses = useDawg((s) => s.nodes);
  const [renaming, setRenaming] = useState<string | null>(null);
  const component = node.component;
  if (!component) return null;
  const controls = exposedControls(track, component, types, statuses);

  return (
    <div className="device-panel component-panel">
      <div className="component-panel-head">
        <input
          key={component.name}
          className="component-name"
          aria-label="Component name"
          placeholder="Component"
          defaultValue={component.name}
          onKeyDown={(e) => {
            e.stopPropagation();
            if (e.key === 'Enter') e.currentTarget.blur();
          }}
          onBlur={(e) => {
            if (e.target.value !== component.name) dispatch({ command: 'renameComponent', args: { trackId: track.id, nodeId: node.id, name: e.target.value } });
          }}
        />
        <Button onClick={() => ui.enterGraph(track.id, node.id)}>
          <Layers size={12} /> Open
        </Button>
        <Button
          title="Add it to the browser, with its plugins' state, for any project. A saved one of the same name is replaced."
          onClick={() =>
            void call('components.save', { trackId: track.id, nodeId: node.id })
              .then(refreshSavedComponents)
              .catch(reportError)
          }
        >
          <Save size={12} /> Save
        </Button>
      </div>
      {controls.length === 0 ? (
        <p className="dock-empty" title="Open it, select a node inside, and pin a parameter in the inspector to show it here">
          No pinned parameters.
        </p>
      ) : (
        <div className="dock-knobs">
          {controls.map(({ exposed, node: inner, param }) => {
            const key = `${exposed.nodeId}:${exposed.param}`;
            const args = { trackId: track.id, nodeId: node.id, target: exposed.nodeId, param: exposed.param };
            return (
              <div
                className="component-control"
                key={key}
                onDoubleClick={(e) => {
                  if ((e.target as Element).closest('.knob-label')) setRenaming(key);
                }}
              >
                <Knob track={track} node={inner} param={param} label={exposed.name} />
                <button
                  className="component-control-remove"
                  title="Take it off the component"
                  aria-label={`Take ${exposed.name} off the component`}
                  onClick={() => dispatch({ command: 'unexposeParam', args })}
                >
                  <X size={10} />
                </button>
                {renaming === key && (
                  <input
                    className="component-control-name"
                    autoFocus
                    aria-label="Control name"
                    defaultValue={exposed.name}
                    onKeyDown={(e) => {
                      e.stopPropagation();
                      if (e.key === 'Enter') e.currentTarget.blur();
                      if (e.key === 'Escape') setRenaming(null);
                    }}
                    onBlur={(e) => {
                      setRenaming(null);
                      if (e.target.value !== exposed.name) dispatch({ command: 'renameExposedParam', args: { ...args, name: e.target.value } });
                    }}
                  />
                )}
              </div>
            );
          })}
        </div>
      )}
    </div>
  );
}
