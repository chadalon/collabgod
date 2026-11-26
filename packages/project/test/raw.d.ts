// Vite reads a file as text when its import ends in ?raw.
declare module '*?raw' {
  const text: string;
  export default text;
}
