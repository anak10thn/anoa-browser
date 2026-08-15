#!/usr/bin/env bash
#
# Installs the portable Linux bundle into a user prefix — no root, no package
# manager. The bundle carries its own Qt, so nothing is installed system-wide
# and nothing conflicts with a distribution Qt.
#
#   curl -fsSL https://raw.githubusercontent.com/porcupine-md/anoa-browser/master/scripts/install-linux.sh | bash
#
# Layout it creates:
#   ~/.local/lib/anoa/     the unpacked bundle (binary, lib/, resources/)
#   ~/.local/bin/anoa  ->  ../lib/anoa/anoa.sh
#
# The symlink points at the *launcher*, never at the raw binary: the launcher is
# what exports LD_LIBRARY_PATH, QT_PLUGIN_PATH and the QtWebEngine paths that
# make the bundle self-contained. Linking the binary directly produces a process
# that starts and then fails to find QtWebEngineProcess.

set -eu

REPO="porcupine-md/anoa-browser"

# Which build this machine needs. Named rather than guessed: downloading the
# wrong one produces "cannot execute binary file" several steps later, with
# nothing pointing back at the download.
case "$(uname -m)" in
    x86_64|amd64)   ARCH="x86_64"  ;;
    aarch64|arm64)  ARCH="aarch64" ;;
    *)
        echo "install-linux.sh: no prebuilt bundle for $(uname -m)" >&2
        echo "  x86_64 and aarch64 are published; build from source:" >&2
        echo "  https://github.com/porcupine-md/anoa-browser#building-from-source" >&2
        exit 1
        ;;
esac
ASSET="anoa-linux-${ARCH}.tar.gz"
PREFIX="${HOME}/.local"
VERSION=""
UNINSTALL=0

usage() {
    cat <<EOF
Usage: install-linux.sh [options]

  --version <vX.Y.Z>  Install this release (default: the latest)
  --prefix <dir>      Install under this prefix (default: \$HOME/.local)
  --uninstall         Remove an installation made by this script
  -h, --help          Show this help

Installs to <prefix>/lib/anoa with a launcher at <prefix>/bin/anoa.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="${2:-}"; shift 2 ;;
        --prefix)  PREFIX="${2:-}";  shift 2 ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "install-linux.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

BUNDLE_DIR="${PREFIX}/lib/anoa"
BIN_DIR="${PREFIX}/bin"
LINK="${BIN_DIR}/anoa"

say()  { printf '%s\n' "$*"; }
warn() { printf '%s\n' "$*" >&2; }
die()  { printf 'install-linux.sh: %s\n' "$*" >&2; exit 1; }

if [ "$UNINSTALL" -eq 1 ]; then
    removed=0
    # Only remove the symlink if it is ours. Someone may have put their own
    # anoa on the PATH, and deleting that would be a surprise.
    if [ -L "$LINK" ] && [ "$(readlink -f "$LINK" 2>/dev/null || true)" = "${BUNDLE_DIR}/anoa.sh" ]; then
        rm -f "$LINK"; say "removed ${LINK}"; removed=1
    elif [ -e "$LINK" ]; then
        warn "left ${LINK} alone: it is not a link into ${BUNDLE_DIR}"
    fi
    if [ -d "$BUNDLE_DIR" ]; then
        rm -rf "$BUNDLE_DIR"; say "removed ${BUNDLE_DIR}"; removed=1
    fi
    [ "$removed" -eq 1 ] || say "nothing to remove"
    exit 0
fi

# ── Preconditions ───────────────────────────────────────────────────────────

[ "$(uname -s)" = "Linux" ] || die "this installer is for Linux; on macOS use: brew install --cask anoa"

if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fsSL "$1" -o "$2"; }
    fetch_stdout() { curl -fsSL "$1"; }
elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -qO "$2" "$1"; }
    fetch_stdout() { wget -qO- "$1"; }
else
    die "needs curl or wget"
fi
command -v tar >/dev/null 2>&1 || die "needs tar"

# ── Resolve the version ─────────────────────────────────────────────────────

if [ -z "$VERSION" ]; then
    say "Resolving the latest release…"
    # Parsed with grep/sed rather than jq, which is not installed by default on
    # most distributions and would make this script fail for a formatting tool.
    VERSION="$(fetch_stdout "https://api.github.com/repos/${REPO}/releases/latest" \
                | grep -m1 '"tag_name"' \
                | sed -E 's/.*"tag_name"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/')" \
        || die "could not reach the GitHub API"
    [ -n "$VERSION" ] || die "could not determine the latest release; pass --version"
fi
case "$VERSION" in v*) ;; *) VERSION="v${VERSION}" ;; esac

URL="https://github.com/${REPO}/releases/download/${VERSION}/${ASSET}"

# ── Download and unpack ─────────────────────────────────────────────────────

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

say "Downloading anoa ${VERSION}…"
fetch "$URL" "${TMP}/${ASSET}" || die "download failed: ${URL}"

# A 404 from a release that has no such asset arrives as an HTML page, and tar
# would report something unhelpful about a truncated archive.
case "$(head -c2 "${TMP}/${ASSET}" | od -An -tx1 | tr -d ' \n')" in
    1f8b) ;;
    *) die "downloaded file is not a gzip archive — is ${VERSION} published with a ${ASSET}?" ;;
esac

say "Unpacking…"
tar xzf "${TMP}/${ASSET}" -C "$TMP"
[ -x "${TMP}/anoa/anoa.sh" ] || die "archive layout unexpected: no anoa/anoa.sh"

# ── Install ─────────────────────────────────────────────────────────────────

mkdir -p "$(dirname "$BUNDLE_DIR")" "$BIN_DIR"

# Swap rather than overwrite: unpacking on top of a running install would mix
# the old and new libraries, and a failure halfway would leave neither working.
if [ -d "$BUNDLE_DIR" ]; then
    rm -rf "${BUNDLE_DIR}.old"
    mv "$BUNDLE_DIR" "${BUNDLE_DIR}.old"
fi
if mv "${TMP}/anoa" "$BUNDLE_DIR"; then
    rm -rf "${BUNDLE_DIR}.old"
else
    [ -d "${BUNDLE_DIR}.old" ] && mv "${BUNDLE_DIR}.old" "$BUNDLE_DIR"
    die "could not install into ${BUNDLE_DIR}"
fi

ln -sfn "${BUNDLE_DIR}/anoa.sh" "$LINK"

# ── Report ──────────────────────────────────────────────────────────────────

say ""
say "Installed anoa ${VERSION}"
say "  bundle:   ${BUNDLE_DIR}"
say "  launcher: ${LINK}"

# Compare against the real PATH entries rather than substring-matching, so that
# a directory whose name merely contains "$BIN_DIR" is not mistaken for it.
on_path=0
IFS=':'
for d in $PATH; do
    [ "$d" = "$BIN_DIR" ] && on_path=1 && break
done
unset IFS

if [ "$on_path" -eq 1 ]; then
    # Being on PATH is not the same as winning it. An earlier install through
    # Homebrew or a package manager keeps answering to `anoa` from a directory
    # that comes first, so the version just installed is not the one that runs
    # and the bug it fixed appears to survive the upgrade.
    winner="$(command -v anoa 2>/dev/null || true)"
    if [ -n "$winner" ] && [ "$winner" != "$LINK" ] \
       && [ "$(readlink -f "$winner" 2>/dev/null)" != "$(readlink -f "$LINK" 2>/dev/null)" ]; then
        other_version="$("$winner" --version 2>/dev/null | head -1)"
        warn ""
        warn "Another anoa comes first on your PATH and will run instead:"
        warn "  ${winner}${other_version:+  (${other_version})}"
        warn "  just installed: ${LINK}  (${VERSION})"
        warn ""
        warn "Remove the other one, or put ${BIN_DIR} ahead of it."
        say ""
        say "Try this one directly:  ${LINK} terminal"
    else
        say ""
        say "Try it:  anoa terminal"
    fi
else
    say ""
    warn "${BIN_DIR} is not on your PATH. Add it:"
    case "$(basename "${SHELL:-/bin/sh}")" in
        zsh)  rc="~/.zshrc" ;;
        bash) rc="~/.bashrc" ;;
        *)    rc="your shell's startup file" ;;
    esac
    warn ""
    warn "  echo 'export PATH=\"${BIN_DIR}:\$PATH\"' >> ${rc}"
    warn "  exec \$SHELL"
    warn ""
    warn "Until then, run it with the full path: ${LINK}"
fi
