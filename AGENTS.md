# AGENTS.md — mirc-sqlite

How this repo is worked, inherited from the house standards.

- **Working process**: every change runs the cycle — issue (what it should
  do + how we'll know it works; proof named up front) → PR whose body is
  the repo-loop shape in `.github/pull_request_template.md` (`For Dread:`
  with text on the first non-blank line, at most 12 non-blank lines,
  exactly one `Doer:` and one `Grader:` naming a current seat, and a
  non-empty `Tested:` that names the command and the result; a visible
  change links its live output there), checked by
  `tools/checks/pr_body_check.py` → CI green → the Wine proof bed → an
  independent grader (FAIL unless proven) → merge → post-ship check. No
  merge without real green CI unless Dread explicitly overrides.
- **Merge authority**: Dread merges. This is a PROD repo, as set by the
  `Repo label: PROD` first line of [README.md](README.md). This DLL is mIRC
  infrastructure; it never ships to a live environment without his explicit go-ahead.
- **Comment ban** (house rule): code and scripts carry no comments.
  `tools/checks/no_new_comments.py` scans every tracked file (machine
  directives only: shebang, encoding line, `type: ignore`, `noqa`,
  `pragma`). The why goes in the PR body or a commit message. Docs,
  records, and memory files are exempt — there the writing is the
  product.
- **Correction ladder**: when corrected, encode the fix at the highest
  feasible layer — structurally impossible > automated check > standing
  rule > skill > memory note. A note-only fix for a recurring error is
  incomplete.
- **Cleanup scope**: this repo cleans up only what it created. Never
  delete/move/tidy another project's files or anything of unclear
  ownership — flag it instead.
- **Test bed** (see LOOP-STATE.md): Wine + real mIRC 7.79 + candidate DLL
  on a private Xvfb display, running real mIRC scripts. The 2009
  DLL is the behavior baseline; the new DLL must match it everywhere
  except the documented version-ceiling fixes.
- **Live systems**: nothing here deploys to, restarts, or touches any
  live mIRC instance or live database. Proof runs use scratch copies only.
