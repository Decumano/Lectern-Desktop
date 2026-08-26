// Locating the app's own files at runtime.
//
// The frontend is a directory of assets that ships alongside the binary, and
// where "alongside" lands differs per package: next to the exe in a Windows
// install or a portable zip, under `../share/lectern` for a .deb/.rpm/Flatpak,
// inside `../Resources` in a macOS .app bundle. Baking the build machine's
// source path in works only for a build tree, so resolution happens here,
// relative to the running executable.
#pragma once

#include <filesystem>

namespace lectern::assets {

/// Absolute path of the running executable, resolved through the OS rather
/// than argv[0] (which a caller controls and need not be a real path).
std::filesystem::path executable_path();

/// The directory holding index.html. Search order:
///   1. $LECTERN_FRONTEND_DIR              — explicit override
///   2. <exe>/frontend                     — Windows install, portable zip
///   3. <exe>/../share/lectern/frontend    — deb, rpm, Flatpak
///   4. <exe>/../Resources/frontend        — macOS .app bundle
///   5. the configure-time default         — running from a build tree
/// Returns the first candidate that actually contains index.html; when none
/// does, returns candidate 2 so the error message names a sensible location.
std::filesystem::path frontend_dir();

}  // namespace lectern::assets
