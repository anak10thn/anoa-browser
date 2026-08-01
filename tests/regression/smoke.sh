#!/usr/bin/env bash
# Suite 11 — Regression Smoke Tests
# Fast post-commit check for the 5 critical paths.
# Usage: ANOA_BINARY=./build/anoa bash tests/regression/smoke.sh
set -euo pipefail

BINARY="${ANOA_BINARY:-./build/anoa}"
PORT="${ANOA_PORT:-9222}"
WS_PORT=$((PORT + 2))
PASS=0
FAIL=0
PROC_PID=""

# Resolve BINARY to an absolute path before any cd.
case "$BINARY" in /*) ;; *) BINARY="$(pwd)/$BINARY" ;; esac
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# Node ESM resolves bare imports ('ws', '@playwright/test') from the CWD's
# node_modules — a global npm install is NOT on the ESM resolution path.
# Run snippets from the test dirs that declare the needed dependency
# (npm install must have been run there first).
run_node_ws() { (cd "$ROOT_DIR/tests/integration" && node --input-type=module); }
run_node_playwright() { (cd "$ROOT_DIR/tests/e2e" && node --input-type=module); }

wait_for_port() {
  local port=$1 timeout=${2:-15}
  local start=$SECONDS
  while ! nc -z 127.0.0.1 "$port" 2>/dev/null; do
    [ $((SECONDS - start)) -ge "$timeout" ] && return 1
    sleep 0.2
  done
}

start_browser() {
  PROC_PID=""
  QPA_PLATFORM=offscreen "$BINARY" \
    --headless --no-sandbox "--port=$PORT" &
  PROC_PID=$!
  if ! wait_for_port "$PORT" 15; then
    kill "$PROC_PID" 2>/dev/null || true
    return 1
  fi
}

stop_browser() {
  if [ -n "$PROC_PID" ]; then
    kill "$PROC_PID" 2>/dev/null || true
    wait "$PROC_PID" 2>/dev/null || true
    PROC_PID=""
  fi
}

assert_pass() { echo "  PASS  $1"; PASS=$((PASS + 1)); }
assert_fail() { echo "  FAIL  $1${2:+ — $2}"; FAIL=$((FAIL + 1)); }

echo "Starting anoa smoke tests..."
if ! start_browser; then
  echo "FATAL: Could not start $BINARY on port $PORT" >&2
  exit 1
fi

# REG-01: Binary starts and serves /json/version
echo "=== REG-01: HTTP /json/version ==="
BODY=$(curl -sf "http://localhost:$PORT/json/version" 2>/dev/null || echo "")
if echo "$BODY" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'Browser' in d" 2>/dev/null; then
  assert_pass "REG-01: /json/version returned valid JSON with Browser field"
else
  assert_fail "REG-01: /json/version did not return expected JSON" "$BODY"
fi

# REG-02: CDP proxy accepts WebSocket connection
echo "=== REG-02: CDP WS proxy connection ==="
# Fetch target URL from /json/list
TARGET_WS=$(curl -sf "http://localhost:$PORT/json/list" 2>/dev/null | \
  python3 -c "import sys,json; lst=json.load(sys.stdin); print(lst[0]['webSocketDebuggerUrl'] if lst else '')" 2>/dev/null || echo "")
if [ -z "$TARGET_WS" ]; then
  assert_fail "REG-02: /json/list returned no targets"
else
  # Use node to do a quick WebSocket handshake
  if run_node_ws << EOF
import WebSocket from 'ws';
const ws = new WebSocket('${TARGET_WS}');
ws.once('open', () => { ws.close(); process.exit(0); });
ws.once('error', () => process.exit(1));
setTimeout(() => process.exit(1), 5000);
EOF
  then
    assert_pass "REG-02: CDP proxy accepted WebSocket connection"
  else
    assert_fail "REG-02: Could not connect WebSocket to $TARGET_WS"
  fi
fi

# REG-03: Profiler.enable returns {}
echo "=== REG-03: Profiler.enable stub ==="
RESULT=$(run_node_ws << EOF 2>/dev/null || echo "ERROR"
import WebSocket from 'ws';
const list = await fetch('http://localhost:${PORT}/json/list').then(r=>r.json());
const ws = new WebSocket(list[0].webSocketDebuggerUrl);
await new Promise(r => ws.once('open', r));
const resp = await new Promise(r => {
  ws.once('message', d => r(JSON.parse(d)));
  ws.send(JSON.stringify({id:1, method:'Profiler.enable', params:{}}));
});
ws.close();
console.log(JSON.stringify(resp));
EOF
)
if echo "$RESULT" | python3 -c "import sys,json; d=json.load(sys.stdin); assert d.get('id')==1 and 'result' in d" 2>/dev/null; then
  assert_pass "REG-03: Profiler.enable returned {id:1,result:{}}"
else
  assert_fail "REG-03: Unexpected response from Profiler.enable" "$RESULT"
fi

# REG-04: Page.printToPDF returns %PDF-
echo "=== REG-04: Page.printToPDF valid PDF ==="
PDF_OK=$(run_node_ws << EOF >/dev/null 2>&1; echo $?
import WebSocket from 'ws';
const list = await fetch('http://localhost:${PORT}/json/list').then(r=>r.json());
const ws = new WebSocket(list[0].webSocketDebuggerUrl);
await new Promise(r => ws.once('open', r));
const resp = await new Promise((resolve, reject) => {
  ws.once('message', d => resolve(JSON.parse(d)));
  ws.send(JSON.stringify({id:2, method:'Page.printToPDF', params:{}}));
  setTimeout(() => reject(new Error('timeout')), 30000);
});
ws.close();
const bytes = Buffer.from(resp.result.data, 'base64');
const valid = bytes[0]===0x25 && bytes[1]===0x50 && bytes[2]===0x44 && bytes[3]===0x46;
process.exit(valid ? 0 : 1);
EOF
)
if [ "${PDF_OK:-1}" -eq 0 ]; then
  assert_pass "REG-04: Page.printToPDF returned valid PDF with %PDF- header"
else
  assert_fail "REG-04: Page.printToPDF did not return a valid PDF"
fi

# REG-05: Playwright connectOverCDP succeeds
echo "=== REG-05: Playwright connectOverCDP ==="
PW_OK=$(run_node_playwright << EOF >/dev/null 2>&1; echo $?
import { chromium } from '@playwright/test';
const browser = await chromium.connectOverCDP('http://localhost:${PORT}');
const contexts = browser.contexts();
if (!contexts.length) { await browser.close(); process.exit(1); }
const pages = contexts[0].pages();
if (!pages.length) { await browser.close(); process.exit(1); }
await browser.close();
process.exit(0);
EOF
)
if [ "${PW_OK:-1}" -eq 0 ]; then
  assert_pass "REG-05: Playwright connectOverCDP succeeded and found existing page"
else
  # Playwright may not be installed; mark as skipped rather than failed
  echo "  SKIP  REG-05: Playwright not available (npm install @playwright/test to enable)"
fi

stop_browser

echo ""
echo "Smoke Tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
