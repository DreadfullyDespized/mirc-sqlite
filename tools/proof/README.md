# Proof bed

`proof.mrc` is the alias suite used to validate the DLL: it exercises the
query shapes from `docs/INTERFACE.md` and logs the results. Run it against
the 2009 baseline DLL and the candidate build, then compare the two logs
with `diff.sh`:

- `bash tools/proof/diff.sh old/proof.log new/proof.log`

The new DLL must match the baseline everywhere except the documented
version-ceiling fixes. Scratch databases only — nothing touches a live
instance.

- `MSQLITE_MRC` env overrides the alias file fetch for offline runs.
