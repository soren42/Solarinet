# ASSIGNMENT — P5-R2 verify (final cross-lab gate, narrow)

ROLE: Reviewer (Specialist). Verify only; no scope amendments.

CONTEXT: Your re-review `p5-rereview.md` left MUST-8 PARTIAL (no write-
deadline test), SHOULD-1 PARTIAL (substring asserts; "TLS listener ready"
never pinned), and one new MEDIUM (fakePanel WIPE-ack teardown race).
Commit `8bcdbca` (diff against `d594b25`) claims to close all three.

TASK — verify each:
1. MUST-8: listener.c WRITE_MS now `#ifndef`-guarded; daemon/Makefile builds
   the harness with -DWRITE_MS=300u; case 16 connects an authorized client
   that never reads and pumps panelListenerWrite until eviction. Confirm the
   test actually exercises the deadline path (not some other disconnect) and
   that production keeps 5000 ms.
2. SHOULD-1: logHasExact pins — missing-denylist line, file-mode refusal
   line, and "TLS listener ready on <addr>:<port>" now emitted from
   panelListenerCreate with the RESOLVED port (duplicate solariPanel.c line
   removed — confirm no behavior regression in the daemon startup path and
   that the runbook's quoted text still matches).
3. MEDIUM race: fakePanel now FIONREAD-drains its held slave fd (bounded
   200×10 ms) before exit. Confirm the logic is sound (shared pty input
   queue) and run `sh status-panel/tools/smoke.sh` REPEATEDLY — at least 10
   runs — and report the pass count.

Also run `make -C status-panel/daemon test` if your sandbox permits loopback
bind; otherwise verify from source and say so in UNVERIFIED. Check the diff
introduced no new defect.

RETURN: write `p5-r2-verify.md` at repo root: per-item VERDICT
(FIXED / NOT FIXED) + evidence, any new defects, overall
`VERDICT: APPROVE` or `VERDICT: REVISE`, and a required UNVERIFIED section.
Modify no other file.
