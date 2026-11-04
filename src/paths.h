// Path validation and the authorized-root registry.
//
// Every work-folder command takes `root` and `rel_path` straight from
// JavaScript. The folder-picker dialog gates the UI, but not the IPC layer:
// any script that reaches an exposed function (e.g. via markup injected from a
// shared document) could otherwise read, overwrite or delete arbitrary paths —
// joining an absolute `rel_path` onto `root` even discards `root` entirely.
// So the commands themselves enforce two rules:
//   1. `rel_path` must be strictly relative: no absolute paths, drive
//      letters, or `..` components (same rule as the server's
//      workspace.cpp `fSafeRelPath`).
//   2. `root` must be a folder the user actually picked in the OS dialog at
//      some point — the picked set persists in authorized_roots.json so
//      restarts keep working without re-picking.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace lectern::paths {

/// Where the app keeps its own state: authorized_roots.json, cloud.json and
/// the webview's profile. Per-user by default (`%APPDATA%/Lectern`,
/// `~/.config/lectern`, `~/Library/Application Support/Lectern`), created if
/// missing.
///
/// $LECTERN_DATA_DIR overrides it, and a portable build redirects it next to
/// the executable via `fUsePortableDataDir` — a portable app that scattered
/// state into the host's profile would not be portable.
std::filesystem::path fAppConfigDir();

/// Redirects `fAppConfigDir` at `root`. Call before anything reads config.
/// Ignored when $LECTERN_DATA_DIR is set, which always wins.
void fUsePortableDataDir(const std::filesystem::path &root);

/// Canonical relative path with forward slashes, or throws std::invalid_argument.
std::string fSafeRelPath(std::string_view rel_path);

/// Records a folder the user picked through the OS dialog, persisting it so
/// the app still trusts it after a restart. The dialog is the trust boundary.
void fAuthorizeRoot(const std::filesystem::path &picked);

/// Canonicalized `root` if (and only if) it's a folder the user has picked
/// through the OS dialog; throws std::invalid_argument otherwise.
std::filesystem::path fCheckRoot(const std::string &root);

/// `fCheckRoot` plus `fSafeRelPath`, joined.
std::filesystem::path fResolveWorkPath(const std::string &root,
                                        const std::string &rel_path);

/// Loads the persisted set. Called once at startup.
void fLoadAuthorizedRoots();

}  // namespace lectern::paths
