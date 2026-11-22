import { beforeEach, describe, expect, it } from 'vitest';
import { NODE_TYPES, type GraphNode, type Track } from '../bridge/protocol';
import { useLayout } from '../store/layoutStore';
import { useUi } from '../store/uiStore';
import { openDevice } from './openDevice';

const node = (type: string): GraphNode => ({ id: 2, type, params: {}, x: 0, y: 0, bypassed: false });
const track = { id: 1, name: 'Track', graph: { nodes: {}, connections: {} } } as unknown as Track;

describe('opening a device from the inspector', () => {
  beforeEach(() => {
    useUi.setState({
      centre: 'arrangement',
      selectedTrackId: 1,
      selectedNode: null,
      graphScope: null,
    });
    useLayout.setState({ dockVisible: true });
  });

  it("opens a Voice Instrument's voice graph in one click", () => {
    openDevice(track, node(NODE_TYPES.voiceInstrument));

    expect(useUi.getState()).toMatchObject({
      centre: 'graph',
      graphScope: { trackId: 1, nodeId: 2 },
      selectedNode: null,
    });
  });

  it('still selects an ordinary device and opens the dock', () => {
    useLayout.setState({ dockVisible: false });
    openDevice(track, node(NODE_TYPES.eq));

    expect(useUi.getState().selectedNode).toEqual({ trackId: 1, nodeId: 2 });
    expect(useLayout.getState().dockVisible).toBe(true);
  });
});
