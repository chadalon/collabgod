# ADR-0012: Outside contributors agree to a contributor license agreement

- **Status:** Proposed
- **Date:** 2026-11-22
- **Deciders:** Chad

## Context

ADR-0003 chose GPL-3.0-or-later and left one question open: whether outside contributors sign anything (issue #2). With no agreement, a contributor keeps their copyright and licenses their work to the project under the GPL like everyone else. From then on the license of that work cannot change without their consent, and every contributor would have to agree to any change.

Chad expects the app to stay open source. The part meant to earn money is a hosted service, which needs no agreement: running software as a service does not distribute it. But he wants to keep the option of offering the app under other terms later, for example for an app store whose rules do not fit the GPL, and may one day run the project through a company.

The timing matters. Today every commit is Chad's or was made by an AI agent working for him, so there is nobody to ask. Adding an agreement later means collecting a signature from every past contributor. Dropping one later costs nothing.

This record is not legal advice.

## Decision

Outside contributors agree to [`CLA.md`](../../CLA.md) before their first contribution is merged. It is a license, not a transfer of copyright. It lets the maintainer license contributions under other terms, promises that any release containing a contribution is also available under a free and open-source license, and passes to the maintainer's successors and assigns.

A contributor agrees by adding their own line to [`CLA-SIGNATURES.md`](../../CLA-SIGNATURES.md) in their first pull request. The maintainer checks for it when reviewing. Work an AI agent does for a maintainer is the maintainer's contribution and needs no line.

## Alternatives considered

- **Nothing formal.** No friction, and the right choice if the license will never change. It closes the option for good once the first outside contribution is merged.
- **Developer Certificate of Origin.** A sign-off line on each commit that says the contributor had the right to submit it. Lighter than an agreement, but it grants no right to relicense.
- **An agreement without the promise in section 4.** Simpler, but it would let the project go closed-only, which is what makes contributors distrust such agreements and is not something Chad wants.
- **Copyright assignment.** Contributors hand over ownership. More than is needed, and harder to ask for.
- **A bot that collects signatures.** The self-hosted `contributor-assistant/github-action` is archived and no longer maintained, and it needs write access to the repository. The hosted alternative means giving a third-party app access to the repository. With one maintainer who reads every pull request and no outside contributors yet, a line in a file is enough. Revisit when checking by hand becomes a chore.

## Consequences

- Outside contributors have one extra step, once.
- The agreement text is a draft written for this project and has not been reviewed by a lawyer. It should be reviewed before the first outside contribution is merged, and in any case as part of the licensing review ADR-0003 asks for before any commercial step. Governing law is one of the things it leaves open.
- The agreement alone does not make a closed or dual-licensed edition possible. That would still need commercial JUCE and Tracktion licenses (ADR-0003), and section 4 means a free version would continue to exist beside it.
- If the project moves to a company, Chad assigns his own copyright to it separately. Contributors do not sign again.
- Whether code produced by AI tools can be copyrighted is unsettled. The agreement makes the contributor responsible for such code, but cannot grant rights that do not exist.
- The pull request template and `CONTRIBUTING.md` mention the agreement, and `AGENTS.md` tells agents not to sign for anyone.

## How we will know

The first outside pull request shows whether the step is clear and whether anyone objects to it. A lawyer's review may replace the text; that would be version 1.1 or 2.0 of the agreement and an update to this record. If Chad decides the license will never change, this record is superseded and the agreement is dropped.
