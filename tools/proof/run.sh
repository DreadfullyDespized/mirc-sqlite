#!/bin/bash
set -euo pipefail
VARIANT=${1:?usage: run.sh old|new dll-path}
DLL_SRC=${2:?usage: run.sh old|new dll-path}
case "$VARIANT" in
  old|new) ;;
  *) echo "variant must be old or new, got: $VARIANT" >&2; exit 2 ;;
esac
[ -f "$DLL_SRC" ] || { echo "dll not found: $DLL_SRC" >&2; exit 2; }
DLL=$(readlink -f "$DLL_SRC")
HERE=$(cd "$(dirname "$0")" && pwd)
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-mirc-proof}
export WINEARCH=win64
export WINEDEBUG=-all
MIRC=${MIRC_EXE:-$WINEPREFIX/drive_c/mIRC/mirc.exe}
[ -f "$MIRC" ] || { echo "mirc.exe not found: $MIRC (set MIRC_EXE)" >&2; exit 2; }
D="${OUT:-/tmp/mirc-sqlite-proof}/$VARIANT"
rm -rf "$D"
mkdir -p "$D/scripts/mrcs"
if [ -n "${MSQLITE_MRC:-}" ]; then
  cp "$MSQLITE_MRC" "$D/scripts/mrcs/msqlite.mrc"
else
  curl -sSL --retry 3 --max-time 120 "https://raw.githubusercontent.com/DreadfullyDespized/mirc/main/scripts/mrcs/msqlite.mrc" -o "$D/scripts/mrcs/msqlite.mrc"
fi
cp "$DLL" "$D/scripts/mrcs/msqlite.dll"
cp "$HERE/proof.mrc" "$D/proof.mrc"
w() { printf 'Z:%s' "$(readlink -f "$1" | sed 's#/#\\#g')"; }
cat > "$D/cfg.mrc" <<EOF
alias t.cfg {
  set %t.log $(w "$D/proof.log")
  set %t.dbdir $(w "$D")
  set %t.variant $VARIANT
}
EOF
printf '[rfiles]\r\nn0=%s\r\nn1=%s\r\nn2=%s\r\n' "$(w "$D/cfg.mrc")" "$(w "$D/scripts/mrcs/msqlite.mrc")" "$(w "$D/proof.mrc")" > "$D/mirc.ini"
sha256sum "$MIRC" "$D/scripts/mrcs/msqlite.dll" "$D/scripts/mrcs/msqlite.mrc" | sed "s#$D/##" > "$D/sha256.txt"
set +e
DISP=${WDISPLAY:-:359}
[ "$DISP" = ":93" ] && { echo "refusing display :93" >&2; exit 3; }
[ -e "/tmp/.X11-unix/X${DISP#:}" ] && { echo "display $DISP already in use, refusing to share" >&2; exit 3; }
Xvfb "$DISP" -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &
XPID=$!
sleep 2
kill -0 $XPID 2>/dev/null || { echo "Xvfb $DISP failed to start" >&2; exit 3; }
export DISPLAY=$DISP
timeout "${WTIMEOUT:-180}" wine "$MIRC" -noconnect -r"$(w "$D")" > "$D/wine-stdout.log" 2>&1 &
MPID=$!
NCLICK=0
T=0
while kill -0 $MPID 2>/dev/null; do
  if [ $((T % 10)) -eq 0 ]; then
    echo "t=$T windows:" >> "$D/dialogs.txt"
    xdotool search --name '.' getwindowname %@ 2>/dev/null >> "$D/dialogs.txt" || echo "xdotool search failed" >> "$D/dialogs.txt"
  fi
  if xdotool search --name '.' getwindowname %@ 2>/dev/null | grep -qvE '^(mIRC.*|About mIRC|Default IME|Wine.*|)$'; then
    echo "foreign window on $DISP, no input sent t=$T" >> "$D/dialogs.txt"
  else
    WID=$(xdotool search --name '^About mIRC$' 2>/dev/null | head -1)
    if [ -n "$WID" ]; then
      sleep 1
      xdotool mousemove --window "$WID" 241 343 click 1 2>/dev/null
      NCLICK=$((NCLICK + 1))
      echo "clicked About mIRC t=$T" >> "$D/dialogs.txt"
    else
      ABT=$(xdotool search --name 'mIRC' 2>/dev/null | head -3)
      if [ -n "$ABT" ]; then
        echo "t=$T mIRC-ish windows: $ABT" >> "$D/dialogs.txt"
      fi
    fi
    UPD=$(xdotool search --name '^mIRC Update$' 2>/dev/null | head -1)
    if [ -n "$UPD" ]; then
      xdotool windowactivate "$UPD" 2>/dev/null
      xdotool key Escape 2>/dev/null
      echo "dismissed mIRC Update t=$T" >> "$D/dialogs.txt"
    fi
  fi
  T=$((T + 1))
  sleep 1
done
wait $MPID
RC=$?
kill $XPID 2>/dev/null
wait $XPID 2>/dev/null
wineserver -k 2>/dev/null
echo "about_ok_clicks=$NCLICK" >> "$D/wine-stdout.log"
echo "mirc_exit=$RC" > "$D/exit.txt"
cat "$D/exit.txt"
if [ -f "$D/proof.log" ]; then
  cat "$D/proof.log"
else
  echo "NO proof.log"
fi
