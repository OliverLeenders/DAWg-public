import { Splitter } from '../components/Splitter';
import { useDawg } from '../store/store';
import { layout, useLayout } from '../store/layoutStore';
import { useUi } from '../store/uiStore';
import { DeviceStrip } from './DeviceStrip';
import { ModulationColumn } from './ModulationColumn';
import { dockTrack } from './dockTrack';
import { TrackFader } from './TrackFader';
import { outputTracksOf } from '../arrangement/groups';

// The device dock under the centre view: the track's modulation column on the left (docs/design/
// macros-and-modulation-column.md, MC12), the device strip of one track, and the track's fader fixed at its right end
// (docs/design/device-strip.md; channel-strip.md, C7).

export function DeviceDock() {
  const height = useLayout((s) => s.dockHeight);
  const project = useDawg((s) => s.project);
  const selectedTrackId = useUi((s) => s.selectedTrackId);
  // The code view edits one Faust node, which may be on a track other than the selected one.
  const codeTrackId = useUi((s) => (s.centre === 'code' ? (s.codeNode?.trackId ?? null) : null));
  const track = dockTrack(project, codeTrackId ?? selectedTrackId);
  // Inside a component or a Voice Instrument while the graph view is: the arrangement shows the track's own level (D4).
  // A track with output tracks is only their header: its outputs have the faders (multi-output-instruments.md, MO12).
  const header = !!project && !!track && outputTracksOf(project, track.id) > 0;
  const scope = useUi((s) => (s.centre === 'graph' && s.graphScope && s.graphScope.trackId === track?.id ? s.graphScope.nodeId : undefined));

  return (
    <div className="device-dock" style={{ height }}>
      <Splitter width={height} direction={-1} axis="y" onResize={layout.setDockHeight} className="splitter horizontal" />
      <div className="dock-row">
        {track && <ModulationColumn key={`mod:${track.id}`} track={track} />}
        {track ? (
          // The strip is a working area: sunk below the dock's panels, its cards raised on it (design-system.md, DS7).
          <div className="device-strip-area working-area">
            <DeviceStrip key={`strip:${track.id}:${scope ?? ''}`} track={track} scope={scope} />
          </div>
        ) : (
          <p className="dock-empty strip-empty">Select a track to see its devices.</p>
        )}
        {track && !header && <TrackFader key={track.id} track={track} />}
      </div>
    </div>
  );
}
