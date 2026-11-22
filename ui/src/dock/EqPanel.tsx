import { RotateCcw, Trash2 } from 'lucide-react';
import { useLayoutEffect, useRef, useState } from 'react';
import { font, readTheme, type Theme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { formatParam, noteHint, parseParam } from '../graph/params';
import type { ParamDescriptor } from '../bridge/protocol';
import { useNodeAnalysis, useNodeDrag, useResponse } from './analysis';
import type { PanelProps } from './panels';
import { dbToY, drawCurve, drawFrequencyGrid, drawHandle, drawSpectrum, freqToX, xToFreq, yToDb } from './panelDraw';
import { MenuItem } from '../components/MenuItem';
import { IconButton, Power } from '../ui/controls';
import { Menu } from '../ui/overlays';

// The EQ's panel (docs/design/phase11-faust-panels.md, P4; channel-strip.md, C1, C5, C8 and C9): the summed curve
// over the spectrum, with a handle for each band in use (frequency on x, gain on y). Bands not in use are hidden. A
// double-click on the curve adds a band: below 0 dB under 100 Hz or over 5 kHz an HP or LP, above it a shelf, and a
// bell elsewhere. Clicking near the 0 dB line and dragging also adds one, as a bell in the middle and a shelf at
// those edges. The context menu adds any type. A double-click on a handle bypasses the band or switches it back on;
// Delete or the handle's menu removes it, and the menu sets its type, channels and slope. The wheel or Alt with a
// vertical drag sets Q; the wheel steps an HP's or LP's slope (6 to 48 dB/oct), also while its handle is held. The
// hovered or dragged band shows its own curve and its values. A band filters both channels, or only the mid or the
// side; a mid or side band's handle carries an M or S, and the panel then draws the mid's and the side's curves.

const RANGE = 24; // dB above and below 0
const WHEEL_STEP = 100; // wheel delta per slope step: one notch of a mouse wheel
const CLICK_SLOP = 5; // pixels a press may move and still count as a click, and the drag that adds a band
const DOUBLE_CLICK_MS = 500; // two clicks this close in time and place count as a double-click
const ZERO_DB_HIT = 10; // how near the 0 dB line a press starts a new band

/** A slope's index (0 to 7) as dB per octave. */
const slopeDb = (index: number) => (Math.round(index) + 1) * 6;

/** The band types, by the value of a band's type parameter (eqTypes in the engine). */
const TYPES = ['bell', 'lowShelf', 'highShelf', 'hp', 'lp', 'notch', 'tilt'] as const;
export type BandType = (typeof TYPES)[number];

/** The low and high ranges where a double-click makes a filter or a shelf instead of a bell. */
export function edgeAt(freq: number): 'low' | 'high' | null {
  return freq < 100 ? 'low' : freq > 5000 ? 'high' : null;
}

/** A band added by double-click: a filter below 0 dB and a shelf above it at the edges, a bell elsewhere. */
export function bandTypeForDoubleClick(freq: number, db: number): BandType {
  const edge = edgeAt(freq);
  if (!edge) return 'bell';
  if (db < 0) return edge === 'low' ? 'hp' : 'lp';
  return edge === 'low' ? 'lowShelf' : 'highShelf';
}

/** A band added by dragging near 0 dB: a bell in the middle and a shelf at the edges, whichever way it is dragged. */
export function bandTypeForDrag(freq: number): BandType {
  const edge = edgeAt(freq);
  return edge === 'low' ? 'lowShelf' : edge === 'high' ? 'highShelf' : 'bell';
}

const TYPE_NAMES: Record<BandType, string> = {
  bell: 'Bell',
  lowShelf: 'Low shelf',
  highShelf: 'High shelf',
  hp: 'High-pass',
  lp: 'Low-pass',
  notch: 'Notch',
  tilt: 'Tilt',
};

const hasGain = (type: BandType) => type === 'bell' || type === 'lowShelf' || type === 'highShelf' || type === 'tilt';
const hasSlope = (type: BandType) => type === 'hp' || type === 'lp';
const hasQ = (type: BandType) => !hasSlope(type);

/** A band's parameters, by the ids of its first type (`id` is their prefix), and its colour: a cut's, or the nth
    band's (tokens.css, --eq-cut and --eq-band-n). */
interface Band {
  id: string;
  tone: 'cut' | number;
}

const bandColour = (theme: Theme, band: Band) => (band.tone === 'cut' ? theme.eqCut : theme.eqBands[band.tone]);

// In the order of the engine's band curves (EqNode::bandSections).
const BANDS: Band[] = [
  { id: 'hp', tone: 'cut' },
  { id: 'low', tone: 0 },
  { id: 'mid1', tone: 1 },
  { id: 'mid2', tone: 2 },
  { id: 'mid3', tone: 3 },
  { id: 'mid4', tone: 4 },
  { id: 'high', tone: 5 },
  { id: 'lp', tone: 'cut' },
];

const param = (band: Band, name: 'On' | 'Used' | 'Type' | 'Mode' | 'Freq' | 'Gain' | 'Q' | 'Slope') => band.id + name;

/** What a band filters, by its mode's value. */
const MODES = ['Stereo', 'Mid', 'Side'];

type Menu = { x: number; y: number; band: number | null; freq: number; db: number };

/** A pointer press: where it started, whether it became a drag, and whether it may add a band. */
interface Press {
  startX: number;
  startY: number;
  moved: boolean;
  create: boolean;
}

export function EqPanel({ track, node, info }: PanelProps) {
  const canvas = useCanvas();
  const edit = useNodeDrag(track.id, node.id, node.params);
  const value = (id: string) => edit.values[id] ?? info.params.find((p) => p.id === id)?.default ?? 0;
  const params = Object.fromEntries(info.params.map((p) => [p.id, value(p.id)]));
  const curve = useResponse(node.type, params);
  const analysis = useNodeAnalysis(track.id, node.id);
  const spectrum = analysis?.spectrum ?? null;
  const sideSpectrum = analysis?.sideSpectrum ?? null;
  const [active, setActive] = useState<number | null>(null);
  const [hover, setHover] = useState<number | null>(null);
  const [dragging, setDragging] = useState(false);
  const [menu, setMenu] = useState<Menu | null>(null);
  const drag = useRef<{ band: number; startY: number; startQ: number; alt: boolean; type?: BandType } | null>(null);
  const press = useRef<Press | null>(null);
  const lastClick = useRef({ time: Number.NEGATIVE_INFINITY, x: 0, y: 0 });
  const wheel = useRef(0); // wheel delta not yet turned into slope steps
  const range = (id: string) => info.params.find((p) => p.id === id)!;
  const isUsed = (band: Band) => value(param(band, 'Used')) > 0.5;
  const isOn = (band: Band) => value(param(band, 'On')) > 0.5;
  const modeOf = (band: Band) => Math.round(value(param(band, 'Mode')));
  const typeOf = (band: Band): BandType => TYPES[Math.round(value(param(band, 'Type')))] ?? 'bell';
  const defaultType = (band: Band): BandType => TYPES[Math.round(range(param(band, 'Type')).default)] ?? 'bell';
  const nameOf = (i: number) => `${TYPE_NAMES[typeOf(BANDS[i])]} ${i + 1}`;
  const set = (values: Record<string, number>) => {
    edit.begin(Object.keys(values));
    edit.set(values);
    edit.end();
  };
  const clampTo = (id: string, v: number) => Math.min(range(id).max, Math.max(range(id).min, v));
  const roundDb = (db: number) => Math.round(db * 10) / 10;

  const handleAt = (band: Band) => ({
    x: freqToX(value(param(band, 'Freq')), canvas.size.width),
    y: dbToY(hasGain(typeOf(band)) ? value(param(band, 'Gain')) : 0, canvas.size.height, RANGE),
  });

  /** Moves the band held by the current drag to a pointer position: frequency and gain, or Q with Alt. */
  const dragTo = (i: number, x: number, y: number) => {
    const b = BANDS[i];
    const q = param(b, 'Q');
    const gain = param(b, 'Gain');
    const d = drag.current;
    if (d?.alt) edit.drag({ [q]: clampTo(q, d.startQ * Math.pow(2, (d.startY - y) / 60)) });
    else
      edit.drag({
        [param(b, 'Freq')]: clampTo(param(b, 'Freq'), xToFreq(x, canvas.size.width)),
        ...(hasGain(d?.type ?? typeOf(b)) ? { [gain]: clampTo(gain, roundDb(yToDb(y, canvas.size.height, RANGE))) } : {}),
      });
  };

  // The band whose curve and values show: the one dragged, else the one under the pointer.
  const shown = dragging ? active : hover;

  useLayoutEffect(() => {
    const ctx = canvas.context();
    if (!ctx || canvas.size.width < 16 || canvas.size.height < 16) return;
    const theme = readTheme();
    // Stereo bands keep the accent; mid and side bands and curves have colours of their own (C8).
    const modeColours = [theme.accent, theme.toneMid, theme.toneSide];
    drawFrequencyGrid(ctx, canvas.size, theme, RANGE, 6);
    if (curve?.side) {
      // In mid and side, the two spectra in their curves' colours, but quieter than the one spectrum, so the
      // curves stay in front: the side's line and fill fainter still, as it is usually the lower one.
      drawSpectrum(ctx, canvas.size, theme, spectrum, { colour: modeColours[1], line: 0.35, fill: 0.18 });
      drawSpectrum(ctx, canvas.size, theme, sideSpectrum, { colour: modeColours[2], line: 0.35, fill: 0.12 });
    } else drawSpectrum(ctx, canvas.size, theme, spectrum);
    if (shown !== null && curve?.bands?.[shown] && isUsed(BANDS[shown]) && isOn(BANDS[shown])) {
      const mode = modeOf(BANDS[shown]);
      ctx.globalAlpha = 0.6;
      drawCurve(ctx, canvas.size, curve.x, curve.bands[shown], RANGE, mode === 0 ? bandColour(theme, BANDS[shown]) : modeColours[mode], 1.5, true);
      ctx.globalAlpha = 1;
    }
    if (curve?.side) {
      // The side's curve, thinner, then the mid's in front over an outline in the background's colour, so the two
      // stay apart where they cross.
      drawCurve(ctx, canvas.size, curve.x, curve.side, RANGE, modeColours[2], 1.5);
      drawCurve(ctx, canvas.size, curve.x, curve.y, RANGE, theme.bg, 5);
      drawCurve(ctx, canvas.size, curve.x, curve.y, RANGE, modeColours[1], 2, true);
    } else if (curve) drawCurve(ctx, canvas.size, curve.x, curve.y, RANGE, theme.accent, 2, true);
    BANDS.forEach((band, i) => {
      if (!isUsed(band)) return;
      const h = handleAt(band);
      // A bypassed band's handle stays where it is, faded.
      ctx.globalAlpha = isOn(band) ? 1 : 0.4;
      drawHandle(ctx, theme, h.x, h.y, bandColour(theme, band), active === i || hover === i);
      const mode = modeOf(band);
      if (mode !== 0) {
        // A ring in the mid's or side's colour, and M or S above the handle (the tooltip opens to its right).
        ctx.beginPath();
        ctx.arc(h.x, h.y, 10, 0, Math.PI * 2);
        ctx.strokeStyle = modeColours[mode];
        ctx.lineWidth = 1.5;
        ctx.stroke();
        ctx.fillStyle = modeColours[mode];
        ctx.font = font(theme, 'xs', { bold: true });
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(mode === 1 ? 'M' : 'S', h.x, h.y - 17);
        ctx.textAlign = 'left';
      }
      ctx.globalAlpha = 1;
    });
  });

  const hit = (x: number, y: number, radius = 12) => {
    let best: number | null = null;
    let distance = radius;
    BANDS.forEach((band, i) => {
      if (!isUsed(band)) return;
      const h = handleAt(band);
      const d = Math.hypot(h.x - x, h.y - y);
      if (d < distance) {
        distance = d;
        best = i;
      }
    });
    return best;
  };

  const local = (e: React.MouseEvent | React.WheelEvent) => {
    const rect = e.currentTarget.getBoundingClientRect();
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
  };

  const anyFree = BANDS.some((b) => !isUsed(b));

  /** Puts a free band in use as a type at a frequency and gain, switched on, and selects it. A band whose first
      type it is comes first, so a project's bands keep their usual places. `asDrag` leaves the gesture open for the
      pointer that added it to keep moving the band. */
  const add = (type: BandType, freq: number, db: number, asDrag = false): number | null => {
    const free = BANDS.map((_, i) => i).filter((i) => !isUsed(BANDS[i]));
    const i = free.find((j) => defaultType(BANDS[j]) === type) ?? free[0];
    if (i === undefined) return null;
    const b = BANDS[i];
    const values = {
      [param(b, 'Used')]: 1,
      [param(b, 'On')]: 1,
      [param(b, 'Type')]: TYPES.indexOf(type),
      [param(b, 'Freq')]: clampTo(param(b, 'Freq'), freq),
      // A new band starts at Q 1, not the descriptors' 1/sqrt(2) (channel-strip.md, C1).
      [param(b, 'Q')]: 1,
      ...(hasGain(type) ? { [param(b, 'Gain')]: clampTo(param(b, 'Gain'), roundDb(db)) } : {}),
    };
    if (asDrag) {
      edit.begin(Object.keys(values));
      edit.set(values);
    } else set(values);
    setActive(i);
    return i;
  };

  /** A double-click on the curve: in the lows (under 100 Hz) and highs (over 5 kHz) an HP or LP below 0 dB and a
      shelf above it, elsewhere a bell. */
  const addAt = (freq: number, db: number) => add(bandTypeForDoubleClick(freq, db), freq, db);

  /** Bypasses a band, or switches it back on. */
  const toggle = (i: number) => set({ [param(BANDS[i], 'On')]: isOn(BANDS[i]) ? 0 : 1 });

  /** Takes a band out of use; it comes back switched on when added again. */
  const remove = (i: number) => {
    set({ [param(BANDS[i], 'Used')]: 0, [param(BANDS[i], 'On')]: 1 });
    if (active === i) setActive(null);
    if (hover === i) setHover(null);
  };

  const endDrag = () => {
    if (drag.current) edit.end();
    drag.current = null;
    setDragging(false);
  };

  /** Ends a press: a simple click records a double-click, and a double-click toggles a handle or adds a band. */
  const endPress = () => {
    const p = press.current;
    press.current = null;
    endDrag();
    if (!p) return;
    if (p.moved) {
      lastClick.current = { time: Number.NEGATIVE_INFINITY, x: 0, y: 0 };
      return;
    }
    const now = performance.now();
    const previous = lastClick.current;
    const isDouble =
      now - previous.time < DOUBLE_CLICK_MS &&
      Math.abs(p.startX - previous.x) < CLICK_SLOP &&
      Math.abs(p.startY - previous.y) < CLICK_SLOP;
    lastClick.current = isDouble
      ? { time: Number.NEGATIVE_INFINITY, x: p.startX, y: p.startY }
      : { time: now, x: p.startX, y: p.startY };
    if (!isDouble) return;
    // Only a double-click on the handle itself bypasses it; a little beside it still adds a band.
    const handle = hit(p.startX, p.startY, 7);
    if (handle !== null) toggle(handle);
    else addAt(xToFreq(p.startX, canvas.size.width), yToDb(p.startY, canvas.size.height, RANGE));
  };

  const cancelPress = () => {
    press.current = null;
    lastClick.current = { time: Number.NEGATIVE_INFINITY, x: 0, y: 0 };
    endDrag();
  };

  const tooltip = shown !== null && isUsed(BANDS[shown]) ? shown : null;
  const tip = tooltip !== null ? handleAt(BANDS[tooltip]) : null;
  const anyUsed = BANDS.some(isUsed);

  return (
    <div className="device-panel eq-panel">
      <div className="eq-graph">
        <canvas
          ref={canvas.ref}
          data-takes-wheel
          className="panel-canvas"
          title={anyUsed ? undefined : 'Double-click or drag near 0 dB to add a band, right-click for more'}
          tabIndex={0}
          onPointerDown={(e) => {
            if (e.button !== 0) return;
            const { x, y } = local(e);
            const i = hit(x, y);
            setActive(i);
            const p: Press = { startX: x, startY: y, moved: false, create: false };
            press.current = p;
            if (i !== null) {
              e.currentTarget.setPointerCapture(e.pointerId);
              const b = BANDS[i];
              const type = typeOf(b);
              const alt = e.altKey && hasQ(type);
              drag.current = { band: i, startY: y, startQ: value(param(b, 'Q')), alt };
              edit.begin(alt ? [param(b, 'Q')] : [param(b, 'Freq'), ...(hasGain(type) ? [param(b, 'Gain')] : [])]);
              setDragging(true);
              return;
            }
            if (anyFree && Math.abs(y - canvas.size.height / 2) <= ZERO_DB_HIT) {
              p.create = true;
              e.currentTarget.setPointerCapture(e.pointerId);
            }
          }}
          onPointerMove={(e) => {
            const { x, y } = local(e);
            const p = press.current;
            const d = drag.current;
            if (d) {
              if (p && Math.hypot(x - p.startX, y - p.startY) > CLICK_SLOP) p.moved = true;
              dragTo(d.band, x, y);
              return;
            }
            if (p?.create) {
              if (Math.hypot(x - p.startX, y - p.startY) <= CLICK_SLOP) return;
              p.moved = true;
              p.create = false;
              const freq = xToFreq(p.startX, canvas.size.width);
              const type = bandTypeForDrag(freq);
              const i = add(type, freq, 0, true);
              if (i === null) return;
              drag.current = { band: i, startY: p.startY, startQ: value(param(BANDS[i], 'Q')), alt: false, type };
              setDragging(true);
              dragTo(i, x, y);
              return;
            }
            setHover(hit(x, y));
          }}
          onPointerLeave={() => {
            if (!drag.current && !press.current?.create) setHover(null);
          }}
          onPointerUp={endPress}
          onPointerCancel={cancelPress}
          onContextMenu={(e) => {
            e.preventDefault();
            const { x, y } = local(e);
            const i = hit(x, y);
            if (i !== null) setActive(i);
            setMenu({
              x: e.clientX,
              y: e.clientY,
              band: i,
              freq: xToFreq(x, canvas.size.width),
              db: yToDb(y, canvas.size.height, RANGE),
            });
          }}
          onKeyDown={(e) => {
            if ((e.key === 'Delete' || e.key === 'Backspace') && active !== null && isUsed(BANDS[active])) {
              e.preventDefault();
              e.stopPropagation();
              remove(active);
            }
          }}
          onWheel={(e) => {
            const { x, y } = local(e);
            const i = hit(x, y) ?? active;
            if (i === null || !isUsed(BANDS[i])) return;
            const b = BANDS[i];
            const q = param(b, 'Q');
            const slopeId = param(b, 'Slope');
            setActive(i);
            // While a handle is held the change is part of its drag; otherwise a step of its own.
            const apply = (values: Record<string, number>) => (drag.current ? edit.drag(values) : set(values));
            if (hasQ(typeOf(b))) apply({ [q]: clampTo(q, value(q) * Math.pow(2, -e.deltaY / 500)) });
            else {
              // Up for steeper, down for gentler: a step per notch, however finely a touchpad scrolls.
              wheel.current -= e.deltaY;
              const steps = Math.trunc(wheel.current / WHEEL_STEP);
              if (steps === 0) return;
              wheel.current -= steps * WHEEL_STEP;
              const slope = clampTo(slopeId, Math.round(value(slopeId)) + steps);
              if (slope !== value(slopeId)) apply({ [slopeId]: slope });
            }
          }}
        />
        {tooltip !== null && tip && (
          <BandTooltip
            name={nameOf(tooltip)}
            colour={bandColour(readTheme(), BANDS[tooltip])}
            mode={modeOf(BANDS[tooltip])}
            on={isOn(BANDS[tooltip])}
            values={[
              formatParam(range(param(BANDS[tooltip], 'Freq')), value(param(BANDS[tooltip], 'Freq'))),
              ...(hasGain(typeOf(BANDS[tooltip]))
                ? [formatParam(range(param(BANDS[tooltip], 'Gain')), value(param(BANDS[tooltip], 'Gain')))]
                : []),
              ...(hasQ(typeOf(BANDS[tooltip]))
                ? [`Q ${formatParam(range(param(BANDS[tooltip], 'Q')), value(param(BANDS[tooltip], 'Q')))}`]
                : [`${slopeDb(value(param(BANDS[tooltip], 'Slope')))} dB/oct`]),
            ]}
            x={tip.x}
            y={tip.y}
            width={canvas.size.width}
            height={canvas.size.height}
          />
        )}
        {curve?.side && (
          <div className="eq-legend">
            <span className="eq-legend-mid">Mid</span>
            <span className="eq-legend-side">Side</span>
          </div>
        )}
      </div>
      {menu && (
        <>
          <Menu onClose={() => setMenu(null)} at={{ x: Math.min(menu.x, window.innerWidth - 220), y: Math.min(menu.y, window.innerHeight - 260) }} className="eq-menu">
            {menu.band !== null ? (
              <BandMenu
                name={nameOf(menu.band)}
                type={typeOf(BANDS[menu.band])}
                slope={Math.round(value(param(BANDS[menu.band], 'Slope')))}
                on={isOn(BANDS[menu.band])}
                mode={modeOf(BANDS[menu.band])}
                onToggle={() => toggle(menu.band!)}
                onType={(type) => set({ [param(BANDS[menu.band!], 'Type')]: TYPES.indexOf(type) })}
                onMode={(mode) => set({ [param(BANDS[menu.band!], 'Mode')]: mode })}
                onResetGain={() => set({ [param(BANDS[menu.band!], 'Gain')]: 0 })}
                onSlope={(slope) => set({ [param(BANDS[menu.band!], 'Slope')]: slope })}
                fields={(['Freq', 'Gain', 'Q'] as const)
                  .filter((name) =>
                    name === 'Gain' ? hasGain(typeOf(BANDS[menu.band!])) : name === 'Q' ? hasQ(typeOf(BANDS[menu.band!])) : true,
                  )
                  .map((name) => {
                    const id = param(BANDS[menu.band!], name);
                    return { label: name === 'Freq' ? 'Frequency' : name, param: range(id), value: value(id) };
                  })}
                onValue={(id, v) => set({ [id]: v })}
                onRemove={() => remove(menu.band!)}
                close={() => setMenu(null)}
              />
            ) : (
              <>
                <div className="menu-heading">Add band</div>
                {TYPES.map((type) => (
                  <button
                    key={type}
                    className="menu-item"
                    disabled={!anyFree}
                    onClick={() => {
                      setMenu(null);
                      add(type, menu.freq, menu.db);
                    }}
                  >
                    {TYPE_NAMES[type]}
                  </button>
                ))}
                {/* How cuts are made (docs/design/parameter-linking.md, section 10): matched to the analogue cut, or the
                    exact reciprocal of the boost, so a linked de-emphasis cancels. */}
                <div className="menu-separator" />
                <div className="menu-heading">Cuts</div>
                <MenuItem
                  checked={Math.round(value('cuts')) === 0}
                  label="Matched to analogue"
                  onClick={() => (setMenu(null), set({ cuts: 0 }))}
                />
                <MenuItem
                  checked={Math.round(value('cuts')) === 1}
                  label="Mirror boosts (cancel exactly)"
                  onClick={() => (setMenu(null), set({ cuts: 1 }))}
                />
              </>
            )}
          </Menu>
        </>
      )}
    </div>
  );
}

function BandTooltip({
  name,
  colour,
  mode,
  on,
  values,
  x,
  y,
  width,
  height,
}: {
  name: string;
  colour: string;
  mode: number;
  on: boolean;
  values: string[];
  x: number;
  y: number;
  width: number;
  height: number;
}) {
  const flip = x > width - 150;

  return (
    <div
      className="eq-tooltip"
      style={{
        '--band': colour,
        left: flip ? undefined : x + 14,
        right: flip ? width - x + 14 : undefined,
        top: Math.min(Math.max(y - 10, 4), height - 44),
      } as React.CSSProperties}
    >
      <span className="eq-tooltip-title">
        {name}
        {mode !== 0 && ` · ${MODES[mode]}`}
        {!on && ' · bypassed'}
      </span>
      <span>{values.join(' · ')}</span>
    </div>
  );
}

/** A labelled dropdown in the band menu. */
function MenuSelect<T extends string | number>({
  label,
  options,
  chosen,
  onChoose,
}: {
  label: string;
  options: Array<{ value: T; label: string }>;
  chosen: T;
  onChoose: (value: T) => void;
}) {
  return (
    <label className="eq-menu-field">
      <span>{label}</span>
      <select
        value={options.findIndex((option) => option.value === chosen)}
        onChange={(e) => onChoose(options[Number(e.target.value)].value)}
      >
        {options.map((option, index) => (
          <option key={String(option.value)} value={index}>
            {option.label}
          </option>
        ))}
      </select>
    </label>
  );
}

/** A typed value in the band menu: Enter or leaving the field applies it, Escape restores the current value. Text
    that isn't a number is ignored. */
function MenuNumber({
  label,
  param,
  value,
  onValue,
}: {
  label: string;
  param: ParamDescriptor;
  value: number;
  onValue: (value: number) => void;
}) {
  const shown = formatParam(param, value);
  const [text, setText] = useState(shown);
  const editing = useRef(false);
  // Follow changes from elsewhere (a drag, automation) while the field isn't being typed in.
  useLayoutEffect(() => {
    if (!editing.current) setText(shown);
  }, [shown]);

  const apply = () => {
    editing.current = false;
    const parsed = parseParam(param, text);
    if (parsed !== null && parsed !== value) onValue(parsed);
    else setText(shown);
  };

  return (
    <label className="eq-menu-field">
      <span>{label}</span>
      <input
        type="text"
        value={text}
        spellCheck={false}
        // A band's frequency takes a note name; the nearest note of what is typed shows here (DS17).
        title={noteHint(param, parseParam(param, text) ?? value)}
        onFocus={(e) => {
          editing.current = true;
          e.currentTarget.select();
        }}
        onChange={(e) => setText(e.target.value)}
        onBlur={apply}
        onKeyDown={(e) => {
          // Keys stay in the field: Delete mustn't remove the band or the device.
          e.stopPropagation();
          if (e.key === 'Enter') {
            apply();
            e.currentTarget.select();
            editing.current = true;
          } else if (e.key === 'Escape') {
            editing.current = false;
            setText(shown);
            e.currentTarget.blur();
          }
        }}
      />
    </label>
  );
}

function BandMenu({
  name,
  type,
  slope,
  on,
  mode,
  onToggle,
  onType,
  onMode,
  onResetGain,
  onSlope,
  fields,
  onValue,
  onRemove,
  close,
}: {
  name: string;
  type: BandType;
  slope: number;
  on: boolean;
  mode: number;
  onToggle: () => void;
  onType: (type: BandType) => void;
  onMode: (mode: number) => void;
  onResetGain: () => void;
  onSlope: (slope: number) => void;
  fields: Array<{ label: string; param: ParamDescriptor; value: number }>;
  onValue: (id: string, value: number) => void;
  onRemove: () => void;
  close: () => void;
}) {
  const run = (action: () => void) => () => {
    close();
    action();
  };

  // The dropdowns leave the menu open, to try their choices in turn; the buttons in the header close it.
  return (
    <>
      <div className="eq-menu-header">
        <span className="menu-heading">{name}</span>
        <Power name={name} on={on} onChange={onToggle} />
        {hasGain(type) && (
          <IconButton title="Reset gain to 0 dB" onClick={run(onResetGain)}>
            <RotateCcw size={13} />
          </IconButton>
        )}
        <IconButton className="eq-menu-remove" title="Remove band" onClick={run(onRemove)}>
          <Trash2 size={13} />
        </IconButton>
      </div>
      <MenuSelect
        label="Type"
        options={TYPES.map((t) => ({ value: t, label: TYPE_NAMES[t] }))}
        chosen={type}
        onChoose={onType}
      />
      <MenuSelect
        label="Channels"
        options={MODES.map((label, index) => ({ value: index, label }))}
        chosen={mode}
        onChoose={onMode}
      />
      {fields.map((field) => (
        <MenuNumber key={field.param.id} {...field} onValue={(v) => onValue(field.param.id, v)} />
      ))}
      {hasSlope(type) && (
        <MenuSelect
          label="Slope"
          options={[0, 1, 2, 3, 4, 5, 6, 7].map((index) => ({ value: index, label: `${slopeDb(index)} dB/oct` }))}
          chosen={slope}
          onChoose={onSlope}
        />
      )}
    </>
  );
}
