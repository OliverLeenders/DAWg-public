import { describe, expect, it } from 'vitest';
import { checkDue, compareVersions, DAY_MS, failureText, formatVersion, parseReleases, parseVersion, pickUpdate } from './updates';

// Update checks (docs/design/update-checks.md).

const v = (text: string) => parseVersion(text)!;

describe('versions', () => {
  it('reads tags with or without a v, a pre-release part and build metadata', () => {
    expect(v('v1.0.1')).toEqual({ major: 1, minor: 0, patch: 1, pre: [] });
    expect(v('1.1')).toEqual({ major: 1, minor: 1, patch: 0, pre: [] });
    expect(v('1.1.0-beta.2+build.7')).toEqual({ major: 1, minor: 1, patch: 0, pre: ['beta', 2] });
    expect(parseVersion('nightly')).toBeNull();
    expect(parseVersion('v1')).toBeNull();
    expect(formatVersion(v('v2.0.0-rc.1'))).toBe('2.0.0-rc.1');
  });

  it('compares as semantic versions', () => {
    const order = ['0.9.9', '1.0.0-alpha', '1.0.0-alpha.1', '1.0.0-alpha.beta', '1.0.0-beta', '1.0.0-beta.2', '1.0.0-beta.11', '1.0.0-rc.1', '1.0.0', '1.0.1', '1.10.0'];
    for (let i = 1; i < order.length; i++) {
      expect(compareVersions(v(order[i - 1]), v(order[i]))).toBeLessThan(0);
      expect(compareVersions(v(order[i]), v(order[i - 1]))).toBeGreaterThan(0);
    }
    expect(compareVersions(v('v1.2.3'), v('1.2.3'))).toBe(0);
  });
});

const github = JSON.stringify([
  { tag_name: 'v1.2.0-beta.1', name: '', body: 'Beta', html_url: 'https://github.com/OliverLeenders/DAWg-public/releases/tag/v1.2.0-beta.1', prerelease: true },
  { tag_name: 'v1.1.0', name: 'DAWg 1.1', body: 'Notes', html_url: 'https://github.com/OliverLeenders/DAWg-public/releases/tag/v1.1.0', prerelease: false },
  { tag_name: 'v1.3.0', name: 'Draft', html_url: 'https://github.com/x', draft: true },
  { tag_name: 'test-build', html_url: 'https://github.com/x' },
  { tag_name: 'v1.0.1', name: 'DAWg 1.0.1', body: '', html_url: 'https://github.com/OliverLeenders/DAWg-public/releases/tag/v1.0.1' },
]);

describe('which release is offered', () => {
  const releases = parseReleases(github);

  it('leaves out drafts and tags that are not versions', () => {
    expect(releases.map((r) => r.tag)).toEqual(['v1.2.0-beta.1', 'v1.1.0', 'v1.0.1']);
    expect(releases[0].name).toBe('DAWg 1.2.0-beta.1');
    expect(parseReleases('not json')).toEqual([]);
    expect(parseReleases('{"message":"Not Found"}')).toEqual([]);
  });

  it('offers the newest release, betas only when asked for', () => {
    expect(pickUpdate(releases, '1.0.0', { beta: false })?.tag).toBe('v1.1.0');
    expect(pickUpdate(releases, '1.0.0', { beta: true })?.tag).toBe('v1.2.0-beta.1');
    expect(pickUpdate(releases, '1.1.0', { beta: false })).toBeNull();
    // A beta user without the switch is offered the release only.
    expect(pickUpdate(releases, '1.1.0-beta.3', { beta: false })?.tag).toBe('v1.1.0');
  });

  it('skips a skipped version, but not a newer one', () => {
    expect(pickUpdate(releases, '1.0.0', { beta: false, skipped: '1.1.0' })).toBeNull();
    expect(pickUpdate(releases, '1.0.0', { beta: false, skipped: '1.0.1' })?.tag).toBe('v1.1.0');
  });

  it('checks at most once a day', () => {
    expect(checkDue(null, 1000)).toBe(true);
    expect(checkDue(1000, 1000 + DAY_MS - 1)).toBe(false);
    expect(checkDue(1000, 1000 + DAY_MS)).toBe(true);
    expect(checkDue(5000, 1000)).toBe(true);
  });

  it('says why a check failed', () => {
    expect(failureText(0, 'GitHub could not be reached')).toBe('GitHub could not be reached');
    expect(failureText(403, '')).toMatch(/limiting/);
    expect(failureText(500, '')).toBe('GitHub answered with status 500');
  });
});
