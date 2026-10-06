# AGENTS.md — mirc-sqlite

How this repo is worked, inherited from the house standards.

- **Working process**: every change runs the cycle — issue (what it should
  do + how we'll know it works; proof named up front) → PR carrying
  `## For Dread` (what changed, the proof, what he's asked to decide) and
  `## Blast radius` → CI green → the Wine proof bed → an independent
  grader (FAIL unless proven) → merge → post-ship check. No merge without
  real green CI unless Dread explicitly overrides.
- **Merge authority**: Dread merges. This DLL is mIRC infrastructure; it
  never ships to a live environment without his explicit go-ahead.
- **Comment ban** (house rule): code and scripts carry no comments. The
  only survivor states a non-obvious why the code cannot express.
  Docs, records, and memory files are exempt — there the writing is the
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
