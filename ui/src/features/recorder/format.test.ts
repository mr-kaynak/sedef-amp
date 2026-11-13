// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { channels, clock, percent, rate, spoken } from './format';
import { takeAnnouncement } from './model';

describe('recorder words', () => {
  it('writes times as a clock, with tenths while recording', () => {
    expect(clock(0)).toBe('0:00');
    expect(clock(42.97)).toBe('0:42');
    expect(clock(83.4)).toBe('1:23');
    expect(clock(3729)).toBe('1:02:09');
    expect(clock(42.37, true)).toBe('0:42.3');
    expect(clock(62.3, true)).toBe('1:02.3');
    expect(clock(5.04, true)).toBe('0:05.0');
    expect(clock(-1)).toBe('0:00');
  });

  it('says times in words', () => {
    expect(spoken(0)).toBe('0 seconds');
    expect(spoken(1)).toBe('1 second');
    expect(spoken(83)).toBe('1 minute 23 seconds');
    expect(spoken(120)).toBe('2 minutes');
    expect(spoken(3600 + 180)).toBe('1 hour 3 minutes');
  });

  it('names rates and channels', () => {
    expect(rate(44100)).toBe('44.1 kHz');
    expect(rate(48000)).toBe('48 kHz');
    expect(rate(88200)).toBe('88.2 kHz');
    expect(channels(1)).toBe('mono');
    expect(channels(2)).toBe('stereo');
    expect(percent(0.404)).toBe('40%');
  });

  it('announces a take starting and how it ended', () => {
    expect(takeAnnouncement(null, 'idle', null)).toBe('');
    expect(takeAnnouncement('starting', 'recording', null)).toBe('Recording');
    expect(takeAnnouncement('saving', 'idle', { seconds: 83.4, ended: 'stopped' })).toBe(
      'Take saved, 1 minute 23 seconds',
    );
    expect(
      takeAnnouncement('saving', 'idle', { seconds: 83.4, ended: 'stopped', dropouts: 2 }),
    ).toBe('Take saved, 1 minute 23 seconds, 2 dropouts');
    expect(takeAnnouncement('saving', 'idle', { seconds: 0, ended: 'empty' })).toBe(
      'Nothing was recorded',
    );
    expect(takeAnnouncement('recording', 'idle', { seconds: 4, ended: 'writeError' })).toBe(
      'The take stopped: the disk could not keep it',
    );
    expect(takeAnnouncement('recording', 'saving', null)).toBe('');
  });
});
