import { useDawg } from '../store/store';
import { describeMapping, endName, linkDriving, linksFrom, mappingUnit, sameAddress } from './links';
import { modulationUi, useModulationUi } from './modulationStore';

// How a control shows its links (docs/design/parameter-linking.md, L16): a mark when it drives or follows another
// parameter, the other ends marked while the pointer is on it, and Link mode's cursor.

export function useLinkMarks(trackId: number, nodeId: number, param: string) {
  const address = { trackId, nodeId, param };
  const project = useDawg((s) => s.project);
  const driving = linkDriving(project, address);
  const drives = linksFrom(project, address);
  const marked = useModulationUi((s) => s.marked.some((m) => sameAddress(m, address)));
  const linking = useModulationUi((s) => s.linking !== null && !sameAddress(s.linking, address));
  const source = useModulationUi((s) => s.linking !== null && sameAddress(s.linking, address));

  const lines: string[] = [];
  if (driving)
    lines.push(
      `Follows ${endName(project, driving.source)} (${describeMapping(driving.mapping, mappingUnit(driving, project))})${driving.active ? '' : ', switched off'}`,
    );
  if (drives.length > 0) lines.push(`Drives ${drives.map((l) => endName(project, l.target)).join(', ')}`);

  const ends = [...(driving ? [driving.source] : []), ...drives.map((l) => l.target)];
  const classes = [
    driving?.active ? 'link-target' : '',
    drives.length > 0 ? 'link-source' : '',
    marked ? 'link-marked' : '',
    linking ? 'link-linking' : '',
    source ? 'link-from' : '',
  ].join(' ');

  return {
    classes,
    /** What the tooltip adds: the links, or what Link mode does. */
    title: linking ? 'Link mode: move it to make it follow the parameter you started from (Escape ends)' : lines.join('\n'),
    events:
      ends.length > 0
        ? { onPointerEnter: () => modulationUi.mark(ends), onPointerLeave: () => modulationUi.mark([]) }
        : {},
  };
}
