import { loader } from '@monaco-editor/react';
import * as monaco from 'monaco-editor/esm/vs/editor/editor.api';
import EditorWorker from 'monaco-editor/esm/vs/editor/editor.worker?worker';
import * as bridge from '../bridge/bridge';

// Monaco, bundled rather than loaded from a CDN because DAWg works offline (docs/design/phase11-faust-panels.md,
// F7), with a Faust grammar, DAWg's theme and completion of the standard libraries.

self.MonacoEnvironment = { getWorker: () => new EditorWorker() };
loader.config({ monaco });

export const FAUST_LANGUAGE = 'faust';
export const FAUST_THEME = 'dawg-dark';

const KEYWORDS = [
  'import', 'component', 'library', 'environment', 'declare', 'with', 'letrec', 'where', 'case', 'process',
  'effect', 'par', 'seq', 'sum', 'prod', 'inputs', 'outputs', 'route', 'waveform', 'soundfile', 'rdtable', 'rwtable',
  'select2', 'select3', 'ffunction', 'fconstant', 'fvariable', 'attach', 'enable', 'control', 'mem', 'prefix',
  'int', 'float', 'singleprecision', 'doubleprecision', 'quadprecision', 'fixedpointprecision',
];

const UI_ELEMENTS = ['button', 'checkbox', 'hslider', 'vslider', 'nentry', 'hgroup', 'vgroup', 'tgroup', 'hbargraph', 'vbargraph'];

const PRIMITIVES = [
  'abs', 'acos', 'asin', 'atan', 'atan2', 'ceil', 'cos', 'exp', 'floor', 'fmod', 'log', 'log10', 'max', 'min', 'pow',
  'remainder', 'rint', 'round', 'sin', 'sqrt', 'tan', 'xor', 'lowest', 'highest',
];

let registered = false;

/** Registers the Faust language and the theme once (they are global to Monaco). */
export function registerFaust() {
  if (registered) return;
  registered = true;

  monaco.languages.register({ id: FAUST_LANGUAGE, extensions: ['.dsp', '.lib'] });
  monaco.languages.setLanguageConfiguration(FAUST_LANGUAGE, {
    comments: { lineComment: '//', blockComment: ['/*', '*/'] },
    brackets: [
      ['(', ')'],
      ['[', ']'],
      ['{', '}'],
    ],
    autoClosingPairs: [
      { open: '(', close: ')' },
      { open: '[', close: ']' },
      { open: '{', close: '}' },
      { open: '"', close: '"' },
    ],
  });

  monaco.languages.setMonarchTokensProvider(FAUST_LANGUAGE, {
    keywords: KEYWORDS,
    ui: UI_ELEMENTS,
    primitives: PRIMITIVES,
    tokenizer: {
      root: [
        [/\/\/.*$/, 'comment'],
        [/\/\*/, 'comment', '@comment'],
        [/"([^"\\]|\\.)*"/, 'string'],
        // Environment access such as fi.lowpass.
        [/[a-zA-Z_]\w*(?=\.)/, 'namespace'],
        [
          /[a-zA-Z_]\w*/,
          { cases: { '@keywords': 'keyword', '@ui': 'type', '@primitives': 'predefined', '@default': 'identifier' } },
        ],
        [/\d+\.\d*([eE][-+]?\d+)?|\.\d+([eE][-+]?\d+)?|\d+([eE][-+]?\d+)?/, 'number'],
        [/<:|:>|~|:|,|@|'/, 'operator'],
        [/[+\-*/%^&|<>=!]+/, 'operator'],
        [/[;()[\]{}]/, 'delimiter'],
      ],
      comment: [
        [/[^/*]+/, 'comment'],
        [/\*\//, 'comment', '@pop'],
        [/[/*]/, 'comment'],
      ],
    },
  });

  // The design tokens (ui/src/tokens.css), read now: Monaco wants its colours as literals when its theme is defined.
  const css = getComputedStyle(document.documentElement);
  const v = (name: string) => css.getPropertyValue(name).trim();
  const hex = (name: string) => v(name).replace('#', '');
  monaco.editor.defineTheme(FAUST_THEME, {
    base: 'vs-dark',
    inherit: true,
    rules: [
      { token: 'comment', foreground: hex('--syntax-comment'), fontStyle: 'italic' },
      { token: 'string', foreground: hex('--syntax-string') },
      { token: 'number', foreground: hex('--syntax-number') },
      { token: 'keyword', foreground: hex('--syntax-keyword') },
      { token: 'type', foreground: hex('--syntax-type') },
      { token: 'predefined', foreground: hex('--syntax-type') },
      { token: 'namespace', foreground: hex('--text-dim') },
      { token: 'operator', foreground: hex('--syntax-operator') },
      { token: 'delimiter', foreground: hex('--text-dim') },
      { token: 'identifier', foreground: hex('--text') },
    ],
    colors: {
      'editor.background': v('--bg'),
      'editor.foreground': v('--text'),
      'editor.lineHighlightBackground': v('--bg-raised'),
      'editor.selectionBackground': v('--accent-soft'),
      'editorLineNumber.foreground': v('--syntax-gutter'),
      'editorLineNumber.activeForeground': v('--text-dim'),
      'editorCursor.foreground': v('--accent'),
      'editorWidget.background': v('--bg-float'),
      'editorWidget.border': v('--border-strong'),
      'editorSuggestWidget.background': v('--bg-float'),
      'editorSuggestWidget.border': v('--border-strong'),
      'editorSuggestWidget.selectedBackground': v('--accent-soft'),
      'editorError.foreground': v('--danger'),
    },
  });

  // Completion: the standard libraries' definitions (from the app), plus keywords and UI elements.
  let library: Promise<Array<{ name: string; signature: string }>> | null = null;
  monaco.languages.registerCompletionItemProvider(FAUST_LANGUAGE, {
    triggerCharacters: ['.'],
    provideCompletionItems: async (model, position) => {
      library ??= bridge.isConnected() ? bridge.call('faust.completions').catch(() => []) : Promise.resolve([]);
      const entries = await library;
      const word = model.getWordUntilPosition(position);
      const line = model.getLineContent(position.lineNumber).slice(0, position.column - 1);
      const prefix = line.match(/([a-zA-Z_]\w*)\.\w*$/)?.[1];
      const range = new monaco.Range(position.lineNumber, word.startColumn, position.lineNumber, word.endColumn);
      const Kind = monaco.languages.CompletionItemKind;

      // After "fi." only that environment's definitions, without the prefix.
      if (prefix) {
        return {
          suggestions: entries
            .filter((e) => e.name.startsWith(`${prefix}.`))
            .map((e) => ({ label: e.name.slice(prefix.length + 1), detail: e.signature, kind: Kind.Function, insertText: e.name.slice(prefix.length + 1), range })),
        };
      }

      return {
        suggestions: [
          ...KEYWORDS.map((k) => ({ label: k, kind: Kind.Keyword, insertText: k, range })),
          ...UI_ELEMENTS.map((k) => ({ label: k, kind: Kind.Class, insertText: k, range })),
          ...PRIMITIVES.map((k) => ({ label: k, kind: Kind.Function, insertText: k, range })),
          ...entries.map((e) => ({ label: e.name, detail: e.signature, kind: Kind.Function, insertText: e.name, range })),
        ],
      };
    },
  });
}

export { monaco };
