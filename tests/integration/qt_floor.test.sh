#!/usr/bin/env bash
# Suite 12 — Qt API Floor (TERM-BUILD-03)
# Usage: QT_FLOOR_PREFIX=/opt/Qt/6.4.3/gcc_64 bash tests/integration/qt_floor.test.sh
#
# CMakeLists.txt declares `find_package(Qt6 6.4 REQUIRED ...)` and distro
# builds really do ship 6.4, but every CI job pins 6.7.3 — so nothing between
# the two numbers is ever exercised. src/terminal/ is the largest block of Qt
# API this repo has ever added at once, and QWebSocket::errorOccurred (6.5+)
# already needed a QT_VERSION_CHECK fallback once. This is the check that
# catches the next \since 6.5 before a user on a distro Qt does.
#
# Syntax-only, and deliberately so: it needs QtCore, QtGui, QtNetwork and
# QtWebSockets headers and nothing else. No WebEngine, no link step, no build —
# which is what makes a second Qt version affordable to install at all. It
# cannot catch an ABI or linker difference; it catches the one thing that
# actually bites, which is calling an API that does not exist yet.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PREFIX="${QT_FLOOR_PREFIX:-${QT_ROOT_DIR:-}}"
PASS=0
FAIL=0

assert_pass() { echo "  PASS  $1"; PASS=$((PASS + 1)); }
assert_fail() { echo "  FAIL  $1${2:+ — $2}"; FAIL=$((FAIL + 1)); }

if [ -z "$PREFIX" ] || [ ! -d "$PREFIX/include/QtCore" ]; then
  echo "QT_FLOOR_PREFIX is not set to a Qt installation (got '${PREFIX:-<unset>}')" >&2
  echo "Install the floor with: aqt install-qt linux desktop 6.4.3 gcc_64 -m qtwebsockets -O /opt/Qt" >&2
  exit 2
fi

# The floor the sources are allowed to assume, read off the Qt being checked so
# a mis-pointed prefix reports itself instead of passing quietly against 6.7.
QT_VERSION=$(sed -n 's/.*QT_VERSION_STR *"\([0-9.]*\)".*/\1/p' "$PREFIX/include/QtCore/qconfig.h" \
             2>/dev/null | head -1)
if [ -z "$QT_VERSION" ]; then
  QT_VERSION=$(basename "$(dirname "$PREFIX")")
fi
echo "=== TERM-BUILD-03: src/terminal against Qt $QT_VERSION ==="

INCLUDES=(-I "$ROOT/src" -I "$PREFIX/include")
for module in QtCore QtGui QtNetwork QtWebSockets; do
  INCLUDES+=(-I "$PREFIX/include/$module")
done

for source in "$ROOT"/src/terminal/*.cpp; do
  name="src/terminal/$(basename "$source")"
  # The embedded viewer's backend reaches into the browser through
  # anoa_browser.h, which needs the widget stack this check deliberately does
  # not install — the point of the check is that it costs nothing but QtCore,
  # QtGui, QtNetwork and QtWebSockets. It has never been checkable here and has
  # been failing the job rather than being named as out of scope.
  #
  # Nothing is lost: the API floor question is about \since-6.5 calls in the
  # viewer's own code, and this file's Qt surface is grab(), width() and
  # height() — 4.x-era API by way of a class the real build compiles anyway.
  if [ "$(basename "$source")" = "inprocess_frame_backend.cpp" ]; then
    echo "  SKIP  TERM-BUILD-03: $name (needs the widget stack; see comment)"
    continue
  fi
  if ERR=$(g++ -fsyntax-only -std=c++17 -fPIC "${INCLUDES[@]}" "$source" 2>&1); then
    assert_pass "TERM-BUILD-03: $name compiles against Qt $QT_VERSION"
  else
    assert_fail "TERM-BUILD-03: $name" "$(head -5 <<<"$ERR")"
  fi
done

# Summary
echo ""
echo "Qt Floor Tests (Qt $QT_VERSION): $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
