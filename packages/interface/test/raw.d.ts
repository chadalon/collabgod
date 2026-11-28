// Vite reads a file as text when its import ends in ?raw.
declare module '*?raw' {
  const text: string;
  export default text;
}

// Vite reads every file matching a pattern, as text, with `query: '?raw'`.
interface ImportMeta {
  glob(pattern: string, options: { query: '?raw'; import: 'default'; eager: true }): Record<string, string>;
}
