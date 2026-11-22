import { Check, type LucideIcon } from 'lucide-react';

// One item of a right-click menu: an icon (or a check mark) in a column of its own, the label, and its shortcut, so
// every context menu lines up the same way.

export function MenuItem({
  icon: Icon,
  label,
  shortcut,
  checked,
  danger,
  disabled,
  title,
  onClick,
}: {
  icon?: LucideIcon;
  label: string;
  shortcut?: string;
  /** A setting's item: a check mark in the icon's place while on. */
  checked?: boolean;
  danger?: boolean;
  disabled?: boolean;
  /** A tooltip, where the label alone doesn't say enough. */
  title?: string;
  onClick: (e: React.MouseEvent) => void;
}) {
  return (
    <button className={`menu-item with-shortcut ${danger ? 'danger' : ''}`} disabled={disabled} title={title} onClick={onClick}>
      <span className="menu-check-label">
        <span className="menu-icon">{checked ? <Check size={12} /> : Icon && <Icon size={13} />}</span>
        {label}
      </span>
      {shortcut && <kbd>{shortcut}</kbd>}
    </button>
  );
}
