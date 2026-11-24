# @collabgod/interface

The app's interface: TypeScript running in JUCE's web view (WebView2 on Windows, WKWebView on macOS), with the timeline drawn on a GPU canvas (ADR-0002).

It is a placeholder page for now. The app shell and typed bridge come with #26, the design system with #33 and the timeline with #31.

| Command | What it does |
| --- | --- |
| `npm run build` | Bundle the page into `dist/` (`-- --watch` to rebuild on change) |
| `npm test` | Unit tests |
| `npm run typecheck` | Strict TypeScript check |

Run them from the repository root with `-w @collabgod/interface`, or use the root `npm run build` and `npm test`, which run every package.

Must hold, from Phase 0 (`spikes/webview-ui`): draw with WebGL2, since WKWebView on the Intel Mac has no WebGPU, and test every shader in both web views, because WebKit refused one that Chromium accepted.
