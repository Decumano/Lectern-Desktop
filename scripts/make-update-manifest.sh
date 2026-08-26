#!/usr/bin/env bash
# Builds and signs the updater's latest.json from a directory of release
# artifacts.
#
# The manifest is what src/updater.cpp fetches; the detached signature beside
# it is what makes it trustworthy. The secret key must come from a CI secret
# and must never be committed — the public half is compiled into the app, so
# rotating the key means every already-installed copy stops accepting updates.
#
# Usage:
#   LECTERN_UPDATE_SECRET_KEY=<hex> scripts/make-update-manifest.sh \
#       <version> <artifact-dir> <release-base-url> <updater-tool> [notes]
#
# Writes <artifact-dir>/latest.json and <artifact-dir>/latest.json.sig
set -euo pipefail

VERSION="${1:?usage: make-update-manifest.sh <version> <dir> <base-url> <tool> [notes]}"
DIR="${2:?missing artifact dir}"
BASE_URL="${3%/}"
TOOL="${4:?missing path to lectern-updater-tool}"
NOTES="${5:-Lectern $VERSION}"

: "${LECTERN_UPDATE_SECRET_KEY:?set LECTERN_UPDATE_SECRET_KEY}"

# Platform key -> the artifact that platform installs from. The Windows entry
# is the NSIS installer rather than the .msi because the updater runs it
# directly; the .msi is published for administrators deploying by policy.
declare -A PATTERNS=(
  [windows-x86_64]='*windows-x64-setup.exe'
  [darwin-x86_64]='*macos.dmg'
  [darwin-aarch64]='*macos.dmg'
  [linux-x86_64]='*linux-x86_64.AppImage'
)

entries=""
for platform in "${!PATTERNS[@]}"; do
  file="$(find "$DIR" -maxdepth 1 -name "${PATTERNS[$platform]}" | head -1 || true)"
  [ -n "$file" ] || { echo "skipping $platform (no artifact)" >&2; continue; }

  name="$(basename "$file")"
  sha="$("$TOOL" sha256 "$file")"
  size="$(wc -c < "$file" | tr -d ' ')"

  [ -n "$entries" ] && entries="$entries,"
  entries="$entries
    \"$platform\": {
      \"url\": \"$BASE_URL/$name\",
      \"sha256\": \"$sha\",
      \"size\": $size
    }"
done

[ -n "$entries" ] || { echo "no artifacts matched; refusing to publish an empty manifest" >&2; exit 1; }

cat > "$DIR/latest.json" <<JSON
{
  "version": "$VERSION",
  "pubDate": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "notes": $(printf '%s' "$NOTES" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))'),
  "platforms": {$entries
  }
}
JSON

"$TOOL" sign "$DIR/latest.json" "$LECTERN_UPDATE_SECRET_KEY" > "$DIR/latest.json.sig"

# Never publish a manifest without confirming the app would accept it.
PUBLIC_KEY="${LECTERN_UPDATE_PUBLIC_KEY:-}"
if [ -n "$PUBLIC_KEY" ]; then
  "$TOOL" verify "$DIR/latest.json" "@$DIR/latest.json.sig" "$PUBLIC_KEY"
fi

echo "==> $DIR/latest.json"
cat "$DIR/latest.json"
