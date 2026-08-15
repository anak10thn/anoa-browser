#!/usr/bin/env bash
#
# Build anoa-x86_64.AppImage from the portable Linux bundle.
#
# The bundle is already the hard part: one binary, its libraries, the Qt
# plugins, QtWebEngineProcess, the resource paks, and a launcher that decides
# which libraries come from the host. An AppImage is that directory plus three
# things — an AppRun, a .desktop file and an icon — squashed into a
# self-mounting archive.
#
# So this deliberately does NOT use linuxdeploy or its Qt plugin. Those exist to
# work out what to bundle, which release.yml has already done with more care
# than a generic tool can: it knows that libGL, libX11 and libstdc++ must come
# from the host, and a tool that helpfully bundles them would put back the
# "Could not initialize GLX" this project spent a release fixing.
#
# Usage:
#   scripts/build-appimage.sh <bundle-dir> [output.AppImage]
#
# where <bundle-dir> is the `anoa` directory from anoa-linux-x86_64.tar.gz, or
# any directory with the same shape.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE="${1:?usage: build-appimage.sh <bundle-dir> [output.AppImage]}"
OUTPUT="${2:-anoa-x86_64.AppImage}"

if [ ! -x "$BUNDLE/anoa" ] || [ ! -f "$BUNDLE/anoa.sh" ]; then
    echo "not an anoa bundle: $BUNDLE" >&2
    echo "expected anoa and anoa.sh inside it" >&2
    exit 2
fi

VERSION="$(sed -n 's/.*project(anoa VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
VERSION="${VERSION:-0.0.0}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
APPDIR="$WORK/AppDir"

echo "==> assembling AppDir (anoa $VERSION)"
mkdir -p "$APPDIR"
cp -a "$BUNDLE"/. "$APPDIR"/

# AppRun is the entry point. It is a wrapper rather than a symlink to anoa.sh
# because the AppImage runtime invokes it with the user's arguments and nothing
# else: $APPDIR is the only reliable way to find the payload, and anoa.sh's own
# BASH_SOURCE resolution would otherwise be resolving a path inside a mount that
# moves on every run.
cat > "$APPDIR/AppRun" <<'APPRUN'
#!/usr/bin/env bash
# The AppImage runtime mounts this read-only under /tmp/.mount_XXXXXX and runs
# AppRun from there, so nothing may be written inside $APPDIR. anoa.sh already
# obeys that: the only thing it creates is a symlink farm under the user's
# cache, which is where it belongs anyway.
HERE="$(dirname "$(readlink -f "${0}")")"
exec "${HERE}/anoa.sh" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"

# The desktop entry. StartupWMClass matters for a Qt app: without it a window
# manager files the window under a name the launcher does not recognise, so the
# icon in the dock is a generic one.
cat > "$APPDIR/anoa.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=anoa
GenericName=Browser
Comment=A browser you drive from a script, a terminal, or a window
Exec=AppRun %u
Icon=anoa
Terminal=false
Categories=Network;WebBrowser;Development;
StartupWMClass=anoa
MimeType=text/html;text/xml;application/xhtml+xml;x-scheme-handler/http;x-scheme-handler/https;
X-AppImage-Version=$VERSION
DESKTOP

# appimagetool wants the icon at the AppDir root under the name the .desktop
# gives, and again under usr/share/icons for desktop integration to find it.
if [ -f "$ROOT/docs/anoa-logo.png" ]; then
    cp "$ROOT/docs/anoa-logo.png" "$APPDIR/anoa.png"
    mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"
    cp "$ROOT/docs/anoa-logo.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/anoa.png"
else
    echo "    no docs/anoa-logo.png; writing a placeholder icon" >&2
    # A 1x1 PNG. appimagetool refuses an AppDir with no icon at all, and a
    # missing file should not be the thing that fails a release build.
    printf '\211PNG\r\n\032\n\0\0\0\rIHDR\0\0\0\1\0\0\0\1\10\6\0\0\0\37\25\304\211\0\0\0\nIDATx\234c\370\17\0\1\1\1\0\30\335\215\260\0\0\0\0IEND\256B`\202' \
        > "$APPDIR/anoa.png"
fi

# Desktop integration also looks here.
mkdir -p "$APPDIR/usr/share/applications"
cp "$APPDIR/anoa.desktop" "$APPDIR/usr/share/applications/anoa.desktop"

echo "==> fetching appimagetool"
TOOL="$WORK/appimagetool"
TOOL_URL="https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage"
if [ -n "${APPIMAGETOOL:-}" ] && [ -x "${APPIMAGETOOL}" ]; then
    TOOL="${APPIMAGETOOL}"
else
    curl -fsSL -o "$TOOL" "$TOOL_URL"
    chmod +x "$TOOL"
fi

# The runtime decides where this AppImage will run at all, so it is not left to
# the default.
#
# appimagetool embeds AppImageKit's own type-2 runtime, which dlopen()s
# libfuse.so.2. Ubuntu has shipped fuse3 and NOT libfuse2 since 22.04, so that
# runtime fails on a current Ubuntu with:
#
#   dlopen(): error loading libfuse.so.2
#
# — before a single line of anoa runs. Verified on Ubuntu 24.04: libfuse.so.2
# absent, libfuse3.so present. Telling users to `apt install libfuse2t64` would
# make a self-contained single file depend on a package the distro removed.
#
# The type2-runtime build links its FUSE statically, so it needs nothing from
# the host but a kernel with /dev/fuse.
RUNTIME="$WORK/runtime-x86_64"
RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64"
if [ -n "${APPIMAGE_RUNTIME:-}" ] && [ -f "${APPIMAGE_RUNTIME}" ]; then
    RUNTIME="${APPIMAGE_RUNTIME}"
else
    curl -fsSL -o "$RUNTIME" "$RUNTIME_URL"
fi
# A truncated or HTML error page here produces an AppImage that fails with
# something far less obvious than a download error, so it is checked now.
if [ ! -s "$RUNTIME" ] || [ "$(head -c 4 "$RUNTIME" | tr -d '\0')" != "$(printf '\177ELF')" ]; then
    echo "downloaded runtime is not an ELF binary: $RUNTIME_URL" >&2
    exit 1
fi
chmod +x "$RUNTIME"

# --appimage-extract-and-run, always: appimagetool is itself an AppImage, and
# a build machine (a container, a CI runner) usually has no FUSE. Waiting to
# find that out at release time is the kind of failure this flag exists for.
echo "==> packing $OUTPUT"
ARCH=x86_64 "$TOOL" --appimage-extract-and-run \
    --no-appstream \
    --runtime-file "$RUNTIME" \
    "$APPDIR" "$OUTPUT"

chmod +x "$OUTPUT"
echo "==> $(basename "$OUTPUT")  $(du -h "$OUTPUT" | cut -f1)"
