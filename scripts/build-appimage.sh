#!/usr/bin/env bash
# Builds an AppImage from a staged install tree.
#
# AppImage is not a CPack generator, so this stages `cmake --install` into an
# AppDir and hands it to linuxdeploy, which pulls in the shared libraries the
# binary needs. The webview itself (webkitgtk) is deliberately NOT bundled: it
# ships with the desktop, is security-sensitive, and bundling a stale copy is
# how you end up shipping an unpatched browser engine.
#
# Usage: scripts/build-appimage.sh <build-dir> <version> [output-dir]
set -euo pipefail

BUILD_DIR="${1:?usage: build-appimage.sh <build-dir> <version> [output-dir]}"
VERSION="${2:?missing version}"
OUTPUT_DIR="${3:-$PWD}"

APPDIR="$(mktemp -d)/Lectern.AppDir"
trap 'rm -rf "$(dirname "$APPDIR")"' EXIT

echo "==> staging install tree into $APPDIR"
cmake --install "$BUILD_DIR" --prefix "$APPDIR/usr"

# linuxdeploy wants the desktop file and icon at the AppDir root.
cp "$APPDIR/usr/share/applications/com.lectern.app.desktop" "$APPDIR/"
cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/com.lectern.app.png" "$APPDIR/"

TOOL_DIR="${LINUXDEPLOY_DIR:-$PWD/.linuxdeploy}"
mkdir -p "$TOOL_DIR"
LINUXDEPLOY="$TOOL_DIR/linuxdeploy-x86_64.AppImage"

if [ ! -x "$LINUXDEPLOY" ]; then
  echo "==> downloading linuxdeploy"
  curl -fsSL -o "$LINUXDEPLOY" \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
  chmod +x "$LINUXDEPLOY"
fi

echo "==> building AppImage"
export VERSION
# The frontend lives under usr/share/lectern; assets.cpp finds it relative to
# the executable, which inside an AppImage resolves through /proc/self/exe to
# the mounted AppDir. Nothing else needs to know it is an AppImage.
"$LINUXDEPLOY" \
  --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/lectern" \
  --desktop-file "$APPDIR/com.lectern.app.desktop" \
  --icon-file "$APPDIR/com.lectern.app.png" \
  --exclude-library 'libwebkit2gtk*' \
  --exclude-library 'libwebkitgtk*' \
  --exclude-library 'libjavascriptcoregtk*' \
  --exclude-library 'libgtk-*' \
  --exclude-library 'libgdk-*' \
  --output appimage

mkdir -p "$OUTPUT_DIR"
mv Lectern*.AppImage "$OUTPUT_DIR/Lectern-${VERSION}-linux-x86_64.AppImage"
echo "==> $OUTPUT_DIR/Lectern-${VERSION}-linux-x86_64.AppImage"
