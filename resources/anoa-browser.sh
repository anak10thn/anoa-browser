#!/bin/sh
SELF="$(readlink -f "$0")"
DIR="$(dirname "$SELF")"
export LD_LIBRARY_PATH="${DIR}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export QT_PLUGIN_PATH="${DIR}/lib${QT_PLUGIN_PATH:+:${QT_PLUGIN_PATH}}"
export QTWEBENGINEPROCESS_PATH="${DIR}/lib/qt6/libexec/QtWebEngineProcess"
export QTWEBENGINERESOURCEPATH="${DIR}/resources"
export QTWEBENGINE_LOCALE="${DIR}/translations/qtwebengine_locales"
exec "${DIR}/anoa-browser" "$@"