// Every keyboard shortcut and mouse gesture, by where it works, for the keyboard and mouse reference (HelpMenu.tsx).
// One area at a time, mouse and keys apart, a short line each. Keep it in step with the handlers (keys.ts, App.tsx,
// useArrangementEditing.ts, useRollEditing.ts, GraphView.tsx, DeviceStrip.tsx, Mixer.tsx).

export type Shortcut = [gesture: string, effect: string];

export interface ShortcutArea {
  title: string;
  mouse: Shortcut[];
  keys: Shortcut[];
}

export const SHORTCUTS: ShortcutArea[] = [
  {
    title: 'Everywhere',
    mouse: [],
    keys: [
      ['Space', 'Play or pause'],
      ['R', 'Record (Shift+R: with a count-in)'],
      ['L', 'Loop on or off'],
      ['Insert', 'Marker at the playhead'],
      ['Tab', 'Arrangement or graph'],
      ['Ctrl+Z', 'Undo (Ctrl+Shift+Z: redo)'],
      ['Ctrl+S', 'Save (Ctrl+Shift+S: save as)'],
      ['Ctrl+N / O / W', 'New, open, close project'],
      ['Ctrl+E', 'Export audio'],
      ['Ctrl+M', 'Mixer window'],
      ['F6', 'Focus the next panel'],
    ],
  },
  {
    title: 'Arrangement',
    mouse: [
      ['Double-click empty space', 'New clip'],
      ['Alt+drag empty space', 'New clip, as long as the drag'],
      ['Double-click a clip', 'Open it in the piano roll'],
      ['Drag a clip', 'Move (Ctrl: off the grid)'],
      ['Alt+drag a clip', 'Copy'],
      ['Alt+click a clip', 'Split at the pointer'],
      ['Drag a clip edge', 'Resize'],
      ['Drag a clip corner', 'Loop the clip'],
      ['Double-click a lane', 'New automation point'],
      ['Click a take', 'Play it'],
      ['Drag across a take', 'Play that part'],
      ['Ruler, top half', 'Set, move or toggle the loop'],
      ['Ruler, bottom half', 'Move the playhead'],
    ],
    keys: [
      ['Delete', 'Delete the selection'],
      ['Ctrl+C / X / V', 'Copy, cut, paste'],
      ['Ctrl+D', 'Duplicate'],
      ['S', 'Split at the playhead'],
      ['Ctrl+J', 'Glue'],
      ['Ctrl+L', 'Loop the clips'],
      ['F2', 'Rename'],
      ['Ctrl+G', 'Group the selected tracks'],
      ['Ctrl+Shift+D', 'Duplicate the track'],
    ],
  },
  {
    title: 'Piano roll',
    mouse: [
      ['Double-click empty space', 'New note'],
      ['Alt+drag empty space', 'New note, as long as the drag'],
      ['Drag a note', 'Move (Ctrl: off the grid)'],
      ['Drag a note edge', 'Resize'],
      ['Alt+drag a note', 'Copy'],
      ['Alt+click a note', 'Split at the pointer'],
      ['Ctrl+Alt+drag a note', 'Link it to another note'],
      ['Double-click a note or lane', 'New point'],
      ['Double-click a point', 'Delete it'],
      ['Ruler handle', 'Resize the clip'],
    ],
    keys: [
      ['Up / Down', 'Transpose (Shift: an octave)'],
      ['Ctrl+Left / Right', 'Move by the grid'],
      ['Left / Right', 'Select the next note'],
      ['Alt+Up / Down', 'Velocity'],
      ['Q', 'Quantise'],
      ['S', 'Split at the playhead'],
      ['H', 'Humanise'],
      ['Delete', 'Delete the selection'],
      ['Ctrl+C / X / V / D', 'Copy, cut, paste, duplicate'],
    ],
  },
  {
    title: 'Graph',
    mouse: [
      ['Right-click', 'Add a node'],
      ['Drag between ports', 'Connect'],
      ['Pull a cable off an input', 'Disconnect'],
      ["Drag a cable's end to a port", 'Move it there (its output end once selected)'],
      ['Drop a node on a cable', 'Insert it'],
      ['Shake a node', 'Take it out of its cables'],
      ['Double-click', 'Open a component'],
      ['Shift+drag a node', 'Move it off the grid'],
      ['Drag a cable', 'Bend it, or move a stretch of it'],
      ['Double-click a cable', 'Add a bend'],
      ['Double-click a bend', 'Take it out'],
      ['Right-click a cable', 'Reset it'],
    ],
    keys: [
      ['Delete', 'Delete the selection'],
      ['Ctrl+C / X / V / D', 'Copy, cut, paste, duplicate'],
      ['Ctrl+B', 'Bypass'],
      ['Ctrl+G', 'Group in a component (Ctrl+Shift+G: ungroup)'],
    ],
  },
  {
    title: 'Devices',
    mouse: [
      ['Drag a card header', 'Move it in the chain'],
      ['+ between cards', 'Add an effect'],
      ['Drag from the browser', 'Add to a track, the strip or the graph'],
      ['Double-click in the browser', 'Add to the selected track'],
      ['Shift+wheel', 'Scroll the strip'],
    ],
    keys: [['Delete', 'Remove the selected device']],
  },
  {
    title: 'Mixer',
    mouse: [],
    keys: [
      ['Left / Right', 'Select a strip'],
      ['M / S', 'Mute, solo'],
    ],
  },
];
