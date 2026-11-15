import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { InventoryError, readInventory, validateInventory } from '../src/inventory.js';
import { parseModuleInfo } from '../src/moduleInfo.js';
import { bundleFor, cid, inventory } from './helpers.js';

// What `plugin_scan scan` wrote for the fake plugins on Linux. CI also reads a
// fresh scan on Windows and macOS with `npm run inventory -- summary`.
const scanned = JSON.parse(readFileSync(new URL('./fixtures/fake-plugins-linux.json', import.meta.url), 'utf8')) as unknown;

describe('reading the scanner output', () => {
  const r = readInventory(scanned);

  it('accepts the C++ scanner output', () => {
    expect(r.inventory.label).toBe('fixtures');
    expect(r.inventory.bundles).toHaveLength(9);
  });

  it('keeps only audio module classes from bundles that scanned', () => {
    expect(r.plugins.map((p) => p.name).sort()).toEqual([
      'CollabGod Nested',
      'CollabGod Old',
      'CollabGod Test Synth',
      'Unload Crasher',
    ]);
  });

  it('gives every failed bundle a reason', () => {
    const reasons = Object.fromEntries(r.failures.map((f) => [f.bundle, f.reason]));
    expect(Object.keys(reasons).sort()).toEqual([
      'crash-on-load.vst3',
      'empty.vst3',
      'hang-on-load.vst3',
      'no-export.vst3',
      'null-factory.vst3',
    ]);
    expect(reasons['crash-on-load.vst3']).toMatch(/^crashed while loading/);
    expect(reasons['hang-on-load.vst3']).toMatch(/time limit/);
    expect(reasons['no-export.vst3']).toMatch(/^no-factory-export/);
    expect(reasons['empty.vst3']).toMatch(/^no-binary/);
  });

  it('keeps a plugin that crashed only while unloading, and says so', () => {
    const p = r.plugins.find((x) => x.name === 'Unload Crasher');
    expect(p?.unloadProblem).toMatch(/^crashed after reading/);
  });

  it('falls back to the factory vendor when the class has none (factory version 1)', () => {
    const p = r.plugins.find((x) => x.name === 'CollabGod Old');
    expect(p?.vendor).toBe('CollabGod Tests');
    expect(p?.version).toBe('');
    expect(p?.infoLevel).toBe('v1');
  });

  it('parses moduleinfo.json, JSON5 and all', () => {
    const p = r.plugins.find((x) => x.name === 'CollabGod Test Synth');
    expect(p?.moduleInfo?.classes[0]?.cid).toBe(p?.cid);
    expect(p?.moduleInfo?.compatibility).toEqual([{ replacement: p?.cid, replaces: ['A'.repeat(32)] }]);
  });
});

describe('validation', () => {
  it('rejects another schema', () => {
    expect(() => validateInventory({ ...inventory('x', []), schema: 'something/2' })).toThrow(InventoryError);
  });

  it('rejects a class ID that is not 32 upper-case hex digits', () => {
    const b = bundleFor({ cid: cid(1), name: 'X' });
    b.probe!.classes[0]!.cid = 'abc';
    expect(() => validateInventory(inventory('x', [b]))).toThrow(/cid/);
  });

  it('names the field that is wrong', () => {
    const inv = inventory('x', [bundleFor({ cid: cid(1), name: 'X' })]) as unknown as { bundles: { ending: string }[] };
    inv.bundles[0]!.ending = 'exploded';
    expect(() => validateInventory(inv)).toThrow(/bundles\[0\]\.ending/);
  });

  it('notes moduleinfo.json that does not parse, and still uses the plugin', () => {
    const r = readInventory(inventory('x', [bundleFor({ cid: cid(1), name: 'X', moduleInfo: '{ not json' })]));
    expect(r.plugins).toHaveLength(1);
    expect(r.notes[0]).toMatch(/moduleinfo.json does not parse/);
  });
});

describe('moduleinfo.json', () => {
  it('upper-cases class IDs and ignores missing fields', () => {
    const parsed = parseModuleInfo('{ Classes: [ { CID: "abcdef0123456789abcdef0123456789" } ] }');
    expect(parsed.ok && parsed.info.classes[0]).toEqual({
      cid: 'ABCDEF0123456789ABCDEF0123456789',
      category: '',
      name: '',
      vendor: '',
      version: '',
    });
  });
});
