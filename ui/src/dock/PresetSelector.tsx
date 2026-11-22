import { Bookmark, Check, ChevronDown, Plus, Trash2, X } from 'lucide-react';
import { useEffect, useMemo, useRef, useState } from 'react';
import { NODE_TYPES, type GraphNode, type NodeTypeInfo, type Track } from '../bridge/protocol';
import { applyPreset, deleteUserPreset, detectMatchingPreset, getPresetsForNode, loadUserPresets, saveUserPreset, type NodePreset } from './presets';
import { Button, IconButton } from '../ui/controls';
import { padsOf } from './drumSynth';
import { Menu } from '../ui/overlays';

const MENU_WIDTH = 240; // as .preset-menu-popup
const MENU_HEIGHT = 380;
const MENU_MARGIN = 10;

interface PresetSelectorProps {
  track: Track;
  node: GraphNode;
  info: NodeTypeInfo;
  compact?: boolean;
}

export function PresetSelector({ track, node, info, compact = false }: PresetSelectorProps) {
  const [open, setOpen] = useState(false);
  const [saving, setSaving] = useState(false);
  const [nameInput, setNameInput] = useState('');
  const [userPresetsVersion, setUserPresetsVersion] = useState(0);
  const [lastAppliedPreset, setLastAppliedPreset] = useState<NodePreset | null>(null);
  const buttonRef = useRef<HTMLButtonElement>(null);
  const [menuPos, setMenuPos] = useState<{ x: number; y: number } | null>(null);

  // Listen for user preset additions / deletions
  useEffect(() => {
    loadUserPresets();
    const handler = (e: Event) => {
      const detail = (e as CustomEvent)?.detail;
      if (!detail?.type || detail.type === node.type) {
        setUserPresetsVersion((v) => v + 1);
      }
    };
    window.addEventListener('dawg_presets_changed', handler);
    return () => window.removeEventListener('dawg_presets_changed', handler);
  }, [node.type]);

  const { user, factory } = useMemo(() => getPresetsForNode(node.type), [node.type, userPresetsVersion]);
  const matched = detectMatchingPreset(node.type, node.params, info);
  const activePreset = matched?.preset ?? lastAppliedPreset;
  const isExactMatch = matched?.isExact ?? false;

  const currentLabel = activePreset
    ? isExactMatch
      ? activePreset.name
      : `${activePreset.name} *`
    : 'Presets';

  const toggleOpen = () => {
    if (!open && buttonRef.current) {
      const rect = buttonRef.current.getBoundingClientRect();
      const x = Math.max(MENU_MARGIN, Math.min(rect.left, window.innerWidth - MENU_WIDTH - MENU_MARGIN));
      const y = Math.max(MENU_MARGIN, Math.min(rect.bottom + 4, window.innerHeight - MENU_HEIGHT - MENU_MARGIN));
      setMenuPos({ x, y });
    }
    setOpen(!open);
    setSaving(false);
    setNameInput('');
  };

  const handleSelect = (preset: NodePreset) => {
    applyPreset(track, node, preset);
    setLastAppliedPreset(preset);
    setOpen(false);
  };

  const handleSave = (e: React.FormEvent) => {
    e.preventDefault();
    if (!nameInput.trim()) return;
    try {
      // Gather current parameter values
      const currentParams: Record<string, number> = {};
      for (const p of info.params) {
        currentParams[p.id] = node.params[p.id] ?? p.default;
      }
      const saved = saveUserPreset(
        node.type,
        nameInput.trim(),
        currentParams,
        node.type === NODE_TYPES.lfo || node.type === NODE_TYPES.waveshaper ? node.modulationShape ?? [] : undefined,
        node.type === NODE_TYPES.drumSynth ? padsOf(node) : undefined,
      );
      setLastAppliedPreset(saved);
      setSaving(false);
      setNameInput('');
      setOpen(false);
    } catch {
      // ignore
    }
  };

  const handleDelete = (e: React.MouseEvent, preset: NodePreset) => {
    e.stopPropagation();
    deleteUserPreset(node.type, preset.id);
    if (lastAppliedPreset?.id === preset.id) {
      setLastAppliedPreset(null);
    }
  };

  return (
    <div className={`preset-selector-container ${compact ? 'compact' : ''}`}>
      <Button
        ref={buttonRef}
        type="button"
        className={`preset-selector-button ${open ? 'active' : ''} ${compact ? 'small' : ''}`}
        title={`Presets for ${info.name}`}
        onClick={toggleOpen}
      >
        <Bookmark size={compact ? 11 : 12} className="preset-icon" />
        <span className="preset-label">{compact && currentLabel.length > 14 ? `${currentLabel.slice(0, 12)}…` : currentLabel}</span>
        <ChevronDown size={11} className="preset-chevron" />
      </Button>

      {open && menuPos && (
        <>
          <Menu
            onClose={() => setOpen(false)}
            at={{ x: menuPos.x, y: menuPos.y }}
            className="preset-menu-popup"
          >
            <div className="preset-menu-header">
              <span className="menu-heading">{info.name} presets</span>
              {!saving && (
                <button
                  type="button"
                  className="preset-add-button"
                  title="Save current settings as new preset"
                  onClick={() => setSaving(true)}
                >
                  <Plus size={11} /> Save new
                </button>
              )}
            </div>

            {saving && (
              <form className="preset-save-form" onSubmit={handleSave}>
                <input
                  type="text"
                  className="preset-name-input"
                  placeholder="Preset name…"
                  value={nameInput}
                  onChange={(e) => setNameInput(e.target.value)}
                  autoFocus
                />
                <div className="preset-save-actions">
                  <Button type="submit" className="preset-save-confirm" disabled={!nameInput.trim()}>
                    Save
                  </Button>
                  <IconButton
                    type="button"
                    title="Cancel"
                    onClick={() => {
                      setSaving(false);
                      setNameInput('');
                    }}
                  >
                    <X size={12} />
                  </IconButton>
                </div>
              </form>
            )}

            <div className="preset-menu-list">
              {user.length > 0 && (
                <>
                  <div className="menu-heading user-heading">User presets</div>
                  {user.map((preset) => (
                    <div key={preset.id} className="preset-item-row">
                      <button
                        type="button"
                        className={`menu-item preset-item ${activePreset?.id === preset.id ? 'active' : ''}`}
                        onClick={() => handleSelect(preset)}
                      >
                        <span className="preset-item-check">
                          {activePreset?.id === preset.id && isExactMatch && <Check size={11} />}
                        </span>
                        <span className="preset-item-name">{preset.name}</span>
                      </button>
                      <button
                        type="button"
                        className="preset-item-delete"
                        title={`Delete preset "${preset.name}"`}
                        onClick={(e) => handleDelete(e, preset)}
                      >
                        <Trash2 size={11} />
                      </button>
                    </div>
                  ))}
                  <div className="menu-separator" />
                </>
              )}

              <div className="menu-heading">Factory presets</div>
              {factory.map((preset) => (
                <button
                  key={preset.id}
                  type="button"
                  className={`menu-item preset-item ${activePreset?.id === preset.id ? 'active' : ''}`}
                  title={preset.description}
                  onClick={() => handleSelect(preset)}
                >
                  <span className="preset-item-check">
                    {activePreset?.id === preset.id && isExactMatch && <Check size={11} />}
                  </span>
                  <span className="preset-item-name">{preset.name}</span>
                  {preset.category && <span className="preset-item-category">{preset.category}</span>}
                </button>
              ))}
            </div>
          </Menu>
        </>
      )}
    </div>
  );
}
