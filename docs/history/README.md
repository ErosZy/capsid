# History and Evidence

This index is for maintainers tracing a decision, regression or acceptance
claim. It is not the getting-started path. Use the [documentation index](../README.md)
for current guides and references.

Historical plans/reviews stay at their existing paths with a prominent current
context banner; their original body and evidence remain unchanged. Six mixed
reference/journal pages have current fronts and retain their captured source
bytes under [the 2026-11-17 migration index](archives/2026-11-17/index.json).
The index binds every source length/hash and 483 legacy anchors, which are
retained at the original entry paths. The [coverage inventory](../documentation-coverage.json)
records the disposition of every page. Within dated sections, words such as “current”, “passed” and
“not released” refer to that section's captured input and date—not necessarily
to today's source or the published beta.

## Release and Qualification Records

- [v0.3.0 qualification and release boundary](../v0.3.0-current-qualification.md):
  published beta.2, source identities, exact CI scope and retained limitations.
- [Final beta.2 publication verification](../evidence/v0.3.0/2026-11-13-ci-resumption/beta2-publication-final-r1.json).
- [v0.3.0 evidence index](../evidence/v0.3.0/2026-11-10/index.json) and
  [historical availability audit](../evidence/v0.3.0/2026-11-10/historical-evidence-availability.json):
  what was retained or unavailable at those capture dates.
- [Current testing reference](../testing.md) and [testing guide](../testing-guide.md).
  The old execution journal is the exact captured
  [testing.md.txt](archives/2026-11-17/testing.md.txt), not the current procedure.
- [Current performance data](../performance-benchmarks.md) and the exact
  [old performance journal](archives/2026-11-17/performance-benchmarks.md.txt).
  The accepted 485b29b measurement, published beta.2 and old checkpoints have
  distinct source identities; none is substituted for another.

## Six Byte-Preserved Reference/Journal Sources

- [FetchRPC v4 source](archives/2026-11-17/fetchrpc-v4.md.txt).
- [WebSocket semantics source](archives/2026-11-17/workerd-websocket-semantics.md.txt).
- [Host design/review source](archives/2026-11-17/host-technical-design-review.md.txt).
- [Performance source](archives/2026-11-17/performance-benchmarks.md.txt).
- [Testing source](archives/2026-11-17/testing.md.txt).
- [Qualification source](archives/2026-11-17/v0.3.0-current-qualification.md.txt).

These `.md.txt` files are exact historical source downloads, not new maintained
instructions. Their old “current/unpublished” language refers to the captured
phase. Do not edit those bytes or reinterpret their test/performance counts as
beta.2 data.

A `qualification-before.md` under a dated evidence directory is a byte-preserved
snapshot. Its adjacent `index.json` supplies the SHA-256 checked by the
repository's documentation auditor. Do not repair an old snapshot's wording or
relative links by changing its bytes.

## Design and Delivery Plans

- [Project roadmap](../../ROADMAP.md).
- [Host technical design and milestone review](../host-technical-design-review.md).
- [FetchRPC v4 milestone plan](../v0.3.0-fetchrpc-v4-plan.md).
- [v0.3.0 implementation design](../v0.3.0-implementation-design.md).
- [Native Fetch branch plan](../v0.3.0-native-fetch-branches-plan.md).
- [Remediation and delivery plan, 2026-10-12](../superpowers/plans/2026-10-12-v0.3.0-remediation-and-delivery.md).
- [Binding security remediation plan, 2026-08-16](../superpowers/plans/2026-08-16-binding-v1-security-audit-remediation.md).

A plan states intended work and acceptance conditions. A checked item is not a
substitute for the corresponding source-bound result or release record.

## Dated Reviews and Handoffs

These retain their original scope and language:

- [AI review brief](../ai-review-brief-v0.2.2-to-v0.3.0.md).
- [GLM takeover review](../v0.3.0-glm-takeover-review.md).
- [GLM continuation handoff](../v0.3.0-glm-continuation-handoff.md).
- [Test-completeness review](../v0.3.0-test-completeness-review.md).
- [Combined implementation review](../v0.3.0-latest-combined-review.md).
- [Fix review](../v0.3.0-glm-fix-review.md).
- [Rounds 5–7 review](../v0.3.0-glm-round5-7-review.md).
- [Agent continuation handoff](../v0.3.0-next-ai-handoff.md).

Do not treat an old handoff's pending tasks, environment assumptions or build
paths as instructions to restart a completed campaign.

## Evidence Rules

Keep source commit/tree, build identity, platform, command, actual exit, raw
reports and genuine prerequisite skips together. Preserve failed/partial runs
and reviewer revisions. Equal source trees do not imply equal embedded binary
identities, and a later pass does not reconstruct the cause of an earlier
failure. Performance claims additionally require the original protocol and
whole-session/environment evidence; release qualification alone is not a new
benchmark point.
