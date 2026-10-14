// On-disk storage for Documents/Sheets/Diagrams/... — the local half of what
// the server exposes over HTTP. Everything here is reachable from the webview
// through the exposed functions in main.cpp, so every path goes through
// paths::resolve_work_path first.
#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace lectern::workfolder {

inline constexpr std::array<std::string_view, 9> kWorkFileExtensions = {
    "mdp", "mds", "mdg", "mdn", "mdl", "mdc", "mde", "mdb", "mdd"};

/// Root-level sidecars that sync alongside work files: the custom-templates
/// store and roaming UI preferences (theme), so both follow the account across
/// devices (the server's workspace listing exposes the same set — see
/// Lectern-Server-Cpp workspace.h kSyncSidecarFiles).
inline constexpr std::array<std::string_view, 2> kSyncSidecarFiles = {
    "_lktpl.json", "_lkprefs.json"};

bool is_work_file(std::string_view name);

/// The tree the sidebar renders: name, relPath, isDir, modified, children.
/// Directories first, then case-insensitive by name.
nlohmann::json walk_work_dir(const std::filesystem::path &dir,
                             const std::string &rel_prefix);

/// The default document a fresh install opens with.
nlohmann::json default_file(const std::string &name);

/// Largest file fetch_remote will download (presentation background images).
inline constexpr size_t kMaxRemoteFetchBytes = 25 * 1024 * 1024;

/// Whether `url` is something fetch_remote may download: http or https only,
/// so a page can't use it to read local files through file:// and the like.
bool is_fetchable_url(std::string_view url);

/// Downloads `url` so the frontend can store a copy inside the work folder —
/// the page itself usually can't, since most image hosts don't send CORS
/// headers. Returns {data: base64, contentType}; throws on a refused URL, an
/// HTTP error, or a body over kMaxRemoteFetchBytes.
nlohmann::json fetch_remote(const std::string &url);

}  // namespace lectern::workfolder
