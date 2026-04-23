#!/usr/bin/env bash
# Suite 10 — Port Layout & Startup Tests
# Usage: ANOA_BINARY=./build/anoa-browser bash tests/integration/port_layout.test.sh
set -euo pipefail

BINARY="${ANOA_BINARY:-./build/anoa-browser}"
PORT="${ANOA_PORT:-9222}"
WS_PORT=$((PORT + 2))
PASS=0
FAIL=0
PROC_PID=""

wait_for_port() {
  local port=$1 timeout=${2:-15}
  local start=$SECONDS
  while ! nc -z 127.0.0.1 "$port" 2>/dev/null; do
    [ $((SECONDS - start)) -ge "$timeout" ] && return 1
    sleep 0.2
  done
}

start_browser() {
  local p=$1; shift
  local extra_args=("$@")
  PROC_PID=""
  QPA_PLATFORM=offscreen "$BINARY" \
    --headless --no-sandbox "--port=$p" "${extra_args[@]}" &
  PROC_PID=$!
  if ! wait_for_port "$p" 15; then
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

# PORT-01: Default ports — HTTP on PORT and WS on PORT+2
echo "=== PORT-01: HTTP and WS ports are listening ==="
if start_browser "$PORT"; then
  HTTP_UP=false; WS_UP=false
  nc -z 127.0.0.1 "$PORT" 2>/dev/null && HTTP_UP=true
  nc -z 127.0.0.1 "$WS_PORT" 2>/dev/null && WS_UP=true
  if $HTTP_UP && $WS_UP; then
    assert_pass "PORT-01: HTTP port $PORT and WS port $WS_PORT are listening"
  else
    assert_fail "PORT-01: HTTP=$HTTP_UP WS=$WS_UP"
  fi
  stop_browser
else
  assert_fail "PORT-01: Browser failed to start"
fi

# PORT-02: Custom port — HTTP on 8800, WS on 8802
echo "=== PORT-02: Custom port layout ==="
CUSTOM_PORT=8800
if start_browser "$CUSTOM_PORT"; then
  HTTP_UP=false; WS_UP=false
  nc -z 127.0.0.1 "$CUSTOM_PORT" 2>/dev/null && HTTP_UP=true
  nc -z 127.0.0.1 $((CUSTOM_PORT + 2)) 2>/dev/null && WS_UP=true
  if $HTTP_UP && $WS_UP; then
    assert_pass "PORT-02: Custom port layout: HTTP=$CUSTOM_PORT WS=$((CUSTOM_PORT+2))"
  else
    assert_fail "PORT-02: HTTP=$HTTP_UP WS=$WS_UP on custom port $CUSTOM_PORT"
  fi
  # Cleanup — use the custom proc PID
  kill "$PROC_PID" 2>/dev/null || true
  wait "$PROC_PID" 2>/dev/null || true
  PROC_PID=""
else
  assert_fail "PORT-02: Browser failed to start on custom port $CUSTOM_PORT"
fi

# PORT-03: Port conflict detection — pre-bind port, then try to start binary
echo "=== PORT-03: Port conflict detection ==="
# Pre-bind port with netcat in listen mode
nc -l 127.0.0.1 "$PORT" &>/dev/null &
NC_PID=$!
sleep 0.3
TMPOUT=$(mktemp)
QPA_PLATFORM=offscreen "$BINARY" \
  --headless --no-sandbox "--port=$PORT" \
  >"$TMPOUT" 2>&1 &
SUBPID=$!
# Give it time to try to bind and fail
sleep 3
if kill -0 "$SUBPID" 2>/dev/null; then
  # Still running even with port conflict — unexpected
  kill "$SUBPID" 2>/dev/null || true
  wait "$SUBPID" 2>/dev/null || true
  # The HTTP server may have failed silently; check /json health
  # This is a soft assertion since Qt doesn't always hard-exit on bind failure
  assert_fail "PORT-03: Binary continued running despite port conflict (may be soft error)"
else
  wait "$SUBPID"
  EXIT_CODE=$?
  if [ "$EXIT_CODE" -ne 0 ]; then
    assert_pass "PORT-03: Binary exited non-zero when port was already in use"
  else
    assert_fail "PORT-03: Binary exited 0 despite port conflict"
  fi
fi
kill "$NC_PID" 2>/dev/null || true
wait "$NC_PID" 2>/dev/null || true
rm -f "$TMPOUT"

# PORT-04: /json/list non-empty after startup (startup navigation)
echo "=== PORT-04: /json/list returns at least one target ==="
if start_browser "$PORT"; then
  BODY=$(curl -sf "http://localhost:$PORT/json/list" 2>/dev/null || echo "[]")
  COUNT=$(echo "$BODY" | python3 -c "import sys,json; print(len(json.load(sys.stdin)))" 2>/dev/null || echo "0")
  if [ "$COUNT" -ge 1 ]; then
    assert_pass "PORT-04: /json/list has $COUNT target(s)"
  else
    assert_fail "PORT-04: /json/list returned empty array or error"
  fi
  stop_browser
else
  assert_fail "PORT-04: Browser failed to start"
fi

# PORT-05: Headless mode on CI (no $DISPLAY)
echo "=== PORT-05: Headless mode works without display ==="
SAVED_DISPLAY="${DISPLAY:-}"
export DISPLAY=""
if start_browser "$PORT"; then
  HTTP_STATUS=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:$PORT/json/version" || echo "000")
  if [ "$HTTP_STATUS" = "200" ]; then
    assert_pass "PORT-05: Headless mode works without DISPLAY"
  else
    assert_fail "PORT-05: HTTP status $HTTP_STATUS without DISPLAY"
  fi
  stop_browser
else
  assert_fail "PORT-05: Browser failed to start without DISPLAY"
fi
[ -n "$SAVED_DISPLAY" ] && export DISPLAY="$SAVED_DISPLAY" || unset DISPLAY

# PORT-07: --no-sandbox flag accepted without crash
echo "=== PORT-07: --no-sandbox flag accepted ==="
if start_browser "$PORT" "--no-sandbox"; then
  HTTP_STATUS=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:$PORT/json/version" || echo "000")
  if [ "$HTTP_STATUS" = "200" ]; then
    assert_pass "PORT-07: --no-sandbox accepted, HTTP still responds"
  else
    assert_fail "PORT-07: HTTP status $HTTP_STATUS with --no-sandbox"
  fi
  stop_browser
else
  assert_fail "PORT-07: Browser failed to start with --no-sandbox"
fi

# PORT-INVALID-LOW: --port 0 exits with code 1 (CFG-03)
echo "=== PORT-INVALID-LOW: --port 0 exits with code 1 ==="
TMPOUT=$(mktemp)
QPA_PLATFORM=offscreen "$BINARY" --headless --no-sandbox --port 0 >"$TMPOUT" 2>&1 || EXIT_CODE=$?
EXIT_CODE=${EXIT_CODE:-0}
if [ "$EXIT_CODE" -eq 1 ]; then
  assert_pass "PORT-INVALID-LOW: --port 0 exits 1 (CFG-03)"
else
  assert_fail "PORT-INVALID-LOW: Expected exit 1, got $EXIT_CODE (CFG-03)"
fi
rm -f "$TMPOUT"

# PORT-INVALID-HIGH: --port 99999 exits with code 1 (CFG-04)
echo "=== PORT-INVALID-HIGH: --port 99999 exits with code 1 ==="
TMPOUT=$(mktemp)
QPA_PLATFORM=offscreen "$BINARY" --headless --no-sandbox --port 99999 >"$TMPOUT" 2>&1 || EXIT_CODE=$?
EXIT_CODE=${EXIT_CODE:-0}
if [ "$EXIT_CODE" -eq 1 ]; then
  assert_pass "PORT-INVALID-HIGH: --port 99999 exits 1 (CFG-04)"
else
  assert_fail "PORT-INVALID-HIGH: Expected exit 1, got $EXIT_CODE (CFG-04)"
fi
rm -f "$TMPOUT"

# Summary
echo ""
echo "Port Layout Tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
