#include "cloud.h"

#include <cpr/cpr.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "diff.h"
#include "paths.h"
#include "util.h"
#include "workfolder.h"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace lectern::cloud {

namespace {

/// Deliberately keeps its pre-rename name: the directory already exists in
/// every synced work folder, and the server side matches on it too, so
/// renaming it would orphan existing sync state.
constexpr const char *kSyncDir = ".officesuite-sync";
constexpr auto kPollInterval = 45s;

/// Serializes sync passes: `nudge_sync`, the background loop, `sync_now` and
/// `connect` can all fire concurrently, and two passes reconciling the same
/// folder at once would race on state.json and the base snapshots.
std::mutex &sync_lock()
{
    static std::mutex lock;
    return lock;
}

// ── Config (which server/account/folder we're connected to) ──

struct CloudConfig
{
    std::string server_url;
    std::string email;
    std::string api_token;
    std::string root;
};

fs::path config_path()
{
    return paths::app_config_dir() / "cloud.json";
}

std::optional<CloudConfig> load_config()
{
    const auto data = util::read_file(config_path());
    if (!data)
    {
        return std::nullopt;
    }
    const auto parsed = json::parse(*data, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
    {
        return std::nullopt;
    }

    CloudConfig config;
    config.server_url = parsed.value("server_url", "");
    config.email = parsed.value("email", "");
    config.api_token = parsed.value("api_token", "");
    config.root = parsed.value("root", "");
    if (config.server_url.empty() || config.api_token.empty() ||
        config.root.empty())
    {
        return std::nullopt;
    }
    return config;
}

void save_config(const CloudConfig &config)
{
    util::write_file_atomic(config_path(),
                            json{{"server_url", config.server_url},
                                 {"email", config.email},
                                 {"api_token", config.api_token},
                                 {"root", config.root}}
                                .dump(2));
}

void clear_config()
{
    std::error_code ec;
    fs::remove(config_path(), ec);
}

// ── Per-folder sync state (the "base" snapshot registry + open conflicts) ──

struct SyncState
{
    uint64_t last_synced_at = 0;
    /// rel path → sha256 of the content both sides last agreed on.
    std::map<std::string, std::string> files;
    /// rel path → why it conflicted.
    std::map<std::string, std::string> conflicts;
    /// Folders both sides last agreed on. An empty folder has no file to
    /// imply it, so without a base record there is no way to tell a folder
    /// created on one side from one deleted on the other.
    std::set<std::string> folders;
};

fs::path sync_dir(const std::string &root)
{
    return fs::path(root) / kSyncDir;
}

fs::path state_path(const std::string &root)
{
    return sync_dir(root) / "state.json";
}

fs::path base_path(const std::string &root, const std::string &rel_path)
{
    return sync_dir(root) / "base" / rel_path;
}

fs::path remote_snapshot_path(const std::string &root,
                              const std::string &rel_path)
{
    return sync_dir(root) / "base" / (rel_path + ".remote");
}

SyncState load_state(const std::string &root)
{
    SyncState state;
    const auto data = util::read_file(state_path(root));
    if (!data)
    {
        return state;
    }
    const auto parsed = json::parse(*data, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
    {
        return state;
    }

    state.last_synced_at = parsed.value("last_synced_at", uint64_t{0});
    if (parsed.contains("files") && parsed["files"].is_object())
    {
        for (const auto &[key, value] : parsed["files"].items())
        {
            if (value.is_object() && value.contains("hash") &&
                value["hash"].is_string())
            {
                state.files[key] = value["hash"].get<std::string>();
            }
        }
    }
    if (parsed.contains("conflicts") && parsed["conflicts"].is_object())
    {
        for (const auto &[key, value] : parsed["conflicts"].items())
        {
            state.conflicts[key] = value.is_object()
                                       ? value.value("reason", "")
                                       : std::string{};
        }
    }
    if (parsed.contains("folders") && parsed["folders"].is_array())
    {
        for (const auto &value : parsed["folders"])
        {
            if (value.is_string())
            {
                state.folders.insert(value.get<std::string>());
            }
        }
    }
    return state;
}

void save_state(const std::string &root, const SyncState &state)
{
    json files = json::object();
    for (const auto &[rel, hash] : state.files)
    {
        files[rel] = json{{"hash", hash}};
    }
    json conflicts = json::object();
    for (const auto &[rel, reason] : state.conflicts)
    {
        conflicts[rel] = json{{"reason", reason}};
    }

    std::error_code ec;
    fs::create_directories(sync_dir(root), ec);
    util::write_file_atomic(state_path(root),
                            json{{"last_synced_at", state.last_synced_at},
                                 {"files", files},
                                 {"conflicts", conflicts},
                                 {"folders", state.folders}}
                                .dump(2));
}

void write_base(const std::string &root,
                const std::string &rel_path,
                const std::string &content)
{
    util::write_file_atomic(base_path(root, rel_path), content);
}

std::optional<std::string> read_base(const std::string &root,
                                     const std::string &rel_path)
{
    return util::read_file(base_path(root, rel_path));
}

void remove_base(const std::string &root, const std::string &rel_path)
{
    std::error_code ec;
    fs::remove(base_path(root, rel_path), ec);
}

void write_remote_snapshot(const std::string &root,
                           const std::string &rel_path,
                           const std::string &content)
{
    util::write_file_atomic(remote_snapshot_path(root, rel_path), content);
}

std::optional<std::string> read_remote_snapshot(const std::string &root,
                                                const std::string &rel_path)
{
    return util::read_file(remote_snapshot_path(root, rel_path));
}

void remove_remote_snapshot(const std::string &root,
                            const std::string &rel_path)
{
    std::error_code ec;
    fs::remove(remote_snapshot_path(root, rel_path), ec);
}

// ── Local scanning ──

void walk_local_files(const fs::path &dir,
                      const std::string &rel_prefix,
                      std::set<std::string> &out,
                      std::set<std::string> *out_dirs)
{
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec))
    {
        if (ec)
        {
            return;
        }
        const std::string name = entry.path().filename().string();
        if (!name.empty() && name.front() == '.')
        {
            continue;
        }

        const std::string rel =
            rel_prefix.empty() ? name : rel_prefix + "/" + name;

        std::error_code kind_ec;
        if (entry.is_directory(kind_ec) && !kind_ec)
        {
            if (out_dirs != nullptr)
            {
                out_dirs->insert(rel);
            }
            walk_local_files(entry.path(), rel, out, out_dirs);
            continue;
        }

        // Everything that is not a dotfile syncs, not only the eight app
        // formats. A work folder holds maps, images and plain notes beside
        // the documents, and silently leaving those behind was indis-
        // tinguishable from the sync losing them. Dotfiles are skipped
        // above, which is what keeps the sync's own metadata out.
        out.insert(rel);
    }
}

std::set<std::string> local_file_paths(const std::string &root,
                                       std::set<std::string> *out_dirs = nullptr)
{
    std::set<std::string> out;
    walk_local_files(fs::path(root), "", out, out_dirs);
    return out;
}

uint64_t local_mtime_ms(const fs::path &path)
{
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    if (ec)
    {
        return 0;
    }
    const auto system_time =
        std::chrono::clock_cast<std::chrono::system_clock>(time);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            system_time.time_since_epoch())
                            .count();
    return millis < 0 ? 0 : static_cast<uint64_t>(millis);
}

// ── HTTP client against the Lectern server ──

/// A line-based three-way merge is meaningless on binary content and would
/// happily produce a corrupt file, so anything holding a NUL byte is treated
/// as unmergeable and raised as a conflict for the user to settle instead.
bool looks_binary(std::string_view content)
{
    const size_t window = std::min<size_t>(content.size(), 8192);
    return content.substr(0, window).find('\0') != std::string_view::npos;
}

/// The server's listings/tombstones supply rel paths that this client joins
/// onto its local root and writes/deletes at. A malicious or compromised
/// server must not be able to reach outside the synced folder with an absolute
/// path, a drive prefix, or `..` — every server-supplied path is checked here
/// before it's used, and invalid ones are simply ignored.
bool valid_remote_rel_path(const std::string &rel_path)
{
    try
    {
        return paths::safe_rel_path(rel_path) == rel_path;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

cpr::Header auth_header(const CloudConfig &config)
{
    return cpr::Header{{"Authorization", "Bearer " + config.api_token}};
}

void flatten_remote(const json &entries,
                    std::map<std::string, std::string> &out,
                    std::set<std::string> *out_dirs)
{
    if (!entries.is_array())
    {
        return;
    }
    for (const auto &entry : entries)
    {
        if (!entry.is_object())
        {
            continue;
        }
        if (entry.value("isDir", false))
        {
            const std::string dir_rel = entry.value("relPath", "");
            if (out_dirs != nullptr && valid_remote_rel_path(dir_rel))
            {
                out_dirs->insert(dir_rel);
            }
            flatten_remote(entry.value("children", json::array()), out, out_dirs);
            continue;
        }
        const std::string rel = entry.value("relPath", "");
        if (valid_remote_rel_path(rel))
        {
            out[rel] = entry.value("hash", "");
        }
    }
}

std::map<std::string, std::string> remote_list(const CloudConfig &config,
                                              std::set<std::string> *out_dirs = nullptr)
{
    const auto response = cpr::Get(cpr::Url{config.server_url + "/api/workspace"},
                                   auth_header(config),
                                   cpr::Timeout{30000});
    if (response.status_code < 200 || response.status_code >= 300)
    {
        throw std::runtime_error("list failed: " +
                                 std::to_string(response.status_code));
    }

    const auto tree = json::parse(response.text, nullptr, false);
    std::map<std::string, std::string> flat;
    flatten_remote(tree, flat, out_dirs);
    return flat;
}

/// Files the server knows were deleted, and when. An older server without the
/// endpoint yields an empty map, which simply disables tombstone handling.
std::map<std::string, uint64_t> remote_tombstones(const CloudConfig &config)
{
    const auto response =
        cpr::Get(cpr::Url{config.server_url + "/api/workspace/deleted"},
                 auth_header(config),
                 cpr::Timeout{30000});
    if (response.status_code == 404)
    {
        return {};
    }
    if (response.status_code < 200 || response.status_code >= 300)
    {
        throw std::runtime_error("tombstone list failed: " +
                                 std::to_string(response.status_code));
    }

    const auto rows = json::parse(response.text, nullptr, false);
    std::map<std::string, uint64_t> out;
    if (!rows.is_array())
    {
        return out;
    }
    for (const auto &row : rows)
    {
        const std::string rel = row.value("relPath", "");
        if (valid_remote_rel_path(rel))
        {
            out[rel] = row.value("deletedAt", uint64_t{0});
        }
    }
    return out;
}

std::string remote_read(const CloudConfig &config, const std::string &rel_path)
{
    const auto response =
        cpr::Get(cpr::Url{config.server_url + "/api/workspace/file"},
                 cpr::Parameters{{"path", rel_path}},
                 auth_header(config),
                 cpr::Timeout{60000});
    if (response.status_code < 200 || response.status_code >= 300)
    {
        throw std::runtime_error("read failed: " +
                                 std::to_string(response.status_code));
    }
    return response.text;
}

void remote_write(const CloudConfig &config,
                  const std::string &rel_path,
                  const std::string &content)
{
    cpr::Header headers = auth_header(config);
    // Bytes, not text: a synced folder now carries images and PDFs too.
    headers["Content-Type"] = "application/octet-stream";

    const auto response =
        cpr::Put(cpr::Url{config.server_url + "/api/workspace/file"},
                 cpr::Parameters{{"path", rel_path}},
                 headers,
                 cpr::Body{content},
                 cpr::Timeout{60000});
    if (response.status_code < 200 || response.status_code >= 300)
    {
        throw std::runtime_error("write failed: " +
                                 std::to_string(response.status_code));
    }
}

void remote_create_folder(const CloudConfig &config,
                          const std::string &rel_path)
{
    cpr::Header headers = auth_header(config);
    headers["Content-Type"] = "application/json";

    const auto response =
        cpr::Post(cpr::Url{config.server_url + "/api/workspace/folder"},
                  headers,
                  cpr::Body{json{{"relPath", rel_path}}.dump()},
                  cpr::Timeout{30000});
    if (response.status_code < 200 || response.status_code >= 300)
    {
        throw std::runtime_error("folder create failed: " +
                                 std::to_string(response.status_code));
    }
}

void remote_delete_folder(const CloudConfig &config,
                          const std::string &rel_path)
{
    const auto response =
        cpr::Delete(cpr::Url{config.server_url + "/api/workspace/entry"},
                    cpr::Parameters{{"path", rel_path}, {"isDir", "true"}},
                    auth_header(config),
                    cpr::Timeout{30000});
    if ((response.status_code < 200 || response.status_code >= 300) &&
        response.status_code != 404)
    {
        throw std::runtime_error("folder delete failed: " +
                                 std::to_string(response.status_code));
    }
}

void remote_delete(const CloudConfig &config, const std::string &rel_path)
{
    const auto response =
        cpr::Delete(cpr::Url{config.server_url + "/api/workspace/entry"},
                    cpr::Parameters{{"path", rel_path}, {"isDir", "false"}},
                    auth_header(config),
                    cpr::Timeout{30000});
    if ((response.status_code < 200 || response.status_code >= 300) &&
        response.status_code != 404)
    {
        throw std::runtime_error("delete failed: " +
                                 std::to_string(response.status_code));
    }
}

std::string login(const std::string &server_url,
                  const std::string &email,
                  const std::string &password)
{
    const auto response =
        cpr::Post(cpr::Url{server_url + "/api/auth/login"},
                  cpr::Header{{"Content-Type", "application/json"}},
                  cpr::Body{json{{"email", email},
                                 {"password", password}}
                                .dump()},
                  cpr::Timeout{30000});
    if (response.status_code < 200 || response.status_code >= 300)
    {
        throw std::runtime_error("login failed: " +
                                 std::to_string(response.status_code));
    }

    const auto user = json::parse(response.text, nullptr, false);
    const std::string token =
        user.is_object() ? user.value("apiToken", "") : "";
    if (token.empty())
    {
        throw std::runtime_error("login returned no API token");
    }
    return token;
}

// ── Reconciliation ──

/// Reconciles one path. Extracted from the loop so a failure on a single
/// file can be caught without abandoning the rest of the pass — one file the
/// server rejects (an oversized notebook, say) used to abort the whole sync
/// and, through connect(), roll the connection back entirely.
void sync_one_path(const CloudConfig &config,
                   const std::string &rel_path,
                   SyncState &state,
                   const std::map<std::string, std::string> &remote_hashes,
                   const std::map<std::string, uint64_t> &tombstones)
{
    const fs::path local_full = fs::path(config.root) / rel_path;

    if (state.conflicts.count(rel_path) > 0)
    {
        // A conflict whose file is gone on both sides has nothing left to
        // resolve; dropping it un-wedges the path (a lingering entry
        // would block syncing a future file under the same name forever).
        std::error_code ec;
        const bool local_gone = !fs::is_regular_file(local_full, ec) || ec;
        if (local_gone && remote_hashes.count(rel_path) == 0)
        {
            state.conflicts.erase(rel_path);
            remove_remote_snapshot(config.root, rel_path);
            remove_base(config.root, rel_path);
            state.files.erase(rel_path);
        }
        // Otherwise leave conflicted files alone until the user resolves.
        return;
    }

    std::error_code ec;
    std::optional<std::string> local_content;
    if (fs::is_regular_file(local_full, ec) && !ec)
    {
        local_content = util::read_file(local_full);
    }

    const std::optional<std::string> local_hash =
        local_content ? std::optional(util::sha256_hex(*local_content))
                      : std::nullopt;

    const auto remote_it = remote_hashes.find(rel_path);
    const std::optional<std::string> remote_hash =
        remote_it == remote_hashes.end()
            ? std::nullopt
            : std::optional(remote_it->second);

    const auto base_it = state.files.find(rel_path);
    const std::optional<std::string> base_hash =
        base_it == state.files.end() ? std::nullopt
                                     : std::optional(base_it->second);

    const bool local_changed = local_hash != base_hash;
    const bool remote_changed = remote_hash != base_hash;

    if (!local_changed && !remote_changed)
    {
        return;
    }

    if (local_changed && !remote_changed)
    {
        if (local_content)
        {
            // A local file the sync state has never seen, on a path the
            // server remembers deleting more recently than this copy was
            // written: that's a stale copy resurfacing (lost state.json,
            // a machine that was offline through the delete, a restored
            // backup) — honor the deletion instead of pushing it back
            // onto every other device. A genuine re-creation has a newer
            // mtime than the tombstone and still syncs up normally (the
            // server clears the tombstone on write).
            if (!base_hash)
            {
                const auto tombstone = tombstones.find(rel_path);
                if (tombstone != tombstones.end() &&
                    tombstone->second > local_mtime_ms(local_full))
                {
                    fs::remove(local_full, ec);
                    remove_base(config.root, rel_path);
                    state.files.erase(rel_path);
                    return;
                }
            }

            remote_write(config, rel_path, *local_content);
            write_base(config.root, rel_path, *local_content);
            state.files[rel_path] = *local_hash;
        }
        else if (base_hash)
        {
            remote_delete(config, rel_path);
            remove_base(config.root, rel_path);
            state.files.erase(rel_path);
        }
        return;
    }

    if (!local_changed && remote_changed)
    {
        if (remote_hash)
        {
            const std::string content = remote_read(config, rel_path);
            util::write_file_atomic(local_full, content);
            write_base(config.root, rel_path, content);
            state.files[rel_path] = *remote_hash;
        }
        else if (base_hash)
        {
            fs::remove(local_full, ec);
            remove_base(config.root, rel_path);
            state.files.erase(rel_path);
        }
        return;
    }

    // Both sides changed.
    if (!local_content && !remote_hash)
    {
        remove_base(config.root, rel_path);
        state.files.erase(rel_path);
        return;
    }

    if (local_content && remote_hash)
    {
        if (base_hash)
        {
            const std::string remote_text = remote_read(config, rel_path);
            const std::string base_text =
                read_base(config.root, rel_path).value_or("");

            const bool mergeable = !looks_binary(base_text) &&
                                   !looks_binary(*local_content) &&
                                   !looks_binary(remote_text);
            const auto merge =
                mergeable ? diff::three_way_merge(base_text, *local_content,
                                                  remote_text)
                          : diff::MergeResult{};
            if (mergeable && merge.clean)
            {
                util::write_file_atomic(local_full, merge.text);
                remote_write(config, rel_path, merge.text);
                write_base(config.root, rel_path, merge.text);
                state.files[rel_path] = util::sha256_hex(merge.text);
            }
            else
            {
                write_remote_snapshot(config.root, rel_path, remote_text);
                state.conflicts[rel_path] =
                    mergeable ? "edit-edit" : "edit-edit (binary)";
            }
        }
        else if (local_hash == remote_hash)
        {
            write_base(config.root, rel_path, *local_content);
            state.files[rel_path] = *local_hash;
        }
        else
        {
            const std::string remote_text = remote_read(config, rel_path);
            write_remote_snapshot(config.root, rel_path, remote_text);
            state.conflicts[rel_path] = "created-both";
        }
        return;
    }

    // One side edited, the other deleted.
    std::string remote_text;
    if (remote_hash)
    {
        try
        {
            remote_text = remote_read(config, rel_path);
        }
        catch (const std::exception &)
        {
            remote_text.clear();
        }
    }
    write_remote_snapshot(config.root, rel_path, remote_text);
    state.conflicts[rel_path] = "edit-delete";
}

/// Runs one reconciliation pass. Returns the files that could not be synced;
/// an empty result means everything is in step. Throws only when the pass
/// could not run at all (server unreachable, credentials rejected).
bool remote_has_children(const std::map<std::string, std::string> &remote_hashes,
                         const std::string &rel_path)
{
    const std::string prefix = rel_path + "/";
    for (const auto &[path, _] : remote_hashes)
    {
        if (path.rfind(prefix, 0) == 0)
        {
            return true;
        }
    }
    return false;
}

/// A folder only reaches the server as a side effect of uploading a file into
/// it, so a folder that is empty -- or holds nothing but other empty folders
/// -- stays invisible there no matter how many times sync runs. This walks the
/// folders themselves, applying the same three-way reasoning sync_one_path
/// applies to files: state.folders is the base that separates "created here"
/// from "deleted there".
///
/// Runs after the file pass, so a folder this sync emptied can be dropped, and
/// so a folder is only judged once the files inside it have settled.
void sync_folders(const CloudConfig &config,
                  SyncState &state,
                  const std::set<std::string> &local_dirs,
                  const std::set<std::string> &remote_dirs,
                  const std::map<std::string, std::string> &remote_hashes,
                  std::vector<std::string> &failures)
{
    std::set<std::string> every = local_dirs;
    every.insert(remote_dirs.begin(), remote_dirs.end());
    every.insert(state.folders.begin(), state.folders.end());

    // Deepest first: a parent is only reconsidered once its children have
    // been, so removing one never strands the folders inside it.
    std::vector<std::string> ordered(every.begin(), every.end());
    const auto depth = [](const std::string &path) {
        return std::count(path.begin(), path.end(), '/');
    };
    std::sort(ordered.begin(), ordered.end(),
              [&depth](const std::string &a, const std::string &b) {
                  return depth(a) != depth(b) ? depth(a) > depth(b) : a < b;
              });

    std::set<std::string> agreed;
    for (const auto &rel_path : ordered)
    {
        const bool in_local = local_dirs.count(rel_path) != 0;
        const bool in_remote = remote_dirs.count(rel_path) != 0;
        const bool in_base = state.folders.count(rel_path) != 0;
        try
        {
            if (in_local && in_remote)
            {
                agreed.insert(rel_path);
            }
            else if (in_local && !in_base)
            {
                remote_create_folder(config, rel_path);
                agreed.insert(rel_path);
            }
            else if (in_remote && !in_base)
            {
                std::error_code ec;
                fs::create_directories(fs::path(config.root) / rel_path, ec);
                if (ec)
                {
                    throw std::runtime_error(ec.message());
                }
                agreed.insert(rel_path);
            }
            else if (in_remote && !in_local)
            {
                // Gone locally. Only drop it remotely once nothing is left
                // inside, so a partly-synced tree cannot lose files.
                if (remote_has_children(remote_hashes, rel_path))
                {
                    agreed.insert(rel_path);
                }
                else
                {
                    remote_delete_folder(config, rel_path);
                }
            }
            else if (in_local)
            {
                // Gone remotely. fs::remove refuses a non-empty directory,
                // which is exactly the safety wanted here -- if anything is
                // still inside, keep the folder and try again next pass.
                std::error_code ec;
                if (!fs::remove(fs::path(config.root) / rel_path, ec) || ec)
                {
                    agreed.insert(rel_path);
                }
            }
        }
        catch (const std::exception &error)
        {
            failures.push_back(rel_path + " — " + error.what());
            spdlog::warn("sync: folder {} failed: {}", rel_path, error.what());
        }
    }
    state.folders = agreed;
}

std::vector<std::string> sync_once(const CloudConfig &config)
{
    std::lock_guard<std::mutex> guard(sync_lock());
    SyncState state = load_state(config.root);
    std::vector<std::string> failures;

    std::set<std::string> remote_dirs;
    const auto remote_hashes = remote_list(config, &remote_dirs);

    std::map<std::string, uint64_t> tombstones;
    try
    {
        tombstones = remote_tombstones(config);
    }
    catch (const std::exception &)
    {
        // Older server, or a transient failure: proceed without tombstones
        // rather than failing the whole pass.
    }

    std::set<std::string> local_dirs;
    std::set<std::string> all_paths = local_file_paths(config.root, &local_dirs);
    for (const auto &[rel, _] : remote_hashes)
    {
        all_paths.insert(rel);
    }
    for (const auto &[rel, _] : state.files)
    {
        all_paths.insert(rel);
    }

    for (const auto &rel_path : all_paths)
    {
        try
        {
            sync_one_path(config, rel_path, state, remote_hashes, tombstones);
        }
        catch (const std::exception &error)
        {
            // Record and move on. Reporting which file failed is the whole
            // point: "write failed: 413" with no path is unactionable.
            failures.push_back(rel_path + " — " + error.what());
            spdlog::warn("sync: {} failed: {}", rel_path, error.what());
        }
    }

    // Every path failing means the problem is the connection, not the files
    // — surface that instead of reporting a clean pass.
    if (!failures.empty() && failures.size() == all_paths.size())
    {
        throw std::runtime_error(failures.front());
    }

    sync_folders(config, state, local_dirs, remote_dirs, remote_hashes,
                 failures);

    state.last_synced_at = static_cast<uint64_t>(util::now_ms());
    save_state(config.root, state);
    return failures;
}

std::string insert_suffix(const std::string &rel_path,
                          const std::string &suffix)
{
    const size_t dot = rel_path.rfind('.');
    if (dot == std::string::npos)
    {
        return rel_path + suffix;
    }
    return rel_path.substr(0, dot) + suffix + "." + rel_path.substr(dot + 1);
}

void resolve_conflict_impl(const CloudConfig &config,
                           const std::string &rel_path,
                           const std::string &choice)
{
    std::lock_guard<std::mutex> guard(sync_lock());
    SyncState state = load_state(config.root);
    if (state.conflicts.count(rel_path) == 0)
    {
        throw std::runtime_error("no such conflict");
    }

    const fs::path local_full = fs::path(config.root) / rel_path;
    const std::string remote_text =
        read_remote_snapshot(config.root, rel_path).value_or("");
    const std::string local_text = util::read_file(local_full).value_or("");

    std::error_code ec;
    if (choice == "local")
    {
        // "Keep mine" when my side deleted the file means delete it remotely
        // too — pushing local_text would upload an empty file.
        if (!fs::is_regular_file(local_full, ec) || ec)
        {
            remote_delete(config, rel_path);
            remove_base(config.root, rel_path);
            state.files.erase(rel_path);
        }
        else
        {
            remote_write(config, rel_path, local_text);
            write_base(config.root, rel_path, local_text);
            state.files[rel_path] = util::sha256_hex(local_text);
        }
    }
    else if (choice == "remote")
    {
        util::write_file_atomic(local_full, remote_text);
        write_base(config.root, rel_path, remote_text);
        state.files[rel_path] = util::sha256_hex(remote_text);
    }
    else if (choice == "both")
    {
        const std::string copy_rel =
            insert_suffix(rel_path, " (remote copy)");
        util::write_file_atomic(fs::path(config.root) / copy_rel, remote_text);
        remote_write(config, copy_rel, remote_text);
        write_base(config.root, copy_rel, remote_text);
        state.files[copy_rel] = util::sha256_hex(remote_text);

        remote_write(config, rel_path, local_text);
        write_base(config.root, rel_path, local_text);
        state.files[rel_path] = util::sha256_hex(local_text);
    }
    else
    {
        throw std::runtime_error("invalid choice");
    }

    state.conflicts.erase(rel_path);
    remove_remote_snapshot(config.root, rel_path);
    save_state(config.root, state);
}

// ── Background loop ──

std::mutex g_failures_mutex;
std::vector<std::string> g_last_failures;

/// Remembers what the most recent pass could not sync.
void remember_failures(const std::vector<std::string> &failures)
{
    std::lock_guard<std::mutex> lock(g_failures_mutex);
    g_last_failures = failures;
}

std::mutex g_loop_mutex;
std::condition_variable g_loop_cv;
std::thread g_loop_thread;
bool g_loop_running = false;
bool g_loop_stop = false;

void start_background_loop()
{
    std::lock_guard<std::mutex> lock(g_loop_mutex);
    if (g_loop_running)
    {
        // A loop from a previous connection is still alive; it re-reads the
        // config every tick, so it will pick up the new connection on its own.
        return;
    }
    g_loop_running = true;
    g_loop_stop = false;

    // A std::thread destroyed while still joinable calls std::terminate. The
    // app calls shutdown() on the way out, but std::exit — which the updater
    // uses to hand off to an installer — and an early error return would both
    // skip it and abort instead. This guard runs at static destruction on
    // every exit path that runs destructors at all.
    static const struct LoopGuard
    {
        ~LoopGuard()
        {
            shutdown();
        }
    } guard;

    g_loop_thread = std::thread([] {
        while (true)
        {
            {
                std::unique_lock<std::mutex> lock(g_loop_mutex);
                if (g_loop_cv.wait_for(
                        lock, kPollInterval, [] { return g_loop_stop; }))
                {
                    break;
                }
            }

            const auto config = load_config();
            if (!config)
            {
                break;  // disconnected
            }
            try
            {
                remember_failures(sync_once(*config));
            }
            catch (const std::exception &error)
            {
                spdlog::debug("sync pass failed: {}", error.what());
            }
        }

        std::lock_guard<std::mutex> lock(g_loop_mutex);
        g_loop_running = false;
    });
}

/// The status object the frontend renders. `syncErrors` is an addition the
/// shared frontend ignores, but it makes the last pass's failures visible to
/// anything that does look — the log, the probe, a future UI.
json status_from_config(const CloudConfig &config)
{
    const SyncState state = load_state(config.root);
    json errors = json::array();
    {
        std::lock_guard<std::mutex> lock(g_failures_mutex);
        for (const auto &failure : g_last_failures)
        {
            errors.push_back(failure);
        }
    }
    return json{{"connected", true},
                {"serverUrl", config.server_url},
                {"email", config.email},
                {"lastSyncedAt", state.last_synced_at},
                {"conflictCount", state.conflicts.size()},
                {"syncErrors", errors}};
}

json disconnected_status()
{
    return json{{"connected", false},
                {"serverUrl", ""},
                {"email", ""},
                {"lastSyncedAt", 0},
                {"conflictCount", 0}};
}

std::string trim_trailing_slashes(std::string value)
{
    while (!value.empty() && value.back() == '/')
    {
        value.pop_back();
    }
    return value;
}

}  // namespace

void nudge_sync()
{
    const auto config = load_config();
    if (!config)
    {
        return;
    }
    std::thread([config = *config] {
        try
        {
            remember_failures(sync_once(config));
        }
        catch (const std::exception &error)
        {
            spdlog::debug("nudge sync failed: {}", error.what());
        }
    }).detach();
}

void resume_on_startup()
{
    if (load_config())
    {
        start_background_loop();
    }
}

void shutdown()
{
    {
        std::lock_guard<std::mutex> lock(g_loop_mutex);
        g_loop_stop = true;
    }
    g_loop_cv.notify_all();
    if (g_loop_thread.joinable())
    {
        g_loop_thread.join();
    }
}

json connect(const std::string &server_url,
             const std::string &email,
             const std::string &password,
             const std::string &root)
{
    CloudConfig config;
    config.server_url = trim_trailing_slashes(server_url);
    config.email = email;
    // Only a folder the user actually picked in the OS dialog may become the
    // sync root the engine writes into.
    paths::check_root(root);
    config.root = root;
    config.api_token = login(config.server_url, email, password);

    save_config(config);

    // Make connect atomic: if the first sync fails (server unreachable, token
    // rejected, ...), roll the config back so the app doesn't sit in a
    // half-connected state with no background loop running.
    try
    {
        const auto failures = sync_once(config);
        for (const auto &failure : failures)
        {
            // Connected, but this file did not go up. Rolling the whole
            // connection back over one file would leave the user with no
            // sync at all and no way to tell why.
            spdlog::warn("connected, but could not sync {}", failure);
        }
        remember_failures(failures);
    }
    catch (...)
    {
        clear_config();
        throw;
    }

    start_background_loop();
    return status_from_config(config);
}

void disconnect()
{
    clear_config();
}

json status()
{
    const auto config = load_config();
    return config ? status_from_config(*config) : disconnected_status();
}

json sync_now()
{
    const auto config = load_config();
    if (!config)
    {
        throw std::runtime_error("not connected");
    }
    const auto failures = sync_once(*config);
    remember_failures(failures);
    if (!failures.empty())
    {
        std::string message =
            "Synced, but " + std::to_string(failures.size()) +
            (failures.size() == 1 ? " file could not be uploaded:\n"
                                  : " files could not be uploaded:\n");
        for (size_t i = 0; i < failures.size() && i < 5; ++i)
        {
            message += "\n  " + failures[i];
        }
        if (failures.size() > 5)
        {
            message += "\n  ...and " +
                       std::to_string(failures.size() - 5) + " more";
        }
        // 413 is by far the most common cause and the least self-explanatory,
        // so name it rather than leaving the user with a bare status code.
        message +=
            "\n\nA 413 means the file is larger than the server accepts.";
        throw std::runtime_error(message);
    }
    return status_from_config(*config);
}

json list_conflicts()
{
    const auto config = load_config();
    if (!config)
    {
        throw std::runtime_error("not connected");
    }

    const SyncState state = load_state(config->root);
    json out = json::array();
    for (const auto &[rel_path, reason] : state.conflicts)
    {
        (void)reason;
        out.push_back(
            {{"relPath", rel_path},
             {"localContent",
              util::read_file(fs::path(config->root) / rel_path).value_or("")},
             {"remoteContent",
              read_remote_snapshot(config->root, rel_path).value_or("")}});
    }
    return out;
}

void resolve_conflict(const std::string &rel_path, const std::string &choice)
{
    if (!valid_remote_rel_path(rel_path))
    {
        throw std::runtime_error("invalid path");
    }
    const auto config = load_config();
    if (!config)
    {
        throw std::runtime_error("not connected");
    }
    resolve_conflict_impl(*config, rel_path, choice);
}

// ── Custom fonts (account-level; see the server's fonts.cpp) ──
// The desktop app has no session/account of its own, so it can't hit
// /api/fonts/:id the way a logged-in browser tab does — instead, when
// cloud-connected, it fetches the bytes through the same authenticated client
// the sync engine uses and hands the JS side a base64 data: URI, which needs
// no further request at all once injected into a @font-face rule.

json list_fonts()
{
    const auto config = load_config();
    if (!config)
    {
        return json::array();  // not connected: no account, no fonts
    }

    const auto response = cpr::Get(cpr::Url{config->server_url + "/api/fonts"},
                                   auth_header(*config),
                                   cpr::Timeout{30000});
    if (response.status_code < 200 || response.status_code >= 300)
    {
        throw std::runtime_error("list fonts failed: " +
                                 std::to_string(response.status_code));
    }

    auto parsed = json::parse(response.text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array())
    {
        return json::array();
    }
    return parsed;
}

std::string font_data_url(const std::string &font_id)
{
    // The id becomes a URL path segment; keep it to the UUID alphabet so a
    // hostile value can't rewrite the request path.
    if (font_id.empty() || font_id.size() > 64)
    {
        throw std::runtime_error("invalid font id");
    }
    for (const char c : font_id)
    {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) == 0 && c != '-')
        {
            throw std::runtime_error("invalid font id");
        }
    }

    const auto config = load_config();
    if (!config)
    {
        throw std::runtime_error("not connected");
    }

    const auto response =
        cpr::Get(cpr::Url{config->server_url + "/api/fonts/" + font_id},
                 auth_header(*config),
                 cpr::Timeout{60000});
    if (response.status_code < 200 || response.status_code >= 300)
    {
        throw std::runtime_error("font fetch failed: " +
                                 std::to_string(response.status_code));
    }

    // The data: URL this returns is spliced into a <style> @font-face rule by
    // the JS side. A server-controlled Content-Type header would be raw text
    // inside that stylesheet, so it's constrained to the known font MIME types
    // rather than trusted verbatim.
    std::string content_type = "font/ttf";
    const auto header = response.header.find("Content-Type");
    if (header != response.header.end())
    {
        const std::string &value = header->second;
        if (value == "font/otf" || value == "font/woff" ||
            value == "font/woff2")
        {
            content_type = value;
        }
    }

    return "data:" + content_type + ";base64," +
           util::base64_encode(response.text);
}

}  // namespace lectern::cloud
