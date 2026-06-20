#!/usr/bin/env bash
# Regenerate data from a real run, serve the folder, and print index.html to a
# PDF with headless Chrome (QUANT_SHOWCASE_STYLE.md §11).
#
# The page inlines its data (data.js) and renders charts + KaTeX synchronously
# during deferred-script execution, so everything is drawn before the load event
# and Chrome's one-shot --print-to-pdf captures a fully-rendered page. (Chrome's
# --virtual-time-budget and the CDP printToPDF path are both unreliable on recent
# macOS builds; the plain command-line print is what works.)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
PORT="${PORT:-8033}"
OUT="$HERE/deterministic-allocator.pdf"
HOST=127.0.0.1

CHROME=""
for c in \
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  "/Applications/Chromium.app/Contents/MacOS/Chromium" \
  "$(command -v google-chrome || true)" \
  "$(command -v chromium || true)"; do
  if [ -n "$c" ] && [ -x "$c" ]; then CHROME="$c"; break; fi
done
[ -z "$CHROME" ] && { echo "error: no Chrome/Chromium found" >&2; exit 1; }

if [ "${SKIP_EXPORT:-0}" = "1" ] && [ -f "$HERE/data.js" ]; then
  echo "[build_pdf] 1/3 reusing existing data (SKIP_EXPORT=1)"
else
  echo "[build_pdf] 1/3 generating data from a real allocator run"
  PYTHONPATH="$ROOT/src" python3 "$HERE/export_data.py"
fi

echo "[build_pdf] 1b/3 pre-rendering math (KaTeX, server-side) → print.html"
node "$HERE/render_math.cjs" "$HERE/index.html" "$HERE/print.html"

echo "[build_pdf] 2/3 serving $HERE on :$PORT (threaded)"
# A threaded server: the rendered page pulls ~dozen web-font files in parallel
# and a single-threaded server stalls the load event.
python3 -c "
import sys, functools
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
h = functools.partial(SimpleHTTPRequestHandler, directory='$HERE')
ThreadingHTTPServer(('127.0.0.1', $PORT), h).serve_forever()
" >/dev/null 2>&1 &
SRV=$!
trap 'kill "$SRV" 2>/dev/null || true' EXIT
sleep 1

echo "[build_pdf] 3/3 printing → $OUT"
rm -f "$OUT"
# The headless renderer is occasionally reaped (or hangs) in sandboxes; run each
# attempt with a watchdog and retry a few times.
for attempt in 1 2 3 4 5 6; do
  UDD="$(mktemp -d)"
  "$CHROME" --headless=new --disable-gpu --no-pdf-header-footer \
    --disable-background-networking --disable-sync --disable-component-update \
    --no-first-run --no-default-browser-check --window-size=760,1100 \
    --user-data-dir="$UDD" \
    --print-to-pdf="$OUT" "http://$HOST:$PORT/print.html" >/dev/null 2>&1 &
  CH=$!
  # Watchdog: wait up to 20s for the PDF, then kill this attempt.
  w=0
  while [ $w -lt 20 ] && [ ! -s "$OUT" ]; do sleep 1; w=$((w+1)); done
  kill -9 "$CH" 2>/dev/null || true
  pkill -9 -f "remote-debugging\|--print-to-pdf=$OUT" 2>/dev/null || true
  [ -s "$OUT" ] && break
  echo "[build_pdf]   attempt $attempt produced no PDF; retrying"
  sleep 1
done

[ -s "$OUT" ] || { echo "[build_pdf] FAILED to produce $OUT"; exit 1; }
echo "[build_pdf] done: $OUT"

if python3 -c "import fitz" 2>/dev/null; then
  python3 "$HERE/verify_pdf.py" "$OUT" || true
fi
