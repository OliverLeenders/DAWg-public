// Which release to offer (docs/design/update-checks.md, UC1, UC2 and UC6): versions from GitHub release tags,
// compared as semantic versions.

export interface Version {
  major: number;
  minor: number;
  patch: number;
  /** The pre-release fields (`beta.2` is ['beta', 2]); empty for a release. */
  pre: Array<string | number>;
}

export interface Release {
  version: Version;
  tag: string;
  name: string;
  notes: string;
  url: string;
  prerelease: boolean;
}

const VERSION = /^v?(\d+)\.(\d+)(?:\.(\d+))?(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$/;

/** A version from a tag such as `v1.0.1` or `1.1.0-beta.2`; null for anything else. */
export function parseVersion(text: string): Version | null {
  const match = VERSION.exec(text.trim());
  if (!match) return null;
  const pre = match[4] ? match[4].split('.').map((field) => (/^\d+$/.test(field) ? Number(field) : field)) : [];
  return { major: Number(match[1]), minor: Number(match[2]), patch: Number(match[3] ?? 0), pre };
}

/** Negative when a is older than b, positive when newer, 0 when the same. */
export function compareVersions(a: Version, b: Version): number {
  const core = a.major - b.major || a.minor - b.minor || a.patch - b.patch;
  if (core !== 0) return core;
  // A pre-release comes before its release.
  if (a.pre.length === 0 || b.pre.length === 0) return b.pre.length - a.pre.length;
  for (let i = 0; i < Math.min(a.pre.length, b.pre.length); i++) {
    const [x, y] = [a.pre[i], b.pre[i]];
    if (x === y) continue;
    // Numbers come before words; numbers compare numerically, words by their characters.
    if (typeof x === 'number' && typeof y === 'number') return x - y;
    if (typeof x === 'number') return -1;
    if (typeof y === 'number') return 1;
    return x < y ? -1 : 1;
  }
  return a.pre.length - b.pre.length;
}

export const formatVersion = (v: Version) => `${v.major}.${v.minor}.${v.patch}${v.pre.length > 0 ? `-${v.pre.join('.')}` : ''}`;

interface GitHubRelease {
  tag_name?: unknown;
  name?: unknown;
  body?: unknown;
  html_url?: unknown;
  prerelease?: unknown;
  draft?: unknown;
}

/** The releases in GitHub's answer, drafts and tags that aren't versions left out. */
export function parseReleases(body: string): Release[] {
  let list: unknown;
  try {
    list = JSON.parse(body);
  } catch {
    return [];
  }
  if (!Array.isArray(list)) return [];
  return (list as GitHubRelease[]).flatMap((r) => {
    const tag = typeof r.tag_name === 'string' ? r.tag_name : '';
    const version = parseVersion(tag);
    if (!version || r.draft === true || typeof r.html_url !== 'string') return [];
    return [
      {
        version,
        tag,
        name: typeof r.name === 'string' && r.name.trim() !== '' ? r.name.trim() : `DAWg ${formatVersion(version)}`,
        notes: typeof r.body === 'string' ? r.body : '',
        url: r.html_url,
        prerelease: r.prerelease === true || version.pre.length > 0,
      },
    ];
  });
}

/** The newest release newer than `current`, beta versions only when asked for, and nothing at or below a skipped one. */
export function pickUpdate(releases: Release[], current: string, options: { beta: boolean; skipped?: string | null }): Release | null {
  const running = parseVersion(current);
  if (!running) return null;
  const skipped = options.skipped ? parseVersion(options.skipped) : null;
  const offered = releases.filter(
    (r) =>
      (options.beta || !r.prerelease) &&
      compareVersions(r.version, running) > 0 &&
      (!skipped || compareVersions(r.version, skipped) > 0),
  );
  return offered.reduce<Release | null>((best, r) => (!best || compareVersions(r.version, best.version) > 0 ? r : best), null);
}

export const DAY_MS = 24 * 60 * 60 * 1000;

/** Whether the automatic check is due: at most once a day (UC3). A clock set back counts as due. */
export const checkDue = (lastCheck: number | null, now: number) => lastCheck === null || now - lastCheck >= DAY_MS || now < lastCheck;

/** What a failed fetch says (UC5). */
export function failureText(status: number, error: string): string {
  if (error) return error;
  if (status === 403 || status === 429) return 'GitHub is limiting requests for now; try again in an hour';
  if (status === 404) return 'No releases were found';
  return `GitHub answered with status ${status}`;
}
