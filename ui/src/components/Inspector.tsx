import { PanelLeftClose } from 'lucide-react';
import { layout } from '../store/layoutStore';
import { useDawg } from '../store/store';
import { useUi } from '../store/uiStore';
import { TrackInspector } from './TrackInspector';
import { TrackOffset } from './TrackOffset';
import { ClipInspector } from './ClipInspector';
import { AudioTrackInspector } from './AudioTrackInspector';
import { InspectorMacros } from './InstrumentSection';
import { trackChain } from './chain';
import { NodeInspector } from './NodeInspector';
import { SendsInspector } from './SendsInspector';
import { findNode } from '../graph/voice';
import { IconButton } from '../ui/controls';

// The left panel: the selected track's instrument and inserts, sends, clip and timing (docs/design/phase6-arrangement.md,
// section 7). Track headers live in the arrangement.

export function Inspector() {
  const project = useDawg((s) => s.project);
  const selectedTrackId = useUi((s) => s.selectedTrackId);
  const selectedClips = useUi((s) => s.selectedClips);
  const selectedNode = useUi((s) => s.selectedNode);
  const centre = useUi((s) => s.centre);
  const id = selectedTrackId ?? project?.trackOrder[0];
  const master = project?.master ?? null;
  const track = id === undefined ? undefined : master?.id === id ? master : project?.tracks[String(id)];

  return (
    <section className="panel tracks">
      <div className="panel-header">
        <h2>Inspector</h2>
        <div className="panel-header-buttons">
          <IconButton title="Hide the inspector (View menu shows it again)" onClick={layout.toggleInspector}>
            <PanelLeftClose size={14} />
          </IconButton>
        </div>
      </div>
      <div className="panel-body">
        {track ? (
          <>
            <div className="inspector-track">
              <div className="track-colour" style={{ background: track.colour }} />
              <span className="track-name">{track.name}</span>
            </div>
            {(() => {
              // In the graph view, the selected node's parameters come first (phase8-node-graph.md, section 5).
              const node = centre === 'graph' && selectedNode?.trackId === track.id ? findNode(track, selectedNode.nodeId) : undefined;
              return node ? <NodeInspector track={track} node={node} /> : null;
            })()}
            {track.kind === 'instrument' && <TrackInspector track={track} />}
            {track.kind === 'audio' && <AudioTrackInspector track={track} />}
            {/* A simple chain's inspector shows the macros under its instrument; others here (MC11). */}
            {(track.kind === 'audio' || !trackChain(track)) && (
              <div className="inspector">
                <InspectorMacros track={track} />
              </div>
            )}
            {(track.kind === 'master' || track.kind === 'group' || track.kind === 'return' || track.kind === 'output') && <TrackInspector track={track} />}
            {track.kind !== 'master' && <SendsInspector track={track} />}
            {/* Few tracks need an offset: it comes last (docs/design/ux-review.md, U11). */}
            {track.kind !== 'master' && <TrackOffset track={track} />}
            {(() => {
              // One selected audio clip: its clip section (double-click on a clip shows it).
              const clips = Object.values(track.clips).filter((c) => selectedClips.has(c.id));
              return clips.length === 1 && clips[0].fileId !== 0 ? <ClipInspector clip={clips[0]} /> : null;
            })()}
          </>
        ) : (
          <p className="empty">No track selected.</p>
        )}
      </div>
    </section>
  );
}
