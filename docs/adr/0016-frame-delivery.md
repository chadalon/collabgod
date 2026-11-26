# ADR-0016: Engine frames reach the page by pull, pending a comparison on both real machines

- **Status:** Proposed
- **Date:** 2026-11-25
- **Deciders:**

## Context

The page draws the playhead, meters and, later, everything that moves from engine state. That state reaches it in one of two ways, and #26 asks that one be chosen from a comparison on both maintainer machines and recorded:

- **Push**: the app sends a frame, with a timestamp, on its own display refresh.
- **Pull**: the page asks the app for a frame as it starts drawing one.

The `webview-ui` spike found that the app's refresh and the page's do not line up (finding 6). With push, drawn frames sometimes had no new engine frame since the last one: 18 of 481 (4%) in `pan-zoom` on the Intel Mac, none on the Windows desktop, and about a quarter on the macOS CI runner. With pull, every drawn frame had fresh data on both machines and both runners, at the same frame rate as push, because the round trip is under a millisecond (finding 7). On Linux, which is not a product platform, pull halved the data rate, because WebKitGTK runs in another process.

## Decision

**Pull is the default for now**, because the spike's numbers favour it on both machines. Both are built (`--frame-delivery push|pull`, or P in the diagnostics panel), and the page counts drawn frames that had no new engine frame. The choice is made on both real machines with the app's own diagnostics, recorded in the table below, and then this record is accepted or changed.

Either way the page draws from the frame's timestamp, never by counting frames.

## Alternatives considered

- **Push only**, as the spike started. Up to 4% of drawn frames stale on the Mac in the spike.
- **Choose now from the spike's numbers alone.** The spike's page was a benchmark, not the app; #26 asks for the comparison in the app.

## Consequences

- Pull makes one bridge call per drawn frame, about 60 a second, each under a millisecond on both machines in the spike. The bridge runs on the message thread, so a slow edit there (engine-projection finding 6) delays the next frame's data with either method.
- With pull, a frame is a round trip old when it is drawn: under a millisecond plus at most one frame. With push, it is up to one frame old.
- With pull, the page sends the frame's queued commands before it pulls, so the frame it pulls already includes them. When it pulled first, a command took a frame longer to show with pull than with push (49.9 ms against about 25 ms at the median on the Windows desktop). In the same measurement, pushed frames reached the page just after it started drawing, so each was drawn a frame after it arrived.
- The diagnostics time a command from when the page queues it to when a drawn frame includes it, the same way for both methods.
- Events that are not per-frame state (an error, a peer joining) are pushed whatever this decides.

## How we will know

Chad runs the app on each machine with `--diagnostics`, plays for a minute with each setting (P switches), and notes the diagnostics:

| Machine | Delivery | fps | Drawn frames without new data | Bridge round trip median / p95 | Command to drawn frame median / p95 |
| --- | --- | --- | --- | --- | --- |
| Windows desktop | push | 60.0 | 6 of 1800 (0.3%) | 0.5 / 0.6 ms | 49.7 / 49.9 ms |
| Windows desktop | pull | 60.0 | 0 of 1799 | 0.3 / 0.6 ms | 24.7 / 33.3 ms |
| Intel MacBook, macOS 13.7 | push | | | | |
| Intel MacBook, macOS 13.7 | pull | | | | |

The Windows rows are 30 seconds each, taken on 2026-11-25 after the change that sends commands before pulling. An earlier run of about a minute each, before that change, gave the same frame rate, 3 of 6342 pushed and 0 of 3596 pulled drawn frames without new data, and a bridge round trip of 0.3 to 0.4 ms at the median.

Until the engine plays (#27, #30) the frames carry only a sequence number and the app's clock, which is enough to compare delivery but not load; the comparison is worth repeating once the timeline (#31) draws real clips.
