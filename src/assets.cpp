#include "assets.h"

#include <vector>

#include "util.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#else
#include <climits>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lectern::assets {

#ifndef LECTERN_DEFAULT_FRONTEND_DIR
#define LECTERN_DEFAULT_FRONTEND_DIR "frontend"
#endif

fs::path executable_path()
{
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true)
    {
        const DWORD written = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0)
        {
            return {};
        }
        // Truncation is reported by filling the buffer exactly; grow and retry.
        if (written < buffer.size())
        {
            return fs::path(std::wstring(buffer.data(), written));
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    {
        return {};
    }
    std::error_code ec;
    const fs::path resolved = fs::canonical(fs::path(buffer.data()), ec);
    return ec ? fs::path(buffer.data()) : resolved;
#else
    std::error_code ec;
    const fs::path resolved = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : resolved;
#endif
}

fs::path frontend_dir()
{
    const auto has_index = [](const fs::path &candidate) {
        std::error_code ec;
        return !candidate.empty() &&
               fs::is_regular_file(candidate / "index.html", ec) && !ec;
    };

    if (const auto configured = util::env("LECTERN_FRONTEND_DIR"))
    {
        if (!configured->empty())
        {
            // An explicit override wins even if it turns out to be wrong, so
            // the resulting error names what the user actually asked for.
            return fs::path(*configured);
        }
    }

    const fs::path exe = executable_path();
    const fs::path exe_dir = exe.empty() ? fs::current_path() : exe.parent_path();

    const std::vector<fs::path> candidates = {
        exe_dir / "frontend",
        exe_dir / ".." / "share" / "lectern" / "frontend",
        exe_dir / ".." / "Resources" / "frontend",
        fs::path(LECTERN_DEFAULT_FRONTEND_DIR),
    };

    for (const auto &candidate : candidates)
    {
        if (has_index(candidate))
        {
            std::error_code ec;
            const fs::path normalized = fs::weakly_canonical(candidate, ec);
            return ec ? candidate : normalized;
        }
    }

    return candidates.front();
}

}  // namespace lectern::assets
