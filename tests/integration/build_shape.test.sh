#!/usr/bin/env bash
# Suite 11 — Build Shape (TERM-BUILD-01, TERM-BUILD-02)
# Usage: bash tests/integration/build_shape.test.sh
#
# Terminal mode is POSIX-only: termios, select() on stdin and SIGWINCH have no
# MSVC equivalent, so src/terminal/* is left out of the Windows build entirely
# and main.cpp reports the unsupported platform at runtime instead.
#
# That arrangement was asserted in AGENTS.md and checked by nothing. It is also
# the one property of this feature that CI cannot catch by building, because
# there is no Windows job — so the only honest check is a negative one, and
# this file is it. Neither case needs Qt, a toolchain for Windows, or the
# built binary; both read the tree.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PASS=0
FAIL=0

assert_pass() { echo "  PASS  $1"; PASS=$((PASS + 1)); }
assert_fail() { echo "  FAIL  $1${2:+ — $2}"; FAIL=$((FAIL + 1)); }

# ── TERM-BUILD-01: runTerminal is compiled out under Q_OS_WIN ───────────────
#
# The #include lines are stripped before preprocessing on purpose. What is
# under test is main.cpp's *own* conditional structure, and feeding the real
# includes to the preprocessor with Q_OS_WIN forced on a Linux host would drag
# Qt down Windows-only paths (down to <windows.h>) and fail for reasons that
# say nothing about this file. Stripping them leaves exactly the #ifdef /
# #ifndef pairs this case exists to check, and needs no Qt at all.
#
# Both directions are asserted. "0 hits with Q_OS_WIN" alone would also pass if
# someone deleted the call, renamed the function, or broke the grep.
echo "=== TERM-BUILD-01: terminal mode is compiled out on Windows ==="
preprocess() {
  sed '/^#include/d' "$ROOT/src/main.cpp" | g++ -E "$@" -x c++ - 2>/dev/null
}

WIN_HITS=$(preprocess -DQ_OS_WIN | grep -c runTerminal || true)
POSIX_HITS=$(preprocess | grep -c runTerminal || true)

if [ "$WIN_HITS" -eq 0 ] && [ "$POSIX_HITS" -ge 1 ]; then
  assert_pass "TERM-BUILD-01: runTerminal survives on POSIX ($POSIX_HITS) and is gone under Q_OS_WIN"
else
  assert_fail "TERM-BUILD-01: Q_OS_WIN hits=$WIN_HITS (want 0), POSIX hits=$POSIX_HITS (want >=1)"
fi

# ── TERM-BUILD-02: every terminal source sits behind if(NOT WIN32) ──────────
#
# Configuring with -DCMAKE_SYSTEM_NAME=Windows would need a Windows toolchain,
# so the check is on the CMakeLists.txt instead — which is where the realistic
# regression is anyway: a new src/terminal/*.cpp appended to the unconditional
# target_sources() block above the guard. That compiles fine everywhere except
# the platform nobody here builds for.
#
# Both bounds of the guard are checked, and so is the reference count. A "no
# strays" assertion alone passes vacuously if every src/terminal/ line is
# deleted or the grep breaks — the same trap TERM-BUILD-01 spells out.
assert_guarded() {
  local id="$1" file="$2"

  local start
  start=$(grep -n '^if(NOT WIN32)' "$file" | head -1 | cut -d: -f1 || true)
  if [ -z "$start" ]; then
    assert_fail "$id: no 'if(NOT WIN32)' block in $(basename "$file") at all"
    return
  fi
  local end
  end=$(awk -v s="$start" 'NR > s && /^endif\(\)/ { print NR; exit }' "$file")
  if [ -z "$end" ]; then
    assert_fail "$id: the 'if(NOT WIN32)' block in $(basename "$file") is never closed"
    return
  fi

  # Every line naming a terminal source, and the subset of them outside the
  # guard. Anything in that subset is built on Windows and cannot compile there.
  local all outside="" line
  all=$(grep -n 'src/terminal/' "$file" | cut -d: -f1 | sort -n)
  for line in $all; do
    if [ "$line" -lt "$start" ] || [ "$line" -gt "$end" ]; then
      outside="$outside $(sed -n "${line}p" "$file" | tr -d ' ')"
    fi
  done

  local count
  count=$(wc -w <<<"$all")
  if [ -n "$outside" ]; then
    assert_fail "$id: outside the guard (lines $start-$end):$outside"
  elif [ "$count" -lt 1 ]; then
    assert_fail "$id: no src/terminal/ references in $(basename "$file") at all"
  else
    assert_pass "$id: all $count src/terminal/ references are inside if(NOT WIN32)"
  fi
}

echo "=== TERM-BUILD-02: no terminal source escapes the WIN32 guard ==="
assert_guarded "TERM-BUILD-02" "$ROOT/CMakeLists.txt"

# The unit test targets carry the same restriction for the same reason —
# tests/unit builds terminal_ui.cpp, which does not exist on Windows either.
echo "=== TERM-BUILD-02b: the terminal unit targets are guarded too ==="
assert_guarded "TERM-BUILD-02b" "$ROOT/tests/unit/CMakeLists.txt"

# Summary
echo ""
echo "Build Shape Tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
