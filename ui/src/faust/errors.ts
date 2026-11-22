// Faust compiler messages for the code view (docs/design/phase11-faust-panels.md, F7).

/** A compiler error with its line, parsed from libfaust's messages ("name : 12 : ERROR : message"). */
export interface FaustError {
  line: number;
  message: string;
}

export function parseErrors(text: string): FaustError[] {
  const errors: FaustError[] = [];
  for (const raw of text.split('\n')) {
    const line = raw.trim();
    if (!line) continue;
    const match = line.match(/:\s*(\d+)\s*:\s*ERROR\s*:\s*(.*)$/i);
    if (match) errors.push({ line: Number(match[1]), message: match[2] });
    else if (errors.length === 0) errors.push({ line: 1, message: line.replace(/^ERROR\s*:\s*/i, '') });
    else errors[errors.length - 1].message += ` ${line}`;
  }
  return errors;
}
