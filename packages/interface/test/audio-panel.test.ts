// SPDX-License-Identifier: GPL-3.0-or-later

import { functions } from '@collabgod/bridge';
import { describe, expect, it } from 'vitest';
import { type AudioBackend, type AudioChoice, type AudioDevices, AudioPanel, type AudioStatus } from '../src/audio';

/** Just enough of an element for the panel to draw into, with no DOM. */
class FakeElement {
  textContent = '';
  className = '';
  hidden = false;
  value = '';
  selected = false;
  children: FakeElement[] = [];
  readonly ownerDocument = { createElement: () => new FakeElement() };
  private listeners: Record<string, (() => void)[]> = {};

  append(...children: FakeElement[]): void {
    this.children.push(...children);
  }
  replaceChildren(...children: FakeElement[]): void {
    this.children = children;
  }
  addEventListener(type: string, fn: () => void): void {
    (this.listeners[type] ??= []).push(fn);
  }
  dispatch(type: string): void {
    for (const fn of this.listeners[type] ?? []) fn();
  }
  /** Every element below this one, depth first. */
  all(): FakeElement[] {
    return this.children.flatMap((c) => [c, ...c.all()]);
  }
}

const example = functions.audioDevices.example.result;
const status = (over: Partial<AudioStatus> = {}): AudioStatus => ({ ...example.status, ...over });
const tick = () => new Promise((resolve) => setTimeout(resolve, 0));

describe('the audio panel', () => {
  it('does not flash a refusal it got past while picking a driver type', async () => {
    const usb = 'Focusrite USB ASIO';
    const openNow = status({ state: 'open', type: 'Windows Audio', device: 'Focusrite USB (Focusrite USB Audio)', message: '' });
    const devices: AudioDevices = {
      types: [
        { name: 'Windows Audio', devices: ['Focusrite USB (Focusrite USB Audio)'] },
        { name: 'ASIO', devices: ['Focusrite Thunderbolt ASIO', usb] },
      ],
      status: openNow,
    };

    const messages: string[] = [];
    const root = new FakeElement();
    let current = openNow;
    let panel: AudioPanel;

    const backend: AudioBackend = {
      list: async () => ({ ...devices, status: current }),
      choose: async (c: AudioChoice) => {
        if (c.device !== usb) {
          // The app refuses, keeps what was open with the refusal as its message (as
          // audioDevices then reports it), and announces it while the panel is still choosing.
          current = { ...current, message: `Cannot open ${c.device}` };
          panel.statusChanged(current);
          await tick();
          return current;
        }
        current = status({ state: 'open', type: 'ASIO', device: usb, chosen: c, message: '' });
        panel.statusChanged(current);
        return current;
      },
      report: () => {},
    };

    panel = new AudioPanel(root as unknown as HTMLElement, backend);
    root.hidden = true;
    await panel.toggle();

    // Record the message line every time the panel draws.
    const replace = root.replaceChildren.bind(root);
    root.replaceChildren = (...children: FakeElement[]) => {
      replace(...children);
      const line = root.all().find((e) => e.className === 'message');
      if (line !== undefined && !line.hidden) messages.push(line.textContent);
    };

    const driver = root.all().find((e) => e.children.some((o) => o.value === 'ASIO'));
    expect(driver).toBeDefined();
    driver!.value = 'ASIO';
    driver!.dispatch('change');
    for (let i = 0; i < 10; i++) await tick();

    expect(current.device).toBe(usb);
    expect(messages).toEqual([]);
  });
});
