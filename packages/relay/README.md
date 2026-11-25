# @collabgod/relay

The relay server. It puts every member's edits into one order, carries presence, and forwards a sample to a member who lacks it. It never sees audio or plugin files and never decides what an edit means: that is the session model's job (`@collabgod/session`).

Empty until Phase 2 (the lobby), which builds it from what the Phase 0 spikes in `spikes/relay` and `spikes/sample-transfer` found. Product code never imports a spike; carry the code over instead.

| Command | What it does |
| --- | --- |
| `npm test` | Unit tests |
| `npm run typecheck` | Strict TypeScript check |

Rules from ADR-0006: a member is not a connection, and the lobby limit is a relay setting, never an assumption of two people.
