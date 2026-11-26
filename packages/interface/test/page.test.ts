// SPDX-License-Identifier: GPL-3.0-or-later

import { backgroundColour } from '@collabgod/bridge';
import { describe, expect, it } from 'vitest';
import { cssRgb } from '../src/selfcheck';
import html from '../index.html?raw';

describe('index.html', () => {
  it("has the contract's background colour, which the web view and the window use", () => {
    const backgrounds = [...html.matchAll(/background:\s*(#[0-9a-fA-F]{6})/g)].map((m) => m[1]?.toLowerCase());
    expect(backgrounds).toEqual([backgroundColour]);
  });

  it('has the elements the shell draws into', () => {
    expect(html).toContain('<canvas id="surface">');
    expect(html).toContain('<pre id="diagnostics" hidden>');
    expect(html).toContain('<script type="module" src="app.js">');
  });
});

describe('cssRgb', () => {
  it('writes a colour the way getComputedStyle reports it', () => {
    expect(cssRgb('#0b0d12')).toBe('rgb(11, 13, 18)');
    expect(cssRgb('#ffffff')).toBe('rgb(255, 255, 255)');
  });
});
