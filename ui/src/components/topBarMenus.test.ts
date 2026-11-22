import { describe, expect, it } from 'vitest';
import topBar from './TopBar.tsx?raw';

// The top bar clips what hangs out of it (`.topbar { overflow: hidden }`, so it never widens the window). A menu or
// popover opened from it shows only if it is drawn into the body at a point in the window (`at`); one placed by its
// class inside the bar is cut off whole, and its invisible backdrop then takes the next click. That is how the File,
// Edit, View and Help menus stopped opening when they moved to the shared Menu (e1dd308).

const sources = import.meta.glob<string>('./*.tsx', { query: '?raw', import: 'default', eager: true });

/** The components the top bar draws from this folder, by file. */
function topBarFiles(): string[] {
  const files = [...topBar.matchAll(/from '\.\/([A-Za-z]+)'/g)].map(([, name]) => `./${name}.tsx`);
  return files.filter((file) => file in sources);
}

/** Each `<Menu …>` and `<Popover …>` opening tag in a source, up to its closing `>` (braces skipped). */
function overlayTags(source: string): string[] {
  const tags: string[] = [];
  for (const match of source.matchAll(/<(Menu|Popover)\b/g)) {
    let depth = 0;
    let end = match.index;
    for (; end < source.length; end++) {
      const c = source[end];
      if (c === '{') depth++;
      else if (c === '}') depth--;
      else if (c === '>' && depth === 0 && source[end - 1] !== '=') break;
    }
    tags.push(source.slice(match.index, end + 1));
  }
  return tags;
}

describe('the top bar\'s menus', () => {
  it('finds the menus the top bar draws', () => {
    const files = topBarFiles();
    for (const file of ['./FileMenu.tsx', './EditMenu.tsx', './ViewMenu.tsx', './HelpMenu.tsx', './MidiSettings.tsx']) {
      expect(files).toContain(file);
    }
  });

  it('open at a point in the window, so the bar does not clip them', () => {
    const placedByClass = topBarFiles().flatMap((file) =>
      overlayTags(sources[file])
        .filter((tag) => !/\sat=\{/.test(tag))
        .map((tag) => `${file}: ${tag.split('\n')[0]}`),
    );
    expect(placedByClass).toEqual([]);
  });

  it('finds the overlay tags it checks', () => {
    expect(overlayTags('<Menu at={{ x: 1, y: 2 }} onClose={() => f()}>a</Menu>')).toEqual(['<Menu at={{ x: 1, y: 2 }} onClose={() => f()}>']);
    expect(overlayTags('<Popover onClose={close} className="x">')).toEqual(['<Popover onClose={close} className="x">']);
  });
});
