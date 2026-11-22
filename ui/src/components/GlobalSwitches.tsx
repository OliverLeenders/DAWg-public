import { create } from 'zustand';
import { restorable, switchClick, tracksWith, type SwitchKey } from '../arrangement/switches';
import { dispatch, useDawg } from '../store/store';
import { TrackSwitch } from '../ui/controls';

// The global mute and solo switches, in the track list header and the mixer window's toolbar
// (docs/design/phase6-arrangement.md, section 7). Each window remembers what it last cleared, until the project
// changes.

const useRemembered = create<Record<SwitchKey, number[]>>()(() => ({ mute: [], solo: [] }));

// Track ids mean nothing in another project.
useDawg.subscribe((s, prev) => {
  if (s.projectState.path !== prev.projectState.path || s.projectState.open !== prev.projectState.open)
    useRemembered.setState({ mute: [], solo: [] });
});

function GlobalSwitch({ which }: { which: SwitchKey }) {
  const project = useDawg((s) => s.project);
  const remembered = useRemembered((s) => s[which]);
  const on = tracksWith(project, which).length;
  const back = on === 0 ? restorable(project, which, remembered).length : 0;
  const [verb, noun] = which === 'mute' ? ['muted', 'mute'] : ['soloed', 'solo'];
  const tracks = (n: number) => `${n} track${n === 1 ? '' : 's'}`;
  const title =
    on > 0
      ? `${tracks(on)} ${verb}: click to un${noun} all (a second click brings them back)`
      : back > 0
        ? `Click to ${noun} the last ${tracks(back)} again`
        : `No track is ${verb}`;

  return (
    <TrackSwitch
      kind={which}
      on={on > 0}
      className={back > 0 ? 'restorable' : undefined}
      title={title}
      disabled={on === 0 && back === 0}
      onClick={() => {
        const click = switchClick(useDawg.getState().project, which, useRemembered.getState()[which]);
        if (click.command) dispatch(click.command);
        useRemembered.setState({ [which]: click.remember });
      }}
    >
      {which === 'mute' ? 'M' : 'S'}
    </TrackSwitch>
  );
}

export function GlobalSwitches() {
  return (
    <div className="global-switches" title="Every track's mute and solo">
      <span className="global-switches-label">All</span>
      <GlobalSwitch which="mute" />
      <GlobalSwitch which="solo" />
    </div>
  );
}
