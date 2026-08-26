#include "workfolder.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "util.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lectern::workfolder {

namespace {

uint64_t modified_ms(const fs::path &path)
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

}  // namespace

bool is_work_file(std::string_view name)
{
    const std::string ext = util::extension_of(name);
    return std::find(kWorkFileExtensions.begin(),
                     kWorkFileExtensions.end(),
                     ext) != kWorkFileExtensions.end();
}

json walk_work_dir(const fs::path &dir, const std::string &rel_prefix)
{
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec)
    {
        throw std::runtime_error("cannot read " + dir.string());
    }

    struct Entry
    {
        std::string name;
        bool is_dir = false;
        json value;
    };
    std::vector<Entry> entries;

    for (const auto &entry : it)
    {
        const std::string name = entry.path().filename().string();

        // Skip dot-prefixed entries (e.g. `.officesuite-sync/`, the cloud
        // sync engine's metadata folder) so internal bookkeeping never shows
        // up in the file tree.
        if (!name.empty() && name.front() == '.')
        {
            continue;
        }

        std::error_code kind_ec;
        const bool is_dir = entry.is_directory(kind_ec);
        if (kind_ec)
        {
            continue;
        }

        const std::string rel_path =
            rel_prefix.empty() ? name : rel_prefix + "/" + name;

        if (is_dir)
        {
            entries.push_back(
                {name,
                 true,
                 json{{"name", name},
                      {"relPath", rel_path},
                      {"isDir", true},
                      {"modified", 0},
                      {"children", walk_work_dir(entry.path(), rel_path)}}});
            continue;
        }

        if (!is_work_file(name))
        {
            continue;
        }

        entries.push_back({name,
                           false,
                           json{{"name", name},
                                {"relPath", rel_path},
                                {"isDir", false},
                                {"modified", modified_ms(entry.path())},
                                {"children", json::array()}}});
    }

    std::sort(entries.begin(),
              entries.end(),
              [](const Entry &a, const Entry &b) {
                  if (a.is_dir != b.is_dir)
                  {
                      return a.is_dir;
                  }
                  return util::to_lower(a.name) < util::to_lower(b.name);
              });

    json out = json::array();
    for (auto &entry : entries)
    {
        out.push_back(std::move(entry.value));
    }
    return out;
}

json default_file(const std::string &name)
{
    static constexpr const char *kContent =
        "# Welcome to Lectern\n\n"
        "Lectern is a lightweight office suite that stores everything in "
        "**Markdown**.\n\n"
        "## Features\n\n"
        "**Documents** rich markdown editing with live preview\n"
        "**Spreadsheets** formula-capable grid stored as cell=value pairs\n"
        "## Markdown Quick Reference\n"
        "| Element | Syntax |\n"
        "|---------|--------|\n"
        "| Bold | **text** |\n"
        "| Italic | *text* |\n"
        "| Heading | # H1 ## H2 |\n"
        "| List | - item |\n"
        "| Blockquote | > text |\n"
        "| Code | ``` code ``` |\n"
        "| Link | [text](url) |\n\n"
        "## Getting Started\n\n"
        "1. Click **New** in the sidebar to create a file\n"
        "2. Write in Markdown, use the toolbar for formatting\n"
        "3. Toggle the **split** view button to preview alongside your "
        "writing\n"
        "4. Click the **download** button to export your file\n\n"
        "> All files are saved automatically to your browser's local "
        "storage.\n\n"
        "Happy writing!";

    return json{{"name", "Welcome to Lectern " + name},
                {"docType", "doc"},
                {"content", kContent}};
}

}  // namespace lectern::workfolder
