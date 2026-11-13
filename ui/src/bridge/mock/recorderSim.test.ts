// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import type { RecorderBridgeState } from '../protocol';
import { demoTab, isRecorderDemo, RecorderSim } from './recorderSim';

function sim() {
  const events: RecorderBridgeState[] = [];
  const s = new RecorderSim((state) => events.push(state));
  return { s, events };
}

describe('RecorderSim', () => {
  it('records while time passes, then saves the take into the list', () => {
    const { s, events } = sim();
    expect(s.list()).toHaveLength(3);
    s.toggle();
    expect(s.state().recorder.status).toBe('starting');
    s.step(0.1);
    expect(s.state().recorder.status).toBe('recording');
    s.step(2);
    expect(s.telemetry(-10, -12).recorder.seconds).toBeCloseTo(2);
    expect(s.telemetry(-10, -12).recorder.diDb).toBe(-10);
    s.toggle();
    expect(s.state().recorder.status).toBe('saving');
    s.step(0.1);
    const take = s.state().recorder.take!;
    expect(s.state().recorder.status).toBe('idle');
    expect(take.ended).toBe('stopped');
    expect(take.seconds).toBeCloseTo(2);
    expect(s.list()[0]).toMatchObject({ name: take.name, path: take.diPath, kind: 'take' });
    expect(events.length).toBeGreaterThan(3);
  });

  it('loads (converting other rates), plays, loops or ends', () => {
    const { s } = sim();
    s.loadPath(s.list()[2]!.path); // the 44.1 kHz file
    expect(s.state().reamp.status).toBe('converting');
    s.step(0.6);
    expect(s.state().reamp.status).toBe('ready');
    expect(s.state().reamp.file?.converted).toBe(true);
    s.setLoop(false);
    s.play(true);
    s.step(127);
    expect(s.state().reamp.playing).toBe(true);
    s.step(2);
    expect(s.state().reamp.playing).toBe(false);
  });

  it('takes a drop in pieces and refuses what it cannot play', () => {
    const { s } = sim();
    expect(s.importBegin('notes.txt', 10)).toBe(0);
    expect(s.state().reamp.error).toMatch(/WAV, AIFF or FLAC/);
    const token = s.importBegin('take.wav', 4);
    expect(token).toBeGreaterThan(0);
    expect(s.importChunk(token + 1, 'AAAA')).toBe(false);
    expect(s.importChunk(token, 'AAAA')).toBe(true);
    expect(s.importEnd(token)).toBe(true);
    expect(s.list()[0]).toMatchObject({ name: 'take', kind: 'imported' });
  });

  it('knows its demos', () => {
    expect(isRecorderDemo('recording')).toBe(true);
    expect(isRecorderDemo('nope')).toBe(false);
    expect(demoTab('converting')).toBe('reamp');
    expect(demoTab('dropouts')).toBe('record');
  });
});
