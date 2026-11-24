// Fails if product code imports, includes or builds anything under spikes/.
//
//   node scripts/check-no-spike-imports.mjs     (also run by `npm test`)
//
// Spikes are throwaway (AGENTS.md): code a spike proved is carried over into the
// product, never referenced where it stands. This looks at the places product code
// lives and at the build files, and ignores comments, which may cite a spike's
// findings.

import { readdir, readFile } from 'node:fs/promises';
import { join, relative, basename, extname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { isMain } from './lib.mjs';

const ROOT = fileURLToPath(new URL('..', import.meta.url));

/** Folders and files that hold product code or decide what the build includes. */
export const PRODUCT_PATHS = [
  'app',
  'cmake',
  'packages',
  'scripts',
  'CMakeLists.txt',
  'package.json',
  'tsconfig.base.json',
];

const SKIP_DIRS = new Set(['node_modules', 'dist', 'build', 'coverage', '.git']);

const SPIKE_PATH = /(^|[\\/'"<(\s])(\.\.?[\\/])*spikes[\\/]/;

/** Removes `//` and `/* *\/` comments from C-like source, leaving strings alone. */
function stripCLikeComments(text) {
  let out = '';
  let i = 0;
  let quote = null;
  while (i < text.length) {
    const c = text[i];
    const next = text[i + 1];
    if (quote !== null) {
      out += c;
      if (c === '\\') {
        out += next ?? '';
        i += 2;
        continue;
      }
      if (c === quote || c === '\n') quote = null;
      i += 1;
    } else if (c === '"' || c === "'" || c === '`') {
      quote = c;
      out += c;
      i += 1;
    } else if (c === '/' && next === '/') {
      while (i < text.length && text[i] !== '\n') i += 1;
    } else if (c === '/' && next === '*') {
      i += 2;
      while (i < text.length && !(text[i] === '*' && text[i + 1] === '/')) {
        if (text[i] === '\n') out += '\n'; // keep line numbers
        i += 1;
      }
      i += 2;
    } else {
      out += c;
      i += 1;
    }
  }
  return out;
}

/** Removes `#` comments from CMake, leaving quoted strings alone. */
function stripCMakeComments(text) {
  return text
    .split('\n')
    .map((line) => {
      let inString = false;
      for (let i = 0; i < line.length; i += 1) {
        if (line[i] === '\\') i += 1;
        else if (line[i] === '"') inString = !inString;
        else if (line[i] === '#' && !inString) return line.slice(0, i);
      }
      return line;
    })
    .join('\n');
}

/** How to read a file, by name; null for files this check does not read. */
function kindOf(file) {
  const name = basename(file);
  if (name === 'CMakeLists.txt') return 'cmake';
  switch (extname(name)) {
    case '.cmake':
      return 'cmake';
    case '.ts':
    case '.tsx':
    case '.mts':
    case '.cts':
    case '.js':
    case '.mjs':
    case '.cjs':
    case '.c':
    case '.cc':
    case '.cpp':
    case '.h':
    case '.hpp':
    case '.m':
    case '.mm':
      return 'c-like';
    case '.json':
    case '.html':
      return 'plain';
    default:
      return null;
  }
}

/** Lines of `text` that reach into spikes/, as { line, text }. Exported for tests. */
export function findSpikeReferences(text, kind) {
  const code =
    kind === 'cmake' ? stripCMakeComments(text) : kind === 'c-like' ? stripCLikeComments(text) : text;
  const found = [];
  code.split('\n').forEach((line, index) => {
    if (SPIKE_PATH.test(line)) found.push({ line: index + 1, text: line.trim() });
  });
  return found;
}

async function* walk(path) {
  let entries;
  try {
    entries = await readdir(path, { withFileTypes: true });
  } catch (error) {
    if (error.code === 'ENOTDIR') {
      yield path;
      return;
    }
    if (error.code === 'ENOENT') return;
    throw error;
  }
  for (const entry of entries) {
    if (entry.isDirectory()) {
      if (!SKIP_DIRS.has(entry.name)) yield* walk(join(path, entry.name));
    } else if (entry.isFile()) {
      yield join(path, entry.name);
    }
  }
}

/** Every spike reference in the product, as { file, line, text }. */
export async function checkRepository(root = ROOT, paths = PRODUCT_PATHS) {
  const self = fileURLToPath(import.meta.url);
  const problems = [];
  for (const start of paths) {
    for await (const file of walk(join(root, start))) {
      const kind = kindOf(file);
      // This script and its test name spikes/ on purpose.
      if (kind === null || file === self || basename(file) === 'check-no-spike-imports.test.mjs') continue;
      const text = await readFile(file, 'utf8');
      for (const hit of findSpikeReferences(text, kind)) {
        problems.push({ file: relative(root, file).split('\\').join('/'), ...hit });
      }
    }
  }
  return problems;
}

if (isMain(import.meta.url)) {
  const problems = await checkRepository();
  if (problems.length > 0) {
    console.error('Product code must not import, include or build anything under spikes/ (AGENTS.md):');
    for (const p of problems) console.error(`  ${p.file}:${p.line}: ${p.text}`);
    console.error('Carry the code you need over into the product instead.');
    process.exit(1);
  }
  console.log('No product code reaches into spikes/.');
}
