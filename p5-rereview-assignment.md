# ASSIGNMENT — P5-R1 re-verify (cross-lab review, scoped)

ROLE: Reviewer (Specialist). You verify fixes; you do not amend scope.

CONTEXT: Your previous whole-branch review is at `p5-final-review.md` (repo
root). Verdict was REVISE with findings MUST-1..MUST-8 and SHOULD-1. One
commit claims to fix all nine: `d594b25` (diff it against its parent
`a3ce9b5`).

TASK: For EACH of MUST-1..MUST-8 and SHOULD-1, verify the fix in commit
d594b25 actually resolves the finding as stated. Read the code, not the
commit message. Where a test claims to prove the fix, run it:

- `make -C status-panel/firmware/test` (host suite; includes new
  panelAlarmIdent cases)
- `make -C status-panel/daemon test` (codec + listener suites; the listener
  suite now pins exact journal text, not-yet-valid certs, unreadable
  denylist, file-mode policy, read-idle eviction via injected clock)
- `sh status-panel/tools/smoke.sh` (provision round-trip + dropped-ACK and
  forged-watermark desync fault injection)

If your sandbox cannot bind loopback, mark the listener suite UNVERIFIED and
verify by reading the test instead — do not guess.

Also check the fix introduced no NEW defect in the touched files:
protocol.{h,c}, firmware/main.c, firmware/panelPower.{h,c},
daemon/listener.c, daemon/solariPanel.c, tools/panelProv.c,
tools/fakePanel.c, tools/smoke.sh, both test suites,
docs/panel/SolariNet_Panel_Standalone_Runbook.html,
docs/panel/SolariNet_Panel_Manual.html.

OUT OF SCOPE: new features, style preferences, re-litigating accepted
design (deferred-ack pattern, forbidden-bit mode masks, D18 unconditional
CYW43 init). Findings outside the nine items only if they are genuine
defects introduced by d594b25.

RETURN: Write `p5-rereview.md` at the repo root:
- Per finding: VERDICT (FIXED / NOT FIXED / PARTIAL) + one-paragraph
  evidence (file:line or test output).
- Any new defects introduced by the fix commit, severity-ranked.
- Overall verdict line: `VERDICT: APPROVE` or `VERDICT: REVISE`.
- UNVERIFIED section (required, never empty by default — at minimum the
  things you could not run).

Do not modify any file other than p5-rereview.md.
