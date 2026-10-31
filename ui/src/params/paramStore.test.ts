// SPDX-License-Identifier: AGPL-3.0-or-later
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { flush, setupMock, type Harness } from '../test/harness';
import { getValue } from './paramStore';
import { beginGesture, endGesture } from './sender';
import { setParam } from './useParam';

describe('parameter flow', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock();
  });
  afterEach(() => {
    vi.useRealTimers();
    h.dispose();
  });

  it('updates optimistically and sends coalesced values to the host', async () => {
    const call = vi.spyOn(h.mock, 'call');
    setParam('pearlGain', 0.6);
    setParam('pearlGain', 0.7);
    expect(getValue('pearlGain')).toBe(0.7);
    await flush();
    const sends = call.mock.calls.filter(([name]) => name === 'sedef_setParam');
    expect(sends).toEqual([['sedef_setParam', 'pearlGain', 0.7]]);
    expect(h.mock.value('pearlGain')).toBe(0.7);
  });

  it('applies host changes to idle parameters', async () => {
    h.mock.hostSet({ reefGain: 0.9 });
    await flush();
    expect(getValue('reefGain')).toBe(0.9);
  });

  it('ignores stale host echoes while the user holds a control', async () => {
    beginGesture('reefGain');
    setParam('reefGain', 0.8);
    h.mock.hostSet({ reefGain: 0.2 });
    await flush();
    expect(getValue('reefGain')).toBe(0.8);
    endGesture('reefGain');
  });

  it('applies a host value that arrived mid-gesture once the user lets go', async () => {
    vi.useFakeTimers({ toFake: ['performance', 'setTimeout', 'clearTimeout'] });
    beginGesture('reefGain');
    setParam('reefGain', 0.8);
    await flush();
    // The plugin sends each change once (a preset load, automation): it must not be lost.
    h.mock.hostSet({ reefGain: 0.3 });
    await flush();
    vi.advanceTimersByTime(300);
    expect(getValue('reefGain')).toBe(0.8);
    endGesture('reefGain');
    vi.advanceTimersByTime(200);
    expect(getValue('reefGain')).toBe(0.8);
    vi.advanceTimersByTime(100);
    expect(getValue('reefGain')).toBe(0.3);
  });

  it('keeps the user value when the user changed it after the host did', async () => {
    vi.useFakeTimers({ toFake: ['performance', 'setTimeout', 'clearTimeout'] });
    beginGesture('reefGain');
    setParam('reefGain', 0.8);
    await flush();
    h.mock.hostSet({ reefGain: 0.3 });
    setParam('reefGain', 0.6);
    await flush();
    endGesture('reefGain');
    vi.advanceTimersByTime(1000);
    expect(getValue('reefGain')).toBe(0.6);
    expect(h.mock.value('reefGain')).toBe(0.6);
  });

  it('drops a host value produced before the host applied a value still on its way', async () => {
    vi.useFakeTimers({ toFake: ['performance', 'setTimeout', 'clearTimeout'] });
    setParam('reefGain', 0.8);
    h.mock.hostSet({ reefGain: 0.3 });
    await flush();
    vi.advanceTimersByTime(1000);
    expect(getValue('reefGain')).toBe(0.8);
    expect(h.mock.value('reefGain')).toBe(0.8);
  });

  it('counts overlapping gestures and tells the host about the outermost only', () => {
    const call = vi.spyOn(h.mock, 'call');
    const gestureCalls = () =>
      call.mock.calls.map(([name]) => name).filter((name) => name.includes('Gesture'));
    beginGesture('reefGain');
    beginGesture('reefGain');
    endGesture('reefGain');
    expect(gestureCalls()).toEqual(['sedef_beginGesture']);
    endGesture('reefGain');
    endGesture('reefGain');
    expect(gestureCalls()).toEqual(['sedef_beginGesture', 'sedef_endGesture']);
  });

  it('accepts host changes again once the echo guard has passed', async () => {
    vi.useFakeTimers({ toFake: ['performance', 'Date'] });
    setParam('reefGain', 0.8);
    vi.advanceTimersByTime(400);
    h.mock.hostSet({ reefGain: 0.3 });
    await flush();
    expect(getValue('reefGain')).toBe(0.3);
  });
});
