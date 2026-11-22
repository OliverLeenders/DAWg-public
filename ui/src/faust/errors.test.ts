import { describe, expect, it } from 'vitest';
import { parseErrors } from './errors';

describe('parseErrors', () => {
  it('reads the line of each libfaust error', () => {
    expect(parseErrors('faust : 2 : ERROR : undefined symbol : nothing\nfaust : 7 : ERROR : syntax error')).toEqual([
      { line: 2, message: 'undefined symbol : nothing' },
      { line: 7, message: 'syntax error' },
    ]);
  });

  it('puts messages without a line on the first line and joins continuation lines', () => {
    expect(parseErrors('ERROR : no process\n  defined')).toEqual([{ line: 1, message: 'no process defined' }]);
    expect(parseErrors('')).toEqual([]);
  });
});
