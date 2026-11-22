import type { GraphNode, NodeWidths, PluginChannels, PortDescriptor } from '../bridge/protocol';

// Port widths in the graph (docs/design/mono-ports.md, MP8, MP9). The last compilation says what each port of a node
// with a mono port got; any other port shows as declared, an adaptive one as stereo (a node the compilation doesn't
// list is all stereo, so its adaptive ports are right). Inside a voice graph, which compiles per voice, ports show as
// declared too.

/** Whether an audio port carries one channel. */
export function isMono(port: PortDescriptor, side: 'in' | 'out', index: number, widths: NodeWidths | undefined): boolean {
  if (port.type !== 'audio') return false;
  const compiled = widths ? (side === 'in' ? widths.inputs : widths.outputs)[index] : undefined;
  return compiled === 1 || compiled === 2 ? compiled === 1 : port.channels === 1;
}

/** A component port's widths, in the order its width button steps through them (MP11). */
export const PORT_WIDTHS: Array<{ channels: number; label: string; title: string }> = [
  { channels: 2, label: 'S', title: 'Stereo: what crosses it is made stereo. Click for mono.' },
  { channels: 1, label: 'M', title: 'Mono: what crosses it is folded to mono, (L + R) / 2. Click for adaptive.' },
  { channels: 0, label: 'A', title: 'Adaptive: mono when what feeds it is. Click for stereo.' },
];

/** A component port's width setting: 2 when absent. */
export const portChannels = (port: PortDescriptor): number => port.channels ?? 2;

/** Whether a stereo cable into this input is folded to mono. */
export function isFolded(index: number, widths: NodeWidths | undefined): boolean {
  return widths?.folded.includes(index) ?? false;
}

/** A port's tooltip: its name and, for audio, its width. */
export function portTitle(port: PortDescriptor, mono: boolean): string {
  return port.type === 'audio' ? `${port.name}: ${mono ? 'Mono' : 'Stereo'}` : port.name;
}

export const FOLD_TITLE = 'Folded to mono: (L + R) / 2';

/** An effect plugin's Channels setting (MP4), in the plugin's menu and the inspector. */
export const PLUGIN_CHANNELS: Array<{ value: PluginChannels; label: string; title: string }> = [
  { value: 'auto', label: 'Auto', title: 'Mono in, mono out where the plugin can; stereo in, stereo out' },
  { value: 'monoToStereo', label: 'Mono → stereo', title: 'One channel in, two out: for reverbs and delays that spread a mono input' },
  { value: 'stereo', label: 'Stereo', title: 'Always two channels in and out' },
];

/** An effect plugin's Channels setting. */
export const pluginChannels = (node: GraphNode): PluginChannels => node.channels ?? 'auto';
