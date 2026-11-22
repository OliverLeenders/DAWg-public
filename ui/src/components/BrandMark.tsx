// The DAWg mark (branding/dawg-mark.svg): a dog face whose eyes are two tape reels joined by the tape, with
// eighth-note flags for ears. The ears and smile take the text colour, the reels the accent.

export function BrandMark({ size = 28 }: { size?: number }) {
  return (
    <svg
      className="brand-mark"
      width={size}
      height={size}
      viewBox="-2 0 28 28"
      fill="none"
      strokeWidth={2}
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
    >
      <g stroke="currentColor">
        <path d="M3 6.5V2.5a5 5 0 0 1 6 4.4" />
        <path d="M15 6.5V2.5a5 5 0 0 1 6 4.4" />
        <path d="M15 22.5a3 3 0 0 1-6 0" />
      </g>
      <g className="brand-reels">
        <circle cx="6" cy="14" r="4" />
        <circle cx="18" cy="14" r="4" />
        <path d="M6 18H18" />
        <circle cx="6" cy="14" r="1" className="brand-hub" />
        <circle cx="18" cy="14" r="1" className="brand-hub" />
      </g>
    </svg>
  );
}
