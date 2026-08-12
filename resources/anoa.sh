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

# Some libraries have to come from the host whenever the host has them.
#
# lib/hostfirst holds our copies of the GLVND dispatch libraries — libGL,
# libGLX, libGLdispatch, libEGL, libOpenGL — together with libX11 and
# libstdc++. The dispatch layers exist to find the graphics driver installed on
# the machine they run on, so shipping them in front of the system's makes them
# hunt for the build box's driver instead. libX11 and libstdc++ are there
# because the host's Mesa gets dlopen()ed into this process and is built
# against the host's copies of both.
#
# Either way the symptom is the same, and it is what a desktop user sees:
#
#   qt.glx: qglx_findConfig: Failed to finding matching FBConfig ...
#   Could not initialize GLX
#   Aborted (core dumped)
#
# They cannot simply be left out either: the binary links libOpenGL.so.0 and
# libEGL.so.1, and a bare container has no GL packages at all, where --headless
# needs none of this to begin with.
#
# So the choice is per library, not per directory. GL alone spans separately
# installable packages — libgl1, libglx0, libopengl0, libegl1, libglvnd0 — and
# any subset can be present. Deciding the whole directory's fate from one
# sentinel file is wrong in both directions, and both were seen on one Ubuntu
# 24.04 box: with only libGL.so.1 installed ours shadowed the host's and GLX
# died; with only libOpenGL.so.0 installed the host's was kept and the binary
# would not start for want of libEGL.so.1.
#
# ldconfig's cache is the authority on what the system has, rather than a list
# of directories guessed here.
if [ -d "${DIR}/lib/hostfirst" ]; then
  ldc="$(command -v ldconfig || echo /sbin/ldconfig)"
  cache="$("$ldc" -p 2>/dev/null || true)"
  missing=""
  for so in "${DIR}"/lib/hostfirst/*.so.*; do
    [ -e "$so" ] || continue
    base="${so##*/}"
    case "$cache" in
      *"	${base} "*) ;;             # the host has it — leave it alone
      *) missing="${missing} ${base}" ;;
    esac
  done
  if [ -n "$missing" ]; then
    # Only the gaps go on the path, through a directory of symlinks rebuilt
    # every launch so it cannot go stale when GL packages are installed later.
    shim="${XDG_CACHE_HOME:-${HOME:-/tmp}/.cache}/anoa/hostfirst"
    if mkdir -p "$shim" 2>/dev/null && rm -f "$shim"/*.so.* 2>/dev/null; then
      for base in $missing; do
        ln -sf "${DIR}/lib/hostfirst/${base}" "${shim}/${base}" 2>/dev/null || true
      done
      export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${shim}"
    else
      # Nowhere writable: the whole directory is still better than not starting.
      export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${DIR}/lib/hostfirst"
    fi
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
