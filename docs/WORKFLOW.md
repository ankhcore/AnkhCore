# AnkhCore 2.5 — Development workflow

Owner rules, 2026-11-08 (decision D19). These are binding for every session.

## 1. Session start (in this order)

1. `HANDOFF.md`: current state, layout and build/test commands.
2. `docs/DECISIONS.md`: what is approved, and the pending decisions.
3. `docs/BACKLOG.md`: open work and known defects.
4. `docs/REQUIREMENTS.md`: the traceability matrix for the area being worked on.
5. The design document of the area (`docs/design/…` or `D:\Ankhcore\docs\…`).

Don't re-investigate anything a document already records unless new evidence contradicts it. Cite the document instead of re-explaining it.

## 2. Before a milestone

- List the approved requirements it touches (REQUIREMENTS IDs) and their dependencies.
- Any deviation, deferral, simplification or removal of an approved requirement is a **scope change**. Propose it with the reason and the alternatives, and wait for approval. Implementation order or difficulty is never permission to drop a requirement.
- When a requirement conflicts with security, licensing, compatibility or architecture, explain the conflict and ask.

## 3. Verification levels

| Level | When | What |
|---|---|---|
| **Quick check** | After a localized change | Only the affected test: the relevant unit suite, or one targeted self-test check / sandbox scenario. Build only the variant you changed. |
| **Integration check** | After a related group of changes | Build every affected component, and run every suite and test that covers them (e.g. app test build + `--selftest` once; setup build + `--sandbox-test`). |
| **Full milestone verification** | Before a milestone commit | Both app builds, `tools\build_setup.cmd`, `test_base`, `test_fanpolicy`, `--sandbox-test`, and `--selftest` ×3. Add host checks where the milestone needs them, with an approved plan. |

Rules:
- Don't rebuild an unchanged component, and don't repeat a passed test without a stated reason (flakiness investigation, changed input).
- A failure is investigated (root cause) before any rerun; a rerun is not a fix.
- Coverage is never reduced to save time or tokens.
- Results go into the test log (section 4), not only into the conversation.

## 4. Records

| What | Where |
|---|---|
| Milestone test logs (commands, results, counts, evidence paths) | `docs/test-logs/<date>_<milestone>.md` (repo) |
| Large evidence (screenshots, captures, raw JSON with personal data) | `D:\Ankhcore\docs\…` (outside Git) |
| Investigation reports and designs | `docs/design/` (repo) or `D:\Ankhcore\docs\` (owner documents) |
| Decisions | `docs/DECISIONS.md` |
| Open work | `docs/BACKLOG.md` |
| Requirement status | `docs/REQUIREMENTS.md` (update the status of each requirement a milestone changes) |

## 5. Reporting

- Conversation updates are short: what changed, the result, what is pending, and links to the files.
- Don't restate completed milestones; link to their log.
