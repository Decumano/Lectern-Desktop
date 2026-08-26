#!/usr/bin/env bash
# Codesigns, notarizes and staples the macOS build.
#
# Two modes, because order matters: the .app has to be signed BEFORE it is
# packaged into a .dmg, or the disk image ends up carrying an unsigned app no
# matter how the image itself is signed.
#
#   scripts/sign-macos.sh app <Lectern.app>   before cpack
#   scripts/sign-macos.sh dmg <Lectern.dmg>   after cpack
#
# You supply the Developer ID. This script never creates one: an ad-hoc
# signature (`codesign -s -`) satisfies a "is it signed" check but fails
# notarization, and Gatekeeper refuses to launch a downloaded app that isn't
# notarized. Ad-hoc signing would be worse than shipping unsigned — it hides
# the problem until a user downloads it.
#
# Required in the environment:
#   MACOS_IDENTITY        e.g. "Developer ID Application: Your Name (TEAMID)"
#   MACOS_TEAM_ID         your 10-character team id
#   MACOS_APPLE_ID        the Apple ID the app-specific password belongs to
#   MACOS_APP_PASSWORD    an app-specific password, not your Apple ID password
#
# In CI, import the certificate into a temporary keychain first — see the
# release workflow.
set -euo pipefail

MODE="${1:?usage: sign-macos.sh <app|dmg> <path>}"
TARGET="${2:?missing path}"

: "${MACOS_IDENTITY:?set MACOS_IDENTITY}"

ENTITLEMENTS="$(cd "$(dirname "$0")/.." && pwd)/packaging/entitlements.plist"

sign_app() {
  local app="$1"

  echo "==> signing nested binaries in $app"
  # Inside-out order matters: a bundle's signature covers everything it
  # contains, so a dylib signed after the bundle invalidates the bundle.
  find "$app/Contents" -type f \( -name '*.dylib' -o -name '*.so' \) -print0 |
    while IFS= read -r -d '' lib; do
      codesign --force --timestamp --options runtime \
        --sign "$MACOS_IDENTITY" "$lib"
    done

  echo "==> signing $app"
  codesign --force --timestamp --options runtime \
    --entitlements "$ENTITLEMENTS" \
    --sign "$MACOS_IDENTITY" "$app"

  echo "==> verifying"
  codesign --verify --deep --strict --verbose=2 "$app"
}

sign_and_notarize_dmg() {
  local dmg="$1"

  : "${MACOS_TEAM_ID:?set MACOS_TEAM_ID}"
  : "${MACOS_APPLE_ID:?set MACOS_APPLE_ID}"
  : "${MACOS_APP_PASSWORD:?set MACOS_APP_PASSWORD}"

  echo "==> signing $dmg"
  codesign --force --timestamp --sign "$MACOS_IDENTITY" "$dmg"

  echo "==> notarizing (this waits for Apple)"
  xcrun notarytool submit "$dmg" \
    --apple-id "$MACOS_APPLE_ID" \
    --team-id "$MACOS_TEAM_ID" \
    --password "$MACOS_APP_PASSWORD" \
    --wait

  # Stapling attaches the notarization ticket so Gatekeeper can check it
  # offline; without it, a first launch with no network is refused.
  echo "==> stapling"
  xcrun stapler staple "$dmg"
  xcrun stapler validate "$dmg"
}

case "$MODE" in
  app)
    [ -d "$TARGET" ] || { echo "no such bundle: $TARGET" >&2; exit 1; }
    sign_app "$TARGET"
    ;;
  dmg)
    [ -f "$TARGET" ] || { echo "no such file: $TARGET" >&2; exit 1; }
    sign_and_notarize_dmg "$TARGET"
    ;;
  *)
    echo "unknown mode '$MODE' (expected app or dmg)" >&2
    exit 2
    ;;
esac

echo "==> done"
