// Reading an inventory written by `plugin_scan scan`. Spike code (issue #14).
//
// The scanner's JSON is checked here field by field: it is written by C++ from
// whatever the plugins reported, and later by a different machine, so nothing in
// it is trusted to have the shape the types claim.

import { parseModuleInfo, type ModuleInfo } from './moduleInfo.js';

export const SCHEMA = 'collabgod.plugin-inventory/1';

/** VST3's category for the class a host instantiates as a plugin. */
export const AUDIO_MODULE_CLASS = 'Audio Module Class';

export interface ClassInfo {
  cid: string;
  category: string;
  name: string;
  vendor: string;
  version: string;
  sdkVersion: string;
  subCategories: string;
  classFlags: number;
  cardinality: number;
  infoLevel: 'unicode' | 'v2' | 'v1' | 'unreadable';
}

export interface Probe {
  result: string;
  error?: string;
  binary: string;
  entryPoint: string;
  loadMs: number;
  factoryInterface: number;
  binaryVersion: Record<string, string> | null;
  factory: { vendor: string; url: string; email: string; flags: number } | null;
  classes: ClassInfo[];
}

export interface Bundle {
  root: number;
  path: string;
  layout: 'bundle' | 'single-file';
  ending: 'exited' | 'crashed' | 'timed-out' | 'could-not-start';
  elapsedMs: number;
  probeWritten: boolean;
  exitCode?: number;
  crash?: string;
  moduleInfo: string | null;
  probe: Probe | null;
  log?: string;
}

export interface Inventory {
  schema: typeof SCHEMA;
  scanner: string;
  label: string;
  machine: { os: string; osVersion: string; arch: string };
  scannedAt: string;
  timeoutMs: number;
  roots: { path: string; bundles: number }[];
  problems: string[];
  bundles: Bundle[];
  durationMs: number;
}

/** One plugin as the lobby would see it: an audio module class from a bundle that scanned. */
export interface Plugin {
  cid: string;
  name: string;
  /** The class's vendor, or the factory's when the class has none (VST3's own rule). */
  vendor: string;
  /** The class version as the plugin reports it. Empty when it reports none. */
  version: string;
  sdkVersion: string;
  subCategories: string;
  infoLevel: ClassInfo['infoLevel'];
  bundle: string;
  /** Version numbers from outside the factory: Info.plist on macOS, the version resource on Windows. */
  binaryVersion: Record<string, string> | null;
  /** The bundle's moduleinfo.json, if it has one and it parses. */
  moduleInfo: ModuleInfo | null;
  /** The probe read everything, then crashed or hung while unloading. */
  unloadProblem: string | null;
}

export interface Failure {
  bundle: string;
  reason: string;
  /** Identity known without loading, from moduleinfo.json, when there is one. */
  moduleInfo: ModuleInfo | null;
}

export interface ReadInventory {
  inventory: Inventory;
  plugins: Plugin[];
  failures: Failure[];
  /** Problems with the inventory's own contents, such as moduleinfo.json that does not parse. */
  notes: string[];
}

export class InventoryError extends Error {}

type Json = unknown;

function isObject(v: Json): v is Record<string, Json> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}

function str(o: Record<string, Json>, k: string, where: string): string {
  const v = o[k];
  if (typeof v !== 'string') throw new InventoryError(`${where}.${k} should be text`);
  return v;
}

function num(o: Record<string, Json>, k: string, where: string): number {
  const v = o[k];
  if (typeof v !== 'number' || !Number.isFinite(v)) throw new InventoryError(`${where}.${k} should be a number`);
  return v;
}

function optStr(o: Record<string, Json>, k: string, where: string): string | undefined {
  return o[k] === undefined ? undefined : str(o, k, where);
}

function oneOf<T extends string>(o: Record<string, Json>, k: string, allowed: readonly T[], where: string): T {
  const v = str(o, k, where);
  if (!(allowed as readonly string[]).includes(v)) throw new InventoryError(`${where}.${k} is '${v}'`);
  return v as T;
}

const CID = /^[0-9A-F]{32}$/;

function readClass(c: Json, where: string): ClassInfo {
  if (!isObject(c)) throw new InventoryError(`${where} should be an object`);
  const cid = str(c, 'cid', where);
  if (!CID.test(cid)) throw new InventoryError(`${where}.cid '${cid}' is not 32 upper-case hex digits`);
  return {
    cid,
    category: str(c, 'category', where),
    name: str(c, 'name', where),
    vendor: str(c, 'vendor', where),
    version: str(c, 'version', where),
    sdkVersion: str(c, 'sdkVersion', where),
    subCategories: str(c, 'subCategories', where),
    classFlags: num(c, 'classFlags', where),
    cardinality: num(c, 'cardinality', where),
    infoLevel: oneOf(c, 'infoLevel', ['unicode', 'v2', 'v1', 'unreadable'] as const, where),
  };
}

function readStringRecord(v: Json, where: string): Record<string, string> | null {
  if (v === null) return null;
  if (!isObject(v)) throw new InventoryError(`${where} should be an object or null`);
  const out: Record<string, string> = {};
  for (const [k, val] of Object.entries(v)) {
    if (typeof val !== 'string') throw new InventoryError(`${where}.${k} should be text`);
    out[k] = val;
  }
  return out;
}

function readProbe(p: Json, where: string): Probe | null {
  if (p === null) return null;
  if (!isObject(p)) throw new InventoryError(`${where} should be an object or null`);
  const classes = p['classes'];
  if (!Array.isArray(classes)) throw new InventoryError(`${where}.classes should be a list`);
  const f = p['factory'];
  let factory: Probe['factory'] = null;
  if (f !== null) {
    if (!isObject(f)) throw new InventoryError(`${where}.factory should be an object or null`);
    factory = {
      vendor: str(f, 'vendor', `${where}.factory`),
      url: str(f, 'url', `${where}.factory`),
      email: str(f, 'email', `${where}.factory`),
      flags: num(f, 'flags', `${where}.factory`),
    };
  }
  const error = optStr(p, 'error', where);
  return {
    result: str(p, 'result', where),
    ...(error === undefined ? {} : { error }),
    binary: str(p, 'binary', where),
    entryPoint: str(p, 'entryPoint', where),
    loadMs: num(p, 'loadMs', where),
    factoryInterface: num(p, 'factoryInterface', where),
    binaryVersion: readStringRecord(p['binaryVersion'] ?? null, `${where}.binaryVersion`),
    factory,
    classes: classes.map((c, i) => readClass(c, `${where}.classes[${i}]`)),
  };
}

function readBundle(b: Json, where: string): Bundle {
  if (!isObject(b)) throw new InventoryError(`${where} should be an object`);
  const moduleInfo = b['moduleInfo'];
  if (moduleInfo !== null && typeof moduleInfo !== 'string') throw new InventoryError(`${where}.moduleInfo should be text or null`);
  const probeWritten = b['probeWritten'];
  if (typeof probeWritten !== 'boolean') throw new InventoryError(`${where}.probeWritten should be true or false`);
  const exitCode = b['exitCode'] === undefined ? undefined : num(b, 'exitCode', where);
  const crash = optStr(b, 'crash', where);
  const log = optStr(b, 'log', where);
  return {
    root: num(b, 'root', where),
    path: str(b, 'path', where),
    layout: oneOf(b, 'layout', ['bundle', 'single-file'] as const, where),
    ending: oneOf(b, 'ending', ['exited', 'crashed', 'timed-out', 'could-not-start'] as const, where),
    elapsedMs: num(b, 'elapsedMs', where),
    probeWritten,
    ...(exitCode === undefined ? {} : { exitCode }),
    ...(crash === undefined ? {} : { crash }),
    moduleInfo,
    probe: readProbe(b['probe'] ?? null, `${where}.probe`),
    ...(log === undefined ? {} : { log }),
  };
}

/** Checks the shape of a parsed inventory. Throws InventoryError naming the first bad field. */
export function validateInventory(data: Json): Inventory {
  if (!isObject(data)) throw new InventoryError('inventory should be a JSON object');
  if (data['schema'] !== SCHEMA) throw new InventoryError(`schema is '${String(data['schema'])}', expected '${SCHEMA}'`);
  const machine = data['machine'];
  if (!isObject(machine)) throw new InventoryError('machine should be an object');
  const roots = data['roots'];
  const problems = data['problems'];
  const bundles = data['bundles'];
  if (!Array.isArray(roots) || !Array.isArray(problems) || !Array.isArray(bundles)) {
    throw new InventoryError('roots, problems and bundles should be lists');
  }
  return {
    schema: SCHEMA,
    scanner: str(data, 'scanner', 'inventory'),
    label: str(data, 'label', 'inventory'),
    machine: {
      os: str(machine, 'os', 'machine'),
      osVersion: str(machine, 'osVersion', 'machine'),
      arch: str(machine, 'arch', 'machine'),
    },
    scannedAt: str(data, 'scannedAt', 'inventory'),
    timeoutMs: num(data, 'timeoutMs', 'inventory'),
    roots: roots.map((r, i) => {
      if (!isObject(r)) throw new InventoryError(`roots[${i}] should be an object`);
      return { path: str(r, 'path', `roots[${i}]`), bundles: num(r, 'bundles', `roots[${i}]`) };
    }),
    problems: problems.map((p, i) => {
      if (typeof p !== 'string') throw new InventoryError(`problems[${i}] should be text`);
      return p;
    }),
    bundles: bundles.map((b, i) => readBundle(b, `bundles[${i}]`)),
    durationMs: num(data, 'durationMs', 'inventory'),
  };
}

function failureReason(b: Bundle): string | null {
  if (b.probe && b.probeWritten && b.probe.result === 'ok') return null;
  if (b.probe && b.probeWritten) return `${b.probe.result}${b.probe.error ? `: ${b.probe.error}` : ''}`;
  switch (b.ending) {
    case 'crashed':
      return `crashed while loading (${b.crash ?? 'no detail'})`;
    case 'timed-out':
      return 'did not finish loading within the time limit';
    case 'could-not-start':
      return 'the probe process could not start';
    case 'exited':
      return `the probe exited with code ${b.exitCode ?? '?'} without a result`;
  }
}

/** Splits an inventory into usable plugins and bundles that could not be read. */
export function readInventory(data: Json): ReadInventory {
  const inventory = validateInventory(data);
  const plugins: Plugin[] = [];
  const failures: Failure[] = [];
  const notes: string[] = [];

  for (const b of inventory.bundles) {
    let moduleInfo: ModuleInfo | null = null;
    if (b.moduleInfo !== null) {
      const parsed = parseModuleInfo(b.moduleInfo);
      if (parsed.ok) moduleInfo = parsed.info;
      else notes.push(`${b.path}: moduleinfo.json does not parse (${parsed.error})`);
    }

    const reason = failureReason(b);
    if (reason !== null || !b.probe) {
      failures.push({ bundle: b.path, reason: reason ?? 'no probe result', moduleInfo });
      continue;
    }

    const probe = b.probe;
    const unloadProblem =
      b.ending === 'crashed' ? `crashed after reading (${b.crash ?? 'no detail'})`
      : b.ending === 'timed-out' ? 'hung after reading'
      : b.ending === 'exited' && b.exitCode !== 0 ? `exited with code ${b.exitCode ?? '?'} after reading`
      : null;

    const audio = probe.classes.filter((c) => c.category === AUDIO_MODULE_CLASS);
    if (audio.length === 0) notes.push(`${b.path}: loaded, but has no ${AUDIO_MODULE_CLASS}`);

    for (const c of audio) {
      plugins.push({
        cid: c.cid,
        name: c.name,
        vendor: c.vendor !== '' ? c.vendor : (probe.factory?.vendor ?? ''),
        version: c.version,
        sdkVersion: c.sdkVersion,
        subCategories: c.subCategories,
        infoLevel: c.infoLevel,
        bundle: b.path,
        binaryVersion: probe.binaryVersion,
        moduleInfo,
        unloadProblem,
      });
    }
  }

  return { inventory, plugins, failures, notes };
}
