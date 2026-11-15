// moduleinfo.json: what a VST3 bundle says about itself without being loaded.
// Spike code (issue #14). The format is JSON5 (trailing commas, comments), as
// Steinberg's ipluginbase documentation shows.

import JSON5 from 'json5';

export interface ModuleInfoClass {
  cid: string;
  category: string;
  name: string;
  vendor: string;
  version: string;
}

export interface ModuleInfo {
  version: string;
  factoryVendor: string;
  classes: ModuleInfoClass[];
  /** Each entry: a class ID in this module that replaces older class IDs. */
  compatibility: { replacement: string; replaces: string[] }[];
}

function text(v: unknown): string {
  return typeof v === 'string' ? v : '';
}

function asList(v: unknown): unknown[] {
  return Array.isArray(v) ? v : [];
}

function asObject(v: unknown): Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v) ? (v as Record<string, unknown>) : {};
}

export function parseModuleInfo(source: string): { ok: true; info: ModuleInfo } | { ok: false; error: string } {
  let raw: unknown;
  try {
    raw = JSON5.parse(source);
  } catch (e) {
    return { ok: false, error: e instanceof Error ? e.message : String(e) };
  }
  const o = asObject(raw);
  return {
    ok: true,
    info: {
      version: text(o['Version']),
      factoryVendor: text(asObject(o['Factory Info'])['Vendor']),
      classes: asList(o['Classes']).map((c) => {
        const k = asObject(c);
        return {
          cid: text(k['CID']).toUpperCase(),
          category: text(k['Category']),
          name: text(k['Name']),
          vendor: text(k['Vendor']),
          version: text(k['Version']),
        };
      }),
      compatibility: asList(o['Compatibility']).map((c) => {
        const k = asObject(c);
        return {
          replacement: text(k['New']).toUpperCase(),
          replaces: asList(k['Old']).map((x) => text(x).toUpperCase()).filter((x) => x !== ''),
        };
      }),
    },
  };
}
