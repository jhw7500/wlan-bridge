# Issue -> AI Review -> Patch -> PR (Design Notes)

Goal: In `wlan-bridge`, after an Issue is created, a human can mention an AI reviewer (e.g. `@claude`) to produce a review. Then the human can trigger follow-up automation via `@ai patch` (generate a patch file) and `@ai pr` (apply patch + open PR).

Status: Design only. No implementation yet.

## Core Requirements

- Works from GitHub Issues (not PRs).
- Review provider can be different from patch provider.
- Patch generation can be different from apply/PR provider.
- Default behavior: when generating a patch, use the most recent AI review comment on the Issue.
- Allow an override later (e.g. point to a specific comment URL), but v1 can start with the default.
- Must remain robust against LLM context explosion (recent Gemini token-limit failures).

## Proposed Workflow (3 Steps)

1) Review (already exists today)
- Trigger: Issue comment mention like `@claude`.
- Output: AI posts a review comment.

2) Patch generation
- Trigger: Issue comment contains `@ai patch`.
- Input:
  - Latest review comment (default).
  - Strictly limited code/context bundle (see "Context Budget" section).
- Output:
  - Upload `patch.diff` as a workflow artifact.
  - Post an Issue comment containing a machine-readable pointer to the artifact.

3) Apply patch + create PR
- Trigger: Issue comment contains `@ai pr`.
- Input:
  - Latest patch artifact pointer (default).
- Output:
  - Create branch, apply patch, commit, open PR.

## Why 3 Steps (vs 2)

Keep 3 steps in v1 because:
- Model/provider separation is easier (review vs patch vs apply).
- Safer operation: humans can inspect patch before PR.
- Easier reruns: regenerate patch without creating new PRs.
- Security/permissions: patch generation can be read-only; PR creation requires write.

If we want to reduce UX friction later, the natural merge is (2)+(3): `@ai pr` would generate patch + apply + PR in one run.

## Comment Markers (Machine-Readable)

To reliably find "latest review" and "latest patch" without fragile text parsing, embed HTML comment markers.

Review marker example (added by the AI reviewer workflow):

<!-- ai:review v1 provider=claude -->

Patch marker example (added by the patch generation workflow):

<!-- ai:patch v1 provider=codex run_id=123 artifact=patch.diff -->

PR marker example (added by the PR creation workflow):

<!-- ai:pr v1 branch=ai/issue-123/patch-123 pr=456 -->

The `@ai patch` flow should:
- Find the latest comment containing `ai:review`.
- If none exists: comment back with instructions ("mention @claude first" or provide explicit scope).

The `@ai pr` flow should:
- Find the latest comment containing `ai:patch`.
- If none exists: comment back ("run @ai patch first").

## Context Budget (Critical: Gemini Token/Context Failures)

Observed failure class in similar setup: Gemini returns `400 INVALID_ARGUMENT` because "input token count exceeds 1048576".

Root cause hypothesis:
- The agent uses toolcalls (GitHub MCP / shell reads) to pull too much repository context.

Design constraints to prevent this:
- Do not allow unlimited repo exploration during patch generation.
- Prepare a bounded "context bundle" before calling the model.

Context bundle rules (v1 recommendation):
- Require Issue to include an explicit list of affected paths (e.g. `paths:` field in Issue template).
- Only include:
  - those files (or small excerpts)
  - plus a minimal project overview (README + 1-2 key docs)
- Hard caps:
  - max N files (e.g. 10)
  - max M lines per file excerpt (e.g. 200)
  - max total bytes (e.g. 200KB)

If the Issue does not include paths:
- Patch generation should refuse and instruct the user to add scope (or provide a follow-up comment with `paths:`).
  - This is intentional: "better no patch than a flaky tool that explodes context".

## Provider/Model Selection per Step

We want per-step providers, using Issue comments as triggers.

Suggested control surface (v1):
- Review provider: whatever is mentioned (`@claude`, `@gemini`, `@codex`).
- Patch provider: configured by repo variables or by command options later.
- Apply+PR: deterministic (no AI) by default.
  - Only invoke an AI provider for conflict resolution or additional requested edits.

Command ideas (future):
- `@ai patch` (uses default patch provider)
- `@ai patch provider=codex`
- `@ai pr` (apply latest patch)
- `@ai pr patch=run:123` (apply specific patch)

## Security / Permissions

Rules to prevent abuse:
- Only accept `@ai patch` / `@ai pr` from:
  - repo members, OR
  - issue author, OR
  - an allowlist.
- Use a GitHub App token for PR creation (limited permissions).
- Ensure workflows do not run on untrusted forks with write tokens.

## Gemini Context Debug Plan (Do This First)

We should fix the "Gemini context explosion" before adding new patch/PR flows.

Phase 1: Evidence
- Reproduce on `wlan-bridge` workflows:
  - run the gemini review workflow on a small PR/issue
  - capture full logs + artifacts
- Confirm which failure mode it is:
  - token-limit error (input too big)
  - timeout/hang
  - 503 overloaded
- Capture "context size" evidence:
  - emit prompt length (bytes)
  - if using a bundle, emit bundle size

Phase 2: Root cause
- Identify which toolcall(s) cause the explosion:
  - full file reads
  - too many files
  - PR history, full repo traversal, etc.

Phase 3: Fix direction (high level)
- Move from "agent fetches arbitrary context" to "workflow prepares bounded context".
- Enforce file/line caps.
- Prefer refusing to run over producing a flaky run.

## Implementation Sketch (Later)

Candidate triggers:
- `.github/workflows/*.yml` with `on: issue_comment`.
- Parse comment body for `@ai patch` / `@ai pr`.

Candidate code:
- Use `gh api` to fetch Issue comments and find latest marker.
- Use `actions/download-artifact` or `gh api` to download patch artifact.
- Use `git apply --check` then `git apply` then commit.
- Use `gh pr create` to open PR.

Open questions to settle before implementation:
- Where does patch provider run (GitHub-hosted runner vs self-hosted)?
- How do we supply repo context safely (paths required vs heuristic search)?
- Should patch generation be allowed on Issues without explicit paths?
