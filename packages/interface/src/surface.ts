// SPDX-License-Identifier: GPL-3.0-or-later
//
// The canvas the interface draws on, filling the window. It is resized and redrawn
// the moment its size changes, inside the ResizeObserver callback, not on the next
// animation frame, so a resize never shows a stale or stretched picture
// (webview-ui finding 11).
//
// For now it draws a placeholder: the name, a status line, and a line that moves with
// the app's clock, so frames can be seen arriving. The timeline replaces it (#31),
// drawn with WebGL2.

import { backgroundColour } from '@collabgod/bridge';

export interface Scene {
  status: string;
  /** The app's clock now, if connected. */
  hostTimeMs: number | undefined;
}

export class Surface {
  private readonly context: CanvasRenderingContext2D;
  private readonly observer: ResizeObserver;
  private width = 0;
  private height = 0;
  private dpr = 1;
  private scene: Scene = { status: '', hostTimeMs: undefined };

  constructor(private readonly canvas: HTMLCanvasElement) {
    const context = canvas.getContext('2d', { alpha: false });
    if (context === null) throw new Error('No 2D canvas context');
    this.context = context;

    this.observer = new ResizeObserver((entries) => {
      const entry = entries[entries.length - 1];
      if (entry === undefined) return;
      const box = entry.devicePixelContentBoxSize?.[0];
      this.resize(entry.contentRect.width, entry.contentRect.height, box?.inlineSize, box?.blockSize);
      this.draw(this.scene);
    });
    try {
      this.observer.observe(canvas, { box: 'device-pixel-content-box' });
    } catch {
      this.observer.observe(canvas); // WebKit before 16.4 has no device-pixel-content-box
    }
    this.resize(canvas.clientWidth, canvas.clientHeight);
  }

  private resize(cssWidth: number, cssHeight: number, deviceWidth?: number, deviceHeight?: number): void {
    this.dpr = window.devicePixelRatio || 1;
    this.width = cssWidth;
    this.height = cssHeight;
    const w = deviceWidth ?? Math.round(cssWidth * this.dpr);
    const h = deviceHeight ?? Math.round(cssHeight * this.dpr);
    if (this.canvas.width !== w) this.canvas.width = w;
    if (this.canvas.height !== h) this.canvas.height = h;
  }

  draw(scene: Scene): void {
    this.scene = scene;
    const { context: g, width, height, dpr } = this;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.fillStyle = backgroundColour;
    g.fillRect(0, 0, width, height);

    g.fillStyle = '#d8dde8';
    g.textAlign = 'center';
    g.textBaseline = 'middle';
    g.font = '600 28px system-ui, -apple-system, "Segoe UI", sans-serif';
    g.fillText('CollabGod', width / 2, height / 2 - 14);
    g.fillStyle = '#7d8597';
    g.font = '13px system-ui, -apple-system, "Segoe UI", sans-serif';
    g.fillText(scene.status, width / 2, height / 2 + 18);

    if (scene.hostTimeMs !== undefined) {
      // One sweep every two seconds of the app's clock.
      const x = ((scene.hostTimeMs % 2000) / 2000) * width;
      g.fillStyle = '#4f8cff';
      g.fillRect(x - 1, height - 3, 2, 3);
    }
  }
}
