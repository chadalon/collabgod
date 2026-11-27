// SPDX-License-Identifier: GPL-3.0-or-later

import { functions } from '@collabgod/bridge';
import { describe, expect, it } from 'vitest';
import {
  type AudioChoice,
  type AudioDevices,
  type AudioStatus,
  audioForm,
  changeChoice,
  chooseType,
  currentChoice,
  formatAudio,
  typeCandidates,
} from '../src/audio';

const example = functions.audioDevices.example.result;
const status = (over: Partial<AudioStatus> = {}): AudioStatus => ({ ...example.status, ...over });
const devices = (s: AudioStatus = status()): AudioDevices => ({ ...example, status: s });

describe('audioForm', () => {
  it('selects what is open, and offers every type and that type’s devices', () => {
    const form = audioForm(devices());
    expect(form.types.map((t) => t.value)).toEqual(['Windows Audio', 'Windows Audio (Exclusive Mode)', 'ASIO']);
    expect(form.type).toBe('Windows Audio');
    expect(form.devices.map((d) => d.value)).toEqual(['Focusrite USB (Focusrite USB Audio)', 'Speakers (Realtek(R) Audio)']);
    expect(form.device).toBe('Focusrite USB (Focusrite USB Audio)');
  });

  it("offers the device's own rate and size first, and shows block sizes in ms", () => {
    const form = audioForm(devices());
    expect(form.sampleRates[0]).toEqual({ value: 0, label: "Device's own" });
    expect(form.sampleRates.map((r) => r.value)).toEqual([0, 44100, 48000, 88200, 96000]);
    expect(form.bufferSizes).toEqual([
      { value: 0, label: "Device's own" },
      { value: 441, label: '441 samples (10.0 ms)' },
    ]);
    expect(form.sampleRate).toBe(0);
    expect(form.bufferSize).toBe(0);
  });

  it('keeps an unplugged chosen device in the list, marked, while the fallback is selected', () => {
    const s = status({
      state: 'fallback',
      device: 'Speakers (Realtek(R) Audio)',
      chosen: { type: 'Windows Audio', device: 'Focusrite USB', sampleRate: 0, bufferSize: 0, audioThreads: 0 },
    });
    const form = audioForm({ types: [{ name: 'Windows Audio', devices: ['Speakers (Realtek(R) Audio)'] }], status: s });
    expect(form.device).toBe('Speakers (Realtek(R) Audio)');
    expect(form.devices).toContainEqual({ value: 'Focusrite USB', label: 'Focusrite USB (unplugged)' });
  });

  it('offers one audio thread per CPU, after the default', () => {
    const form = audioForm(devices(status({ cpus: 4 })));
    expect(form.audioThreads.map((t) => t.value)).toEqual([0, 1, 2, 3, 4]);
    expect(form.threads).toBe(0);
  });

  it('marks a remembered type this machine does not have', () => {
    const s = status({ state: 'none', type: '', device: '', chosen: { type: 'ASIO', device: 'X', sampleRate: 0, bufferSize: 0, audioThreads: 0 } });
    const form = audioForm({ types: [{ name: 'CoreAudio', devices: ['MacBook Pro Speakers'] }], status: s });
    expect(form.types).toContainEqual({ value: 'ASIO', label: 'ASIO (not on this machine)' });
  });
});

describe('changeChoice', () => {
  const base = { type: 'Windows Audio', device: 'Focusrite', sampleRate: 48000, bufferSize: 256, audioThreads: 2 };

  it('a new type or device starts at its own rate and size, keeping the threads', () => {
    expect(changeChoice(base, 'type', 'ASIO')).toEqual({ type: 'ASIO', device: '', sampleRate: 0, bufferSize: 0, audioThreads: 2 });
    expect(changeChoice(base, 'device', 'Speakers')).toEqual({ ...base, device: 'Speakers', sampleRate: 0, bufferSize: 0 });
  });

  it('numbers arrive from a select as text', () => {
    expect(changeChoice(base, 'sampleRate', '44100').sampleRate).toBe(44100);
    expect(changeChoice(base, 'bufferSize', '512').bufferSize).toBe(512);
    expect(changeChoice(base, 'audioThreads', '1').audioThreads).toBe(1);
  });

  it('starts from what is open', () => {
    const s = status({ state: 'fallback', device: 'Speakers', chosen: { ...base } });
    expect(currentChoice(s)).toEqual({ ...base, device: 'Speakers' });
  });
});

describe('choosing a driver type', () => {
  const focusrite = 'Speakers (2- Focusrite USB Audio)';
  const machine: AudioDevices['types'] = [
    { name: 'Windows Audio', devices: [focusrite, 'Realtek Digital Output'] },
    { name: 'Windows Audio (Exclusive Mode)', devices: ['Realtek Digital Output', focusrite] },
    // As on Chad's desktop: ASIO's default is a driver for an interface that is not there.
    { name: 'ASIO', devices: ['Focusrite Thunderbolt ASIO', 'Focusrite USB ASIO', 'Realtek ASIO'] },
    { name: 'Empty', devices: [] },
  ];
  const openNow = status({ state: 'open', type: 'Windows Audio', device: focusrite, message: '' });
  const onMachine: AudioDevices = { types: machine, status: openNow };
  const current = currentChoice(openNow);

  /**
   * A fake app: refuses a device in `broken` and keeps what was open; takes a device in `busy`
   * but falls back to another, as when another program holds it; opens the rest.
   */
  const app = (broken: string[] = [], busy: string[] = []) => {
    const asked: AudioChoice[] = [];
    let open = openNow;
    const choose = async (c: AudioChoice): Promise<AudioStatus> => {
      asked.push(c);
      if (broken.includes(c.device)) return { ...open, message: `Cannot open ${c.device}` };
      open = busy.includes(c.device)
        ? status({ state: 'fallback', type: c.type, device: 'Realtek Digital Output', chosen: c, message: `${c.device} is in use` })
        : status({ state: 'open', type: c.type, device: c.device, chosen: c, message: '' });
      return open;
    };
    return { asked, choose };
  };

  it('names a device, so the default is never remembered as "whatever is default"', () => {
    for (const c of typeCandidates(onMachine, current, 'ASIO')) expect(c.device).not.toBe('');
  });

  it('keeps the open device when the new type has it, then tries the rest in the driver’s order', () => {
    expect(typeCandidates(onMachine, current, 'Windows Audio (Exclusive Mode)').map((c) => c.device)).toEqual([focusrite, 'Realtek Digital Output']);
    expect(typeCandidates(onMachine, current, 'ASIO').map((c) => c.device)).toEqual(['Focusrite Thunderbolt ASIO', 'Focusrite USB ASIO', 'Realtek ASIO']);
  });

  it('a type with no devices listed is still asked for, and the app decides', () => {
    expect(typeCandidates(onMachine, current, 'Empty')).toEqual([{ ...current, type: 'Empty', device: '', sampleRate: 0, bufferSize: 0 }]);
  });

  it('opens the first device that works when the default will not open', async () => {
    const { asked, choose } = app(['Focusrite Thunderbolt ASIO']);
    const s = await chooseType(onMachine, current, 'ASIO', choose);
    expect(s).toMatchObject({ state: 'open', type: 'ASIO', device: 'Focusrite USB ASIO' });
    expect(asked.map((c) => c.device)).toEqual(['Focusrite Thunderbolt ASIO', 'Focusrite USB ASIO']);
  });

  it('stops at the first device that opens', async () => {
    const { asked, choose } = app();
    const s = await chooseType(onMachine, current, 'Windows Audio (Exclusive Mode)', choose);
    expect(s).toMatchObject({ type: 'Windows Audio (Exclusive Mode)', device: focusrite });
    expect(asked).toHaveLength(1);
  });

  it('keeps a device the app took but could not keep open, rather than picking another', async () => {
    const { asked, choose } = app([], [focusrite]);
    const s = await chooseType(onMachine, current, 'Windows Audio (Exclusive Mode)', choose);
    expect(asked).toHaveLength(1);
    expect(s).toMatchObject({ state: 'fallback', chosen: { type: 'Windows Audio (Exclusive Mode)', device: focusrite } });
  });

  it('when nothing opens, ends with the refusal and what was open before', async () => {
    const { asked, choose } = app(['Focusrite Thunderbolt ASIO', 'Focusrite USB ASIO', 'Realtek ASIO']);
    const s = await chooseType(onMachine, current, 'ASIO', choose);
    expect(asked).toHaveLength(3);
    expect(s).toMatchObject({ type: 'Windows Audio', device: focusrite, message: 'Cannot open Realtek ASIO' });
  });
});

describe('formatAudio', () => {
  it('says what is open, how late callbacks are counted, and that CoreAudio counts no dropouts', () => {
    const line = formatAudio(status({ deviceDropouts: -1 }), 3);
    expect(line).toContain('Focusrite USB (Focusrite USB Audio) (Windows Audio), 44100 Hz, 441 samples (10.0 ms)');
    expect(line).toContain('1 of 16 CPUs for audio, settled');
    expect(line).toContain('late callbacks 3 (timed by the app)');
    expect(line).toContain('device dropouts not counted by this device');
  });

  it('shows why the output is not as chosen', () => {
    expect(formatAudio(status({ state: 'fallback', message: 'X is not available' }), 0)).toContain('\n  X is not available');
    expect(formatAudio(status({ state: 'none', message: 'No audio output is open' }), 0)).toContain('Audio: no output open. No audio output is open');
    expect(formatAudio(undefined, undefined)).toBe('Audio: not known yet');
  });
});
