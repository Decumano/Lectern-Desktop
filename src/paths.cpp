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

fs::path fRootsFile()
{
    return fAppConfigDir() / "authorized_roots.json";
}

}  // namespace

void fUsePortableDataDir(const fs::path &root)
{
    std::lock_guard<std::mutex> lock(g_data_dir_mutex);
    g_portable_data_dir = root;
}

fs::path fAppConfigDir()
{
    if (const auto override_dir = util::fEnv("LECTERN_DATA_DIR"))
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
        base = fs::path(util::fEnvOr("APPDATA", "."));
    }
    const fs::path dir = base / "Lectern";
#elif defined(__APPLE__)
    base = fs::path(util::fEnvOr("HOME", "."));
    const fs::path dir = base / "Library" / "Application Support" / "Lectern";
#else
    const auto xdg = util::fEnv("XDG_CONFIG_HOME");
    base = (xdg && !xdg->empty()) ? fs::path(*xdg)
                                  : fs::path(util::fEnvOr("HOME", ".")) /
                                        ".config";
    const fs::path dir = base / "lectern";
#endif

    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::string fSafeRelPath(std::string_view rel_path)
{
    const auto fInvalid = [] {
        return std::invalid_argument("invalid path");
    };

    if (rel_path.empty())
    {
        throw fInvalid();
    }
    if (rel_path.front() == '/' || rel_path.front() == '\\')
    {
        throw fInvalid();
    }

    std::vector<std::string> segments;
    std::string current;
    const auto fFlush = [&] {
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
            throw fInvalid();
        }
        // A colon is either a Windows drive prefix ("C:") or an NTFS
        // alternate data stream ("file.mdp:evil"); neither belongs here.
        if (current.find(':') != std::string::npos)
        {
            throw fInvalid();
        }
        segments.push_back(current);
        current.clear();
    };

    for (const char c : rel_path)
    {
        if (c == '/' || c == '\\')
        {
            fFlush();
        }
        else if (c == '\0')
        {
            throw fInvalid();
        }
        else
        {
            current.push_back(c);
        }
    }
    fFlush();

    if (segments.empty())
    {
        throw fInvalid();
    }

    std::string out = segments.front();
    for (size_t i = 1; i < segments.size(); ++i)
    {
        out += "/";
        out += segments[i];
    }
    return out;
}

void fLoadAuthorizedRoots()
{
    const auto data = util::fReadFile(fRootsFile());
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

void fAuthorizeRoot(const fs::path &picked)
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
    // fLoadAuthorizedRoots — would otherwise be silently dropped, and the
    // user would find a folder they had already picked no longer trusted.
    fLoadAuthorizedRoots();

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

    util::fWriteFileAtomic(fRootsFile(), json(all).dump(2));
}

fs::path fCheckRoot(const std::string &root)
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

fs::path fResolveWorkPath(const std::string &root,
                           const std::string &rel_path)
{
    return fCheckRoot(root) / fSafeRelPath(rel_path);
}

}  // namespace lectern::paths
