// Builders for inventories in tests. Spike code (issue #14).

import { SCHEMA, type Bundle, type ClassInfo, type Inventory } from '../src/inventory.js';

export interface PluginSpec {
  cid: string;
  name: string;
  vendor?: string;
  version?: string;
  bundle?: string;
  binaryVersion?: Record<string, string> | null;
  moduleInfo?: string | null;
  /** How the probe process ended. Defaults to a clean exit. */
  ending?: Bundle['ending'];
}

export function cid(n: number): string {
  return n.toString(16).toUpperCase().padStart(32, '0');
}

export function audioClass(spec: PluginSpec): ClassInfo {
  return {
    cid: spec.cid,
    category: 'Audio Module Class',
    name: spec.name,
    vendor: spec.vendor ?? 'Vendor',
    version: spec.version ?? '1.0.0',
    sdkVersion: 'VST 3.7.9',
    subCategories: 'Fx',
    classFlags: 0,
    cardinality: 0x7fffffff,
    infoLevel: 'unicode',
  };
}

export function bundleFor(spec: PluginSpec): Bundle {
  const ending = spec.ending ?? 'exited';
  return {
    root: 0,
    path: spec.bundle ?? `${spec.name}.vst3`,
    layout: 'bundle',
    ending,
    elapsedMs: 12,
    probeWritten: true,
    ...(ending === 'exited' ? { exitCode: 0 } : {}),
    ...(ending === 'crashed' ? { crash: 'signal 11 (Segmentation fault)' } : {}),
    moduleInfo: spec.moduleInfo ?? null,
    probe: {
      result: 'ok',
      binary: 'Contents/x86_64-win/x.vst3',
      entryPoint: 'InitDll',
      loadMs: 10,
      factoryInterface: 3,
      binaryVersion: spec.binaryVersion ?? null,
      factory: { vendor: spec.vendor ?? 'Vendor', url: '', email: '', flags: 16 },
      classes: [audioClass(spec)],
    },
  };
}

export function failedBundle(path: string, ending: Bundle['ending'], moduleInfo: string | null = null): Bundle {
  return {
    root: 0,
    path,
    layout: 'bundle',
    ending,
    elapsedMs: 30000,
    probeWritten: false,
    ...(ending === 'crashed' ? { crash: 'exception 0xC0000005' } : {}),
    moduleInfo,
    probe: null,
  };
}

export function inventory(label: string, bundles: Bundle[], os = 'windows'): Inventory {
  return {
    schema: SCHEMA,
    scanner: '0.1.0',
    label,
    machine: { os, osVersion: '10.0.26100', arch: 'x86_64' },
    scannedAt: '2026-11-14T00:00:00Z',
    timeoutMs: 30000,
    roots: [{ path: 'C:/Program Files/Common Files/VST3', bundles: bundles.length }],
    problems: [],
    bundles,
    durationMs: 1234,
  };
}

export function moduleInfoText(opts: { version: string; classes: { cid: string; name: string; version: string }[]; replaces?: { replacement: string; old: string[] }[] }): string {
  // Written as JSON5, the way plugins ship it: trailing commas and a comment.
  const classes = opts.classes
    .map((c) => `{ "CID": "${c.cid}", "Category": "Audio Module Class", "Name": "${c.name}", "Vendor": "Vendor", "Version": "${c.version}", },`)
    .join('\n');
  const compat = (opts.replaces ?? []).map((r) => `{ "New": "${r.replacement}", "Old": [ ${r.old.map((o) => `"${o}", // an older edition\n`).join('')} ], },`).join('\n');
  return `{\n "Name": "x", "Version": "${opts.version}",\n "Factory Info": { "Vendor": "Vendor", },\n "Compatibility": [ ${compat} ],\n "Classes": [\n${classes}\n ],\n}`;
}
