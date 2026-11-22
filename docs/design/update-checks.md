# Update checks

Status: accepted (owner, 2026-11-09), built.

DAWg says when a newer release exists; it never downloads or installs anything itself (docs/PLAN.md, the owner's 1.0
list). The owner took every recommendation below.

## Decisions

**UC1. Releases come from GitHub.** The app asks GitHub's public API for the releases of `OliverLeenders/DAWg`
(`https://api.github.com/repos/OliverLeenders/DAWg/releases`); there is no server of DAWg's own. A release's tag is
its version, with or without a leading `v`: `v1.0.1`, `1.1.0-beta.2`. Drafts are ignored.

**UC2. Versions compare as semantic versions.** Major, minor and patch numerically; a version with a pre-release
part (`-beta.2`) comes before the same version without one, and pre-release parts compare by their dot-separated
fields, numbers numerically. A tag that isn't a version is ignored.

**UC3. When it checks.** At startup, in the background and at most once a day, unless the automatic check is off;
and whenever the user picks Help › Check for updates…, which always works. The automatic check waits a few seconds
after startup so it never competes with opening a project.

**UC4. Privacy.** The request carries nothing but what any web request does, plus a `User-Agent` of `DAWg/<version>`
(GitHub refuses requests without one): no identifier, no statistics. On first start a notice says that DAWg checks
GitHub for new releases, with **Check for updates** and **Don't check**; nothing is fetched until it is answered.
Help has the same switch (Check automatically).

**UC5. How it tells.** A quiet notice at the bottom of the window, "DAWg 1.0.1 is available", that never takes the
focus, interrupts playback or recording, or opens a dialog by itself. Clicking it shows the release's name and notes
with **Download** (opens the release page in the browser), **Skip this version** (no notice for it again; a newer
one still shows) and **Later** (the notice goes until the next check). A manual check says so also when DAWg is up
to date, or when the check failed and why.

**UC6. Pre-releases** are left out unless "Include beta versions" is on (Help). A user running a beta is offered the
next beta, or the release it leads to, only with that switch on; the release itself is always offered.

**UC7. Settings.** The user's settings (not the project) keep: the automatic check on or off, beta versions, the
skipped version, the last check's time and whether the first-start notice was answered (`ui.updates.*`).

## Building it

- The app fetches (`updates.fetch`, on a background thread; the answer arrives as `dawg_updates` with the HTTP status
  and the body) and opens a release page (`app.openUrl`, only for pages of the DAWg repository on GitHub). Everything
  else is in the UI, where it is tested: `ui/src/updates/updates.ts` parses and compares versions and picks the
  release to offer; `UpdateNotice.tsx` shows the notice, the first-start notice and the release dialog.
- On Linux, JUCE fetches through libcurl, loaded when first used (`JUCE_LOAD_CURL_SYMBOLS_LAZILY`), so DAWg still
  starts on a system without it, and the check then fails quietly.
- Checked in the app (a separate dev instance, 2026-11-09): the first-start notice, Help's three items, and a real
  check against GitHub. The repository is private for now, so GitHub's public API answers 404 and the manual check
  says "No releases were found"; checks find releases once the repository (and its releases) are public. The
  notice and dialog for a release on offer were not seen in the app, only in the tests of the choice behind them.
