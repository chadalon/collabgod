# @collabgod/interface

The app's interface: TypeScript running in JUCE's web view (WebView2 on Windows, WKWebView on macOS), with the timeline drawn on a GPU canvas (ADR-0002). It talks to the app only through [`@collabgod/bridge`](../bridge).

For now it is the app shell (#26): it connects to the app, keeps engine frames coming, draws a placeholder and shows the diagnostics panel. The design system comes with #33 and the timeline with #31.

| Command | What it does |
| --- | --- |
| `npm run build` | Bundle the page into `dist/`, which the app serves and copies beside itself when it builds |
| `npm run dev` | The same, rebuilding on every save |
| `npm test` | Unit tests, including the self-check against a fake app |
| `npm run typecheck` | Strict TypeScript check |

Run them from the repository root with `-w @collabgod/interface`, or use the root `npm run build` and `npm test`, which run every package.

## Working on the page

A development build of the app serves the page from this folder's `dist/` and reloads it when it changes. Debug builds are development builds; any build is one with `--dev`, or with `--ui-dir <folder>` to serve another folder.

```sh
npm run dev -w @collabgod/interface         # in one terminal: rebuild on every save
npm run build -- --config Debug             # once: a Debug build of everything
```

Then start the Debug app (`build/app/collabgod_artefacts/Debug/CollabGod`; on macOS add `.app/Contents/MacOS/CollabGod`, on Windows `.exe`). Every save rebuilds the page and the app reloads it. A change to the bridge contract needs `npm run build` again, because the app's C++ is generated from it.

A Release build serves the copy beside it, so it runs without the source tree.

## Diagnostics

**Ctrl+Shift+D** (**Cmd+Shift+D** on a Mac), or `--diagnostics` at start, opens a panel with:

- the bridge round trip (a ping to the app and back, once a second),
- how engine frames arrive, push or pull, and how many drawn frames had no new engine frame (**P** switches, to compare them; ADR-0016),
- how long a command takes to show in a frame,
- the engine commands sent, and in how many batches.

The page also sends these to the app every 30 seconds, which writes them to its log: `~/Library/Logs/CollabGod/CollabGod.log` on macOS, `%APPDATA%\CollabGod\CollabGod.log` on Windows.

## Must hold

- **Draw with WebGL2**, since WKWebView on the Intel Mac has no WebGPU, and test every shader in both web views, because WebKit refused one that Chromium accepted (`webview-ui` findings 2 and 3).
- **Redraw the moment the size changes**, in the `ResizeObserver` callback, as `src/surface.ts` does (`webview-ui` finding 11).
- **Animate from timestamps, never by counting frames**: the app's display refresh and the page's do not line up (`webview-ui` finding 6). `FrameFeed.hostTimeNowMs` gives the app's clock now.
- **Send engine commands only through `CommandQueue`**: at most one batch per display frame.
- **Nothing but drawing may depend on the drawing loop.** A covered or minimised window stops `requestAnimationFrame`: on the Mac it stopped entirely in the #26 checks. Pulled engine frames stop with it (ADR-0016). Work that must go on while the window is out of sight, such as applying another member's edits, runs from the bridge's events and timers, never from frames. `CommandQueue` already sends after 100 ms when no frame comes.
- **The background colour is the contract's `backgroundColour`**, so the web view and the window match the page before it draws. `test/page.test.ts` checks `index.html`.
