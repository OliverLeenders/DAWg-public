import { VolumeX } from 'lucide-react';
import * as bridge from '../bridge/bridge';
import { useDawg } from '../store/store';
import { Button } from '../ui/controls';

// Output protection (docs/design/output-protection.md, S3): the device output was muted because it ran away. It stays
// muted until the user unmutes it; playback carries on underneath.

const REASONS = {
  overload: 'The output stayed above full scale.',
  extreme: 'The output jumped far beyond full scale.',
} as const;

export function OutputMuteBanner() {
  const reason = useDawg((s) => s.telemetry.outputMute ?? null);
  if (!reason) return null;

  return (
    <div className="output-mute-banner" role="alert">
      <VolumeX size={16} />
      <span>
        <strong>Output muted to protect your ears and speakers.</strong> {REASONS[reason]} Turn down or remove whatever
        caused it, then unmute.
      </span>
      <Button onClick={() => bridge.call('audio.unmute').catch(() => {})}>
        Unmute
      </Button>
    </div>
  );
}
