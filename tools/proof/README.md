# Proof bed

Runs real mIRC under Wine and diffs the 2009 `msqlite.dll` baseline against
the new build over the alias suite in `proof.mrc`.

## Prerequisite: the mIRC installer

The proof needs a mIRC installer, which is proprietary software and is
intentionally **not** committed to this repo. Provide it yourself:

1. Download the installer from the vendor's download page (mirc.com).
2. Save it as `tools/proof/deps/mirc-installer.exe` (this path is gitignored).

The `check-installer` CI job looks for that path: the proof runs when it is
present and is skipped otherwise.

## Running it

Local (Linux with Wine):

- `WINEPREFIX=/path/to/prefix bash tools/proof/run.sh old /path/to/2009/msqlite.dll`
- `WINEPREFIX=/path/to/prefix bash tools/proof/run.sh new build/msqlite.dll`
- `bash tools/proof/diff.sh /tmp/mirc-sqlite-proof/old/proof.log /tmp/mirc-sqlite-proof/new/proof.log`

`run.sh` expects an installed `mirc.exe` at `$WINEPREFIX/drive_c/mIRC/mirc.exe`
(or set `MIRC_EXE`). Install once with
`wine tools/proof/deps/mirc-installer.exe /S` inside the prefix.

## Notes

- The 2009 baseline DLL is fetched from the mirc scripts repo at CI time;
  for local runs pass its path as the `dll-path` argument.
- `MSQLITE_MRC` env overrides the alias file fetch for offline runs.
