import { FilePlus2, FolderOpen, History, LayoutTemplate, TriangleAlert, X } from 'lucide-react';
import { useMemo, useState } from 'react';
import type { RecentProject } from '../bridge/protocol';
import { projectActions, useDawg } from '../store/store';
import { BrandMark } from './BrandMark';
import { Button, IconButton } from '../ui/controls';

// The start page shown while no project is open (docs/design/phase5-project-workflow.md, section 8):
// New / Open / templates on the left, recent projects on the right, a recovery banner after a crash.

export function relativeTime(ms: number, now = Date.now()) {
  const minutes = Math.round((now - ms) / 60000);
  if (minutes < 1) return 'just now';
  if (minutes < 60) return `${minutes} min ago`;
  const hours = Math.round(minutes / 60);
  if (hours < 24) return `${hours} h ago`;
  const days = Math.round(hours / 24);
  if (days === 1) return 'yesterday';
  if (days < 7) return `${days} days ago`;
  // In English, as the rest of the UI is, whatever the system's language (docs/design/ux-review.md, U3).
  return new Date(ms).toLocaleDateString('en-GB', {
    day: 'numeric',
    month: 'short',
    year: 'numeric',
  });
}

export function formatSummary(p: Pick<RecentProject, 'tempo' | 'tracks' | 'lengthSeconds'>) {
  const seconds = Math.round(p.lengthSeconds);
  const length = `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
  return `${Math.round(p.tempo * 100) / 100} BPM · ${p.tracks} track${p.tracks === 1 ? '' : 's'} · ${length}`;
}

export function WelcomeScreen() {
  const recent = useDawg((s) => s.recent);
  const templates = useDawg((s) => s.templates);
  const recovery = useDawg((s) => s.projectState.recovery);
  const [filter, setFilter] = useState('');

  const visible = useMemo(() => {
    const f = filter.trim().toLowerCase();
    return recent.filter((p) => !f || `${p.name} ${p.folder}`.toLowerCase().includes(f));
  }, [recent, filter]);

  return (
    <main className="welcome">
      {recovery && (
        <div className="recovery-banner">
          <TriangleAlert size={16} />
          <span>
            DAWg didn’t close properly. An autosave of <strong>{recovery.name || 'Untitled'}</strong> from{' '}
            {relativeTime(recovery.time)} has changes that were never saved.
          </span>
          <span className="spacer" />
          <Button className="primary" onClick={() => projectActions.recover(true)}>
            Recover
          </Button>
          <Button onClick={() => projectActions.recover(false)}>
            Discard
          </Button>
        </div>
      )}

      <section className="welcome-start">
        <h1>
          <BrandMark size={56} />
          <span>
            DAW<span>g</span>
          </span>
        </h1>
        <button className="welcome-action" onClick={() => projectActions.newProject()}>
          <FilePlus2 size={18} /> New project <kbd>Ctrl+N</kbd>
        </button>
        <button className="welcome-action" onClick={() => projectActions.open()}>
          <FolderOpen size={18} /> Open… <kbd>Ctrl+O</kbd>
        </button>

        <h2>Templates</h2>
        <div className="template-cards">
          {templates.map((t) => (
            <button
              key={t.id}
              className="template-card"
              title={`New project from “${t.name}”`}
              onClick={() => projectActions.newProject(t.id)}
            >
              <LayoutTemplate size={18} />
              <span>{t.name}</span>
            </button>
          ))}
        </div>
      </section>

      <section className="welcome-recent">
        <div className="welcome-recent-header">
          <h2>
            <History size={13} /> Recent
          </h2>
          <input className="search" placeholder="Search" value={filter} onChange={(e) => setFilter(e.target.value)} />
        </div>
        <div className="recent-list">
          {visible.map((p) => (
            <div
              key={p.path}
              className={`recent-row ${p.missing ? 'missing' : ''}`}
              title={p.missing ? `Missing: ${p.path}` : p.path}
              onClick={() => !p.missing && projectActions.open(p.path)}
            >
              <span className="recent-name">{p.name}</span>
              <span className="recent-date">{p.missing ? 'missing' : relativeTime(p.opened)}</span>
              <span className="recent-folder">{p.folder}</span>
              <span className="recent-summary">{formatSummary(p)}</span>
              {p.missing && (
                <IconButton
                  className="recent-remove"
                  title="Remove from list"
                  onClick={(e) => {
                    e.stopPropagation();
                    projectActions.removeRecent(p.path);
                  }}
                >
                  <X size={12} />
                </IconButton>
              )}
            </div>
          ))}
          {recent.length === 0 && <p className="empty">No recent projects yet.</p>}
          {recent.length > 0 && visible.length === 0 && <p className="empty">Nothing matches.</p>}
        </div>
      </section>
    </main>
  );
}
