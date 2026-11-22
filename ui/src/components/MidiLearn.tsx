import { ChevronRight } from 'lucide-react';
import { useState, type ReactNode } from 'react';
import type { AutomationTarget, Track } from '../bridge/protocol';
import { showAutomation } from '../arrangement/AutomationHeaders';
import { mapToMacro, mappable } from '../dock/assign';
import { endName, linkDriving, linksFrom, removeLinks } from '../dock/links';
import { modulationUi } from '../dock/modulationStore';
import { MACRO_COUNT, macroName, macrosOf } from '../dock/macros';
import { infoOf } from '../graph/nodeInfo';
import { findNode } from '../graph/voice';
import { actions, dispatch, useDawg } from '../store/store';
import { Menu } from '../ui/overlays';

// MIDI learn (docs/design/phase9-export-midi-input.md, M8): right-click a control to map the next controller moved to
// it, or to forget its mapping. A learning control blinks; a mapped one is underlined.

export interface MidiTarget {
  trackId: number;
  /** 0 for a track parameter ("gain" or "pan"). */
  nodeId: number;
  param: string;
}

const same = (a: MidiTarget | null, b: MidiTarget) =>
  a !== null && a.trackId === b.trackId && a.nodeId === b.nodeId && a.param === b.param;

/** The track a target is on, and whether its parameter can be mapped to a macro (macros-and-modulation-column.md, MC9). */
function macroTarget(target: MidiTarget): Track | null {
  const s = useDawg.getState();
  const track = s.project?.master?.id === target.trackId ? s.project.master : s.project?.tracks[String(target.trackId)];
  const node = track && target.nodeId !== 0 ? findNode(track, target.nodeId) : undefined;
  if (!track || !node) return null;
  const param = infoOf(track.id, node, s.nodeTypes, s.nodes)?.params.find((p) => p.id === target.param);
  return param && mappable(node, param.modulatable) ? track : null;
}

/** `contents`: the wrapper takes no room of its own, for a control (a knob's dial) rather than a label. */
export function MidiLearnable({ target, children, contents }: { target: MidiTarget; children: ReactNode; contents?: boolean }) {
  const learning = useDawg((s) => same(s.learning, target));
  const mapping = useDawg((s) => s.project?.midiMappings?.find((m) => same(m, target)));
  const [menu, setMenu] = useState<{ x: number; y: number; track: Track | null } | null>(null);
  const [macrosOpen, setMacrosOpen] = useState(false);
  const project = useDawg((s) => s.project);
  const driving = target.nodeId !== 0 ? linkDriving(project, target) : undefined;
  const drives = target.nodeId !== 0 ? linksFrom(project, target) : [];

  return (
    <span
      className={`midi-learnable ${contents ? 'contents' : ''} ${learning ? 'midi-learning' : ''} ${mapping ? 'midi-mapped' : ''}`}
      title={mapping ? `MIDI: CC ${mapping.controller}, channel ${mapping.channel}${mapping.device ? ` (${mapping.device})` : ''}` : undefined}
      onContextMenu={(e) => {
        e.preventDefault();
        e.stopPropagation();
        setMacrosOpen(false);
        setMenu({ x: e.clientX, y: e.clientY, track: macroTarget(target) });
      }}
    >
      {children}
      {menu && (
        <>
          <Menu onClose={() => setMenu(null)} at={{ x: menu.x, y: menu.y }}>
            {menu.track && (
              <div className="submenu" onPointerEnter={() => setMacrosOpen(true)} onPointerLeave={() => setMacrosOpen(false)}>
                <button className="menu-item with-shortcut" onClick={() => setMacrosOpen((open) => !open)}>
                  <span>Map to Macro</span>
                  <ChevronRight size={12} />
                </button>
                {macrosOpen && (
                  <div className="context-menu submenu-popup">
                    {Array.from({ length: MACRO_COUNT }, (_, knob) => (
                      <button
                        key={knob}
                        className="menu-item"
                        onClick={() => {
                          setMenu(null);
                          mapToMacro(target.trackId, target.nodeId, target.param, knob);
                        }}
                      >
                        {knob + 1} · {macroName(macrosOf(menu.track!), knob)}
                      </button>
                    ))}
                  </div>
                )}
              </div>
            )}
            {target.nodeId !== 0 && (
              <button
                className="menu-item"
                title="Then move the parameter that should follow this one"
                onClick={() => (setMenu(null), modulationUi.startLinking(target))}
              >
                Link to another parameter…
              </button>
            )}
            {driving && (
              <button className="menu-item" onClick={() => (setMenu(null), removeLinks([driving.id]))}>
                Stop following {endName(project, driving.source)}
              </button>
            )}
            {drives.length > 0 && (
              <button className="menu-item" onClick={() => (setMenu(null), removeLinks(drives.map((l) => l.id)))}>
                {drives.length === 1 ? `Unlink ${endName(project, drives[0].target)}` : `Unlink the ${drives.length} parameters it drives`}
              </button>
            )}
            {learning ? (
              <button className="menu-item" onClick={() => (setMenu(null), actions.midiLearn(null))}>
                Cancel MIDI learn
              </button>
            ) : (
              <button className="menu-item" onClick={() => (setMenu(null), actions.midiLearn(target))}>
                MIDI learn (move a controller)
              </button>
            )}
            <button
              className="menu-item"
              onClick={() => {
                // Reveal (or add) the parameter's automation lane (docs/design/phase10-automation.md, section 5).
                setMenu(null);
                const project = useDawg.getState().project;
                const track =
                  project?.master?.id === target.trackId ? project.master : project?.tracks[String(target.trackId)];
                if (!track) return;
                const automationTarget: AutomationTarget =
                  target.nodeId === 0
                    ? { kind: target.param === 'pan' ? 'pan' : 'gain', nodeId: 0, param: target.param, name: '' }
                    : { kind: 'node', nodeId: target.nodeId, param: target.param, name: '' };
                showAutomation(track, automationTarget);
              }}
            >
              Show automation
            </button>
            <button
              className="menu-item"
              disabled={!mapping}
              onClick={() => {
                setMenu(null);
                dispatch({ command: 'removeMidiMapping', args: { trackId: target.trackId, nodeId: target.nodeId, param: target.param } });
              }}
            >
              Forget MIDI mapping
            </button>
          </Menu>
        </>
      )}
    </span>
  );
}
