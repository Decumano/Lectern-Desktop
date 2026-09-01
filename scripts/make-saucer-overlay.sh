#!/usr/bin/env bash
# Builds a vcpkg overlay port for saucer from a vcpkg checkout's own copy.
#
# vcpkg's saucer 6.0.1#8 does not build against a current Linux toolchain, and
# it pulls in a GTK that must not be used. Three changes, none of which alter
# what the library does:
#
#   1. Drop the `gtk` dependency. The port builds GTK 4 from source, but the
#      WebKitGTK it links against is the system's (or, inside Flatpak, the
#      runtime's), built against that GTK. Two GTKs in one process do not
#      work — the system's has to win. The symptom otherwise is a link that
#      fails on gdk_wayland_* symbols, because vcpkg's GTK is X11-only.
#
#   2. Replace -Werror with -Wno-subobject-linkage. saucer compiles itself
#      with -Werror -pedantic-errors -Wfatal-errors; GCC — unlike the Clang
#      upstream develops against — reports the `eraser::method<0, lambda>`
#      bases of saucer::webview as having internal linkage. Dropping -Werror
#      also covers whatever the next GCC decides to warn about in there.
#
#   3. Swap the argument order in glaze.inl's two concepts. saucer 6.0.1
#      writes glz::read_supported<Format, T>; glaze >= 5.1 declares it
#      <T, Format>. This is the same version skew that put src/bridge.h on a
#      nlohmann serializer — the glaze path is compiled but never used, so
#      this only has to parse.
#
# Usage: scripts/make-saucer-overlay.sh <vcpkg-root> <overlay-dir>
#
# Then point vcpkg at it:  -DVCPKG_OVERLAY_PORTS=<overlay-dir>
set -euo pipefail

VCPKG_ROOT_DIR="${1:?usage: make-saucer-overlay.sh <vcpkg-root> <overlay-dir>}"
OVERLAY_DIR="${2:?missing overlay dir}"

SRC="$VCPKG_ROOT_DIR/ports/saucer"
DEST="$OVERLAY_DIR/saucer"

if [ ! -f "$SRC/portfile.cmake" ]; then
  echo "no saucer port at $SRC" >&2
  exit 1
fi

echo "==> copying $SRC to $DEST"
mkdir -p "$OVERLAY_DIR"
rm -rf "$DEST"
cp -r "$SRC" "$DEST"

echo "==> dropping the gtk dependency"
# Edited as JSON rather than by line range: the port's formatting is not a
# contract, and a bad range would silently produce a manifest missing other
# dependencies.
python3 - "$DEST/vcpkg.json" <<'PY'
import json, sys

path = sys.argv[1]
with open(path) as f:
    manifest = json.load(f)

before = len(manifest["dependencies"])
manifest["dependencies"] = [
    dep for dep in manifest["dependencies"]
    if not (isinstance(dep, dict) and dep.get("name") == "gtk")
]

if len(manifest["dependencies"]) == before:
    sys.exit("no gtk dependency found — has the port changed?")

with open(path, "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
PY

echo "==> patching the source at build time"
python3 - "$DEST/portfile.cmake" <<'PY'
import sys

path = sys.argv[1]
with open(path) as f:
    portfile = f.read()

inl = "${SOURCE_PATH}/include/saucer/serializers/glaze/glaze.inl"
edits = "\n".join([
    'vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt"',
    '    "-Werror" "-Wno-subobject-linkage")',
    f'vcpkg_replace_string("{inl}"',
    '    "glz::read_supported<opts.format, T>"',
    '    "glz::read_supported<T, opts.format>")',
    f'vcpkg_replace_string("{inl}"',
    '    "glz::write_supported<opts.format, T>"',
    '    "glz::write_supported<T, opts.format>")',
    "",
    "vcpkg_cmake_configure(",
])

if "vcpkg_cmake_configure(" not in portfile:
    sys.exit("no vcpkg_cmake_configure() call in the portfile — has it changed?")

portfile = portfile.replace("vcpkg_cmake_configure(", edits, 1)

with open(path, "w") as f:
    f.write(portfile)
PY

echo "==> $DEST"
