import { beforeEach, describe, expect, it } from 'vitest';
import { CENTRE_ORDER, nextCentre, pointsOf, ui, useUi } from './uiStore';

describe('the centre views', () => {
  beforeEach(() => useUi.setState({ centre: 'arrangement', codeReturn: 'arrangement', codeNode: null, selectedNode: null }));

  it('cycles the arrangement and the graph, without the code view', () => {
    expect(CENTRE_ORDER).not.toContain('code');
    expect(nextCentre('arrangement', 'arrangement')).toBe('graph');
    expect(nextCentre('graph', 'arrangement')).toBe('arrangement');
  });

  it('opens the code view from a Faust node and closes it back to where it was opened', () => {
    ui.setCentre('graph');
    ui.editCode(2, 7);
    expect(useUi.getState()).toMatchObject({
      centre: 'code',
      codeReturn: 'graph',
      codeNode: { trackId: 2, nodeId: 7 },
      selectedNode: { trackId: 2, nodeId: 7 },
    });

    // Another node from the dock keeps the way back.
    ui.editCode(2, 8);
    expect(useUi.getState().codeReturn).toBe('graph');

    ui.closeCode();
    expect(useUi.getState().centre).toBe('graph');
  });

  it('leaves the code view with Tab, back to its view', () => {
    ui.setCentre('graph');
    ui.editCode(1, 3);
    ui.toggleCentre();
    expect(useUi.getState().centre).toBe('graph');
  });

  it('does nothing when closing outside the code view', () => {
    ui.setCentre('graph');
    ui.closeCode();
    expect(useUi.getState().centre).toBe('graph');
  });
});

describe('point selection', () => {
  beforeEach(() => ui.selectNotesAndPoints([], null));

  it('drops notes without points, and keeps points only in notes that stay selected', () => {
    ui.selectNotesAndPoints([1, 2], { lane: 'pitch', notes: new Map([[1, [0]], [2, [1]], [3, []]]) });
    expect([...useUi.getState().selectedPoints!.notes.keys()]).toEqual([1, 2]);

    ui.selectNotes([2]);
    expect([...useUi.getState().selectedPoints!.notes.keys()]).toEqual([2]);
    ui.selectNotes([4], true);
    expect([...useUi.getState().selectedPoints!.notes.keys()]).toEqual([2]);
    ui.selectNotes([4]);
    expect(useUi.getState().selectedPoints).toBeNull();
  });

  it("reads one note's points in one lane", () => {
    ui.selectPoints({ lane: 'pressure', notes: new Map([[1, [0, 2]]]) });
    const selection = useUi.getState().selectedPoints;
    expect(pointsOf(selection, 1, 'pressure')).toEqual([0, 2]);
    expect(pointsOf(selection, 1, 'pitch')).toEqual([]);
    expect(pointsOf(selection, 2, 'pressure')).toEqual([]);
  });
});
