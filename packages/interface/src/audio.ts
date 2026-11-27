// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audio settings panel (#27): driver type, output device, sample rate, block size and
// audio threads. Every change is sent to the app at once as a whole choice, which it opens
// and remembers; the app answers with what is open, and sends audioChanged when a device is
// unplugged or comes back. Plain DOM until the design system (#33) restyles it.

import type {
  AudioChoice as AudioChoiceSchema,
  AudioDevices as AudioDevicesSchema,
  AudioStatus as AudioStatusSchema,
  Infer,
} from '@collabgod/bridge';

export type AudioChoice = Infer<typeof AudioChoiceSchema>;
export type AudioDevices = Infer<typeof AudioDevicesSchema>;
export type AudioStatus = Infer<typeof AudioStatusSchema>;

export interface Option<V> {
  value: V;
  label: string;
}

/** What each control offers and which entry is selected. */
export interface AudioForm {
  types: Option<string>[];
  type: string;
  devices: Option<string>[];
  device: string;
  sampleRates: Option<number>[];
  sampleRate: number;
  bufferSizes: Option<number>[];
  bufferSize: number;
  audioThreads: Option<number>[];
  threads: number;
}

const blockMs = (samples: number, rate: number): string => (rate > 0 ? ` (${((1000 * samples) / rate).toFixed(1)} ms)` : '');

/**
 * The form for what the app listed. It shows what is open, so a fallback device is visible
 * as such; the type and device the person chose stay in the lists even while unplugged.
 */
export function audioForm(devices: AudioDevices): AudioForm {
  const { status } = devices;
  const chosen = status.chosen;
  const type = status.type !== '' ? status.type : chosen.type;
  const device = status.device !== '' ? status.device : chosen.device;

  const types: Option<string>[] = devices.types.map((t) => ({ value: t.name, label: t.name }));
  if (type !== '' && !types.some((t) => t.value === type)) types.push({ value: type, label: `${type} (not on this machine)` });

  const listed = devices.types.find((t) => t.name === type)?.devices ?? [];
  const deviceOptions: Option<string>[] = listed.map((d) => ({ value: d, label: d }));
  if (chosen.device !== '' && chosen.type === type && !listed.includes(chosen.device)) {
    deviceOptions.push({ value: chosen.device, label: `${chosen.device} (unplugged)` });
  }

  const rate = status.sampleRate;
  const sampleRates: Option<number>[] = [
    { value: 0, label: "Device's own" },
    ...status.sampleRates.map((r) => ({ value: r, label: `${r} Hz` })),
  ];
  const bufferSizes: Option<number>[] = [
    { value: 0, label: "Device's own" },
    ...status.bufferSizes.map((b) => ({ value: b, label: `${b} samples${blockMs(b, rate)}` })),
  ];

  const audioThreads: Option<number>[] = [{ value: 0, label: 'Default (1 until #113)' }];
  for (let n = 1; n <= Math.max(1, status.cpus); n++) audioThreads.push({ value: n, label: n === 1 ? '1 (no worker threads)' : String(n) });

  return {
    types,
    type,
    devices: deviceOptions,
    device,
    sampleRates,
    sampleRate: status.sampleRates.includes(chosen.sampleRate) ? chosen.sampleRate : 0,
    bufferSizes,
    bufferSize: status.bufferSizes.includes(chosen.bufferSize) ? chosen.bufferSize : 0,
    audioThreads,
    threads: chosen.audioThreads,
  };
}

/** The choice to send when one control changes. A new type or device starts at its own rate and size. */
export function changeChoice(current: AudioChoice, field: keyof AudioChoice, value: string | number): AudioChoice {
  switch (field) {
    case 'type':
      return { ...current, type: String(value), device: '', sampleRate: 0, bufferSize: 0 };
    case 'device':
      return { ...current, device: String(value), sampleRate: 0, bufferSize: 0 };
    case 'sampleRate':
      return { ...current, sampleRate: Number(value) };
    case 'bufferSize':
      return { ...current, bufferSize: Math.trunc(Number(value)) };
    case 'audioThreads':
      return { ...current, audioThreads: Math.trunc(Number(value)) };
  }
}

/** The choice as the form shows it: what is open, with the person's own rate, size and threads. */
export function currentChoice(status: AudioStatus): AudioChoice {
  return {
    type: status.type !== '' ? status.type : status.chosen.type,
    device: status.device !== '' ? status.device : status.chosen.device,
    sampleRate: status.chosen.sampleRate,
    bufferSize: status.chosen.bufferSize,
    audioThreads: status.chosen.audioThreads,
  };
}

/** One line for the diagnostics panel. `lateCallbacks` is the latest frame's, which is newer than the status's. */
export function formatAudio(status: AudioStatus | undefined, lateCallbacks: number | undefined): string {
  if (status === undefined) return 'Audio: not known yet';
  const late = lateCallbacks ?? status.lateCallbacks;
  const dropouts = status.deviceDropouts < 0 ? 'not counted by this device' : String(status.deviceDropouts);
  const counts = `late callbacks ${late} (timed by the app) · device dropouts ${dropouts}`;

  if (status.state === 'none') return `Audio: no output open. ${status.message} · ${counts}`;

  const line =
    `Audio: ${status.device} (${status.type}), ${status.sampleRate} Hz, ${status.bufferSize} samples` +
    `${blockMs(status.bufferSize, status.sampleRate)}, ${status.audioThreads} of ${status.cpus} CPUs for audio, ` +
    `${status.settled ? 'settled' : 'settling'} · ${counts}`;
  return status.message === '' ? line : `${line}\n  ${status.message}`;
}

/** What the panel needs from the app. */
export interface AudioBackend {
  list(): Promise<AudioDevices>;
  choose(choice: AudioChoice): Promise<AudioStatus>;
  report(text: string): void;
}

/** The panel itself. Shown and hidden by the shell; refreshes its lists whenever it is shown. */
export class AudioPanel {
  private devices: AudioDevices | undefined;
  private busy = false;

  constructor(
    private readonly root: HTMLElement,
    private readonly backend: AudioBackend,
  ) {}

  get hidden(): boolean {
    return this.root.hidden !== false;
  }

  async toggle(): Promise<void> {
    this.root.hidden = !this.hidden;
    if (!this.hidden) await this.refresh();
  }

  /** Lists the devices again and redraws. */
  async refresh(): Promise<void> {
    try {
      this.devices = await this.backend.list();
      this.render();
    } catch (e) {
      this.backend.report(`audioDevices: ${e instanceof Error ? e.message : String(e)}`);
    }
  }

  /** The app says the output changed. While open, the lists are scanned again, since a device came or went. */
  statusChanged(status: AudioStatus): void {
    if (this.devices === undefined) return;
    this.devices = { ...this.devices, status };
    if (!this.hidden) void this.refresh();
  }

  private async change(field: keyof AudioChoice, value: string): Promise<void> {
    if (this.devices === undefined || this.busy) return;
    this.busy = true;
    try {
      const choice = changeChoice(currentChoice(this.devices.status), field, value);
      const status = await this.backend.choose(choice);
      this.devices = { ...this.devices, status };
      await this.refresh();
    } catch (e) {
      this.backend.report(`chooseAudioOutput: ${e instanceof Error ? e.message : String(e)}`);
    } finally {
      this.busy = false;
    }
  }

  private render(): void {
    if (this.devices === undefined) return;
    const form = audioForm(this.devices);
    const { status } = this.devices;
    const doc = this.root.ownerDocument;

    const row = <V extends string | number>(label: string, field: keyof AudioChoice, options: Option<V>[], selected: V): HTMLElement => {
      const wrapper = doc.createElement('label');
      wrapper.textContent = label;
      const select = doc.createElement('select');
      for (const o of options) {
        const option = doc.createElement('option');
        option.value = String(o.value);
        option.textContent = o.label;
        option.selected = o.value === selected;
        select.append(option);
      }
      select.addEventListener('change', () => void this.change(field, select.value));
      wrapper.append(select);
      return wrapper;
    };

    const heading = doc.createElement('h2');
    heading.textContent = 'Audio output';

    const state = doc.createElement('p');
    state.className = `state ${status.state}`;
    state.textContent =
      status.state === 'none'
        ? 'No output is open.'
        : `${status.sampleRate} Hz, ${status.bufferSize} samples${blockMs(status.bufferSize, status.sampleRate)}, ` +
          `output latency ${status.outputLatencyMs.toFixed(1)} ms${status.state === 'fallback' ? ' (fallback)' : ''}`;

    const message = doc.createElement('p');
    message.className = 'message';
    message.textContent = status.message;
    message.hidden = status.message === '';

    const hint = doc.createElement('p');
    hint.className = 'hint';
    hint.textContent = 'Changes apply at once and are remembered. Ctrl+, (Cmd+, on a Mac) closes this.';

    this.root.replaceChildren(
      heading,
      row('Driver', 'type', form.types, form.type),
      row('Output', 'device', form.devices, form.device),
      row('Sample rate', 'sampleRate', form.sampleRates, form.sampleRate),
      row('Block size', 'bufferSize', form.bufferSizes, form.bufferSize),
      row('Audio threads', 'audioThreads', form.audioThreads, form.threads),
      state,
      message,
      hint,
    );
  }
}
