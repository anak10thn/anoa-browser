#!/usr/bin/env bash
# Launcher for the portable Linux bundle.
#
# Layout (relative to this script):
#   anoa-browser                       single binary; `anoa-browser terminal`
#                                      is the viewer subcommand (RPATH $ORIGIN/lib)
#   lib/                               bundled shared libraries + Qt plugins
#   lib/qt6/libexec/QtWebEngineProcess Chromium helper process
#   resources/                         QtWebEngine .pak resource files
#   translations/qtwebengine_locales/  locale .pak files
#   qt.conf                            Qt path overrides (plugins, libexec, …)
set -u

# Resolve the real script location through any chain of symlinks
# (e.g. Homebrew installs bin/anoa-browser -> libexec/anoa-browser.sh).
SOURCE="${BASH_SOURCE[0]}"
while [ -L "$SOURCE" ]; do
  DIR="$(cd "$(dirname "$SOURCE")" >/dev/null 2>&1 && pwd)"
  SOURCE="$(readlink "$SOURCE")"
  [[ "$SOURCE" != /* ]] && SOURCE="$DIR/$SOURCE"
done
DIR="$(cd "$(dirname "$SOURCE")" >/dev/null 2>&1 && pwd)"

# Bundled shared libraries. The binary and every bundled .so already carry an
# $ORIGIN RPATH; LD_LIBRARY_PATH covers dlopen()ed libraries (GL, VA-API, …)
# that resolve outside the RPATH chain.
export LD_LIBRARY_PATH="${DIR}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

# Qt plugin resolution (platforms/, xcbglintegrations/, …). qt.conf next to
# the binary handles this too — the env var keeps things working even when
# the binary is invoked through a path Qt does not derive qt.conf from.
export QT_PLUGIN_PATH="${DIR}/lib${QT_PLUGIN_PATH:+:${QT_PLUGIN_PATH}}"

# QtWebEngine component locations (Qt 6 environment variable names).
export QTWEBENGINEPROCESS_PATH="${DIR}/lib/qt6/libexec/QtWebEngineProcess"
export QTWEBENGINE_RESOURCES_PATH="${DIR}/resources"
export QTWEBENGINE_LOCALES_PATH="${DIR}/translations/qtwebengine_locales"

exec "${DIR}/anoa-browser" "$@"
