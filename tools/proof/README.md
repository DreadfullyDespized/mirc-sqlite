# Proof bed

`proof.mrc` is the alias suite used to validate the DLL: it exercises the
query shapes from `docs/INTERFACE.md` and logs the results. Run it against
the 2009 baseline DLL and the candidate build, then compare the two logs
with `diff.sh`:

- `bash tools/proof/diff.sh old/proof.log new/proof.log`

The new DLL must match the baseline everywhere except the documented
version-ceiling fixes. Step s16's AUTHFN-DENY line is an expected
difference from the 2009 baseline; `diff.sh` accepts only
`AUTHFN-DENY create=1 errno=0` on the baseline side and
`AUTHFN-DENY create=0 errno=23` on the 3.54.0 side. Steps s17 and s18 cover a
`create_function` callback and a nested same-connection call refused
with error 200, with the outer query left clean. Scratch databases
only — nothing touches a live instance.

- `MSQLITE_MRC` env overrides the alias file fetch for offline runs.
