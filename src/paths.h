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
//      workspace.cpp `safe_rel_path`).
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
/// the executable via `use_portable_data_dir` — a portable app that scattered
/// state into the host's profile would not be portable.
std::filesystem::path app_config_dir();

/// Redirects `app_config_dir` at `root`. Call before anything reads config.
/// Ignored when $LECTERN_DATA_DIR is set, which always wins.
void use_portable_data_dir(const std::filesystem::path &root);

/// Canonical relative path with forward slashes, or throws std::invalid_argument.
std::string safe_rel_path(std::string_view rel_path);

/// Records a folder the user picked through the OS dialog, persisting it so
/// the app still trusts it after a restart. The dialog is the trust boundary.
void authorize_root(const std::filesystem::path &picked);

/// Canonicalized `root` if (and only if) it's a folder the user has picked
/// through the OS dialog; throws std::invalid_argument otherwise.
std::filesystem::path check_root(const std::string &root);

/// `check_root` plus `safe_rel_path`, joined.
std::filesystem::path resolve_work_path(const std::string &root,
                                        const std::string &rel_path);

/// Loads the persisted set. Called once at startup.
void load_authorized_roots();

}  // namespace lectern::paths
