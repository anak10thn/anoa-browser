#!/usr/bin/env bash
# Launcher for the portable Linux bundle.
#
# Layout (relative to this script):
#   anoa                       single binary; `anoa terminal`
#                                      is the viewer subcommand (RPATH $ORIGIN/lib)
#   lib/                               bundled shared libraries + Qt plugins
#   lib/qt6/libexec/QtWebEngineProcess Chromium helper process
#   resources/                         QtWebEngine .pak resource files
#   translations/qtwebengine_locales/  locale .pak files
#   qt.conf                            Qt path overrides (plugins, libexec, …)
set -u

# Resolve the real script location through any chain of symlinks
# (e.g. Homebrew installs bin/anoa -> libexec/anoa.sh).
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

# OpenGL comes from the host when the host has it.
#
# lib/gl holds our copies of the GLVND dispatch libraries — libGL, libGLX,
# libGLdispatch, libEGL, libOpenGL. Those exist to find the graphics driver
# installed on the machine they run on, so shipping them in front of the
# system's makes them hunt for the build box's driver instead. On a desktop that
# fails as "qglx_findConfig: Failed to finding matching FBConfig" and Qt aborts.
#
# They cannot simply be left out either: the binary links libOpenGL.so.0, and a
# bare container has no GL packages, where --headless needs none of this to
# begin with. So the fallback is appended only when the host is missing the one
# library the binary cannot start without.
if [ -d "${DIR}/lib/gl" ]; then
  have_host_gl=""
  for d in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib /lib/x86_64-linux-gnu; do
    [ -e "$d/libOpenGL.so.0" ] && { have_host_gl=1; break; }
  done
  if [ -z "$have_host_gl" ]; then
    export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${DIR}/lib/gl"
  fi
fi

# Qt plugin resolution (platforms/, xcbglintegrations/, …). qt.conf next to
# the binary handles this too — the env var keeps things working even when
# the binary is invoked through a path Qt does not derive qt.conf from.
export QT_PLUGIN_PATH="${DIR}/lib${QT_PLUGIN_PATH:+:${QT_PLUGIN_PATH}}"

# QtWebEngine component locations (Qt 6 environment variable names).
export QTWEBENGINEPROCESS_PATH="${DIR}/lib/qt6/libexec/QtWebEngineProcess"
export QTWEBENGINE_RESOURCES_PATH="${DIR}/resources"
export QTWEBENGINE_LOCALES_PATH="${DIR}/translations/qtwebengine_locales"

exec "${DIR}/anoa" "$@"
