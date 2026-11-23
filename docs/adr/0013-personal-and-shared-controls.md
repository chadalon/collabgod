# ADR-0013: What you hear is yours, what changes the song is shared

- **Status:** Accepted
- **Date:** 2026-11-22
- **Deciders:** Chad

## Context

ADR-0005 gave each person their own transport and left three things to settle: whether the loop range, solo and the metronome are personal or shared. It also said follow mode "locks your playhead to the other person's" without saying what happens when a follower uses their own transport controls.

Chad wanted both a personal and a shared version of these controls to be possible, without making the app more complicated to use. Phase 1 builds the loop and the metronome (#30), so the answer is needed now.

## Decision

One rule decides every such control: **what only changes what you hear is yours; what changes the song is shared.**

- **The loop range is personal**, like the playhead.
- **The metronome is personal.**
- **Solo is personal.** It is for auditioning. **Mute is shared**, because it is part of the mix.
- **In follow mode the member being followed drives.** A follower gets that member's playhead, play and stop, and loop range. A follower who seeks, plays, stops or changes the loop range leaves follow mode and is back on their own transport.

The shared version of a personal control comes through follow mode, not through a second set of controls: to loop the same bars as someone, follow them.

## Alternatives considered

- **A personal and a shared copy of each control.** Rejected: two loop ranges and two solo buttons per person is the complication Chad asked to avoid.
- **A follower's changes drive the member they follow.** Rejected: with both people following each other this is the shared transport that ADR-0005 rejected, and one person could no longer audition while the other listens.
- **A shared loop range.** Rejected: two people working on different sections would move each other's loop.

## Consequences

- Personal controls are presence or local state, never operations. They do not enter the ordered edit log (ADR-0004) and are not undoable. A person's loop range is sent as presence so that followers can use it and others can see it.
- Mute is an operation, like any other change to the mix.
- Nothing here assumes two people: a member follows one other member, and any number can follow the same one (ADR-0006).
- Whether personal state is remembered between sessions, and where, is for the issues that build it. It is not saved as part of the shared session.
- The interface has to show that you are following, whom, and that touching the transport will stop it.
- To steer, a member asks the other to follow them. A one-step way to ask ("follow me") would make that easy; it is not required for v1.

## How we will know

The Phase 2 gate, as for ADR-0005: two people arrange audio clips together in one session. If "follow me, listen to this loop" works without anyone losing their place, the decision stands.
