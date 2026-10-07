# Proof bed

Runs real mIRC under Wine and diffs the 2009 `msqlite.dll` baseline against
the new build over the alias suite in `proof.mrc`.

## Prerequisite: mIRC in the Wine prefix

The proof needs a mIRC installation, which is proprietary software and is
intentionally **not** committed to this repo. Provide it yourself: install
mIRC into the Wine prefix the proof runs under
(`$WINEPREFIX/drive_c/mIRC/mirc.exe`), or point `MIRC_EXE` at an existing
`mirc.exe`. The workflow and scripts never touch an installer.

## Running it

Local (Linux with Wine):

- `WINEPREFIX=/path/to/prefix bash tools/proof/run.sh old /path/to/2009/msqlite.dll`
- `WINEPREFIX=/path/to/prefix bash tools/proof/run.sh new build/msqlite.dll`
- `bash tools/proof/diff.sh /tmp/mirc-sqlite-proof/old/proof.log /tmp/mirc-sqlite-proof/new/proof.log`

`run.sh` expects an installed `mirc.exe` at `$WINEPREFIX/drive_c/mIRC/mirc.exe`
(or set `MIRC_EXE`). Set up the Wine prefix and install mIRC into it yourself
first.

## Notes

- The 2009 baseline DLL is fetched from the mirc scripts repo at CI time;
  for local runs pass its path as the `dll-path` argument.
- `MSQLITE_MRC` env overrides the alias file fetch for offline runs.
