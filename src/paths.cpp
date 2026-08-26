#include "paths.h"

#include <nlohmann/json.hpp>

#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

#include "util.h"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lectern::paths {

namespace {

std::mutex g_mutex;
std::set<fs::path> g_authorized_roots;

std::mutex g_data_dir_mutex;
fs::path g_portable_data_dir;

fs::path roots_file()
{
    return app_config_dir() / "authorized_roots.json";
}

}  // namespace

void use_portable_data_dir(const fs::path &root)
{
    std::lock_guard<std::mutex> lock(g_data_dir_mutex);
    g_portable_data_dir = root;
}

fs::path app_config_dir()
{
    if (const auto override_dir = util::env("LECTERN_DATA_DIR"))
    {
        if (!override_dir->empty())
        {
            const fs::path dir(*override_dir);
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_data_dir_mutex);
        if (!g_portable_data_dir.empty())
        {
            const fs::path dir = g_portable_data_dir;
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir;
        }
    }

    fs::path base;

#ifdef _WIN32
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr,
                                       &raw)))
    {
        base = fs::path(raw);
        CoTaskMemFree(raw);
    }
    if (base.empty())
    {
        base = fs::path(util::env_or("APPDATA", "."));
    }
    const fs::path dir = base / "Lectern";
#elif defined(__APPLE__)
    base = fs::path(util::env_or("HOME", "."));
    const fs::path dir = base / "Library" / "Application Support" / "Lectern";
#else
    const auto xdg = util::env("XDG_CONFIG_HOME");
    base = (xdg && !xdg->empty()) ? fs::path(*xdg)
                                  : fs::path(util::env_or("HOME", ".")) /
                                        ".config";
    const fs::path dir = base / "lectern";
#endif

    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::string safe_rel_path(std::string_view rel_path)
{
    const auto invalid = [] {
        return std::invalid_argument("invalid path");
    };

    if (rel_path.empty())
    {
        throw invalid();
    }
    if (rel_path.front() == '/' || rel_path.front() == '\\')
    {
        throw invalid();
    }

    std::vector<std::string> segments;
    std::string current;
    const auto flush = [&] {
        if (current.empty())
        {
            return;
        }
        if (current == ".")
        {
            current.clear();
            return;
        }
        if (current == "..")
        {
            throw invalid();
        }
        // A colon is either a Windows drive prefix ("C:") or an NTFS
        // alternate data stream ("file.mdp:evil"); neither belongs here.
        if (current.find(':') != std::string::npos)
        {
            throw invalid();
        }
        segments.push_back(current);
        current.clear();
    };

    for (const char c : rel_path)
    {
        if (c == '/' || c == '\\')
        {
            flush();
        }
        else if (c == '\0')
        {
            throw invalid();
        }
        else
        {
            current.push_back(c);
        }
    }
    flush();

    if (segments.empty())
    {
        throw invalid();
    }

    std::string out = segments.front();
    for (size_t i = 1; i < segments.size(); ++i)
    {
        out += "/";
        out += segments[i];
    }
    return out;
}

void load_authorized_roots()
{
    const auto data = util::read_file(roots_file());
    if (!data)
    {
        return;
    }

    const auto parsed = json::parse(*data, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto &entry : parsed)
    {
        if (entry.is_string())
        {
            g_authorized_roots.insert(fs::path(entry.get<std::string>()));
        }
    }
}

void authorize_root(const fs::path &picked)
{
    std::error_code ec;
    const fs::path canonical = fs::canonical(picked, ec);
    if (ec)
    {
        throw std::invalid_argument("cannot resolve that folder");
    }

    // Re-read before writing. The file is replaced wholesale with the
    // in-memory set, so anything already on disk that this process never
    // loaded — a second instance's folder, or a caller that skipped
    // load_authorized_roots — would otherwise be silently dropped, and the
    // user would find a folder they had already picked no longer trusted.
    load_authorized_roots();

    std::vector<std::string> all;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_authorized_roots.insert(canonical).second)
        {
            return;  // already trusted; nothing to persist
        }
        for (const auto &root : g_authorized_roots)
        {
            all.push_back(root.string());
        }
    }

    util::write_file_atomic(roots_file(), json(all).dump(2));
}

fs::path check_root(const std::string &root)
{
    std::error_code ec;
    const fs::path canonical = fs::canonical(root, ec);
    if (ec)
    {
        throw std::invalid_argument("cannot resolve that folder");
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_authorized_roots.find(canonical) == g_authorized_roots.end())
    {
        throw std::invalid_argument(
            "folder is not an authorized work folder — pick it again via the "
            "folder dialog");
    }
    return canonical;
}

fs::path resolve_work_path(const std::string &root,
                           const std::string &rel_path)
{
    return check_root(root) / safe_rel_path(rel_path);
}

}  // namespace lectern::paths
