// In-app update check.
//
// The Tauri build had no updater — the web app's Download page read GitHub
// Releases and users reinstalled by hand. This adds the check the desktop app
// was missing, on the same model Tauri's own updater uses: a small JSON
// manifest, an Ed25519 signature over its exact bytes, and a public key
// compiled into the binary. An unsigned or badly signed manifest is discarded
// without being parsed, so the update path cannot be hijacked by whoever
// happens to control the URL or the CDN in front of it.
//
// What it deliberately does not do is replace the running binary. Installing
// is handed to the platform's own installer, which is the piece that already
// knows about elevation, file locking and uninstall entries:
//   - Windows: runs the downloaded setup .exe and exits
//   - macOS:   opens the downloaded .dmg and exits
//   - Linux:   points at the download; the package manager owns the install
// Inside Flatpak the check is skipped entirely — the store updates the app.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace lectern::updater {

struct Update
{
    std::string version;
    std::string notes;
    std::string url;
    std::string sha256;
    uint64_t size = 0;
};

/// True when this build can check at all: a public key and a manifest URL were
/// configured, and the app isn't running inside a Flatpak sandbox.
bool enabled();

/// Fetches and verifies the manifest, returning the newer release when there
/// is one. Blocking; returns nullopt on any failure, including a bad
/// signature — the caller cannot distinguish "up to date" from "could not
/// check", and shouldn't: neither is worth interrupting the user for.
std::optional<Update> check();

/// Downloads the asset to a temp file and verifies its SHA-256 against the
/// manifest. Returns the path on success.
std::optional<std::string> download(const Update &update);

/// Hands the downloaded file to the platform installer and returns true when
/// the app should now quit. On Linux this only reveals the file and returns
/// false, since the package manager owns installation.
bool install(const std::string &downloaded_path);

/// Runs `check` on a background thread and, if an update is waiting, asks the
/// user with a native dialog. Called once at startup; returns immediately.
void run_startup_check();

/// Compares dotted numeric versions. Exposed for testing.
/// Returns <0, 0 or >0 like strcmp.
int compare_versions(const std::string &left, const std::string &right);

/// Verifies a detached Ed25519 signature (hex) over `payload` against a hex
/// public key. Exposed for testing.
bool verify_signature(const std::string &payload,
                      const std::string &signature_hex,
                      const std::string &public_key_hex);

}  // namespace lectern::updater
