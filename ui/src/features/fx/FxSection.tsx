// SPDX-License-Identifier: AGPL-3.0-or-later
import { Knob } from '../../controls/Knob';
import { Segmented } from '../../controls/Segmented';
import { useParamValue } from '../../params/useParam';
import { telemetry, useLiveSelector } from '../../state/live';
import { formatMs } from '../../params/format';
import { tapTempo } from '../metronome/actions';
import { DecayGraph } from './DecayGraph';
import { FxPanel } from './FxPanel';
import { TakesSketch } from './TakesSketch';
import { TapeHeads } from './TapeHeads';
import styles from './FxSection.module.css';

function EffectiveTime() {
  const ms = useLiveSelector(telemetry, (t) => (t ? Math.round(t.driftTimeMs) : null));
  return (
    <div className={styles.readout} aria-live="off">
      <span className={styles.readoutLabel}>Repeats every</span>
      <span className={styles.readoutValue}>{ms === null ? '—' : formatMs(ms)}</span>
      <span className={styles.footnote}>Repeats ring out after you switch Drift off.</span>
    </div>
  );
}

function DriftTiming() {
  const sync = Math.round(useParamValue('driftSync'));
  const bpm = useParamValue('tempo');
  return (
    <div className={styles.timing}>
      <div className={styles.field}>
        <span className={styles.fieldLabel}>Sync</span>
        <Segmented param="driftSync" labels={['Free', 'Host', 'Tap']} />
      </div>
      {sync === 0 ? (
        <Knob param="driftTime" label="Time" size="lg" />
      ) : (
        <Knob param="driftNote" label="Note" size="lg" />
      )}
      {sync === 2 && (
        <div className={styles.tapBox}>
          <button type="button" className={styles.tap} onClick={() => void tapTempo()}>
            Tap
          </button>
          <span className={styles.bpm}>{bpm.toFixed(1).replace(/\.0$/, '')} bpm</span>
        </div>
      )}
      <EffectiveTime />
    </div>
  );
}

export function FxSection() {
  return (
    <div className={styles.section}>
      <FxPanel name="Drift" kind="Tape echo" enableParam="driftEnabled" className={styles.drift}>
        <div className={styles.driftTop}>
          <div className={styles.heads}>
            <TapeHeads />
            <div className={styles.field}>
              <span className={styles.fieldLabel}>Playback heads</span>
              <Segmented
                param="driftHeads"
                labels={['1', '2', '3', '4']}
                ariaLabel="Playback heads"
              />
            </div>
          </div>
          <DriftTiming />
        </div>
        <div className={styles.knobRow}>
          <Knob param="driftFeedback" label="Feedback" />
          <Knob param="driftTapeMod" label="Wow & flutter" />
          <Knob param="driftTapeDrive" label="Tape drive" />
          <span className={styles.divider} />
          <Knob param="driftBass" label="Bass" />
          <Knob param="driftTreble" label="Treble" />
          <span className={styles.divider} />
          <Knob param="driftWidth" label="Width" />
          <Knob param="driftMix" label="Mix" />
        </div>
      </FxPanel>

      <FxPanel
        name="Fathom"
        kind="Reverb"
        enableParam="fathomEnabled"
        extra={<Segmented param="fathomMode" size="sm" />}
      >
        <div className={styles.fathom}>
          <DecayGraph />
          <div className={styles.knobRow}>
            <Knob param="fathomMix" label="Mix" />
            <Knob param="fathomDecay" label="Decay" />
            <Knob param="fathomPreDelay" label="Pre-delay" />
            <Knob param="fathomLowCut" label="Low cut" />
            <Knob param="fathomHighCut" label="High cut" />
          </div>
        </div>
      </FxPanel>

      <FxPanel name="Doubler" kind="Double tracking" enableParam="doublerEnabled">
        <div className={styles.doubler}>
          <TakesSketch />
          <div className={styles.knobRow}>
            <Knob param="doublerSpread" label="Spread" />
            <Knob param="doublerVariation" label="Variation" />
            <span className={styles.divider} />
            <Knob param="doublerWidth" label="Width" />
            <Knob param="doublerMix" label="Mix" />
          </div>
        </div>
      </FxPanel>
    </div>
  );
}
